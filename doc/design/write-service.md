# 统一写服务（Write Service）：把"直接写文件"收敛到一个可审计、可策略化的入口

> 状态：**已全部实现（0.25.0）**：后端 `platform/write_service`（写文件 + 目录/删除/改名 + 内存 FS 后端 + 指标）+ 全量迁移 + CTest 门禁；插件 `src/writeService.js` + 迁移 + JS 门禁；MCP 写工具 dry-run。目标读者：后端/插件维护者。
> 触发问题：工程根反复出现 `.fpgalsp`（已修，见 `doc/status.md` 第三十五轮）——症状是
> "某处拼了路径就写"，根因是**写入没有统一入口、没有路径策略、没有机械门禁**。
> 本提案回答：能不能抽象一层"写缓存/日志/工程文件"的服务，所有写入都必须经过它。

---

## 1. 现状证据（2026-11-22 代码实测）

### 1.1 后端（`src/`）

| 类别 | 现状写法 | 数量/位置 | 问题 |
| --- | --- | --- | --- |
| 文本写文件 | `platform::write_file_utf8` | 19 处（`analysis.cpp:553`、`flow/probe.cpp` ×9、`flow/flow_runner.cpp:139/1051`、`flow/flow_logic.cpp:503`、`mcp/bridge.cpp:4334`、`server/ide_features.cpp:448`、`td/td_diagnostics.cpp:353`、`platform/encoding.cpp:73` 自身） | 只有"写 UTF-8"语义；**不校验路径落在哪、不原子、不审计** |
| 原子写（各自实现） | `.fpgalsp-tmp` + rename | `bridge.cpp:4333`、`ide_features.cpp:447-456`、`td_diagnostics.cpp:353` 各写一遍 | 三份"临时文件+重命名+回滚"逻辑，细节已开始漂移（后缀、重试、失败清理不一致） |
| 二进制/特殊写 | `std::ofstream` | `app/mcp_tcp.cpp`（端点文件）、`td/td_install.cpp`（安装缓存）、`platform/log.cpp`（日志） | 与上面两套 API 并存 |
| 日志 | `platform::log` 直接 `ofstream` | `src/platform/log.cpp` | 日志是唯一有"统一入口"的类别，其余没有 |
| 目录/删除/改名 | `create_directories` 19、`remove` 15、`rename` 5 | 分散在 flow/probe/td/server | 与写入同源问题：路径不经策略 |
| 管道/标准流 | Win32 `WriteFile` | `flow/programmer.cpp`、`td/td_session.cpp`、`app/mcp_tcp.cpp` | **不是文件写入**（子进程 stdin/stdout），不属于本提案范围 |

### 1.2 插件（`vscode-extension/`）

| 位置 | 写什么 | 问题 |
| --- | --- | --- |
| `extension.js:745` | `highlight.txt`（语义高亮调试报告） | 裸 `fs.writeFileSync`；路径靠调用点自己拼 |
| `extension.js:908` | `.vscode/mcp.json`（安装 MCP 配置） | 裸写；无备份、无原子性 |
| `src/launchCheck.js` | 扩展启动日志 | 裸写；路径约定只写在注释里 |
| `test/*.js` | 测试夹具 | 可保留（测试专用） |

结论：**后端 80% 的写已经过 `write_file_utf8`，但那一层只管编码，不管"写到哪、能不能写、写坏了怎么办"**；
插件则是完全裸写。这不是"要不要抽象"的问题，而是"再抽象一层策略"的问题。

---

## 2. 提案：一层写入服务 + 四类 root + 一条策略

### 2.1 分层（后端）

```
业务代码（flow / probe / analysis / server / mcp / td）
        │  只允许调用 write service，禁止裸 ofstream / write_file_utf8（门禁保证）
        ▼
platform::write_service            ← 本提案新增
  ├─ 路径策略：root 白名单 + 规范化 + 越界拒绝（含 .. / 绝对路径 / 大小写 / junction）
  ├─ 语义原语：AtomicWrite / AppendLog / EnsureDir / Remove / Rename
  ├─ 策略选项：dry_run / backup / allow_write / expected_encoding（UTF-8、无 BOM、保留 CRLF）
  ├─ 审计：<state>/logs/writes.jsonl（时间、root、相对路径、字节数、ok/error、调用者标签）
  └─ 指标：成功/失败/拒绝计数（`fpgalsp.metrics` 可读）
        ▼
platform::encoding（现有 read_file_utf8 / write_file_utf8）← 只作为最底层实现，业务不可直接调
```

### 2.2 四类 root（写入目标分类）

| root | 物理位置 | 谁能写 | 规则 |
| --- | --- | --- | --- |
| `kState` | `platform::state_dir(project.dir)`（`%TEMP%\fpgalsp\<名>-<hash8>`） | 后端 | cache / probe / flow / logs / analysis / endpoint；**唯一允许自由创建的根** |
| `kProject` | `.al` 所在目录 | 后端 + 插件 | **只允许显式用户文件**（`.al` / `.adc` / `.sdc` / `chip_debugger/*.mdn`），必须 `allow_write` + 备份 + 原子写 + 审计；**禁止 `.fpgalsp`、禁止其它未登记文件** |
| `kRun` | `<prj>_Runs/<run>/`（TD 生成，我们偶尔改 `flow.status`） | 后端 | 只允许"TD 自己的文件"；写之前必须已存在；审计标记 `td-owned` |
| `kTemp` | `%TEMP%\fpgalsp\<pid>-…` | 后端/插件 | 仅短期临时件；进程退出清理 |

> 关键点：**没有任何 root 允许"拼 `.fpgalsp`"**。工程目录里的 `.fpgalsp` 在策略层直接拒绝
> （错误信息带调用者标签），而不是靠 code review 记住。

### 2.3 API 草案（C++）

```cpp
namespace fpgalsp::platform {

struct WriteOutcome {
    bool ok = false;
    std::string error;      // 失败原因（含被拒绝的 root/越界原因）
    std::string path;       // 实际写入的绝对路径（dry_run 时为"将会写"的路径）
};

struct WriteOptions {
    bool dry_run = false;         // agent 的"预演"：只校验路径与内容，不落盘
    bool backup = false;          // kProject 默认 true：写前 <name>.fpgalsp-bak
    bool allow_write = false;     // kProject 必须显式 true（对应 --allow-write）
    bool keep_crlf = true;        // 文本换行策略：保留原文件 CRLF/LF
    const char* caller = nullptr; // 审计用标签，如 "probe.config"
};

class WriteService {
  public:
    WriteService(std::string project_dir, std::string state_root = {});

    // kState
    WriteOutcome WriteCache(std::string_view rel, std::string_view text, const WriteOptions& = {});
    WriteOutcome WriteProbe(std::string_view rel, std::string_view text, const WriteOptions& = {});
    WriteOutcome AppendLog(std::string_view sink, std::string_view line);   // logs/<sink>.log
    WriteOutcome WriteEndpoint(std::string_view name, std::string_view json);

    // kRun（TD-owned：写前必须存在；不允许新建目录）
    WriteOutcome WriteRunFile(std::string_view run_dir, std::string_view rel, std::string_view text,
                              const WriteOptions& = {});

    // kProject（用户文件：allow_write + backup + 原子写 + 审计）
    WriteOutcome WriteProjectFile(std::string_view rel, std::string_view text,
                                  const WriteOptions& = {});

    // 原语（业务一般不直接用）
    WriteOutcome AtomicWrite(const std::string& absolute_path, std::string_view bytes,
                             const WriteOptions& = {});
    WriteOutcome EnsureDir(std::string_view root, std::string_view rel);
    WriteOutcome Remove(std::string_view root, std::string_view rel);
};
```

**策略实现要点**
1. root 归一化：`normalize_path` + 小写比较；词法 `IsWithin` + canonical 物理复核（junction/symlink）。
2. 拒绝规则：目标不在 root 内、路径含 `.fpgalsp`、`kProject` 下非登记扩展名、`kRun` 新建目录 → `ok=false` + 明确 reason。
3. 原子写：同目录 `.<name>.tmp-<pid>-<seq>` → `FlushFileBuffers` → `rename`（失败重试 3 次，最后 `MoveFileEx(REPLACE_EXISTING)`）；失败清理 tmp。
4. 编码：UTF-8 无 BOM；`keep_crlf` 时按原文件换行风格写回；非法字节走 `sanitize_utf8`。
5. 审计：`<state>/logs/writes.jsonl`，一行一条 JSON（时间/root/rel/bytes/ok/caller）；写入本身也走同一个 AtomicWrite（避免递归：审计 sink 用追加模式 + 单独的小锁）。
6. never-throw：所有方法返回 `WriteOutcome`，异常在内部 `catch` 并转成 `ok=false`。

### 2.4 插件侧（对称设计）

新增 `vscode-extension/src/writeService.js`：

```js
// 与后端同名语义：root 校验 + 原子写 + never-throw + 可选 dryRun
const { ok, path, error } = writeService.writeStateFile('highlight.txt', text);
const { ok } = writeService.writeProjectFile(root, '.vscode/mcp.json', json, { backup: true });
```

* `stateRoot` 用现有 `src/stateDir.js` 的 `resolveStateDir(root)`（与后端 `state_dir()` 逐字节一致）。
* 只允许两类目标：state root 下的任意相对路径；**显式白名单**的工程文件（`.vscode/mcp.json`、`.al`）。
* 现有 3 处裸写（`extension.js:745/908`、`launchCheck.js`）迁过去；`test/*` 保留裸写。
* 门禁（见下）保证"以后新增的写也走 service"。

### 2.5 机械门禁（防复发，和 `.fpgalsp` 门禁同一思路）

| 门禁 | 内容 | 注册 |
| --- | --- | --- |
| `docs_no_raw_writes`（C++） | 扫 `src/` 代码里的 `std::ofstream` / `write_file_utf8(` / `std::filesystem::rename` / `create_directories`，只允许出现在 `platform/write_service.cpp`、`platform/encoding.cpp`、`platform/log.cpp`（白名单元数据说明理由） | CTest `label=docs` |
| 插件门禁（JS） | `test/run.js` 里扫 `vscode-extension/src/*.js` 的 `fs.writeFileSync` / `createWriteStream`，只允许 `src/writeService.js` | `node test/run.js` |
| 策略单测 | `tests/unit/write_service_test.cpp`：越界/`.fpgalsp`/未登记扩展名必须拒绝；dry_run 不落盘；backup 生成 `.bak`；CRLF 保留；审计行可解析 | CTest unit |

---

## 3. 迁移路线（按风险从低到高，每阶段可独立验收）

### Phase 1：服务骨架 + 状态类写入迁移（低风险，建议先做）
- 新增 `platform/write_service.{h,cpp}`（策略 + 原子写 + 审计 + dry_run）。
- 迁移 `kState` 类写入：`analysis.cpp:553`、`flow/probe.cpp` 的 cache/config、`td/td_diagnostics.cpp:353`、
  `app/mcp_tcp.cpp` 端点文件；`flow.status` 的改写走 `kRun`。
- 加 `docs_no_raw_writes` 门禁（先只负责"新增违规"）。
- **验收**：`ctest` 全绿；`probe`/`analysis` 相关单测 + 真机 CorpusAlpha flow 跑一步、probe_status 正常；
  `<state>/logs/writes.jsonl` 有记录。

### Phase 2：工程文件写入统一（中风险，含 allow-write）
- `kProject`：`flow_logic.cpp:503`（`.al` 加源文件）、`probe.cpp:462`（`user.mdn`）、
  `ide_features.cpp` 的原子写迁入；统一 `allow_write` 判定、备份、回滚、dry-run。
- 把 MCP 工具的 `--allow-write` 判定从各 handler 上移到 service（一处策略）。
- **验收**：`unit.mcp_bridge`/`unit.commands` 全绿；真机对 `.al` 做一次 add/rename 再还原；审计可追溯。

### Phase 3：插件对称迁移（低风险）
- 新增 `src/writeService.js`；迁移 3 处裸写；加 JS 门禁测试；`node test/run.js` 全绿；
  真实 VS Code 套件重跑（MCP 配置安装、highlight 报告两条链路）。

### Phase 4：增强（0.25.0 已落地）
- **内存 FS 后端**：`writes::MakeMemoryBackend()` + `SetBackendForTest/ResetBackendForTest`，单测可完全不落盘（见 `unit_write_service` 的 `MemoryBackendKeepsDiskUntouched`）；
- **写指标进 `fpgalsp.metrics`**：`result.writes = {ok,failed,denied,bytes,dirsCreated,removals,renames,backups,byRoot{state,project,run,temp}}`；
- **MCP 写工具 dry-run**：`fpga_project_add_source` / `fpga_project_rename` / `fpga_project_update` / `fpga_probe_config` 的 `dry_run:true` 预览，返回将发生的变化，不写盘、不启动 TD；
- **目录/删除/改名统一**：`writes::EnsureDir/Remove/Rename`，业务代码里的 `create_directories/remove/remove_all/rename/ensure_directory` 全部迁移（平台维护类动作如 TD 会话日志归位用 `// fpgalsp-write-exempt: <理由>` 行内豁免，门禁识别）。

**成本估算**：Phase 1 ≈ 1–1.5 人日，Phase 2 ≈ 1 人日，Phase 3 ≈ 0.5 人日；
门禁与测试已含在内。收益：路径策略、原子性、审计、dry-run 各只实现一次，
`.fpgalsp` 这类"拼路径就写"的缺陷从"靠记忆"变成"编译期/门禁期不可能"。

---

## 4. 风险与对策

| 风险 | 对策 |
| --- | --- |
| 迁移改变现有文件行为（BOM/CRLF/权限） | 每个迁移点先加 golden 测试（写前/写后字节比较）；`keep_crlf` 默认保留 |
| 原子写在 Windows 上被占用/被杀软锁 | 3 次重试 + `MoveFileEx(REPLACE_EXISTING)`；失败保留 tmp 并报错（不静默） |
| 审计文件无限增长 | 追加 + 单文件 5 MB 轮转；失败忽略（审计失败不影响主写） |
| 服务自身成为瓶颈 | 单次写是 µs 级；审计走无锁追加；不做全局锁 |
| 门禁误报 | 白名单按文件 + 理由；新增根（如 kProject 新扩展名）必须显式登记，迫使 review |

---

## 5. 明确不做

* 不做"内存数据库/虚拟文件系统"作为主存储（工程要求文件就是事实源）。
* 不接管 TD 自己的写（`_Runs/`、`log/` 由 TD 写；我们只读 + 少量 `kRun` 改写）。
* 不接管 stdout/子进程管道（LSP/MCP 协议流、TD stdin）。
* 不改报告解析/索引的**读**路径（读已统一 `read_file_utf8`，不是本轮问题）。

---

## 6. 需要决策的点

1. **范围**：只做 Phase 1（状态类写入 + 门禁），还是 Phase 1+2（含工程文件写入/allow-write 收口）？
2. **审计默认开关**：`writes.jsonl` 默认开（可观测）还是仅 `--verbose`/debug 时开（少一个文件）？
3. **插件 Phase 3** 是否本轮一起做（顺带把 `.vscode/mcp.json` 安装改成原子写+备份）？

---

## 7. 实现记录（0.24.0）

**后端**：`src/platform/write_service.{h,cpp}`
* `writes::Write(project_dir, path, bytes, {.dry_run, .backup, .caller})` 是唯一写入口；`writes::AppendLine` 供日志/追加；
  `writes::Classify` 可单测地判定 root。
* root 策略：`kState`（`state_dir`，可建目录）→ `kProject`（只允许 `*.al/*.adc/*.sdc`、`chip_debugger/*.mdn|*.cfg`，
  **父目录必须已存在**）→ `kRun`（`<prj>_Runs/**`，可建目录）→ `kTemp`（临时目录下含 `fpgalsp` 的 scratch）；
  任何 `.fpgalsp` 段、`..`、NUL 与未知根全部拒绝并带原因。
* 原子写（`.tmp-<pid>-<seq>` + rename + `MoveFileEx` 兜底 + 3 次重试）、备份到 `<state>/backup/`、
  审计 `<state>/logs/writes.jsonl`（`FPGALSP_WRITE_AUDIT=0` 关闭）、never-throw。
* 已迁移：analysis 缓存、probe（mdn/config/cwc/cache/trigger）、flow_runner（run 文件/flow.status/last_flow）、
  flow_logic/ide_features/bridge 的工程文件写、td_install 安装缓存、td_diagnostics 器件缓存、mcp_tcp 端点文件。
* 门禁：`docs_no_raw_writes`（`tools/audit/check_raw_file_writes.py`，白名单仅 write_service/encoding/log）；
  单测 `tests/unit/write_service_test.cpp`（8 条：`.fpgalsp`/白名单/越界/dry-run/备份/原子覆盖/AppendLine）。

**插件**：`vscode-extension/src/writeService.js`
* `writeStateFile` / `writeProjectFile`（白名单 `.vscode/mcp.json`、`.mcp.json`、`*.al`）/ `appendLog` / `appendText`；
  同样的 root 策略 + 原子写 + dryRun + never-throw；备份 `<file>.fpgalsp-bak`（仅插件侧工程文件，保持既有行为）。
* 已迁移：`highlight.txt`、`.vscode/mcp.json` 安装、`launchCheck` 扩展日志；门禁在 `test/run.js`（裸写只允许 `src/writeService.js`），
  新增 9 条测试（总数 58）。

**有意偏差**
1. `kProject` 不替用户在工程里新建目录（`chip_debugger/` 由 probe 代码显式 `ensure_directory` 保持旧行为）。
2. `kTemp` 判定放在 `kProject` 之后：工程目录即使位于 `%TEMP%` 下也优先按工程处理。
3. 插件的 `.mcp.json`（Portable 安装选项）额外放行，避免既有功能回归。
4. Phase 4（内存 FS、写指标、agent dry-run UI）未做。

---

## 8. 0.25.0 收尾记录

* **IO 全部收口**：后端 39 处目录/删除/改名调用迁移到 `writes::EnsureDir/Remove/Rename`；
  仅 3 处行内豁免（`td_session.cpp` 的 TD 会话日志归位 `<al>/td_*.log → <al>/log/`，理由写在
  `// fpgalsp-write-exempt:` 行内，门禁会跳过）。`chip_debugger/` 目录允许创建、其 `*.mdn|*.cfg`
  允许删除；用户 `.al/.adc/.sdc` 仍拒绝删除/改名。
* **门禁扩展**：`docs_no_raw_writes` 现在同时禁止 `write_file_utf8`/`std::ofstream`/
  `create_directories`/`remove`/`remove_all`/`rename`/`ensure_directory`；白名单仅平台实现
  （write_service / encoding / log）+ 行内豁免（必须带理由）。
* **内存 FS + 指标**：见 §3 Phase 4；`unit_write_service` 13 条；`fpgalsp.metrics.writes` 实测返回。
* **MCP dry-run**：4 个写工具实测 `dry_run:true` 返回预览且 `.al` mtime 不变（CorpusAlpha 真机）。
* 验证：`ctest` 71/71（release）、插件 58/58、真机 CorpusAlpha 单步 flow 正常、工程目录仍无 `.fpgalsp`。

---

## 9. 0.26.0：Copy 收口 + 一个高亮相关的根因修复

* **`std::filesystem::copy_file` 也统一了**：新增 `writes::Copy`（源任意 root；目标只允许
  state/run/temp 或工程内登记目录 `chip_debugger/`、`log/`）。迁移 probe 的 4 处位流复制 +
  1 处 user.cfg 备份、flow_runner 的 user.cfg 备份、strategy 的工程副本、td_session 的跨卷
  日志复制。门禁 `docs_no_raw_writes` 增加 `copy_file(`。指标新增 `writes.copies`。
* **语义高亮错位的根因（顺带修掉）**：`DocumentStore` 用 uri 字符串查"打开中的文档"，
  而客户端 uri 是 `file:///y%3A/...`（小写盘符 + 百分号编码），服务端内部
  `path_to_uri()` 产出 `file:///Y:/...` —— 查表永远 miss，`LoadText` 回落到**磁盘**，
  于是"未保存的修改"对语义高亮/格式化不可见；格式化或编辑后 token 仍按旧文本算，
  颜色错位，切走再切回（重新 didOpen/请求 full）才恢复。
  修法：`DocumentStore::GetTextByPath()` 按归一化路径查找（大小写/分隔符/编码无关），
  `LspServer::LoadText` 改用它。单测 `GetTextByPathFindsBufferForClientStyleUri` +
  真机探针（客户端风格 uri 下 2094 个 token 全部落在标识符上，0 错位）。
