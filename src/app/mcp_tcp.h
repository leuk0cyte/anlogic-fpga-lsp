// =============================================================================
//  src/app/mcp_tcp.h —— Phase 2 / B1：LSP 进程内的 MCP loopback 监听器
// =============================================================================
//  先落地“单 Engine、多 transport”的核心：LSP 进程在 127.0.0.1 上再监听一个 MCP
//  newline-JSON 端口，Bridge 复用同一个 LspServer（索引/报告/工程缓存只有一份）。
//  VS Code 侧的 attach 代理（--mcp --attach-port）在下一步接入；当前先用直连验证。
// =============================================================================

#pragma once

#include <memory>
#include <string>

namespace fpgalsp::server {
class LspServer;
}

namespace fpgalsp::app {

class McpTcpHost {
public:
    McpTcpHost();
    ~McpTcpHost();

    McpTcpHost(const McpTcpHost&) = delete;
    McpTcpHost& operator=(const McpTcpHost&) = delete;

    // port=0 时由系统分配；成功后 actual_port() 返回真实端口。
    //   shared_server != nullptr：复用该 LSP 进程的 Engine（`--stdio --mcp-port`，单 Engine 多 transport）
    //   shared_server == nullptr：每个连接自建 Bridge（`--mcp --mcp-port`，独立 MCP-over-TCP 服务端）
    bool Start(unsigned short port, server::LspServer* shared_server, const std::string& root,
               bool allow_flow, bool allow_write, std::string* error);
    unsigned short actual_port() const;
    void Shutdown();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// MCP 模式下的薄代理：stdin/stdout <-> 127.0.0.1:<port>，只做字节泵。
// 返回进程退出码（0 正常，3 连接失败）。
// P-A3：如果 `<stateDir>/mcp-endpoint-<port>.json` 里有 token，会在第一条消息
// （initialize）的 `params._meta.fpgalspToken` 里补上 —— 旧扩展无需改动。
int RunTcpAttachProxy(unsigned short port);

// P-A3：端点广告。MCP 监听启动后把 {port, token, pid, root} 写到
// `<stateDir>/mcp-endpoint-<port>.json`（原子替换），扩展据此连接，不再靠
// FNV 派生端口 + 探测。stateDir 取 FPGALSP_STATE_DIR；未设置时不写（旧行为）。
// 返回写出的路径（空 = 没写）。
std::string WriteMcpEndpoint(unsigned short port, const std::string& root,
                             const std::string& token);
void RemoveMcpEndpoint(const std::string& path);

}  // namespace fpgalsp::app