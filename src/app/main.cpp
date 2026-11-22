// =============================================================================
//  src/app/main.cpp —— fpgalsp-native 可执行入口
// =============================================================================
//    fpgalsp-native.exe --stdio [--td <root>]   LSP 模式（Content-Length framing）
//                                  initialize / 文档同步 / 诊断发布 / fpgaLsp/* /
//                                  workspace/executeCommand 全部由 server::LspServer
//                                  注册（见 doc/commands.md）。
//    fpgalsp-native.exe --mcp [--root <dir>] [--project <al>] [--allow-flow] [--allow-write]
//                                  MCP 模式（newline-delimited JSON）：39 个工具，
//                                  协议与工具表见 src/mcp/。默认只开放只读工具。
//
//  stdout 归协议独占：所有诊断日志走 stderr 或文件，绝不写 stdout。
//
//  ⚠ 退出路径（exit / stdin EOF / 异常）都必须关掉 TD 会话，否则会留孤儿
//    td_commands_prompt.exe。LspServer::~LspServer() 会兜底，这里再显式调用一次
//    Shutdown()，保证顺序确定（先杀 TD，再让 transport 析构）。
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "app/mcp_tcp.h"
#include "jsonrpc/dispatcher.h"
#include "jsonrpc/json_util.h"
#include "jsonrpc/transport.h"
#include "platform/log.h"
#include "platform/number.h"  // A-1：--mcp-port/--attach-port 严格解析（from_chars）
#include "platform/path.h"    // state_dir()（Wave 3 / W1：状态目录可外移）
#include "platform/version.h"
#include "platform/write_service.h"
#include "mcp/bridge.h"
#include "mcp/mcp_server.h"
#include "server/async_dispatcher.h"
#include "server/lsp_server.h"

namespace {

using fpgalsp::jsonrpc::Dispatcher;
using fpgalsp::jsonrpc::HandlerResult;
using fpgalsp::jsonrpc::Json;
using fpgalsp::jsonrpc::TransportBaseCommon;
using fpgalsp::jsonrpc::TransportMode;

constexpr const char* kServerVersion = fpgalsp::platform::kServerVersion;

struct Options {
    TransportMode mode = TransportMode::Lsp;
    bool mode_set = false;
    bool help = false;
    std::string log_path;
    bool verbose = false;
    std::string td_path;  // 显式 TD 安装根（对应 Python serve(td_explicit=...)）
    // MCP 模式专用（对应 Python mcp.main 的 --root/--project/--allow-flow/--allow-write）
    std::string root;
    std::string project;
    std::string verible_path;  // 与 Python 的 --verible 兼容；内嵌 formatter 不依赖它
    // Phase 2 / B1：LSP 进程内 loopback MCP 端口（0 = 不启用）
    int mcp_port = 0;
    unsigned short attach_port = 0;
    bool allow_flow = false;
    bool allow_write = false;
};

void PrintUsage() {
    std::fprintf(stderr,
                 "fpgalsp-native %s\n"
                 "用法:\n"
                 "  fpgalsp-native.exe --stdio [--td <TD安装根>] [--project <AL>] [--mcp-port <n>] [--log <path>] [--verbose]\n"
                 "  fpgalsp-native.exe --mcp   [--root <dir>] [--project <AL>] [--mcp-port <n>]\n"
                 "                            [--allow-flow] [--allow-write] [--verible <path>]\n"
                 "                            [--log <path>] [--verbose]\n"
                 "  fpgalsp-native.exe --mcp --attach-port <n>   （薄代理：接入 LSP 进程的 loopback MCP）\n"
                 "  -h/--help 显示本帮助并以 0 退出\n",
                 kServerVersion);
}

bool ParseArgs(int argc, char** argv, Options& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--stdio") {
            out.mode = TransportMode::Lsp;
            out.mode_set = true;
        } else if (arg == "--mcp") {
            out.mode = TransportMode::Mcp;
            out.mode_set = true;
        } else if (arg == "--log" && i + 1 < argc) {
            out.log_path = argv[++i];
        } else if (arg == "--td" && i + 1 < argc) {
            out.td_path = argv[++i];
        } else if (arg == "--root" && i + 1 < argc) {
            out.root = argv[++i];
        } else if (arg == "--project" && i + 1 < argc) {
            out.project = argv[++i];
        } else if (arg == "--verible") {
            // 与 Python 的 --verible 兼容：可带值也可不带值；内嵌 formatter 不依赖它。
            if (i + 1 < argc && argv[i + 1][0] != '-') out.verible_path = argv[++i];
        } else if (arg == "--mcp-port" && i + 1 < argc) {
            // A-1：以前是 std::atoi —— "abc" 静默变 0（= 不启用 TCP 宿主）、
            // "-1" 变 0、"70000" 被 static_cast 截断成 4464（连到别的端口）。
            // 端口现在严格解析：非法/越界一律报错并以 2 退出（与其它参数错误一致）。
            const auto port = fpgalsp::platform::ParseDecimal<int>(argv[++i]);
            if (!port) {
                std::fprintf(stderr, "--mcp-port: %s\n", port.error().c_str());
                return false;
            }
            if (*port < 0 || *port > 65535) {
                std::fprintf(stderr, "--mcp-port: 端口必须在 0..65535: '%s'\n", argv[i]);
                return false;
            }
            out.mcp_port = *port;
        } else if (arg == "--attach-port" && i + 1 < argc) {
            // A-1：unsigned short 的 from_chars 自带范围检查（70000 -> out_of_range），
            // 不再有 static_cast 静默截断。
            const auto port = fpgalsp::platform::ParseDecimal<unsigned short>(argv[++i]);
            if (!port) {
                std::fprintf(stderr, "--attach-port: %s\n", port.error().c_str());
                return false;
            }
            out.attach_port = *port;
        } else if (arg == "--allow-flow") {
            out.allow_flow = true;
        } else if (arg == "--allow-write") {
            out.allow_write = true;
        } else if (arg == "--verbose") {
            out.verbose = true;
        } else if (arg == "-h" || arg == "--help") {
            out.help = true;
            return true;
        } else {
            std::fprintf(stderr, "未知参数: %s\n", std::string(arg).c_str());
            PrintUsage();
            return false;
        }
    }
    return true;
}

// -----------------------------------------------------------------------------
// MCP 模式
// -----------------------------------------------------------------------------
//  协议层（版本协商 / tools/list / tools/call / 结果截断 / 错误语义）全部在
//  src/mcp/mcp_server.cpp；工具实现在 src/mcp/bridge.cpp。
//  这里只负责：把消息喂给 mcp::McpServer、把响应写回 transport。
//
//  ⚠ 与 LSP 的关键差异：工具失败是 **isError 结果**而不是 JSON-RPC error，
//    所以不能走 jsonrpc::Dispatcher（它会包成 error 对象）。

// -----------------------------------------------------------------------------
// serve 循环
// -----------------------------------------------------------------------------

int Serve(TransportBaseCommon& transport, Dispatcher& dispatcher, bool is_lsp,
          fpgalsp::mcp::McpServer* mcp_server,
          fpgalsp::server::AsyncDispatcher* async_dispatcher) {
    bool shutdown_requested = false;
    bool exit_requested = false;

    while (!exit_requested) {
        auto rr = transport.ReadMessage();

        switch (rr.status) {
            case TransportBaseCommon::ReadStatus::EndOfStream:
                fpgalsp::platform::default_logger().info("stdin EOF, stopping");
                return exit_requested ? 0 : (shutdown_requested ? 0 : 0);

            case TransportBaseCommon::ReadStatus::Malformed:
                // 坏帧：回 ParseError 并继续读，不退出
                dispatcher.SendParseError();
                if (!is_lsp) {
                    // MCP 模式下坏行同样回 ParseError（对应 mcp.py 的 continue）
                }
                continue;

            case TransportBaseCommon::ReadStatus::TooLarge:
                dispatcher.SendParseError("body too large");
                continue;

            case TransportBaseCommon::ReadStatus::Message:
                break;
        }

        const Json& msg = rr.message;

        // ---- MCP 模式：整条消息交给 mcp::McpServer ----
        if (mcp_server != nullptr) {
            const std::string mcp_method = fpgalsp::jsonrpc::GetString(msg, "method");
            // P-B4：tools/call 在内部 worker 上跑，读循环继续 —— 否则取消通知进不来。
            // 结果由 writer 写回（transport 写自带锁；progress 也走同一把锁）。
            auto writer = [&transport](const Json& response) {
                transport.WriteMessage(response);
            };
            if (auto response = mcp_server->HandleRouted(msg, writer)) {
                transport.WriteMessage(*response);
            }
            // 对应 Python mcp.py：应答 shutdown 后结束
            if (mcp_method == "shutdown") break;
            continue;
        }

        // exit 由主循环处理（LSP 规范：exit 不应答）
        if (fpgalsp::jsonrpc::GetString(msg, "method") == "exit") {
            exit_requested = true;
            break;
        }
        // shutdown 需要应答 null
        if (is_lsp && fpgalsp::jsonrpc::GetString(msg, "method") == "shutdown") {
            shutdown_requested = true;
            if (auto id = fpgalsp::jsonrpc::GetMessageId(msg); id.has_value()) {
                transport.WriteMessage(fpgalsp::jsonrpc::MakeResult(*id, Json(nullptr)));
            }
            continue;
        }
        if (!is_lsp && fpgalsp::jsonrpc::GetString(msg, "method") == "shutdown") {
            if (auto id = fpgalsp::jsonrpc::GetMessageId(msg); id.has_value()) {
                transport.WriteMessage(fpgalsp::jsonrpc::MakeResult(*id, Json(nullptr)));
            }
            // 对应 mcp.py：写出 shutdown 响应后结束
            break;
        }

        if (async_dispatcher != nullptr) {
            async_dispatcher->Dispatch(msg);
        } else {
            dispatcher.Dispatch(msg);
        }
    }

    return 0;
}

// -----------------------------------------------------------------------------
// 把 stdin/stdout 切到二进制模式
// -----------------------------------------------------------------------------
// Windows CRT 在文本模式下会把 '\n' 翻译成 "\r\n"。LSP 的 header 本身就以
// "\r\n" 结尾，如果输出流是文本模式，我们自己写的 '\r' 后面那个 '\n' 会再被
// 翻译一次，于是变成 "\r\r\n" —— 违反 LSP 规范，严格的客户端会解析失败。
// 同理输入侧也会破坏 Content-Length 的精确字节读取。
// 因此必须在开始收发协议帧之前切换为二进制模式。
void SetBinaryMode() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

}  // namespace

int main(int argc, char** argv) {
    SetBinaryMode();

    Options options;
    if (!ParseArgs(argc, argv, options)) {
        return 2;
    }
    if (options.help) {
        PrintUsage();
        return 0;
    }

    // Phase 2 / B1：MCP attach 代理只做字节泵；连接失败返回 3（扩展可回退独立 MCP）。
    if (options.mode == TransportMode::Mcp && options.attach_port != 0) {
        return fpgalsp::app::RunTcpAttachProxy(options.attach_port);
    }

    const bool is_lsp = (options.mode == TransportMode::Lsp);

    // LSP 的 --project 对应 Python 的 os.environ["FPGALSP_PROJECT"]（lsp_server.cpp 读它）。
    if (is_lsp && !options.project.empty()) {
#ifdef _WIN32
        _putenv_s("FPGALSP_PROJECT", options.project.c_str());
#else
        setenv("FPGALSP_PROJECT", options.project.c_str(), 1);
#endif
    }

    // 默认日志路径与 Python 对齐：
    //   LSP -> <cwd>/.fpgalsp/logs/server.log
    //   MCP -> <root>/.fpgalsp/logs/mcp.log
    if (options.log_path.empty()) {
        std::error_code ec;
        const std::filesystem::path base =
            is_lsp ? std::filesystem::current_path(ec)
                   : std::filesystem::path(options.root.empty() ? "." : options.root);
        const std::filesystem::path logs =
            std::filesystem::path(fpgalsp::platform::state_dir(base.string())) / "logs";
        // 统一写服务：日志目录在状态目录下（kState；base = cwd 或工程根，均按 state_dir 分类）。
        if (fpgalsp::platform::writes::EnsureDir(
                base.string(), logs.string(),
                fpgalsp::platform::writes::Options{.caller = "app.logs_dir"})
                .ok) {
            options.log_path = (logs / (is_lsp ? "server.log" : "mcp.log")).string();
        }
    }

    // 日志：绝不用 stdout。--log 指定文件；--verbose 额外写 stderr。
    {
        fpgalsp::platform::LogOptions log_options;
        log_options.file_path = options.log_path;
        log_options.level = options.verbose ? fpgalsp::platform::LogLevel::Debug : fpgalsp::platform::LogLevel::Info;
        log_options.also_stderr = options.verbose;
        fpgalsp::platform::set_default_logger(fpgalsp::platform::create_logger(log_options));
    }

    fpgalsp::platform::default_logger().info(is_lsp ? "fpgalsp-native 以 LSP 模式启动"
                                           : "fpgalsp-native 以 MCP 模式启动");

    auto transport = fpgalsp::jsonrpc::MakeTransport(options.mode, std::cin, std::cout);

    Dispatcher dispatcher(*transport);

    // LSP 模式：全部 handler 由 server 层注册（标准 LSP + fpgaLsp/* + executeCommand）
    std::unique_ptr<fpgalsp::server::LspServer> lsp_server;
    std::unique_ptr<fpgalsp::mcp::Bridge> mcp_bridge;
    std::unique_ptr<fpgalsp::mcp::McpServer> mcp_server;
    // Phase 2 / A2：LSP 模式把重请求放到 worker 池；MCP 模式保持同步。
    // 声明在 lsp_server 之后：析构顺序保证先停 worker，再销毁 server/transport。
    std::unique_ptr<fpgalsp::server::AsyncDispatcher> async_dispatcher;
    std::unique_ptr<fpgalsp::app::McpTcpHost> mcp_tcp_host;
    if (is_lsp) {
        lsp_server = std::make_unique<fpgalsp::server::LspServer>(*transport, options.td_path);
        lsp_server->RegisterAll(dispatcher);
        async_dispatcher =
            std::make_unique<fpgalsp::server::AsyncDispatcher>(dispatcher, *transport, 2);
        if (options.mcp_port > 0) {
            std::error_code ec;
            const std::string root = std::filesystem::current_path(ec).string();
            std::string mcp_error;
            mcp_tcp_host = std::make_unique<fpgalsp::app::McpTcpHost>();
            if (mcp_tcp_host->Start(static_cast<unsigned short>(options.mcp_port),
                                    lsp_server.get(), root, options.allow_flow,
                                    options.allow_write, &mcp_error)) {
                fpgalsp::platform::default_logger().info(
                    "mcp-tcp listening on 127.0.0.1:" +
                    std::to_string(mcp_tcp_host->actual_port()));
            } else {
                fpgalsp::platform::default_logger().warn("mcp-tcp disabled: " + mcp_error);
                mcp_tcp_host.reset();
            }
        }
    } else {
        // MCP 模式：Bridge（feature 层）+ McpServer（协议层）
        fpgalsp::mcp::BridgeOptions bridge_options;
        bridge_options.root = options.root.empty() ? std::string(".") : options.root;
        bridge_options.project = options.project;
        bridge_options.td_path = options.td_path;

        fpgalsp::mcp::McpServerOptions server_options;
        server_options.allow_flow = options.allow_flow;
        server_options.allow_write = options.allow_write;
        server_options.server_version = kServerVersion;

        mcp_bridge = std::make_unique<fpgalsp::mcp::Bridge>(bridge_options);
        mcp_server = std::make_unique<fpgalsp::mcp::McpServer>(*mcp_bridge, server_options);
        mcp_server->SetNotificationWriter([&transport](const Json& note) {
            // 进度通知走同一条 newline-delimited 通道；绝不写 stdout 之外的东西
            transport->WriteMessage(note);
        });

        // `--mcp --mcp-port <n>`：独立 MCP 服务端在 loopback 上再开一个 TCP 入口。
        // 与 `--stdio --mcp-port`（共享 LSP Engine）不同，这里每个 TCP 连接自建 Bridge，
        // 不依赖任何 LSP 客户端；stdio 通道照常工作，进程在 stdin 关闭时退出。
        if (options.mcp_port > 0) {
            std::string mcp_error;
            mcp_tcp_host = std::make_unique<fpgalsp::app::McpTcpHost>();
            // shared_server = nullptr：独立进程，没有可复用的 LSP Engine
            if (mcp_tcp_host->Start(static_cast<unsigned short>(options.mcp_port), nullptr,
                                    bridge_options.root, options.allow_flow,
                                    options.allow_write, &mcp_error)) {
                fpgalsp::platform::default_logger().info(
                    "mcp-tcp listening on 127.0.0.1:" +
                    std::to_string(mcp_tcp_host->actual_port()));
            } else {
                fpgalsp::platform::default_logger().warn("mcp-tcp disabled: " + mcp_error);
                mcp_tcp_host.reset();
            }
        }
    }

    int exit_code = 0;
    try {
        exit_code = Serve(*transport, dispatcher, is_lsp, mcp_server.get(),
                          async_dispatcher.get());
    } catch (const std::exception& exc) {
        fpgalsp::platform::default_logger().error(std::string("fatal: ") + exc.what());
        std::fprintf(stderr, "fpgalsp-native fatal: %s\n", exc.what());
        exit_code = 1;
    } catch (...) {
        std::fprintf(stderr, "fpgalsp-native fatal: unknown exception\n");
        exit_code = 1;
    }

    // 三条退出路径（exit / EOF / 异常）统一在这里收尾：先停异步 worker，再关 TD
    // （杀整棵进程树），最后让 transport/server 析构。
    if (async_dispatcher) async_dispatcher->Shutdown();
    if (mcp_tcp_host) mcp_tcp_host->Shutdown();
    if (lsp_server) lsp_server->Shutdown();
    if (mcp_bridge) mcp_bridge->Shutdown();
    return exit_code;
}