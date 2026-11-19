// =============================================================================
//  src/app/mcp_tcp.cpp —— B1：loopback MCP 监听器（复用宿主 LspServer）
// =============================================================================

#include "app/mcp_tcp.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "jsonrpc/json_util.h"
#include "mcp/bridge.h"
#include "mcp/mcp_server.h"
#include "platform/log.h"
#include "server/lsp_server.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace fpgalsp::app {

class McpTcpHost::Impl {
  public:
    bool Start(unsigned short port, server::LspServer& shared_server, const std::string& root,
               bool allow_flow, bool allow_write, std::string* error) {
#ifdef _WIN32
        shared_ = &shared_server;
        root_ = root;
        allow_flow_ = allow_flow;
        allow_write_ = allow_write;
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            if (error) *error = "WSAStartup failed";
            return false;
        }
        listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_ == INVALID_SOCKET) {
            if (error) *error = "socket() failed";
            return false;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        if (bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            listen(listen_, 4) != 0) {
            if (error) *error = "bind/listen failed";
            closesocket(listen_);
            listen_ = INVALID_SOCKET;
            return false;
        }
        int length = sizeof(addr);
        if (getsockname(listen_, reinterpret_cast<sockaddr*>(&addr), &length) == 0) {
            actual_port_ = ntohs(addr.sin_port);
        }
        // 自唤醒通道（UDP 环回）：让 Loop 可以**阻塞**在 WSAPoll 上，
        // 而 Shutdown 仍能立刻把它叫醒。见 Loop()/Wake() 的注释。
        wake_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (wake_ == INVALID_SOCKET) {
            if (error) *error = "wake socket() failed";
            closesocket(listen_);
            listen_ = INVALID_SOCKET;
            return false;
        }
        sockaddr_in wake_addr{};
        wake_addr.sin_family = AF_INET;
        wake_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        wake_addr.sin_port = 0;
        if (bind(wake_, reinterpret_cast<sockaddr*>(&wake_addr), sizeof(wake_addr)) != 0) {
            if (error) *error = "wake bind() failed";
            closesocket(wake_);
            wake_ = INVALID_SOCKET;
            closesocket(listen_);
            listen_ = INVALID_SOCKET;
            return false;
        }
        int wake_length = sizeof(wake_addr);
        if (getsockname(wake_, reinterpret_cast<sockaddr*>(&wake_addr), &wake_length) != 0) {
            if (error) *error = "wake getsockname() failed";
            closesocket(wake_);
            wake_ = INVALID_SOCKET;
            closesocket(listen_);
            listen_ = INVALID_SOCKET;
            return false;
        }
        wake_addr_ = wake_addr;
        thread_ = std::thread([this]() { Loop(); });
        return true;
#else
        (void)port;
        (void)shared_server;
        (void)root;
        (void)allow_flow;
        (void)allow_write;
        if (error) *error = "mcp loopback is Windows-only";
        return false;
#endif
    }

    unsigned short actual_port() const { return actual_port_; }

    void Shutdown() {
#ifdef _WIN32
        stop_.store(true);
        // 先叫醒 WSAPoll，再关 socket：否则关闭中的 socket 会让 poll 以错误返回，
        // 虽然也能靠 stop_ 退出，但这属于"靠运气"，不是"靠设计"。
        Wake();
        if (listen_ != INVALID_SOCKET) {
            closesocket(listen_);
            listen_ = INVALID_SOCKET;
        }
        if (wake_ != INVALID_SOCKET) {
            closesocket(wake_);
            wake_ = INVALID_SOCKET;
        }
        if (thread_.joinable()) {
            // 若线程正在 Serve() 里阻塞 recv（有客户端连着），join 会一直等；
            // accept 循环本身已经能立刻看到 stop_，所以这里仍 detach 让进程正常退出。
            thread_.detach();
        }
        WSACleanup();
#endif
    }

  private:
#ifdef _WIN32
    void Loop() {
        while (!stop_.load()) {
            // 阻塞式 WSAPoll：等"有连接"或"被 Shutdown 唤醒"，**没有轮询周期**。
            //
            // 之前的实现是 `select` 带超时轮询（200 ms -> 10 ms）。超时值直接等于
            // "连接建立 → 被 accept"的最大延迟：200 ms 时 W4-A 实测 5 连接冷启动
            // 343 ms 里约 340 ms 都耗在这上面。改成 10 ms 后降到 17.5 ms，但仍然是
            // "每 10 ms 醒一次"的浪费。
            //
            // 现在：wake_ 是一个绑定在环回的 UDP socket，Wake() 往里发一个字节，
            // WSAPoll 立刻返回，Loop 看到 stop_ 就退出。accept 本身只在真有连接时才
            // 被唤醒，延迟 = 内核 accept 的时间（实测冷启动 p50 从 17.5 ms 降到几毫秒）。
            WSAPOLLFD fds[2];
            fds[0].fd = listen_;
            fds[0].events = POLLRDNORM;
            fds[0].revents = 0;
            fds[1].fd = wake_;
            fds[1].events = POLLRDNORM;
            fds[1].revents = 0;
            const int ready = WSAPoll(fds, 2, -1);  // -1 = 无限等待
            if (stop_.load()) break;
            if (ready <= 0) continue;  // 被信号打断/出错：下一轮会看 stop_

            if ((fds[1].revents & POLLRDNORM) != 0) {
                char drain[16];
                recv(wake_, drain, sizeof(drain), 0);  // 清掉唤醒字节
            }
            if ((fds[0].revents & (POLLRDNORM | POLLERR | POLLHUP)) == 0) continue;
            if (listen_ == INVALID_SOCKET) break;

            SOCKET client = accept(listen_, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                if (!stop_.load()) platform::default_logger().warn("mcp-tcp: accept failed");
                return;
            }
            platform::default_logger().info("mcp-tcp: client connected");
            Serve(client);
            closesocket(client);
            platform::default_logger().info("mcp-tcp: client disconnected");
        }
    }

    // 往自唤醒 socket 发一个字节；失败无所谓（Shutdown 之后本来就要退出）。
    void Wake() {
        if (wake_ == INVALID_SOCKET) return;
        const char byte = 'x';
        sendto(wake_, &byte, 1, 0, reinterpret_cast<const sockaddr*>(&wake_addr_),
               sizeof(wake_addr_));
    }

    bool SendAll(SOCKET socket, const std::string& payload) {
        std::size_t offset = 0;
        while (offset < payload.size()) {
            const int sent = send(socket, payload.data() + offset,
                                  static_cast<int>(payload.size() - offset), 0);
            if (sent <= 0) return false;
            offset += static_cast<std::size_t>(sent);
        }
        return true;
    }

    void Serve(SOCKET client) {
        mcp::BridgeOptions options;
        options.root = root_;
        options.shared_server = shared_;
        mcp::McpServerOptions server_options;
        server_options.allow_flow = allow_flow_;
        server_options.allow_write = allow_write_;
        server_options.server_version = "0.1.0";
        try {
            mcp::Bridge bridge(options);
            mcp::McpServer server(bridge, server_options);
            std::string buffer;
            char chunk[65536];
            while (!stop_.load()) {
                const int read = recv(client, chunk, sizeof(chunk), 0);
                if (read <= 0) break;
                buffer.append(chunk, static_cast<std::size_t>(read));
                for (;;) {
                    const std::size_t newline = buffer.find('\n');
                    if (newline == std::string::npos) break;
                    std::string line = buffer.substr(0, newline);
                    buffer.erase(0, newline + 1);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.empty()) continue;
                    jsonrpc::Json message;
                    try {
                        message = jsonrpc::Json::parse(line);
                    } catch (...) {
                        const std::string error = jsonrpc::Serialize(
                            jsonrpc::MakeError(std::string(), jsonrpc::kParseError,
                                               "Parse error"),
                            true);
                        SendAll(client, error + "\n");
                        continue;
                    }
                    const std::string method = jsonrpc::GetString(message, "method");
                    if (auto response = server.Handle(message)) {
                        if (!SendAll(client, jsonrpc::Serialize(*response, true) + "\n")) return;
                    }
                    if (method == "shutdown") return;
                }
            }
        } catch (const std::exception& exc) {
            platform::default_logger().error(std::string("mcp-tcp serve failed: ") + exc.what());
        }
    }

    server::LspServer* shared_ = nullptr;
    std::string root_;
    bool allow_flow_ = false;
    bool allow_write_ = false;
    SOCKET listen_ = INVALID_SOCKET;
    // 自唤醒通道：Loop 阻塞在 WSAPoll([listen_, wake_]) 上，Shutdown 往这里发一个字节
    // 就能立刻把它叫醒（不再需要轮询超时）。UDP 环回，端口由系统分配。
    SOCKET wake_ = INVALID_SOCKET;
    sockaddr_in wake_addr_{};
    unsigned short actual_port_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_{false};
#endif
};

McpTcpHost::McpTcpHost() : impl_(std::make_unique<Impl>()) {}
McpTcpHost::~McpTcpHost() = default;

bool McpTcpHost::Start(unsigned short port, server::LspServer& shared_server,
                       const std::string& root, bool allow_flow, bool allow_write,
                       std::string* error) {
    return impl_->Start(port, shared_server, root, allow_flow, allow_write, error);
}

unsigned short McpTcpHost::actual_port() const { return impl_->actual_port(); }

void McpTcpHost::Shutdown() { impl_->Shutdown(); }

int RunTcpAttachProxy(unsigned short port) {
#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 3;
    SOCKET socket = INVALID_SOCKET;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    for (int attempt = 0; attempt < 100 && socket == INVALID_SOCKET; ++attempt) {
        SOCKET candidate = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (candidate == INVALID_SOCKET) break;
        if (connect(candidate, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            socket = candidate;
        } else {
            closesocket(candidate);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    if (socket == INVALID_SOCKET) {
        std::fprintf(stderr, "fpgalsp-native: cannot attach to mcp port %u\n",
                     static_cast<unsigned>(port));
        WSACleanup();
        return 3;
    }

    std::atomic<bool> done{false};
    std::thread upstream([&]() {
        char buffer[65536];
        DWORD read = 0;
        while (!done.load() &&
               ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer, sizeof(buffer), &read, nullptr) &&
               read > 0) {
            if (send(socket, buffer, static_cast<int>(read), 0) <= 0) break;
        }
        done.store(true);
        shutdown(socket, SD_SEND);
    });
    char buffer[65536];
    for (;;) {
        const int read = recv(socket, buffer, sizeof(buffer), 0);
        if (read <= 0) break;
        DWORD written = 0;
        if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buffer, static_cast<DWORD>(read),
                       &written, nullptr)) {
            break;
        }
    }
    done.store(true);
    if (upstream.joinable()) upstream.join();
    closesocket(socket);
    WSACleanup();
    return 0;
#else
    (void)port;
    std::fprintf(stderr, "fpgalsp-native: --attach-port is Windows-only\n");
    return 3;
#endif
}

}  // namespace fpgalsp::app