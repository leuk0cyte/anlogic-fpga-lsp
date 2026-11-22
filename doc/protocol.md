# 可执行入口与传输模式

`fpgalsp-native.exe` 既是 LSP 服务端，也是 MCP 服务端；两种模式共用同一套协议栈与
业务层，仅 framing 与序列化密度不同。

```bat
:: LSP 模式（Content-Length framing）
build\bin\fpgalsp-native.exe --stdio

:: MCP 模式（newline-delimited JSON, stdio）
build\bin\fpgalsp-native.exe --mcp

:: MCP over loopback TCP —— 两种模式都支持 --mcp-port：
::   * --stdio --mcp-port <n>：LSP 进程内挂 MCP，共享同一个 Engine（扩展用这条）
::   * --mcp   --mcp-port <n>：独立进程再开一个 TCP 入口，每连接自建 Bridge
build\bin\fpgalsp-native.exe --stdio --mcp-port <n>
build\bin\fpgalsp-native.exe --mcp --mcp-port <n> --root <dir>

:: 薄代理：接入已运行的 LSP 进程的 loopback MCP
build\bin\fpgalsp-native.exe --mcp --attach-port <n>

:: 可选：日志写文件（stdout 永远归协议独占）
build\bin\fpgalsp-native.exe --stdio --log build\server.log --verbose
```

完整开关见 `-h`：`--stdio` / `--mcp` / `--mcp-port` / `--attach-port` / `--root` /
`--project` / `--td` / `--verible` / `--allow-flow` / `--allow-write` / `--log` / `--verbose`。

> **自报版本**：`--help` 首行、LSP `initialize.serverInfo.version`、MCP
> `initialize.serverInfo.version` 三处都取 `src/platform/version.h` 的
> `kServerVersion`，发布时与 `vscode-extension/package.json` 的 `version` 一起改；
> `ctest -R docs_version_consistency` 会核对 CMake / header / package.json 三处一致。

## 单条消息上限

各条路径都有上限，且都是**先判上限再分配**：

| 路径 | 上限 |
| --- | --- |
| LSP `Content-Length` 帧 | `jsonrpc::kMaxBodyBytes` = 64 MiB，超限回 `InvalidRequest` 并继续读 |
| MCP loopback TCP | 复用同一个 `kMaxBodyBytes`，超限回 `InvalidRequest` 后关闭连接 |
| MCP stdio | 有界读行 |

## 两个容易踩的规范要点

1. **UTF-16 列落在代理对中间时的行为**（复现 `fpgalsp/lsp.py` 的 `utf16_to_py`）：
   返回**该字符的起始索引**，不做四舍五入。中文占 1 码元，emoji 占 2 码元。
2. **framing 差异不只是分隔符**：LSP 的 body 可以含换行；MCP 的 body **绝不能**
   含裸换行，因此两种模式使用不同的 JSON 序列化密度。Windows 上还必须把
   stdin/stdout 切到二进制模式，否则 CRT 会把 `\r\n` 再翻译成 `\r\r\n`。

## 日志纪律

`stdout` 永远归协议独占。所有诊断信息走 `stderr` 或 `--log` 指定的文件；
**禁止**在服务端代码里用 `std::print` / `std::println`（它们默认写 stdout）。

## MCP loopback：端点广告与令牌（P-A3）

`--mcp-port <n>` 启动后，LSP 进程把 `{version,port,token,pid,root,startedAt}` 写到
`<FPGALSP_STATE_DIR>/mcp-endpoint-<port>.json`（先写 `.tmp` 再 rename，退出时删除）。
扩展据此连接，不再靠 FNV 派生端口 + TCP 探测。

* **令牌**：每进程 16 字节随机十六进制；TCP 会话的首条消息可带
  `initialize.params._meta.fpgalspToken`。带了就必须相等（否则 `-32001` 并断开）；
  没带时只有 `FPGALSP_MCP_REQUIRE_TOKEN=1` 才拒绝（两版本灰度）。
  token 不写任何日志。
* **`--attach-port` 代理**会从端点文件读 token 并注入第一条消息，
  所以旧扩展不需要改动也能在强制档下工作。
* **多客户端**：每连接一个会话线程，`FPGALSP_MCP_CLIENTS`（默认 8）为上限，
  超限回 `-32002`；`tools/call` 在内部 worker 上执行，读循环继续跑，
  因此 `notifications/cancelled` 能真正送达。
