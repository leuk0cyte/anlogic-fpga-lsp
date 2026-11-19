# 端到端协议优化设计（自定义 `fpgaLsp/*` + MCP + 传输/进程）

> 范围：从编辑器/agent 的一次操作，到后端进程产出结果、回到 UI 的整条链路。
> 约束：**LSP 侧必须保持标准合规**；自定义 `fpgaLsp/*` 方法与 MCP 层可以自由重构，**不再以 Python 版接口为契约**。
> 本文只回答一个问题：*除了"LSP 要合规"之外，还能怎么改*。

**证据等级标记**：`实测` = 本机当前 exe 探针/仓库审计实测；`审计` = 引用 `doc/audit/2026-11-19/` 已有实测；`待测` = 代码可见但未量化。

---

## 0. 一页结论：按投入产出比排序的 Top 9

| # | 编号 | 提案 | 优先级 | 核心收益（有数） | 成本 | 风险 |
|---|---|---|---|---|---|---|
| 1 | **B1** | 工具结果去重：`content[0].text` 与 `structuredContent` 二选一 | P0 | 同一 `fpga_project_structure` 响应 **79,892 B → ≈28.5 KB（−64%）**（实测） | 1 人日 | 老客户端只读 `text` |
| 2 | **C1** | `fpgaLsp/indexChanged` 带 `modules[]` 数据，客户端原地打补丁 | P0 | 一次保存的重取 **16,426 B → 0**（实测树大小），且少一次全树重建 | 1.5 人日 | 增量与树不一致 → 需 generation 兜底 |
| 3 | **D1** | MCP 报告路径接 `ReportStore`（现在每次 `BuildReports`） | P0 | 冷 **768–784 ms** → 命中 **0.54 ms**（审计） | 1 人日 | 快照与 Bridge 工程视图不一致 |
| 4 | **A1** | MCP loopback 支持多客户端并发（现在串行服务 1 个） | P0 | 第二个 agent 不再无限等待；后到者 p95 从"整段会话时长"→ 毫秒级（待测） | 3–4 人日 | 共享 `LspServer` 线程安全 |
| 5 | **A3** | 端点广告（状态文件 + 随机端口）+ 令牌鉴权 | P0 | 堵住"本机任意进程可读工程文件/跑 flow"；消除端口冲突与抢占 | 2–3 人日 | 状态文件竞态、旧扩展兼容 |
| 6 | **B2** | 800 KB 整份丢弃 → 分页/游标 | P1 | 大工程报告从"完全不可用"变成可分页可用（56.54 MB 级，审计） | 3 人日 | 响应形状变化 |
| 7 | **C3** | `reportsChanged` 带 `revision`，`fpgaLsp/reports` 支持 `ifRevision` | P1 | 一次 flow 完成后的 4 次 section 重取 → 1 次；命中路径 **461–489 ms** 只付一次（审计） | 2.5 人日 | revision 与 mask 快照槽位耦合 |
| 8 | **D2** | 报告装饰项（`NewestRoutedDb` + `FindTd` + `analysis.json`）每请求重做 | P1 | `FindTd` **0.89–0.93 ms**/次 + 目录遍历；占 `section=summary`（3.7 ms）约 27% | 1 人日 | 缓存失效点要接 watcher |

| 9 | **D9** | `reportsChanged` 为算通知载荷而构建 `kAll` 全量快照（Lead 补充） | P0 | 每次报告变化在 IO 线程上付一次冷构建 **768–784 ms**（审计）→ 载荷只需 revision + 段名，降到 µs 级 | 0.5 人日 | 无（纯删工作量） |

**如果只做 1/2/3**：编辑器保存后的回传字节 ≈ −16 KB，agent 的 `fpga_project_structure` 字节 −64%，MCP 报告从 0.8 s 降到亚毫秒。三项互不冲突、都只动一个文件。

---

## 1. 现状链路图与浪费点

### 1.1 一次保存：`didSave` → 索引 → 推送 → UI 重取

```
[VS Code]                    [LSP IO 线程]                        [IndexService]          [worker]
didSave{text: 全文}
   │  (textDocumentSync.save.includeText=true, lsp_server.cpp:343-345)
   ├────────────────────────► docs_.Save
   │                          ProjectForPathShared            0.16 ms @200 条目（审计 INDEX.md:176）
   │                          index_service_->OnFileChanged ──► ParseFile 0.24 ms
   │                                                           Clone+Replace 0.10 ms @200（审计 INDEX.md:70）
   │                          MaybeNotifyIndexChanged ◄──────── generation 变了
   │  ◄── fpgaLsp/indexChanged {al, generation, paths}        ← W1  只带提示，~150 B，不带任何节点数据
   │                                                                     （lsp_server.cpp:465-475）
applyIndexChanged(event)
   │  modules 为空 → refresh()                               ← W2  客户端**已实现** modules[] 打补丁
   │                                                              （moduleTree.js:221-255），服务端从不发
   ├──► fpgaLsp/moduleTree {uri}
   │                          IO 线程同步建整棵树              ← W3  moduleTree 不在 heavy 白名单，
   │                            IndexForProject::Get               与 didChange 抢同一个 IO 线程
   │                            features::ModuleTree          ← W4  watcher 首事件前每 250 ms 一次全量 stamp（非每请求，见 D5 更正）
   │                                                              0.30 ms @200 文件/窗口（审计 F2）
   │  ◄── 16,426 B（实测 corpus_alpha；1000 节点级工程按节点线性，待测）  ← W5  一次保存 = 一份全树
   整棵树丢弃重建 + fire()
   │
   ├── 并行：RunDiagnostics(全量) / ScheduleLint / ScheduleTdCheck（这部分设计是合理的）
   └── ◄── textDocument/publishDiagnostics
```

### 1.2 一次 agent 工具调用：MCP → 后端 → 返回

```
[MCP client] ──stdio 换行帧 / TCP──► [Serve 循环 main.cpp:143-213]
                                       │ mcp_server->Handle(msg) **同步**       ← W6  读循环被占，
                                       ▼                                             flow_run/flow_wait（最长 7200 s）
                                   McpServer::HandleSingle → CallTool           期间同一连接一切停摆
                                       ▼                                        （main.cpp:174-182）
                                   Bridge::Call（39 分支字符串链，<0.03 µs，审计）
                                       ├─ 符号类 → Bridge **自己的** IndexService   ← W7  引擎两份：
                                       ├─ 报告类 → reports::BuildReports(proj)     宿主 LspServer 有 ReportStore，
                                       │            （bridge.cpp:2077 / :3387）    这里没有 → 768–784 ms（审计）
                                       │            ← W8  绕过 ReportStore，每次全量重解析
                                       └─ flow  → FlowRunner
                                       ▼
                                   WrapToolResult
                                       ├─ text = value.dump(1)  （缩进 1，21.7 KB=29 µs，审计）← W9
                                       ├─ structuredContent = 同一份 value 再序列化一遍  → 双份 payload
                                       └─ > 800,000 B → **整份丢弃**，只回 truncated   ← W10（mcp_server.cpp:119-126）
                                       ▼
                                   transport.WriteMessage
```

### 1.3 MCP 监听层（`--mcp-port`）

```
VS Code 扩展 ──TCP 127.0.0.1:45000+FNV(root)%1000──┐
另一个 agent ──TCP 同上 ──────────────────────────┼─► listen(4) → accept → Serve(A) 串行
任意本机进程 ──TCP 同端口范围（1000 个候选）───────┘   → 只有等 A 断开才轮到 B      ← W11（mcp_tcp.cpp:164-172）
                                                     无令牌、无握手、端口可被抢注   ← W12（extension.js:66-72）
```

---

## 2. 先分清「已实现可优化」与「缺失」

> 本节是后面所有提案的前提。声称缺失的项都用 grep 验证过（`grep -rn "<符号>" src/`）。

| 能力 | 状态 | 证据 |
|---|---|---|
| MCP `annotations`（readOnlyHint / destructiveHint） | **已实现**，只有 2 个字段 | `tools.cpp:377-380` |
| MCP 工具 `title` 字段 | 已实现 | `tools.cpp:371` |
| MCP `notifications/progress` | **已实现**，但只有 `flow_wait` 会发 | `mcp_server.cpp:174-212`；`bridge.cpp:3434` |
| MCP 版本协商（3 个版本） | 已实现；不支持的版本**静默回落** 2024-11-05 | `mcp_server.cpp:253-276` |
| MCP 批量数组 | 已实现 | `mcp_server.cpp:219-226` |
| MCP `resources/list`、`resources/templates/list`、`prompts/list` | 已实现但**恒为空** | `mcp_server.cpp:286-300`；实测 `resources/list` = 50 B |
| MCP `structuredContent` + `isError` 语义 | 已实现（且与 `content` 重复） | `mcp_server.cpp:89-138` |
| MCP `outputSchema` | **缺失** | grep `outputSchema` = 0 命中；实测 `tools/list` 39 个 tool 均无该键 |
| MCP `tools/list` 分页（`cursor` / `nextCursor`） | **缺失** | `mcp_server.cpp:280-284` 一次性返回；`grep nextCursor` = 0 |
| MCP 取消（`notifications/cancelled`） | **缺失** | 所有无 id 消息直接 `nullopt`（`mcp_server.cpp:241-245`），无 cancelled 分支 |
| MCP `resources/read`、`resources/subscribe` | **缺失** | `HandleSingle` 只认 3 个 list 方法，其余一律 −32601（`mcp_server.cpp:319`） |
| MCP 结果分页 / 部分结果 | **缺失** | `WrapToolResult` 只有"全有或全无"（`mcp_server.cpp:119-126`） |
| 自定义 `indexChanged.modules[]` | **客户端已支持 / 服务端从未发送** | 客户端 `moduleTree.js:221-255`；服务端 `MaybeNotifyIndexChanged` 只塞 `al/generation/paths`（`lsp_server.cpp:470-474`），`SendNotification` 的 params 里没有 `modules` |
| `fpgaLsp/reports` 分节 + `limit` | **已实现** | `commands.cpp:307-336`；`report_store.h:96-99` |
| `fpgaLsp/reportsChanged` | 已实现，只带 `{stage, sections, generatedAt}` | `lsp_server.cpp:477-491` |
| `ReportStore::CheckForChanges`（请求时兜底） | **声明 + 实现，但 0 调用者（死代码）** | `report_store.h:103`、`report_store.cpp:197`；`grep CheckForChanges src/` 只有自身 |
| `fpgaLsp/flowStatus` 日志 200 行上限 | 已实现，且**每次全量重发** | `flow_runner.cpp:446`（`TrimLog(...,200)`）、`flow_runner.h:100` |
| LSP `$/cancelRequest` | 已实现，**只对 heavy 白名单** | `async_dispatcher.cpp:125-128`、`:44-61` |
| LSP server→client 请求（`client/registerCapability`） | **缺失** | `grep registerCapability` = 0；transport 无出向请求 ID 设施 |
| LSP 进度（`window/workDoneProgress/create`） | **缺失** | `capabilities` 无 `workDoneProgress`（`lsp_server.cpp:336-396`） |
| `semanticTokens/full/delta` | 已声明 `delta:true` 且实现，但**实为整段替换** | `lsp_server.cpp:389` vs `:647-677`（`previousResultId` 被 `(void)previous` 丢弃） |
| MCP 工程发现缓存 | **已实现**（不要再提"每次 DiscoverProjects"） | `bridge.cpp:404-487`（5 s TTL + 目录/`.al` stamp 校验） |

---

## 3. A 组：传输与进程模型

### A1 · MCP loopback 只能串行服务一个客户端
| 项 | 内容 |
|---|---|
| **优先级** | P0 |
| **现状** | `mcp_tcp.cpp:164-172`：`accept()` 后同步调用 `Serve(client)`，返回才回到 `Loop()`；`listen(listen_, 4)`（`:53`）只是内核 backlog，不让服务端并发处理。`Serve()` 内部建 `Bridge`+`McpServer`（`:204-205`），读循环阻塞在 `recv`（`:209`）。 |
| **问题** | 第二个连接不会被 `accept`，客户端表现为"TCP 连上了，`initialize` 永远不回来"。同时接 VS Code 与一个 agent 时，后到者挂起时长 = 先到者会话时长（待测，但结构上无上界）。 |
| **方案** | `McpTcpHost::Impl` 增加 `struct Session { SOCKET socket; std::thread thread; std::atomic<bool> stop; mcp::Bridge bridge; mcp::McpServer server; }`；`Loop()` 在 `accept` 成功后 `std::thread(&Impl::ServeSession, this, client)`，把 session 收进 `std::vector<std::unique_ptr<Session>>`（`std::mutex sessions_mutex_`）；`Shutdown()` 遍历置 stop、`shutdown(SD_BOTH)`、join。新增 `max_clients`（默认 8）与空闲超时（默认 30 min）。`Serve()` 的 `while (!stop_.load())` 改为读 `session.stop`。 |
| **收益** | 两个 MCP 客户端互不阻塞；`flow_run`/`flow_wait` 不再冻结整个 host（只冻结自己那条连接）。 |
| **成本** | 3–4 人日（含并发审计） |
| **风险** | 共享 `LspServer` 的线程安全：`IndexService`/`docs_`/`state_` 已用 shared_mutex，但 `flow_`（`flow_mutex_` 单实例）与 `td_service_` 需复核；每 session 独立 `Bridge`（变状态无锁，见 `bridge.cpp:496-501` 的注释）必须保留，不能共享 `Bridge`。 |

### A2 · 每连接重建 Bridge + 每连接 `initialize` 的冷启动
| 项 | 内容 |
|---|---|
| **优先级** | P1 |
| **现状** | 每个 TCP 连接新建 `Bridge`（`mcp_tcp.cpp:204-205`），构造时走 `CachedDefaultAl` → `CachedProjectListEntries`（`bridge.cpp:404-433`，进程级 5 s TTL 缓存）。 |
| **问题** | 工程发现已经进程级复用（W4-A），所以"每连接 +9–11 ms"这个审计数字（`MCP.md:108`）**现在应已大幅下降，残值待测**。仍未复用：`.al` 的 `ParseAl` 结果、`td_choice_`、`devices_`、`IndexService`（每 Bridge 一份）。 |
| **方案** | (1) 在 `McpTcpHost::Start()` 里构造一个 prototype `Bridge` 并丢弃（或保留为 `proto_`），把 `projects_`/`td_choice_`/`devices_` 的进程级缓存预热好；(2) 把 `Bridge::projects_`/`td_choice_` 提升为 `bridge.cpp` 里的进程级缓存（与 `g_project_list_cache` 同风格，键 `(al_path, mtime, size)`）；(3) `initialize` 结果里的 `instructions`（447 B）与新 `capabilities.experimental`（B8）按 `(version, flags)` 缓存成字符串，避免每连接重复构建 JSON。 |
| **收益** | 冷连接开销 −（残值 待测）；`td_info`/`project_info` 的 `FindTd` 0.89–0.93 ms 归零（见 D2）。 |
| **成本** | 2 人日 |
| **风险** | 缓存与 `.al` 变更的时效；`devices_` 与 TD 会话生命周期绑定，不能跨连接复用（TD 进程会被 `Shutdown()` 杀）。 |

### A3 · 端点靠 FNV 派生端口，无鉴权、可抢占
| 项 | 内容 |
|---|---|
| **优先级** | P0 |
| **现状** | 端口 = `45000 + FNV1a(root) % 1000`（`extension.js:66-72`）；LSP 进程 `--mcp-port N` 绑定 loopback（`main.cpp:307-321`、`mcp_tcp.cpp:48-58`）；**没有任何令牌/握手**，`initialize` 谁都能发；`read_file` / `flow_run`（带 `--allow-flow`）直接可用。 |
| **问题** | (1) 端口空间只有 1000 个，多工作区/多窗口冲突概率非零，冲突时 mcp-tcp 被禁用并回落独立进程（可用性退化）；(2) 端口可预测 → 本机任意进程（含恶意脚本）可扫 1000 个端口找到服务端并读工程文件、跑 flow；(3) 无上限的入站消息（单条 ≤64 MB，`transport.h:57`，`mcp_tcp.cpp:219-229`）可被用来打爆内存。 |
| **方案** | (1) `--mcp-port 0` → 系统分配，`actual_port()` 已有（`mcp_tcp.cpp:59-62`）；(2) 启动后写 `platform::state_dir(root)/mcp-endpoint-<pid>.json`：`{schemaVersion:1, pid, port, token, root, allowFlow, allowWrite, startedAt, serverVersion}`，其中 `token = 32 hex`（`std::random_device` + 进程内计数器）；(3) `McpServer::HandleSingle` 在 `initialize` 分支校验 `params._meta.fpgalspToken`（或 `params.arguments.token`），不匹配回 `-32001 Unauthorized` 并让 host `closesocket`；(4) 扩展 `mcpEndpointForWorkspace()` 替换 `mcpPortForWorkspace()`：读所有 endpoint 文件，过滤 `pid` 存活 + `root` 匹配，取 `startedAt` 最新；(5) 保留 FNV 端口作为**旧服务端**回落（`attachPort` 逻辑不动）。 |
| **收益** | 消除端口冲突与抢占；未授权本机进程无法调用任何工具；多窗口同根可共存（每 pid 一条记录）。 |
| **成本** | 2–3 人日 |
| **风险** | endpoint 文件残留（需 pid 存活检查 + 启动清理）；`--attach-port` 字节泵不需要改（token 在 `initialize` 体内天然透传）；若把 token 写进日志会失效 —— 日志里必须脱敏。 |

### A4 · 用命名管道替代 loopback TCP（Windows 原生）
| 项 | 内容 |
|---|---|
| **优先级** | P2 |
| **现状** | 仅 loopback TCP（`mcp_tcp.cpp:43-58`），Windows-only（`:96-104` 非 Windows 直接失败）。 |
| **问题** | TCP 带来三个与业务无关的负担：端口空间/防火墙弹窗（首次监听 loopback 也可能触发）、可被同机任意进程连接、需要 A3 的令牌补丁。命名管道的 DACL 天然等于"当前用户"，且无端口可扫。 |
| **方案** | 新增 `--mcp-pipe <name>`（默认 `\\.\pipe\fpgalsp\<rootHash>-<pid>`），`CreateNamedPipeW(PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE)`，`SECURITY_ATTRIBUTES` 用当前用户 SID 的 DACL；`Serve()` 的 `recv/send` 换成 `ReadFile/WriteFile`，其余（换行分帧、`McpServer`、并发模型）完全复用 A1 的 Session。`RunTcpAttachProxy` 加一个 `--attach-pipe` 变体（stdin↔pipe 字节泵），供只支持 stdio 的 MCP 客户端使用。 |
| **收益** | 无端口、无防火墙、无需令牌；连接建立延迟比 loopback TCP 更低（待测）；`--mcp-port` 保留为跨机/容器场景。 |
| **成本** | 3–5 人日 |
| **风险** | 只覆盖 Windows（项目已 Windows-only，可接受）；客户端配置需要通过代理，配置模板（`src/mcpConfig.js`）要同步。 |

### A5 · 「单 Engine 多 transport」只做了两个工具
| 项 | 内容 |
|---|---|
| **优先级** | P1 |
| **现状** | `BridgeOptions::shared_server` 只被 `FeatureServer()` 使用（`bridge.cpp:4077-4087`），即 `code_actions` / `rename_edits` 两个工具；符号类工具用 `Bridge::index_service_`（`bridge.h:234`），报告类直接 `reports::BuildReports`（`bridge.cpp:2077`、`:3387`），`flow_status` 同样绕过 `ReportStore`。 |
| **问题** | 同一个工作区在进程里存在 **两份索引**（宿主 + Bridge）和 **一份未缓存的报告解析路径**。冷索引 23.4 ms @200 文件 / 104 ms @1000（审计 `INDEX.md:57-58`）、冷报告 768–784 ms 被重复支付；两套索引还会因 TTL/失效策略不同而给出不一致的符号结果。 |
| **方案** | 在 `LspServer` 上暴露 `server::IndexService& index_service()`、`server::ReportStore& report_store()`（现在它们是 private，`lsp_server.h:434-436`）；`Bridge::EnsureHdlIndex()` 在 `options_.shared_server != nullptr` 时改用宿主 `IndexService`（保留 `hdl_snapshot_` 语义）；`Bridge::ProjectReports`/`FlowStatus` 在共享模式下改为 `report_store().Get(proj, mask, false, limit)`。`ProjectReports` 增加 `BridgeOptions::report_store`，独立 `--mcp` 模式由 Bridge 自建一个。 |
| **收益** | 报告冷解析 768–784 ms 只付一次；索引 23.4 ms 只建一份；符号结果与 LSP 严格一致（少一类漂移 bug）。 |
| **成本** | 5–8 人日（跨 `src/mcp` + `src/server`） |
| **风险** | `ReportStore` 的 `Project` 视图（`dir/name/al_path`）必须与 Bridge 的 `ProjectFor(args)` 一致，否则会 cache miss 或串工程；需要 `projects_` 键统一为 `al_path`（小写）。 |

### A6 · 进程预热（prewarm）
| 项 | 内容 |
|---|---|
| **优先级** | P2 |
| **现状** | 一切按需初始化：首次 `didOpen` 建索引、首次 `fpgaLsp/reports` 建报告快照、首次工具调用探测 TD（`bridge.cpp:894`、`commands.cpp:386`）。 |
| **问题** | 用户/agent 的**第一个**动作承担全部冷成本：索引 23.4 ms @200、报告 3.7 ms（summary）/ 768 ms（full）、`FindTd` 0.9 ms。首屏体验与"改完就保存"的时序叠加。 |
| **方案** | `LspServer::FeatureInitialize` 末尾（`lsp_server.cpp:405` 之后）派发一个低优先级后台任务：`index_service_->SetProject(p); index_service_->Get();` + `td::FindTd(configured)` + `report_store_->Get(p, kFiles|kArtifacts)`。用 `std::async`/专用 1 线程池，且在 `Shutdown()` 里 join。`--mcp` 模式在 `Bridge` 构造后同样预热默认工程。 |
| **收益** | 首个请求的冷成本从"用户可见延迟"变成"启动后台"；首屏待测（预期 ≥20 ms @200 文件）。 |
| **成本** | 1–2 人日 |
| **风险** | 后台线程与 `--td` 探测的竞态；关闭时不能留线程（`main.cpp:356-361` 的收尾顺序要扩）。 |

---

## 4. B 组：MCP 协议层

### B1 · 工具结果双份 payload（`text` + `structuredContent`）
| 项 | 内容 |
|---|---|
| **优先级** | P0 |
| **现状** | `WrapToolResult`：`text = value.dump(1, ' ')`（缩进 1）写进 `content[0].text`，随后 `out["structuredContent"] = value`（`mcp_server.cpp:118-137`）。两条通道承载**同一份数据**。 |
| **问题** | 实测 `fpga_project_structure`：总 **38,393 B**（text 21,260 + structured 14,257）；`include_ports=true`：总 **79,892 B**（text 45,300 + structured 27,564）。即 ≈91% 的载荷是重复语义。客户端还要付两遍解析（审计 `parse` 0.195–0.304 ms/次，`MCP.md:115-116`）。服务端 CPU 不是瓶颈（`dump(1)` 只占 1.9%，`MCP.md:118`），**瓶颈是字节与客户端解析**。 |
| **方案** | 在 `McpServerOptions` 里携带协商后的版本（`negotiated_version`，由 `initialize` 写入），`WrapToolResult` 按版本分派：`< 2025-06-18` → 只写 `content[0].text`；`>= 2025-06-18` → 只写 `structuredContent`（+ `content[0].text` 里放一行人类摘要，如 `"project_structure: 51 nodes"`，≤200 B）；两边都可读 `_meta.fpgalsp.payload="structured"`。同时把 `dump(1)` 改成 `dump(-1)`（紧凑）作为保底（若仍要留 text）。 |
| **收益** | `project_structure(include_ports)` **79,892 → ≈28.5 KB（−64%）**，默认参数 **38,393 → ≈15 KB**；客户端解析次数减半。 |
| **成本** | 1 人日（含 golden 探针更新） |
| **风险** | 只读 `content[0].text` 的老客户端（部分 IDE 集成）会拿不到数据 → 用协议版本门控，而不是全局删除；`isError` 结果保持现状（体积小）。 |

### B2 · 800 KB 阈值是"整份丢弃"，大结果完全不可用
| 项 | 内容 |
|---|---|
| **优先级** | P1 |
| **现状** | `text.size() > kMaxToolResultBytes (800000)` → 整个 result 换成 `{ok:false, error:"Tool result too large; pass a smaller limit", truncated:true}`（`mcp_server.cpp:119-126`，`mcp_server.h:72`）。 |
| **问题** | 10 MB timing 报告工程的完整报告载荷是 **56.54 MB**（审计 `REPORTS.md:51`），远超阈值：agent 拿不到任何数据，且错误信息不给可执行参数。`limit` 只在 `project_reports`（默认 100，`bridge.cpp:2060`）和 `project_structure`（`max_nodes`）上存在，`diagnostics`/`references`/`workspace_symbols` 只有"最大条数"，没有游标。 |
| **方案** | (1) `WrapToolResult(const Json&, WrapOptions)` 增加 `offset`/`page_bytes`（默认 700,000）、`truncate_plan`（对数组节点按元素裁剪，优先裁 `paths`/`groups`/`diagnostics`/`references`）；(2) 超限时返回 `{ok:true, truncated:true, nextOffset:N, returned:K, total:M, note:"call again with offset=N"}`，**不丢已取到的部分**；(3) 给 `project_structure`/`project_reports`/`diagnostics`/`references`/`workspace_symbols` 的 `inputSchema` 增加 `offset`（`tools.cpp`）；(4) 保留 800,000 作为硬上限（安全网）。 |
| **收益** | 大工程从"不可用"变成"分页可用"；单次响应恒 ≤800 KB；agent 行为可预测（不因结果大小随机失败）。 |
| **成本** | 3 人日 |
| **风险** | 响应形状变化（`truncated:true` 从"失败"变"成功+可续"）需要更新 golden 与扩展侧消费逻辑；`offset` 语义必须对"排序稳定"的节点成立（树/数组均稳定，除并发修改 —— 用 `generation` 检测）。 |

### B3 · 缺 `outputSchema`（2025-06-18 结构化输出的配套项）
| 项 | 内容 |
|---|---|
| **优先级** | P1 |
| **现状** | `Tool` 结构只有 `name/title/description/input_schema/requires_flag`（`tools.cpp:77-357`），`ToolToJson` 只写 `inputSchema` + `annotations`（`tools.cpp:368-382`）；实测 39 个 tool 均无 `outputSchema`。 |
| **问题** | 客户端无法校验/自动补全 `structuredContent`，只能靠模型读文本；也让"双写"（B1）缺少可协商依据（规范：声明 `outputSchema` 时 `structuredContent` 必须符合它）。 |
| **方案** | `Tool` 增加 `std::optional<Json> output_schema`；先覆盖 8 个高频工具（`project_structure`、`project_reports`、`flow_status`、`flow_log`、`diagnostics`、`module_info`、`document_symbols`、`read_file`），字段名与现有 JSON 键一一对应；`ToolToJson` 在 `negotiated_version >= 2025-06-18` 时输出。用 golden 快照测试锁住"schema ↔ 实际响应键"的一致性（新增 `tests/mcp_output_schema_test`：对每个工具跑一次，断言响应键 ⊆ schema 属性集）。 |
| **收益** | 客户端可在调用前知道形状；B1 的"只发 structuredContent"有规范依据；减少 agent 反复试参数。 |
| **成本** | 2–3 人日（8 个 schema + 测试） |
| **风险** | schema 与实现漂移 → 靠上面的 golden 测试兜；`additionalProperties:false` 会让多余键变成违规，需要按工具实际输出精确写。 |

### B4 · MCP 侧完全没有取消
| 项 | 内容 |
|---|---|
| **优先级** | P1 |
| **现状** | `HandleSingle`：无 id 的消息一律 `nullopt`（`mcp_server.cpp:241-245`），没有 `notifications/cancelled` 分支；`tools/call` 在 Serve 循环里同步执行（`main.cpp:174-182`）。 |
| **问题** | `flow_wait` 最长 7200 s（`bridge.cpp:3407`），`strategy_compare` 可跑几十分钟；客户端超时/用户 Ctrl-C 后服务端仍在算，且**同一连接无法再发任何请求**（W6）。 |
| **方案** | (1) `McpServer` 增加 `std::unordered_map<std::string /*requestId*/, std::shared_ptr<std::atomic<bool>>> cancels_`，`tools/call` 执行前登记；(2) `HandleSingle` 增加 `method == "notifications/cancelled"` 分支：`params.requestId` → 置位，按规范返回 `-32800`（或 `isError` + `_meta.cancelled`，二选一并写进文档）；(3) `ToolDispatcher` 增加 `SetCancelToken(std::shared_ptr<std::atomic<bool>>)`，`FlowWait` 的循环（`bridge.cpp:3428-3447`）与 `StrategyCompare` 每轮检查；(4) `flow_cancel` 保持独立（取消整个 flow，而不只是这次等待）。 |
| **收益** | agent 超时后 1 s 内释放；同一连接恢复可用；长任务不再钉死连接。 |
| **成本** | 4–6 人日 |
| **风险** | 引入线程池是必要的（同步执行时 `notifications/cancelled` 读不到）—— 这会把 `main.cpp` 的 MCP 分支从"完全同步"改成"有界工作池 + 结果队列"，与 A1 的 Session 模型要一起设计。 |

### B5 · 长任务缺 `job` + `subscribe`（只有 runId，没有订阅通道）
| 项 | 内容 |
|---|---|
| **优先级** | P2 |
| **现状** | `flow_run` 返回 `runId`（`tools.cpp:117-123` 的 schema；`flowController.js:257-264` 消费）；MCP 侧唯一的观察手段是 `flow_status` / `flow_wait` 轮询，而 `flow_status` 每次都 `BuildReports`（`bridge.cpp:3387`）。 |
| **问题** | 轮询既贵又慢（每次全量重建报告），且事件（`flowEvent` 的 `log`/`step`/`done`）在 MCP 侧完全不可见。 |
| **方案** | (1) 把 run 暴露成资源：`fpgalsp://runs/<runId>`（`resources/templates/list` 注册）；(2) `resources/read` 返回 `{state, seq, lines:[...], issues}`，支持 `_meta.sinceSeq`；(3) 实现 `resources/subscribe` + `notifications/resources/updated`（MCP 规范内，客户端可选支持）；(4) 若客户端不支持，保留 `flow_wait` 但让它返回增量 `sinceSeq`。 |
| **收益** | agent 不再轮询全量报告；实时看到 TD 日志；与 B6 共用资源基础设施。 |
| **成本** | 3–5 人日（依赖 B6） |
| **风险** | 各客户端对 `resources/subscribe` 支持度参差（需 capabilities 协商 + 轮询兜底）。 |

### B6 · `resources` 恒空
| 项 | 内容 |
|---|---|
| **优先级** | P2 |
| **现状** | `resources/list`、`resources/templates/list` 返回空数组（`mcp_server.cpp:286-295`）；`resources/read` 不存在 → `-32601`。 |
| **问题** | 报告/日志/`.al`/ADC/SDC 这些"天然是资源"的东西只能通过 `read_file`/`search_text` 取，agent 要多轮试路径；`read_file` 的 `limit`/`max_chars` 上限（200,000 字符）还会截断大报告。 |
| **方案** | `resources/list` 返回：工程 `.al`、`*_Runs/<latest>/` 下的报告清单、`.fpgalsp/logs/*.log`；`resources/templates/list` 提供 `fpgalsp://report/{al}/{stage}/{name}` 与 `fpgalsp://log/{al}/{kind}`；`resources/read` 走同一个"限额 + 游标"实现（复用 B2 的裁剪器）。`resources/list` 的 `_meta.generation` 用 D8 的统一令牌。 |
| **收益** | agent 直接按 URI 取数据，少 1–2 轮探索；大报告可分段读。 |
| **成本** | 3 人日 |
| **风险** | 资源枚举要避免每请求扫盘（用 `ReportStore` 的 runs 目录 stamp）；文件路径泄露面扩大（配合 A3 的鉴权）。 |

### B7 · 进度通知的语义与覆盖面
| 项 | 内容 |
|---|---|
| **优先级** | P2 |
| **现状** | 只有 `flow_wait` 发 `notifications/progress`（`mcp_server.cpp:174-212` + `bridge.cpp:3434`），且 `progress` 传的是**已等待秒数**（`elapsed`），不是 0..1 比例；`total` 字段从来没写。 |
| **问题** | 客户端进度条按 0..1 解释会瞬间打满或溢出；`project_reports`（可 700 ms+）、`strategy_compare`（分钟级）没有任何进度。 |
| **方案** | (1) `progress_` 的签名改为 `(double done, double total, const std::string& msg)`，或者约定"有 `total` 时为绝对量，无 `total` 时为 0..1"；(2) `flow_wait` 传 `total=timeout`、`done=elapsed`；(3) `ReportStore::Get` 增加 `ProgressFn` 回调，`ProjectReports` 在构建期按文件数发进度（需要引入 thread_local 的 emitter，因为 `Get` 是同步的）；(4) `StrategyCompare` 按策略数发。 |
| **收益** | 客户端进度正确；长任务可观测（配合 B4 的取消更有意义）。 |
| **成本** | 2–3 人日 |
| **风险** | `thread_local` emitter 与 A1 的多线程 Session 要隔离（每线程一份，不能共享）。 |

### B8 · `initialize` 的扩展能力声明
| 项 | 内容 |
|---|---|
| **优先级** | P2 |
| **现状** | `capabilities` 只有 `tools.listChanged=false`（`mcp_server.cpp:261-264`）；`instructions` 是 447 B 固定串；不支持的版本静默回落（`:259`）。 |
| **问题** | 客户端无法知道"这个服务端支持分节报告 / 可续页 / 有 generation / 需要令牌"，只能试错（`reportsService.js:126-137` 就是靠"响应里有没有 `section` 字段"来猜服务端能力，属于 B8 缺失的直接后果）。 |
| **方案** | `initialize` 结果增加 `capabilities.experimental.fpgalsp = {schemaVersion:2, pageable:true, conditionalReports:true, generation:"u64", tokenRequired:true, sections:[...], resources:true}`；`instructions` 按 flags 生成（只列启用的工具类别）；`negotiated_version` 存进 `McpServerOptions`（B1 用）。 |
| **收益** | 消灭客户端的能力试探逻辑；服务端可安全地演进响应形状。 |
| **成本** | 1 人日 |
| **风险** | `experimental` 是规范允许的扩展位，但同名键语义要写进本文档的契约小节。 |

### B9 · 解除 Python 对拍约束后的 schema 治理
| 项 | 内容 |
|---|---|
| **优先级** | P2 |
| **现状** | `tools.cpp:4-9` 明确写"schema 不要手改，必须同步 Python，否则 `tests/compare_mcp_schema.py` 变红"；`tools.h:10` 同义；`main.cpp:62` / `mcp_server.h:4` / `mcp_server.cpp:4-8` 仍以 Python 为参考实现；该脚本以 CTest 形式注册（`tests/CMakeLists.txt:87-97`）。 |
| **问题** | 用户已明确放弃 Python 接口兼容。继续保留对拍约束会**阻塞** B1（结果形状）、B2（分页字段）、B3（`outputSchema`）—— 每改一次 schema 都要同步一份不再对齐的 Python 实现。 |
| **方案** | (1) 摘掉 `tests/CMakeLists.txt:87-97` 的 CTest 注册与 `tests/compare_mcp_schema.py`，替换为"golden 快照 + `outputSchema` 一致性测试"（B3 里那个，基线可用 `doc/audit/2026-11-19/fix/baseline/mcp-raw.json`）；(2) 把 `tools.cpp` / `tools.h` 头部注释改为"本文件是协议契约的**唯一**来源"，并在 `doc/protocol.md` 加一节 MCP 工具表（从 `tools/list` 实测输出生成）；(3) 39 个工具的 schema 从内联 C++ 构造器改为一份 `.json` 数据文件（编译期嵌入），使 schema 可被工具/客户端直接复用（当前每次 `tools/list` 都要现场构造 20 KB JSON，实测 32 工具 20,305 B / 39 工具 24,069 B，构造耗时 待测）。 |
| **收益** | 解锁 B1/B2/B3；schema 单一来源；`tools/list` 从"构造 JSON"变成"拼接常量"。 |
| **成本** | 0.5–1 人日 |
| **风险** | 失去与历史实现的逐字对拍保护 → 用 golden 快照补上（`doc/audit/2026-11-19/fix/baseline/mcp-raw.json` 可直接作为基线）。 |

---

## 5. C 组：自定义 `fpgaLsp/*` 协议

### C1 · `indexChanged` 只带提示，不带数据（客户端已准备好接收数据）
| 项 | 内容 |
|---|---|
| **优先级** | P0 |
| **现状** | 服务端只发 `{al, generation, paths}`（`lsp_server.cpp:465-475`）；客户端 `applyIndexChanged` 已完整实现 `modules[]` 原地打补丁（`moduleTree.js:221-255`：`{module, portCount, defined}` 直接改节点），但 `modules` 永远是空 → 每次都走 `refresh()` 全量重取（`:233`）。 |
| **问题** | 实测一次 `moduleTree` 是 **16,426 B**（corpus_alpha）；一个保存动作只需改 1 个节点的 `portCount`。当前等于"每保存一次重启一次整棵树"，并伴随整棵 `_wrap` 重建 + `fire()`。 |
| **方案** | (1) `IndexService::OnFileChanged()` 返回值从 `void` 扩展为 `struct GenerationPatch {uint64_t generation; std::vector<std::string> changed_modules; std::vector<std::string> removed_modules;}`（内部已有模块表 diff 信息时零成本；没有就对比前后 `modules_by_key_`）；(2) `MaybeNotifyIndexChanged(changed, patch)` 发 `{al, generation, paths, modules:[{module, portCount, defined, file}], full:false}`，`modules` 超过 200 条或涉及拓扑变化（新增/删除实例）时改发 `{full:true}`；(3) 客户端在 `_applyModuleUpdates` 里补 `removed` 处理（从 `_childrenOf` 缓存里剔除）。 |
| **收益** | 一次保存：重取 **16,426 B → 0**（实测值参照），少一次全树构建；保存→树更新的端到端延迟去掉一次 RPC RTT。 |
| **成本** | 1.5 人日 |
| **风险** | 增量与树不一致 → 保留 `generation` 校验（客户端维护 `lastGeneration`，收到 `full:true` 或 gap 时退回全量）；`defined`/`portCount` 之外的变化（例化位置移动）不在 `modules` 语义内，必须走 `full:true`。 |

### C2 · `moduleTree` 只有全量形态，没有按路径懒加载
| 项 | 内容 |
|---|---|
| **优先级** | P1 |
| **现状** | `HandleModuleTree` 只读 `uri` 与 `includePrimitives`（`commands.cpp:228-231`），`features::ModuleTree` 一次建整棵树，预算 500 子节点/节点、50000 节点（`commands.cpp:213`）；截断只用布尔 `truncated` 表示。 |
| **问题** | 树随工程规模线性增长（16,426 B 是 51 节点级小工程；50000 节点上限意味着最坏 10 MB+，待测）。VS Code 的树视图实际只展开到用户点开的层级 —— 首屏只要 root + 第一层。截断后也没有"怎么继续取"的手段。 |
| **方案** | (1) `fpgaLsp/moduleTree` 增参数 `{path?: string, depth?: int(1..64), maxChildren?: int(1..500), maxNodes?: int}`；`path` 用稳定实例路径（`top/U_A/U_B`，与 `symbol_path` 的格式对齐）；(2) 响应增 `{nodes:[...], hasMore:bool, nextPath?:string, generation:uint64}`；(3) 服务端按 `path` 定位后只展开 `depth` 层（`features::ModuleTree` 加 `start_path`/`depth` 参数或新增 `features::ModuleSubtree`）；(4) 客户端 `_childrenOf` 改成按需 `await rpc.request(moduleTree, {path, depth:1})`，把结果填进节点缓存。 |
| **收益** | 首屏载荷 16,426 B → 约 1–2 KB（待测）；大工程不再触发节点预算截断；展开时延与节点数解耦。 |
| **成本** | 4 人日（含客户端） |
| **风险** | 实例路径稳定性（递归实例、同名例化）—— 必须复用已有的路径构造逻辑，不能另造一套；`generation` 变了要作废已缓存子树（C1 已提供）。 |

### C3 · `reportsChanged` 不带 revision，客户端只能全量重取
| 项 | 内容 |
|---|---|
| **优先级** | P1 |
| **现状** | 推送 `{stage:"all", sections:[...], generatedAt}`（`lsp_server.cpp:477-491`）；客户端收到后 `reportsService.invalidate()` **清空所有 section 缓存**并 `force` 重取（`extension.js:1391-1400`；`reportsService.js:58-64`、`:85-110`）；`fpgaLsp/reports` 响应里没有 revision（`commands.cpp:372-407` 只加 `section/project/al/flow/routed_db/strategies/analysis`）。 |
| **问题** | 一次 flow 产生新报告 → 4 个 section（summary/timing/area/qor）全部重取；大工程命中路径 **461–489 ms / 56.54 MB**（审计 `REPORTS.md:51,102`），等于把同一份数据在 UI 上重传多次。 |
| **方案** | (1) `ReportsSnapshot::revision` 已经存在（`report_store.h:54`），`FeatureReports` 加 `data["revision"] = snapshot->revision`；(2) 请求支持 `ifRevision`：相等时直接返回 `{ok:true, unchanged:true, revision}`（不构造 data）；(3) `reportsChanged` 携带 `{revision, sections, generatedAt}`（不再需要客户端猜）；(4) 客户端 `ReportsService.fetch` 带 `ifRevision`，`invalidate(section)` 只清推送里列出的 section。 |
| **收益** | 一次 flow 完成后的 4 次重取 → 1 次；未变化的 section 直接 `unchanged`；命中路径的 461–489 ms 只付一次。 |
| **成本** | 2.5 人日 |
| **风险** | revision 是 per-project 的（`report_store.cpp` `ProjectState::revision`），而快照按 `(mask,limit)` 分槽（`report_store.h:155-164`）→ revision 相等不代表 `(mask,limit)` 快照存在，仍可能构建一次；语义要在契约里写清"revision 只表示底层报告文件未变"。 |

### C4 · `flowStatus` 每次带 200 行日志 + 扫 `_Runs` 目录
| 项 | 内容 |
|---|---|
| **优先级** | P1 |
| **现状** | `FeatureFlowStatus` 把 `runner->Status().log`（上限 200 行，`flow_runner.cpp:446`、`flow_runner.h:100`）整份塞进 `flow.log`，并遍历 `*_Runs` 每个子目录构造 `artifacts.runs[]`（`commands.cpp:420-452`）。 |
| **问题** | 客户端已经有 `flowEvent` 实时流 + 10 s 兜底轮询（`flowController.js:18`、`:326-333`），**轮询结果里的日志是纯冗余**；200 行 × 单行 60–120 B ≈ 12–24 KB/次（推算，待测），外加每次一次目录枚举。 |
| **方案** | (1) `fpgaLsp/flowStatus` 增参数 `{logLines?: int(0..200, 默认 0), logSinceSeq?: int, runs?: bool(默认 false)}`；(2) `FlowState` 加 `uint64_t log_seq`（每条 `Emit` 递增，`flow_runner.cpp:445`）；(3) 响应 `flow.log` 为空数组、`flow.logSeq = seq`、`flow.logDropped = n`；`artifacts.runs` 只在 `runs=true` 时返回；(4) `flow_wait`（MCP）不需要日志，继续用 `Status().ToJson()`。 |
| **收益** | 每次兜底轮询少 12–24 KB（推算）+ 一次目录枚举；flow 长时间运行时这是唯一持续产生的流量。 |
| **成本** | 1.5 人日 |
| **风险** | 老客户端若依赖 `flow.log` 会丢日志 → 提供 `logLines=200` 兼容开关并写进 capabilities（B8）；`artifacts.runs` 被 `FlowTree` 使用（`flowTree.js`），默认 false 会让树空 → 客户端必须同步改。 |

### C5 · `flowEvent` 每个日志行一条通知
| 项 | 内容 |
|---|---|
| **优先级** | P1 |
| **现状** | `Record()` 把 TD 输出按行拆分，**每行**发一条 `Emit("log", line)`（`flow_runner.cpp:466-484`）；每条事件是一个独立 JSON-RPC 通知（`lsp_server.cpp:1740-1743`）。 |
| **问题** | 一次完整 flow 的 TD 输出可达数千行 → 数千条帧，每帧有 `Content-Length` 头 + 完整 JSON 信封（`runId/type/stage/elapsed/message/ok` 约 100 B 固定开销）。客户端每行一次 `appendLine`（`flowController.js:412-418`）+ 一次诊断解析（`parseFlowIssue`）。 |
| **方案** | 在 `FlowRunner` 里加 `LogBatcher`：累积到 50 行或 100 ms（取先到）flush 一条 `{type:"log", lines:[...], seq, dropped}`；`done`/`error`/`step` 事件 flush 前必须先把批次发出（保证顺序）；`seq` 连续可检测丢失。客户端 `onFlowEvent` 增加 `Array.isArray(event.lines)` 分支，循环 `appendLine`。 |
| **收益** | 事件数下降 1–2 个数量级（待测，取决于 TD 输出密度）；每条固定开销被摊薄。 |
| **成本** | 2 人日 |
| **风险** | 顺序与终态事件；`elapsed` 是按行记录的，批量后每行需要带自己的时间戳（`{t, message}`）否则 UI 时间列会失真。 |

### C6 · heavy 白名单没覆盖索引类方法 → 它们仍在 IO 线程
| 项 | 内容 |
|---|---|
| **优先级** | P1 |
| **现状** | 白名单只有 `fpgaLsp/reports`、flow/analysis/program 等与 `textDocument/codeAction`（`async_dispatcher.cpp:44-61`），注释明确写"索引类方法等 A1 接入后再迁入"；`fpgaLsp/moduleTree` 不在其中。 |
| **问题** | `moduleTree`/`documentSymbol`/`workspaceSymbol`/`references` 都在 IO 线程同步执行；索引快照现在已经是不可变 `shared_ptr`（`IndexService` 返回 `shared_ptr<const HdlIndex>`），迁入条件已满足。一次大工程 `moduleTree`（O(nodes)）会把同一时刻的 `didChange` 排到后面（按键延迟）。 |
| **方案** | (1) `IsHeavyMethod` 白名单增加 `"fpgaLsp/moduleTree"`、`"textDocument/documentSymbol"`、`"workspace/symbol"`、`"textDocument/references"`、`"fpgaLsp/flowStatus"`；(2) 让 handler 能看见取消：`AsyncDispatcher` 在 `Invoke` 前设置 `thread_local const std::atomic<bool>* g_cancel_token`，提供 `jsonrpc::CancelRequested()`；在 `features::ModuleTree::BuildNode` 循环与 `FindOccurrences` 循环里检查；(3) `WriteResult` 已经是线程安全的（`transport_.WriteMessage` 内部锁）。 |
| **收益** | 按键路径不再被重请求挡住（历史上 `codeAction` 曾把 p95 从 1 ms 拖到 343 ms，`async_dispatcher.cpp:56-59`）；取消能真正生效。 |
| **成本** | 2 人日 |
| **风险** | 索引类 handler 必须只读快照（现在满足），不能碰 `docs_` 的可变状态；`thread_local` 令牌必须在 `finally` 语义下清理（RAII guard）。 |

### C7 · 服务端没有请求合并（single-flight 只在客户端）
| 项 | 内容 |
|---|---|
| **优先级** | P2 |
| **现状** | `ReportsService` 在客户端做 single-flight（`reportsService.js:95-106`），服务端 `FeatureReports` 直接 `report_store_->Get(...)`（`commands.cpp:336`）；并发构建由 `build_mutex_` 串行（`report_store.h:186-193`）。 |
| **问题** | 任何绕过该客户端的调用方（另一个扩展、MCP、`fpgalsp.reports` 命令，`commands.cpp:933-935`）都会各自触发一次 `Get`；同一 `(mask,limit)` 的并发请求在服务端是"一个构建 + 其余等锁"，而不是"共享同一个结果"。 |
| **方案** | `ReportStore` 增加 `std::map<uint64_t /*slot*/, std::shared_future<std::shared_ptr<const ReportsSnapshot>>> building_`；`Get()` 命中 building 就 `wait` 同一个 future；构建线程完成时 `set_value` 并移出表。 |
| **收益** | 冷构建从 N 次等待变成 1 次计算 + N−1 次共享；对 `fpgalsp.reports` 命令 + 面板同时刷新有效。 |
| **成本** | 2 人日 |
| **风险** | 与 D3（分级构建）叠加时要避免死锁（future 表与 `build_mutex_` 的获取顺序固定）。 |

### C8 · `workspace/executeCommand` 返回形状不统一
| 项 | 内容 |
|---|---|
| **优先级** | P2 |
| **现状** | 有的命令返回 `{ok:...}` 结构化对象（`commands.cpp:933-935` 的 `fpgalsp.reports`），有的返回 `null`（`HandleNull`，`lsp_server.cpp:493-496` 注册给一批命令），有的返回裸对象。 |
| **问题** | 客户端需要按命令名分支处理（`flowController.js` 对 `runFlow` 单独判 `ok`）；错误语义混在 `ok:false` 与 JSON-RPC error 之间。 |
| **方案** | 统一 envelope：`{ok:bool, error?:string, result?:any, generation:u64}`，`null` 命令迁移为 `{ok:true, result:null}`；在 `doc/protocol.md` 的 commands 表里标注每个命令的返回类型。 |
| **收益** | 客户端一处解包；错误可统一展示。 |
| **成本** | 1 人日 |
| **风险** | 兼容性 → 提供 `params.envelope=true` 开关跑一个版本，扩展先切。 |

---

## 6. D 组：后端处理链路

### D1 · MCP 报告路径绕过 `ReportStore`（每次全量重解析）
| 项 | 内容 |
|---|---|
| **优先级** | P0 |
| **现状** | `Bridge::ProjectReports` → `reports::BuildReports(proj)`（`bridge.cpp:2077`）；`Bridge::FlowStatus` 同样（`:3387`）。`src/mcp/**` 里 `grep 'ReportStore\|report_store'` = **0 命中**（只有这两处 `reports::BuildReports` 调用），`bridge.h` 也没有相关成员。 |
| **问题** | 与 LSP 侧（`commands.cpp:336` 走 store）形成两条路径：agent 每调一次 `fpga_project_reports` 或 `fpga_flow_status` 就重读+重解析全部报告文件。冷 **768–784 ms**（审计 `REPORTS.md:66`），10 MB timing 工程每次如此。 |
| **方案** | (1) `BridgeOptions` 增加 `server::ReportStore* report_store`；`shared_server` 非空时从宿主取，否则 `Bridge` 自持一个（`std::unique_ptr<server::ReportStore>`）；(2) `ProjectReports` 改为 `store->Get(proj, SectionMask(section), false, limit)` + 现有 `SummarizeReports` 裁剪；(3) `FlowStatus` 同理（它只要 `artifacts`，用 `kFiles|kArtifacts` 即可）。 |
| **收益** | 768–784 ms → **0.54 ms**（命中，审计 `REPORTS.md:96`）；`flow_status` 也会顺带变快。 |
| **成本** | 1 人日（改 2 个函数 + 1 个字段） |
| **风险** | Bridge 的 `Project` 与 store 的 `ProjectState` 键必须都是 `lower(al_path)`，否则每次 miss；`limit` 参与快照键（`report_store.h:155`），不同 `limit` 会各存一份快照（内存 `limit` 组合要收敛到少数档位，如 100/500/1000）。 |

### D2 · 报告装饰项每请求重做（`NewestRoutedDb` + `FindTd` + `analysis.json`）
| 项 | 内容 |
|---|---|
| **优先级** | P1 |
| **现状** | `FeatureReports` 每次都：遍历 `*_Runs/phy_1` 找最新 `*_pr.db`（`commands.cpp:267-289`、`:381`）、`td::FindTd(configured)`（`:386`）、读 `.fpgalsp/cache/analysis.json`（`:396-407`）。 |
| **问题** | 命中快照本身只要 **0.54 ms**，但这三项装饰是固定成本：`FindTd` **0.89–0.93 ms**（审计 `MCP.md:146`，且 `src/td` 里**没有任何缓存**，`grep FindTd` 无 static/cache），`AvailableStrategies` 还要再扫一遍 TD 安装根；目录遍历随 `phy_1` 文件数增长。对 `section=summary`（3.7 ms 构建）来说装饰占 ~27%。 |
| **方案** | (1) `td::FindTd` 加进程内缓存 `{explicit_root → TdChoice, 30 s TTL}`（`td_install.cpp:413`），`fpga_td_info` 增加 `refresh:true` 绕过；(2) 装饰结果按 `(project.al_path, revision, td_choice.roots)` 缓存进 `LspServer`（`mutable std::mutex decor_mutex_`），`reportsChanged`/`didChangeWatchedFiles` 时失效；(3) `analysis.json` 用 `(path,mtime,size)` stamp 缓存（与 `ReportStore::FileStamp` 同款）。 |
| **收益** | −1 ms/请求（`FindTd` 归零）+ 目录遍历归零；`summary` 请求 3.7 ms → ≈2.4 ms。 |
| **成本** | 1 人日 |
| **风险** | 用户中途装/卸 TD 后 30 s 内看到旧选择（`fpga_td_info` 可强制刷新）；`routed_db` 变化靠 `*_Runs` 目录 mtime（已有 stamp 机制可复用）。 |

### D3 · 报告构建串行化：轻请求被重构建挡住 ~98%
| 项 | 内容 |
|---|---|
| **优先级** | P1 |
| **现状** | `ReportStore` 用 `build_mutex_` 串行化所有构建（`report_store.h:186-193`，注释说"F3 起构建在 `state_mutex_` 之外"）；不同 `mask` 的快照分开缓存但共享同一把构建锁。 |
| **问题** | 审计实测：一次 `kAll` 重构建（670–752 ms）期间，`section=summary` 的轻请求（1.9 KB）被阻塞 **~98%**（`REPORTS.md:124`）。也就是说 `section` 分节在"有重构建"时完全失效 —— 而分节的全部意义就是让 UI 先看到轻的。 |
| **方案** | (1) 构建按 `mask` 分级：`summary`（`kFiles|kArtifacts`）不取 `build_mutex_`，只取 `cache_mutex_`（解析结果已按文件粒度缓存，`LookupOrParse`）；(2) `kAll` 继续独占 `build_mutex_`；(3) 发布改成"两段"：先发布 `files/artifacts` 的最小快照（~2 KB），再发布完整快照，中间态用 `partial:true` 标记；(4) 用 C7 的 future 表让同一 `mask` 的等待者共享。 |
| **收益** | 轻请求 p95 从"重构建时长"降到 ≈1 ms；UI 首屏不再被 700 ms 的报告构建拖住。 |
| **成本** | 3 人日 |
| **风险** | `cache_mutex_` 与 `build_mutex_` 的加锁顺序必须固定（现在是"构建期间不持 state_mutex_，`current_` 由 build_mutex_+cache_mutex_ 保护"），改成分级后 `current_` 成员变量不再够用 → 需要把 `ProjectState*` 改成参数传递（避免两个构建线程互相覆盖 `current_`）。 |

### D4 · 命中路径的深拷贝 + 非紧凑序列化
| 项 | 内容 |
|---|---|
| **优先级** | P1 |
| **现状** | `FeatureReports` 在 `include_raw` 或未指定 `section` 时直接 `return snapshot->data`（`commands.cpp:343-346`，运行时深拷贝整棵 JSON）；LSP 非紧凑序列化走 `Serialize(compact=false)`（`transport.h:183` → `json_util` 的 `AddPythonStyleSpaces`）。 |
| **问题** | 审计：`snapshot->data` 拷贝 **246–267 ms**（10 MB 报告，工作集 +90 MB）、`Serialize(false)` **224–236 ms**（其中 spaces 110–121 ms）；命中路径合计 **461–489 ms**（`REPORTS.md:97-102`）。而这次拷贝的唯一目的是"让 `HandlerResult` 持有一份可变 JSON"。 |
| **方案** | (1) `jsonrpc::HandlerResult` 增加 `std::optional<std::string> raw_body`，`Dispatcher`/`AsyncDispatcher` 的写路径遇到它就原样写帧（`transport.WriteMessage(std::string_view)`）；(2) `FeatureReports` 在 `(revision, mask, limit)` 命中时直接从快照序列化（零拷贝 + 紧凑序列化），并把序列化结果按槽位缓存成 `std::string`（LRU 1 槽，≈33 MB）；(3) 需要装饰时用"序列化视图"而非拷贝（先写 `data` 的前缀键，再写装饰键）。 |
| **收益** | 461–489 ms → **~30–50 ms**（若缓存字节则 ~2 ms，审计 `REPORTS.md:112-115`）；工作集 −90 MB/请求。 |
| **成本** | 3–4 人日 |
| **风险** | LSP 是非紧凑模式（`compact_json()==false`），现有 `AddPythonStyleSpaces` 对 32.7 MB 也要 110 ms → 优化必须同时决定"是否维持 Python 风格的加空格"，而 `AddPythonStyleSpaces` 的存在理由是对拍；用户已放弃 Python 兼容（B9），可以**直接去掉**（字节数 −（32.7 MB 的空格）→ 待测，客户端 JSON 解析不受影响，因为 JSON 空白无语义）。 |

### D5 · LSP 标准内两处浪费：假 delta + watcher 覆盖不全
| 项 | 内容 |
|---|---|
| **优先级** | P1 |
| **现状** | (a) `capabilities.semanticTokensProvider.full.delta = true`（`lsp_server.cpp:389`），但 `HandleSemanticTokensDelta` 返回 `{start:0, deleteCount:<旧长度>, data:<全新数组>}`，且 `previousResultId` 被 `(void)previous` 丢弃（`:647-677`）——等于把"旧数组 + 新数组"都传一遍，比 `full` 还贵。(b) watcher 链路**其实是通的**（这是对任务背景与审计 `INDEX.md:216` 推断的更正）：`extension.js:282-284` 通过 `synchronize.fileEvents` 挂了 `**/*.{v,sv,vh,svh,vhd,vhdl,adc,sdc,al}` 的 FileSystemWatcher，而 vscode-languageclient 的 `FileSystemWatcherFeature.registerRaw()` 是**直接挂 onDidCreate/Change/Delete 监听**（`node_modules/vscode-languageclient/lib/common/fileSystemWatcher.js:45-56`、`client.js:1246`），**不依赖**服务端 `client/registerCapability` → 第一个匹配事件到达后 `watcher_active=true`（`lsp_server.cpp:438`），此后不再做全量 stamp 校验。审计测到的"45% 在 `ValidateStampsLocked`"是**审计探针客户端**（裸 LSP 客户端、不配 fileEvents）的口径，不代表 VS Code 现场。残留的真实缺口三条：① watcher glob 不含报告产物（`*_Runs/**` 的 `.rpt/.qor/.fsp`），报告失效只能靠 `ReportStore` 每请求 `AnyStaleUnlocked` stat；② 首事件之前每 250 ms（TTL，`index_service.cpp:156-170`）一次全量 stamp（@200 文件 0.30 ms/窗口，审计 F2），外部改动最多延迟 250 ms 可见；③ 非 VS Code 客户端（其它编辑器、测试探针、MCP-only）不配 fileEvents → 永远走 TTL 路径，这才是需要 `client/registerCapability` 兜底的场景。 |
| **问题** | (a) 每次输入都付"旧 token 数组 + 新 token 数组"，大文件数十 KB 的纯浪费；(b) 在当前 VS Code 现场**不是**每请求 0.85–3.96 ms 的固定开销（watcher 首事件后就不再校验），残余成本是报告文件的每请求 stat 与首事件前的 TTL 校验（可忽略）；真正的危害是**报告产物不在 watcher 覆盖范围内** → 新报告落盘不会推 `reportsChanged`（详见 D7）。审计里那个"85% 固定开销"只对未配 fileEvents 的客户端成立。 |
| **方案** | (a) 二选一：① 把 `delta` 声明改 `false`（一行，立刻停止浪费，客户端只拉 full）；② 做真正的前后缀 diff —— `data` 是 5 元组数组，找最长公共前缀/后缀，发 1 条 `{start, deleteCount, data}` 覆盖中段，通常 1–2 条编辑。(b) **主路径不需要 `registerCapability`**：① 把 `synchronize.fileEvents` 的 glob 扩到 `**/*.{v,sv,vh,svh,vhd,vhdl,adc,sdc,al,rpt,qor,fsp,status}`（客户端一行；注意 `*_Runs/**` 会产生事件风暴，客户端侧已按 ~100 ms 批处理）；② 或服务端在 flow 结束回调里调 `report_store_->CheckForChanges(project)`（见 D7，不依赖客户端行为）。`client/registerCapability` 仅作为"非 VS Code 客户端"的兜底：先看 `initialize.params.capabilities.workspace.didChangeWatchedFiles.dynamicRegistration`，为 true 才发，失败按现状回落。 |
| **收益** | (a) 大文件每次按键 −（旧数组字节，待测；10k token ≈ 200 KB 级）；(b) 报告变化从"下次请求 stat 才发现"变成事件驱动，`reportsChanged` 的推送延迟从"取决于用户是否再次请求"降到 ~100 ms 事件批次；顺带覆盖"flow 结束但 UI 不刷新"这个功能缺陷。 |
| **成本** | (a) 0.5 人日（方案①）/ 2 人日（方案②）；(b) 0.5 人日（glob 一行 + 复用 D7 的 `CheckForChanges`） |
| **风险** | (b) 报告目录事件量大（一次 phy 会写十几个文件）→ `InvalidatePaths` 需要去抖/合并；glob 扩到 `.rpt` 会多占少量句柄。 |

### D6 · 取消不协作：worker 只在边界检查
| 项 | 内容 |
|---|---|
| **优先级** | P2 |
| **现状** | `AsyncDispatcher` 只在调用前和返回后检查 `cancelled`（`async_dispatcher.cpp:181-190`、`:233-236`）；handler 内部拿不到令牌。 |
| **问题** | 被取消的 700 ms 报告构建仍跑完并占住 1/2 个 worker（`main.cpp:306` 起 2 个）；连续两次取消 = 两个 worker 都在做无用功，第三条请求开始排队。 |
| **方案** | 与 C6 同一套设施：`thread_local const std::atomic<bool>* g_cancel_token` + `jsonrpc::CancelRequested()`；在 `ReportStore::Get` 的逐文件循环、`features::ModuleTree::BuildNode`、`bridge.cpp` 的 `search_text` 文件循环、`FindOccurrences` 里检查并提前返回。 |
| **收益** | 取消真正释放算力；队列等待从"整段构建"降到 µs。 |
| **成本** | 2–3 人日 |
| **风险** | 提前返回要返回"取消"而不是"部分结果"，否则客户端会缓存半份数据。 |

### D7 · `ReportStore::CheckForChanges` 是死代码（`reportsChanged` 缺请求时兜底）
| 项 | 内容 |
|---|---|
| **优先级** | P2 |
| **现状** | `CheckForChanges` 有声明（`report_store.h:103`）与实现（`report_store.cpp:197`），但 `grep CheckForChanges src/` 只有这两处；`SendReportsChanged` 的唯一调用点是 `didChangeWatchedFiles` 且要求 `InvalidatePaths(...) != 0`（`lsp_server.cpp:446-456`）。 |
| **问题** | 头文件注释承诺"LSP 没有 watcher 事件时的兜底"，实际没有任何调用者 → flow 写完新报告**不会**推 `reportsChanged`（原因不是 watcher 没注册，而是 watcher 的 glob 只覆盖 `.v/.sv/.vhd/.adc/.sdc/.al`，不覆盖 `*_Runs/**` 的 `.rpt/.qor`，见 D5-(b)），UI 只能靠 `flowController.finish() → refreshAll()` 碰运气。 |
| **方案** | (1) 在 `FlowRunner` 的 `done`/`error` 事件回调里（`lsp_server.cpp:1754-1768` 的 notify lambda）调用 `report_store_->CheckForChanges(project)`，为 true 就 `SendReportsChanged`；(2) 或者接在 `FeatureReports` 入口（成本 ≈1.1 ms/请求，审计 `REPORTS.md:189`，仅在 watcher 未激活时执行）。方案 (1) 更便宜，选它。 |
| **收益** | 正确性：一份新报告出现后 UI 能收到一次通知（配合 C3 的 revision 就能精确重取）。 |
| **成本** | 0.5 人日 |
| **风险** | flow 的 `done` 回调在 worker 线程上，`CheckForChanges` 需要 `state_mutex_` unique 锁（已有）；要注意不要在持有 flow 锁时进 store 锁。 |

### D8 · 索引 `generation` 与报告 `revision` 是两套令牌
| 项 | 内容 |
|---|---|
| **优先级** | P2 |
| **现状** | 索引用 `index_service_->stats().generation`（`lsp_server.cpp:467`），报告用 `ReportStore` 的 per-project `revision`（`report_store.h:113`），二者无关；`fpgalsp.metrics` 分别暴露（`commands.cpp:978-996`）。 |
| **问题** | 客户端无法用**一个**令牌判断"我看到的世界是否过期"：乱序响应（async worker）与取消后的迟到响应都可能用旧数据覆盖新数据；C2 的懒加载子树也需要"父树是否还有效"的判定。 |
| **方案** | 定义 `workspaceGeneration`：`u64 = hash(index_generation, report_revision, project.al mtime_ns)`，由 `LspServer` 维护并在**所有**自定义响应里作为 `generation` 返回；`indexChanged`/`reportsChanged` 都带它；客户端对每个视图记住 `generation`，响应 `generation < 当前` 就丢弃。MCP 侧通过 `initialize.capabilities.experimental.fpgalsp.generation`（B8）暴露同一字符串。 |
| **收益** | 消除"迟到响应覆盖新数据"这一类偶发 UI 错乱；C2 的缓存失效有统一依据。 |
| **成本** | 2 人日 |
| **风险** | 令牌语义要写清"只增不减、跨工程切换要重置"；客户端要按视图（moduleTree/flow/reports）分别记录，不能全局一个。 |

### D9 · `reportsChanged` 的载荷是"用一次全量构建换来的"（Lead 补充）
| 项 | 内容 |
|---|---|
| **优先级** | P0 |
| **现状** | `SendReportsChanged(project)`（`lsp_server.cpp:477-491`）为了填 `params.sections`，直接 `report_store_->Get(project, reports::sections::kAll)`（`:479`）——构建/读取**全量**报告快照，随后只取 `snapshot->mask` 与 `generated_at`，快照立刻丢弃。它由 `didChangeWatchedFiles` 在 **IO 线程**上调用（`:446-456`），触发条件是 `InvalidatePaths(...) != 0`（真有报告文件变化）。 |
| **问题** | 报告刚被改写时（flow 刚结束、或用户覆盖 `.rpt`），kAll 快照通常已失效 → 这次 `Get` 就是一次**冷构建**：审计实测 10.11 MB timing 报告 **768–784 ms**（`REPORTS.md:51`）。这段时间 IO 线程不读 stdin，`didChange` 全部排队（与 F1 修掉的 `codeAction` 堵 IO 线程同类）；换来的只是一个 ~80 B 的通知。客户端收到通知后还要再取一次报告（C3），同一个快照被建**两遍**。 |
| **方案** | ① 载荷不需要快照：段名可由"本次失效的文件属于哪类"直接推出（`InvalidatePaths` 入参 + `ReportStore` 的 path→kind 映射），`generatedAt` 用 `revision` 代替；② 若仍要精确 `sections`，用**轻掩码** `Get(project, kFiles|kArtifacts)`（审计 3.7 ms 冷 / 0.54 ms 命中）而不是 kAll；③ 发送移到 worker 或 flow 回调线程，不占 IO 线程。推荐 ①+③。 |
| **收益** | 每次报告变化在 IO 线程上的阻塞 **768–784 ms → 亚毫秒级**；`builds` 从"报告变化 2 次"降到 1 次。纯减法，不改客户端契约。 |
| **成本** | 0.5 人日 |
| **风险** | `sections` 语义从"快照里有哪些段"变成"本次涉及哪些段"；`extension.js:1391-1400` 目前收到就刷新、不读该字段，兼容。 |

---

## 7. 建议的落地顺序

### 第一批（1–2 周，三项都只动单文件、都可独立验收）

| 项 | 内容 | 验收标准（可验证） |
|---|---|---|
| **B1** | 结果去重（`mcp_server.cpp` + `McpServerOptions.negotiated_version`） | ① 同一 `fpga_project_structure{include_ports:true}` 在 `corpus_alpha` 上的响应字节 **79,892 → ≤30,000**；② 默认参数 **38,393 → ≤16,000**；③ 新旧两种形态的 `structuredContent` 与旧 `content[0].text` **JSON deep-equal**（单测跑 39 个工具各一次）；④ 协商版本为 `2024-11-05` 的客户端仍能拿到全量文本。 |
| **C1** | `indexChanged` 带 `modules[]` | ① 保存一个 `.v` 后，`fpgaLsp/indexChanged` 的 `params.modules` 非空且 `generation` 递增；② 该次保存引发的 `fpgaLsp/moduleTree` 请求数 **= 0**（服务端日志计数 + 客户端 `moduleProvider.stats.requests` 不变）；③ 该次保存的回传字节从 **16,426 B（moduleTree 全量）→ ≤400 B（indexChanged 带 modules）**；④ 手工制造"拓扑变化"（新增例化）时收到 `full:true` 并回退全量且树正确。 |
| **D1** | MCP 报告接 `ReportStore` | ① 同一工程连续 3 次 `fpga_project_reports`：`ReportStore.stats().builds` 只 +1，`parses` 不增（通过 `fpgalsp.metrics` 或新增 MCP `_meta.stats` 读取）；② 第 2 次调用 p50 **≤5 ms**（原 768–784 ms 冷构建后）；③ 响应字段与改造前逐字段一致（golden diff 为空）。 |

**追加一项**：**D9**（0.5 人日，纯减法：把 `SendReportsChanged` 的 `kAll` 取快照改掉）也建议放进第一批——它不改任何契约，却把"报告一落盘就卡 0.8 s"这条最常见的卡顿直接消掉；验收标准：`didChangeWatchedFiles` 处理一次报告变化的耗时 **≤5 ms**（现为冷构建 768–784 ms），且 `reportsChanged` 仍到达客户端。

**为什么是这三项**：都在"字节/耗时"上直接可测，改动面各一个文件，且互不依赖；B1 立刻降低所有 MCP 会话的带宽与解析成本，C1 消灭最频繁的一次无效全量重取，D1 把最慢的一条后端路径变成缓存命中。

### 第二批（2–4 周，处理 P0 的可用性与安全）

| 项 | 内容 | 验收标准（可验证） |
|---|---|---|
| **A1 + A2** | MCP 多客户端并发 + 连接冷启动收敛 | ① 两个 MCP 客户端同时连接，A 执行 `flow_run`（或 `flow_wait` 睡眠 60 s）期间，B 的 `ping`/`tools/list` **p95 ≤ 50 ms** 且不超时；② `max_clients=8` 时第 9 个连接被明确拒绝（收到错误而不是挂起）；③ 第 2 个连接的 `initialize` → 首个 `tools/call` **≤2 ms**（现值为 9–11 ms 的审计值，改造后重测）；④ 关闭 host 时所有 session 线程 join，无残留（进程退出码 0，无僵尸）。 |
| **A3** | 端点广告 + 令牌 | ① 无 `_meta.fpgalspToken` 的 `initialize` 返回 **-32001** 且连接被关闭；② 正确 token 的调用全部成功；③ 两个同根工作区窗口各写一条 `mcp-endpoint-<pid>.json`，扩展分别连到**不同**实际端口且都可用；④ 杀掉 LSP 进程后扩展在 ≤1 s 内判定端点失效并回落（无 1000 端口扫描）；⑤ endpoint 文件里 `token` 不出现在任何日志（grep 日志为空）。 |
| **B2** | 分页/游标替代整份丢弃 | ① 1000 节点工程 `fpga_project_structure{max_nodes:50000}` 返回 `truncated:true` + `nextOffset`，单条响应 **≤800,000 B**；② 连续按 `nextOffset` 取完，拼接后节点数与一次性全量（本地对照）一致；③ 不再出现 `"Tool result too large"`；④ `project_reports` 在 56.54 MB 级工程上同样可分段取完。 |

**为什么第二批是这三项**：A1/A3 是"可用性/安全"的 P0（现在两个客户端互斥、本机任意进程可读工程文件），B2 决定 agent 在大工程上能不能干活；它们都需要新增基础设施（Session / endpoint 文件 / 裁剪器），一起做能复用同一批测试脚手架。

### 第三批（按需，已在第 3–6 节列出）
C3+C4（推送/轮询字节）、D2（装饰缓存）、D3（分级构建）、D5（真 delta + watcher glob 覆盖报告产物）、C2（懒加载）、B4（取消）—— 这些依赖第二批的设施（generation 令牌、Session 线程池、裁剪器），建议在前两批稳定后再排。

---

## 8. 附：本次实测数据与复现方法

**环境**：`Y:\fpga-lsp-cpp\build\bin\fpgalsp-native.exe`（2026-11-19 22:53 构建，6,132,736 B）；工程 `tests/gate-corpus/corpus_alpha`（`demo_project.al`，10,061 B）。

| 测量项 | 值 | 方法 |
|---|---|---|
| LSP `initialize` 结果 | 1,046 B | `--stdio`，Content-Length 帧 |
| LSP `fpgaLsp/moduleTree`（全树，默认参数） | **16,426 B** | 同上 |
| LSP `fpgaLsp/flowStatus`（无 flow） | 179 B | 同上 |
| LSP `fpgaLsp/reports{section:summary}` | 未取到（`-32800 Request cancelled`） | 见下方注 |
| MCP `initialize` 结果 | 447 B | `--mcp` |
| MCP `tools/list`（默认只读 32 工具） | **20,305 B** | 同上 |
| MCP `tools/list`（`--allow-flow --allow-write`，39 工具） | **24,069 B** | 同上 |
| MCP `fpga_project_structure`（默认） | **38,393 B**（text 21,260 / structured 14,257） | 同上 |
| MCP `fpga_project_structure{include_ports:true}` | **79,892 B**（text 45,300 / structured 27,564） | 同上 |
| MCP `fpga_project_list` / `project_info` / `flow_status` / `diagnostics` / `constraints_status` | 961 / 1,699 / 439 / 306 / 1,381 B | 同上 |
| MCP `resources/list` | 50 B（空数组） | 同上 |
| MCP 39 个 tool 中带 `outputSchema` 的个数 | **0** | 解析 `tools/list` 响应 |

> 注：`fpgaLsp/reports` 在"把 `shutdown` 紧跟在请求后"的探针里返回了 `-32800 Request cancelled` —— 这是探针自身的流水线写法（`shutdown` 到达时 `reports` 还在 worker 上，`AsyncDispatcher::Shutdown()` 置位所有 inflight 令牌，`async_dispatcher.cpp:106-118`），不是产品缺陷；但它说明了一件事：**LSP 侧 `shutdown` 不做 drain**（协议上合规：`shutdown` 之后客户端不应再发请求），如果将来要做到"优雅退出等重请求收尾"，需要额外语义。真机测量该请求应当串行等待响应。

**复现命令（PowerShell，只读探针，不构建）**：

```powershell
$tmp = Join-Path $env:TEMP 'fpgalsp-probe'; New-Item -ItemType Directory -Force $tmp | Out-Null
$root = 'Y:\fpga-lsp-cpp\tests\gate-corpus\corpus_alpha'
@(
 '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"p","version":"1"}}}',
 '{"jsonrpc":"2.0","id":2,"method":"tools/list"}',
 '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"fpga_project_structure","arguments":{"include_ports":true}}}'
) -join "`n" | Set-Content -Encoding utf8NoBOM (Join-Path $tmp 'in.jsonl')
Start-Process Y:\fpga-lsp-cpp\build\bin\fpgalsp-native.exe `
  -ArgumentList @('--mcp','--root',$root) -WorkingDirectory $tmp `
  -RedirectStandardInput (Join-Path $tmp 'in.jsonl') -RedirectStandardOutput (Join-Path $tmp 'out.jsonl') `
  -RedirectStandardError (Join-Path $tmp 'err.txt') -NoNewWindow -Wait
Get-Content (Join-Path $tmp 'out.jsonl') | ForEach-Object { $_.Length }
# 要复现 39 工具的 24,069 B：上面 -ArgumentList 追加 '--allow-flow','--allow-write'
# 要复现 LSP 侧数字：自己拼 Content-Length 帧（必须 UTF-8 无 BOM），--stdio 启动
```

**引用自仓库审计的数字**（未重测，按出处引用）：`REPORTS.md:51/66/96-102/112-115/124/141/189`（报告构建、命中、拷贝、序列化、轻请求阻塞）、`MCP.md:6/103-108/115-119/146`（工具耗时、传输开销、序列化、`FindTd`）、`INDEX.md:70-76/85-86/203`（索引增量、stamp 校验、watcher 收益）。

---

## 9. 明确不做（避免返工）

| 项 | 结论与理由 |
|---|---|
| 传输层零拷贝 / `sync_with_stdio(false)` / 每 header 两次分配 | 端到端 stdio 稳态 p50 0.999 ms，框架+传输 ≤0.15 ms（约 15%），70 KB 响应的序列化只 0.16 ms（审计 `MCP.md:103`、`:118`）→ 收益 µs 级，**不值得动**。真正的传输问题是并发（A1）与鉴权（A3）。 |
| MCP `tools/list` 分页（`cursor`） | 39 个工具 24,069 B，每会话一次。分页只会增加一次往返。**除非**未来工具数 >100 再考虑。 |
| 把 `fpgaLsp/*` 合并进 `workspace/executeCommand` | 已有 22 个命令走 `executeCommand`（`protocol.js:35-56`），其余是"请求/响应"形状的查询（`moduleTree`/`reports`/`flowStatus`），语义上是 `textDocument/*` 类查询而非命令。合并只会让客户端更难做请求合并（C7）与 `$/cancelRequest`（C6）。 |
| 在 LSP 侧新增推送通道（如 WebSocket） | 现有 stdio 推送已够用（`flowEvent`/`indexChanged`/`reportsChanged`）；问题不在通道，在**推送内容**（C1/C3/C5）。 |
| 恢复 Python 接口对拍 | 用户已明确放弃；保留反而阻塞 B1/B2/B3（见 B9）。 |
| 换二进制序列化（CBOR / MessagePack）或传输层压缩 | 两端都被规范锁死：LSP 的 body **必须是 JSON 文本**（`Content-Length` + JSON），MCP 也是 JSON-RPC 2.0；stdio 管道上加压缩需要两端同时改，且 VS Code 语言客户端不支持。真要省字节，正确做法是**减少重复与无效全量**（B1/C1/C3/D9），而不是换编码——那只会把"同一份数据发两遍"压得更小，仍然发两遍。 |
| 超大结果走"文件旁路"（写临时文件、返回路径） | 对 56 MB 级报告确实比任何编码都有效，但它把"读结果"变成"再开一个文件"，agent 侧多一次工具调用与一轮解析，临时文件的生命周期/清理/权限（跨用户可读）都是新风险。正确顺序是 **B2（分页游标）+ D3（按段裁剪）**；仅当单段仍 >800 KB 且客户端支持 `resources/read` 时，再用 MCP `resources` 暴露（协议内、非旁路，见 B5）。 |