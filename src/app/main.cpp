// =============================================================================
//  src/app/main.cpp —— fpgalsp-native 可执行入口
// =============================================================================
//    fpgalsp-native.exe --stdio [--td <root>]   LSP 模式（Content-Length framing）
//                                  initialize / 文档同步 / 诊断发布 / fpgaLsp/* /
//                                  workspace/executeCommand 全部由 server::LspServer
//                                  注册（见 src/server/COMMANDS.md）。
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
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "jsonrpc/dispatcher.h"
#include "jsonrpc/json_util.h"
#include "jsonrpc/transport.h"
#include "platform/log.h"
#include "mcp/bridge.h"
#include "mcp/mcp_server.h"
#include "server/lsp_server.h"

namespace {

using fpgalsp::jsonrpc::Dispatcher;
using fpgalsp::jsonrpc::HandlerResult;
using fpgalsp::jsonrpc::Json;
using fpgalsp::jsonrpc::TransportBaseCommon;
using fpgalsp::jsonrpc::TransportMode;

constexpr const char* kServerVersion = "0.1.0";

struct Options {
    TransportMode mode = TransportMode::Lsp;
    bool mode_set = false;
    std::string log_path;
    bool verbose = false;
    std::string td_path;  // 显式 TD 安装根（对应 Python serve(td_explicit=...)）
    // MCP 模式专用（对应 Python mcp.main 的 --root/--project/--allow-flow/--allow-write）
    std::string root;
    std::string project;
    bool allow_flow = false;
    bool allow_write = false;
};

void PrintUsage() {
    std::fprintf(stderr,
                 "fpgalsp-native %s\n"
                 "用法:\n"
                 "  fpgalsp-native.exe --stdio [--td <TD安装根>] [--log <path>] [--verbose]\n"
                 "  fpgalsp-native.exe --mcp   [--log <path>] [--verbose]\n",
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
        } else if (arg == "--allow-flow") {
            out.allow_flow = true;
        } else if (arg == "--allow-write") {
            out.allow_write = true;
        } else if (arg == "--verbose") {
            out.verbose = true;
        } else if (arg == "-h" || arg == "--help") {
            PrintUsage();
            return false;
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
          fpgalsp::mcp::McpServer* mcp_server) {
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
            if (auto response = mcp_server->Handle(msg)) {
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

        dispatcher.Dispatch(msg);
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

    // 日志：绝不用 stdout。--log 指定文件；--verbose 额外写 stderr。
    {
        fpgalsp::platform::LogOptions log_options;
        log_options.file_path = options.log_path;
        log_options.level = options.verbose ? fpgalsp::platform::LogLevel::Debug : fpgalsp::platform::LogLevel::Info;
        log_options.also_stderr = options.verbose;
        fpgalsp::platform::set_default_logger(fpgalsp::platform::create_logger(log_options));
    }

    const bool is_lsp = (options.mode == TransportMode::Lsp);
    fpgalsp::platform::default_logger().info(is_lsp ? "fpgalsp-native 以 LSP 模式启动"
                                           : "fpgalsp-native 以 MCP 模式启动");

    auto transport = fpgalsp::jsonrpc::MakeTransport(options.mode, std::cin, std::cout);

    Dispatcher dispatcher(*transport);

    // LSP 模式：全部 handler 由 server 层注册（标准 LSP + fpgaLsp/* + executeCommand）
    std::unique_ptr<fpgalsp::server::LspServer> lsp_server;
    std::unique_ptr<fpgalsp::mcp::Bridge> mcp_bridge;
    std::unique_ptr<fpgalsp::mcp::McpServer> mcp_server;
    if (is_lsp) {
        lsp_server = std::make_unique<fpgalsp::server::LspServer>(*transport, options.td_path);
        lsp_server->RegisterAll(dispatcher);
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
    }

    int exit_code = 0;
    try {
        exit_code = Serve(*transport, dispatcher, is_lsp, mcp_server.get());
    } catch (const std::exception& exc) {
        fpgalsp::platform::default_logger().error(std::string("fatal: ") + exc.what());
        std::fprintf(stderr, "fpgalsp-native fatal: %s\n", exc.what());
        exit_code = 1;
    } catch (...) {
        std::fprintf(stderr, "fpgalsp-native fatal: unknown exception\n");
        exit_code = 1;
    }

    // 三条退出路径（exit / EOF / 异常）统一在这里收尾：先关 TD（杀整棵进程树），
    // 再让 transport/server 析构。析构里还会兜底调用一次 Shutdown（幂等）。
    if (lsp_server) lsp_server->Shutdown();
    if (mcp_bridge) mcp_bridge->Shutdown();
    return exit_code;
}