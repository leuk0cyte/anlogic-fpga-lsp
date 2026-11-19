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
        if (listen_ != INVALID_SOCKET) {
            closesocket(listen_);
            listen_ = INVALID_SOCKET;
        }
        if (thread_.joinable()) {
            // Windows 上 closesocket 不保证唤醒所有阻塞点；进程即将退出，
            // detach 让 OS 回收线程，避免 join 卡住整个 LSP 退出路径。
            thread_.detach();
        }
        WSACleanup();
#endif
    }

  private:
#ifdef _WIN32
    void Loop() {
        while (!stop_.load()) {
            // 用 select 带超时轮询 accept。
            //
            // 注意：Shutdown() 走的是 `stop_` + `closesocket` + `detach()`（见上），
            // **退出路径并不 join 这个轮询线程**，所以"超时周期要够长以便 join"的
            // 旧理由已不成立。保留"带超时"只是为了让线程自己能定期看到 stop_。
            //
            // 超时值直接决定"连接建立 → 被 accept"的延迟：旧值 200 ms 意味着平均
            // 多等 ~100 ms（W4-A 实测：5 连接冷启动 343 ms 里约 340 ms 就是它）。
            // 10 ms 把这段压到 ~5 ms（实测冷启动 p50 17.5 ms、min 3.7 ms），
            // 空转代价只是 100 次/秒的 select；backlog 会缓冲 accept 前到达的请求，
            // 复核实测"连接后同包发 initialize+tools/call"10/10 成功。
            //
            // 彻底的解法是阻塞 accept + 自唤醒事件（WSAEventSelect / 自连一对 socket），
            // 但那要重做 Shutdown 语义；这里取最小改动。
            fd_set read_set;
            FD_ZERO(&read_set);
            FD_SET(listen_, &read_set);
            timeval timeout{0, 10000};  // 10 ms
            const int ready = select(0, &read_set, nullptr, nullptr, &timeout);
            if (ready <= 0) continue;
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