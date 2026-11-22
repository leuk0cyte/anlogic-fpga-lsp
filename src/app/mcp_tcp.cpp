// =============================================================================
//  src/app/mcp_tcp.cpp —— B1：loopback MCP 监听器（复用宿主 LspServer）
// =============================================================================

#include "app/mcp_tcp.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "jsonrpc/json_util.h"
#include "jsonrpc/transport.h"
#include "mcp/bridge.h"
#include "mcp/mcp_server.h"
#include "platform/log.h"
#include "platform/never_throw.h"
#include "platform/number.h"
#include "platform/write_service.h"
#include "platform/path.h"
#include "platform/version.h"
#include "server/lsp_server.h"

#include <filesystem>
#include <fstream>
#include <process.h>
#include <random>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace fpgalsp::app {

namespace {

// P-A3：每进程一个随机 token（16 字节 -> 32 位十六进制）。
std::string GenerateToken() {
    std::random_device device;
    std::mt19937_64 engine((static_cast<std::uint64_t>(device()) << 32) ^ device());
    static const char* kHex = "0123456789abcdef";
    std::string token;
    token.reserve(32);
    for (int i = 0; i < 4; ++i) {
        std::uint64_t value = engine();
        for (int octet = 0; octet < 8; ++octet) {
            token.push_back(kHex[(value >> (octet * 4)) & 0xF]);
        }
    }
    return token;
}

// stateDir = FPGALSP_STATE_DIR（扩展总会设置）；没设置就不写端点文件。
std::string StateDir() {
    const char* configured = std::getenv("FPGALSP_STATE_DIR");
    return configured != nullptr ? std::string(configured) : std::string();
}

std::string EndpointPath(unsigned short port) {
    const std::string dir = StateDir();
    if (dir.empty()) return {};
    // 统一写服务：端点文件目录 = FPGALSP_STATE_DIR（Classify 按 state_dir 命中 kState）。
    // 失败照旧忽略（随后 Write 返回错误、调用方拿到空路径）。
    platform::IgnoreResult(platform::writes::EnsureDir(std::string(), dir, {.caller = "mcp.endpoint_dir"}));
    return platform::path_join(dir, "mcp-endpoint-" + std::to_string(port) + ".json");
}

// 从一条消息里取 params._meta.fpgalspToken。
std::string TokenOfMessage(const jsonrpc::Json& message) {
    const auto params = message.find("params");
    if (params == message.end() || !params->is_object()) return {};
    const auto meta = params->find("_meta");
    if (meta == params->end() || !meta->is_object()) return {};
    const auto token = meta->find("fpgalspToken");
    if (token == meta->end() || !token->is_string()) return {};
    return token->get<std::string>();
}

}  // namespace

// P-A3：端点广告（原子替换，避免扩展读到半个文件）。
std::string WriteMcpEndpoint(unsigned short port, const std::string& root,
                             const std::string& token) {
    const std::string path = EndpointPath(port);
    if (path.empty()) return {};
    jsonrpc::Json endpoint = jsonrpc::Json::object();
    endpoint["version"] = 1;
    endpoint["port"] = port;
    endpoint["token"] = token;
    endpoint["pid"] = static_cast<std::int64_t>(_getpid());
    endpoint["root"] = root;
    endpoint["startedAt"] =
        static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count());
    // 统一写服务：状态目录 + 原子写 + 审计（2026-11-22 写服务改造）。
    const platform::writes::Result written =
        platform::writes::Write(root, path, endpoint.dump(1, ' '),
                                platform::writes::Options{.caller = "mcp.endpoint"});
    return written.ok ? path : std::string();
}

void RemoveMcpEndpoint(const std::string& path) {
    if (path.empty()) return;
    // 统一写服务：端点文件在 FPGALSP_STATE_DIR 下（kState；空 project_dir 时 Classify 仍
    // 按 state_dir/ temp 分类）。失败照旧忽略（与旧的 fs::remove(error_code) 一致）。
    platform::IgnoreResult(platform::writes::Remove(std::string(), path, false,
                                   platform::writes::Options{.caller = "mcp.endpoint_cleanup"}));
    platform::IgnoreResult(platform::writes::Remove(std::string(), path + ".tmp", false,
                                   platform::writes::Options{.caller = "mcp.endpoint_cleanup"}));
}

// 读端点文件里的 token（给 attach 代理用）。
std::string EndpointTokenForPort(unsigned short port) {
    const std::string path = EndpointPath(port);
    if (path.empty()) return {};
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    try {
        const jsonrpc::Json endpoint = jsonrpc::Json::parse(in);
        if (endpoint.contains("token") && endpoint["token"].is_string()) {
            return endpoint["token"].get<std::string>();
        }
    } catch (...) {
        // 半个文件 / 旧版本：当作没有 token
    }
    return {};
}

class McpTcpHost::Impl {
  public:
    bool Start(unsigned short port, server::LspServer* shared_server, const std::string& root,
               bool allow_flow, bool allow_write, std::string* error) {
#ifdef _WIN32
        shared_ = shared_server;
        root_ = root;
        allow_flow_ = allow_flow;
        allow_write_ = allow_write;
        // P-A1：每个客户端一个会话线程；上限是为了不让"多开 agent"把 LSP 进程拖垮。
        // 环境变量便于压测时调整（1 = 退回旧的串行行为）。
        max_clients_ = 8;
        if (const char* configured = std::getenv("FPGALSP_MCP_CLIENTS"); configured != nullptr) {
            // A-1：以前 std::atoi 把非法值静默变成 0（0 不在 1..64 -> 保持默认，
            // 但连一条日志都没有）。现在严格解析 + 显式记录"为什么没生效"。
            const auto parsed = platform::ParseDecimal<int>(configured);
            if (parsed.has_value() && *parsed >= 1 && *parsed <= 64) {
                max_clients_ = *parsed;
            } else {
                platform::default_logger().debug(
                    std::string("FPGALSP_MCP_CLIENTS ignored (want 1..64): ") + configured);
            }
        }
        // P-A3：每进程随机 token；FPGALSP_MCP_REQUIRE_TOKEN=1 时才强制（两版本灰度：
        // 先让旧扩展在"给了 token 就校验、没给就放行"的宽松模式下跑一轮）。
        token_ = GenerateToken();
        if (const char* required = std::getenv("FPGALSP_MCP_REQUIRE_TOKEN");
            required != nullptr && std::strcmp(required, "1") == 0) {
            require_token_ = true;
        }
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
        endpoint_path_ = WriteMcpEndpoint(actual_port_, root_, token_);
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

    // 兜底：main 一定会调 Shutdown()；万一没有，别让 joinable thread 在析构里 terminate。
    ~Impl() {
        if (thread_.joinable()) thread_.detach();
        for (std::thread& worker : sessions_) {
            if (worker.joinable()) worker.detach();
        }
    }

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
        // P-A1：会话线程可能正阻塞在 recv 上 —— 必须 shutdown 对端 socket 才能唤醒。
        // （只 closesocket 不够：另一个线程阻塞在 recv 时，close 的语义是未定义的。）
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            for (SOCKET client : clients_) {
                ::shutdown(client, SD_BOTH);
            }
        }
        std::vector<std::thread> pending;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            pending.swap(sessions_);
        }
        for (std::thread& worker : pending) {
            if (worker.joinable()) worker.join();
        }
        if (thread_.joinable()) thread_.join();  // accept 循环：listen 已关 + Wake 过，会正常退出
        RemoveMcpEndpoint(endpoint_path_);
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
            // P-A1：以前是"accept -> Serve（阻塞到客户端断开）-> 再 accept"，
            // 第二个 MCP 客户端根本连不上（connect 成功但永远没响应）。
            // 现在每连接一个会话线程，共享同一个 Engine。
            if (active_.load() >= max_clients_) {
                // 先回一条明确的 JSON-RPC error，再干净地收尾。
                // ⚠ 不能 SendAll 完立刻 closesocket：对端可能已经发来 initialize
                // 而我们没读 —— 关闭带未读数据的 socket 会发 RST，客户端看到的是
                // "连接被中止"(10053) 而不是我们刚写的错误。
                // 正确顺序：shutdown(SD_SEND) -> 排空接收缓冲 -> closesocket。
                const std::string error = jsonrpc::Serialize(
                    jsonrpc::MakeError(std::string(), -32002, "Too many MCP clients"),
                    true);
                SendAll(client, error + "\n");
                ::shutdown(client, SD_SEND);
                DWORD timeout_ms = 500;
                setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                           reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
                char drain[4096];
                while (recv(client, drain, sizeof(drain), 0) > 0) {
                }
                closesocket(client);
                platform::default_logger().warn("mcp-tcp: client rejected (limit=" +
                                                std::to_string(max_clients_) + ")");
                continue;
            }
            platform::default_logger().info("mcp-tcp: client connected");
            // ⚠ 必须**先**把连接计入 active_/clients_ 再起线程：会话线程可能在
            // fetch_add 之前就开始服务（实测会让"第 N+1 个连接被拒"的判定漏掉，
            // 表现为 max_clients 偶发不生效）。
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            clients_.insert(client);
            active_.fetch_add(1);
            sessions_.emplace_back([this, client]() {
                Serve(client);
                closesocket(client);
                {
                    std::lock_guard<std::mutex> guard(sessions_mutex_);
                    clients_.erase(client);
                }
                active_.fetch_sub(1);
                platform::default_logger().info("mcp-tcp: client disconnected");
            });
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
        server_options.server_version = platform::kServerVersion;
        try {
            mcp::Bridge bridge(options);
            mcp::McpServer server(bridge, server_options);
            bool authenticated_ = false;
            // P-B4：工具的响应/进度现在可能来自 MCP worker 线程，而会话线程同时在写。
            // 没有这把锁，两个线程的 JSON 行会互相穿插（客户端直接解析失败）。
            auto write_mutex = std::make_shared<std::mutex>();
            auto write_line = [this, client, write_mutex](const std::string& payload) {
                std::lock_guard<std::mutex> lock(*write_mutex);
                return SendAll(client, payload);
            };
            server.SetNotificationWriter([write_line](const jsonrpc::Json& note) {
                write_line(jsonrpc::Serialize(note, true) + "\n");
            });
            std::string buffer;
            char chunk[65536];
            while (!stop_.load()) {
                const int read = recv(client, chunk, sizeof(chunk), 0);
                if (read <= 0) break;
                buffer.append(chunk, static_cast<std::size_t>(read));
                // 单条消息上限：复用 stdio 路径已经在用的同一个常量
                // （jsonrpc::kMaxBodyBytes，见 transport.h），不另造一个数字。
                // 为什么必须有：对端只要一直发不带 '\n' 的字节且不关闭连接，
                // buffer 就会无限增长 -> 进程 OOM。stdio 路径早有这条防线，
                // 这条 TCP 路径是照着旧读循环抄的，之前漏了。
                // 超限的处理原则：**明确失败**，不做静默截断/丢弃 ——
                // 先回一条 JSON-RPC InvalidRequest，再记一条 error 并关连接。
                if (buffer.size() > jsonrpc::kMaxBodyBytes) {
                    const std::string error = jsonrpc::Serialize(
                        jsonrpc::MakeError(std::string(), jsonrpc::kInvalidRequest,
                                           "Message too large"),
                        true);
                    write_line(error + "\n");
                    platform::default_logger().error(
                        std::string("mcp-tcp: message exceeds ") +
                        std::to_string(jsonrpc::kMaxBodyBytes) + " bytes; closing connection");
                    return;
                }
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
                        write_line(error + "\n");
                        continue;
                    }
                    const std::string method = jsonrpc::GetString(message, "method");
                    // P-A3：首条消息鉴权。
                    //   * 带了 token -> 必须相等，否则 -32001 并断开；
                    //   * 没带 token -> 只有 require_token_（FPGALSP_MCP_REQUIRE_TOKEN=1）
                    //     才拒绝，宽松期放行（旧扩展的 attach 代理不带 token）。
                    if (!authenticated_) {
                        const std::string supplied = TokenOfMessage(message);
                        if (!supplied.empty()) {
                            if (supplied != token_) {
                                write_line(jsonrpc::Serialize(
                                               jsonrpc::MakeError(std::string(), -32001,
                                                                  "Invalid MCP token"),
                                               true) +
                                           "\n");
                                return;
                            }
                            authenticated_ = true;
                        } else if (require_token_) {
                            write_line(jsonrpc::Serialize(
                                           jsonrpc::MakeError(std::string(), -32001,
                                                              "MCP token required"),
                                           true) +
                                       "\n");
                            return;
                        } else {
                            authenticated_ = true;
                        }
                    }
                    // P-B4：tools/call 交给 MCP worker，本会话线程继续读
                    // （否则取消通知永远进不来）。
                    auto writer = [write_line](const jsonrpc::Json& response) {
                        write_line(jsonrpc::Serialize(response, true) + "\n");
                    };
                    if (auto response = server.HandleRouted(message, writer)) {
                        if (!write_line(jsonrpc::Serialize(*response, true) + "\n")) return;
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
    // P-A1：会话（每客户端一个线程）
    int max_clients_ = 8;
    std::atomic<int> active_{0};
    std::mutex sessions_mutex_;
    std::vector<std::thread> sessions_;
    std::set<SOCKET> clients_;
    // P-A3：端点 + 令牌（宽松期：没带 token 也放行；带了就必须对）
    std::string token_;
    bool require_token_ = false;
    std::string endpoint_path_;
#endif
};

McpTcpHost::McpTcpHost() : impl_(std::make_unique<Impl>()) {}
McpTcpHost::~McpTcpHost() = default;

bool McpTcpHost::Start(unsigned short port, server::LspServer* shared_server,
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

    // P-A3：如果端点文件里有 token，注入到第一条消息（initialize）的
    // params._meta.fpgalspToken 里。旧扩展不改也能连上——它只是被代理补了一个字段。
    const std::string token = EndpointTokenForPort(port);
    std::atomic<bool> done{false};
    std::thread upstream([&]() {
        char buffer[65536];
        DWORD read = 0;
        bool first_message = true;
        std::string pending;
        while (!done.load() &&
               ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer, sizeof(buffer), &read, nullptr) &&
               read > 0) {
            if (!first_message) {
                if (send(socket, buffer, static_cast<int>(read), 0) <= 0) break;
                continue;
            }
            pending.append(buffer, static_cast<std::size_t>(read));
            const std::size_t newline = pending.find('\n');
            if (newline == std::string::npos) {
                if (pending.size() > jsonrpc::kMaxBodyBytes) break;  // 病态输入
                continue;
            }
            std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!token.empty()) {
                try {
                    jsonrpc::Json message = jsonrpc::Json::parse(line);
                    if (message.contains("params") && message["params"].is_object()) {
                        message["params"]["_meta"]["fpgalspToken"] = token;
                        line = jsonrpc::Serialize(message, true);
                    }
                } catch (...) {
                    // 解析不了就原样转发，让服务端回协议错误
                }
            }
            const std::string frame = line + "\n";
            if (send(socket, frame.data(), static_cast<int>(frame.size()), 0) <= 0) break;
            if (!pending.empty()) {
                if (send(socket, pending.data(), static_cast<int>(pending.size()), 0) <= 0) break;
            }
            first_message = false;
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