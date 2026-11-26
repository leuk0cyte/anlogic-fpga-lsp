# tools/perf/vscode —— VS Code 插件侧性能探针

配套报告：`audit/2026-11-19/perf/VSCODE.md`。全部脚本**只读** `vscode-extension/`，
数据落在 `%TEMP%\fpgalsp-perf\vscode\`，结果 JSON 落在 `results/`。

## 一键复跑

```powershell
pwsh -File tools/perf/vscode/run_all.ps1      # 约 4 分钟，重写 results/*.json
```

需要：Node（v22+ 即可，实测 v26.13.1）、Python（仅 `bench_lsp_roundtrip.py` 用）、
生产二进制 `build\bin\fpgalsp-native.exe`（只读运行，**不会**被覆盖）。

## 脚本

| 脚本 | 作用 | 依赖 |
| --- | --- | --- |
| `prep_workspace.js` | 把 `tests/gate-corpus/corpus_alpha` 复制到 `%TEMP%\fpgalsp-perf\vscode\project`（插件会往工作区写 `.fpgalsp/logs/`，**绝不能**指向 `C:/work/CorpusAlpha`） | — |
| `gen_payloads.js` | 生成合成 `fpgaLsp/reports` 载荷：~27 MB（含 raw）、8.4 MB（去 raw）、559 B（summary）+ manifest | — |
| `bench_reports.js` | `JSON.parse` / `buildReportView` / 每节 `renderReportSection` / 整页 `renderReportsHtml` 的 ms 与内存；并检查 summary 载荷渲染出的是空表 | `--expose-gc` |
| `bench_events.js` | 事件路径：`applyIndexChanged`（就地/忽略/刷新）、树重建+渲染（36/1000 节点）、`reportsPanel.refresh/_patch`、`reportsTree`/`flowTree`/`FlowController.pollOnce` | `--expose-gc` |
| `bench_activate.js` | 真 `extension.js` 的 `activate()` 成本、订阅数、启动请求；`save`/`active-editor`/`indexChanged`/`open` 到实际请求的墙钟延迟；同步 `appendLog` 在 C:/Y: 两种盘的单价 | `--expose-gc` |
| `bench_keystroke.js` | 每次按键路径：`veribleDiagnostics.didChange` 同步段、400 ms flush、`publishDiagnostics` 在 1/10/50 条 tab 诊断下的主线程阻塞（默认 200 KB，`--kb` 可调） | `--expose-gc` |
| `audit_static.js` | 静态审计：`setInterval`/`setTimeout`/事件订阅/`JSON.parse`/同步 fs/`fire(undefined)` 的 file:line 清单 + 通知注册计数 | — |
| `bench_lsp_roundtrip.py` | 真实 exe 的 `moduleTree` / `reports{section}` / `debugHighlight` / `info` 往返（server+IPC）与响应字节数 | `tests/golden/tools/goldenlib` |
| `lib/harness.js` | 统计（3 轮 × 21 样本、p50/p95）、整机 CPU 占用差分、`require('vscode')` mock 加载、结果写盘 | — |

## 约定（与 audit/2026-11-19/perf/README.md 一致）

- 每条基准 **3 轮 × ≥21 样本**，报 p50/p95；脚本输出保留 `csv,` 行便于汇总。
- 每个脚本记录运行期间的整机 CPU 占用（`os.cpus()` 差分）；>20% 的轮次在报告里标注"可能偏高"。
- `--expose-gc` 只用于内存数字（`JSON.parse` 的 heap 增量），否则数字是上界。
- 合成载荷在报告里标 `合成载荷`（本机没有任何工程有 flow 产物，26 MB 报告无法真实生成）。
- 派生列：`keystroke-<kb>kb.json` 按文件大小分开存，避免后者覆盖前者。