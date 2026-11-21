// =============================================================================
//  src/mcp/guide.cpp —— fpga_guide 的 7 段正文
// =============================================================================
//  读者假设：一个完全没有上下文的 agent。所以每段都写成
//  "先做什么 -> 调哪个工具 -> 凭什么判定成功/失败"，而不是概念介绍。
//
//  实测来源（都在这台机器上踩过）：
//    * 2026-11 corpus_delta：KIT-2501 / KIT-1004 / KIT-8414 / PRJ-8806；
//    * flow 成功但 phy_1/cw/compiled.cwc 不存在（mark_debug 为空）；
//    * 改了 user.mdn 只跑 route，probe_status 报 stale；
//    * 真实工程自带 HDL-7225 CRITICAL-WARNING，不是失败。
// =============================================================================

#include "mcp/guide.h"

#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace fpgalsp::mcp {
namespace {

using TopicText = std::pair<std::string_view, std::string_view>;

constexpr std::string_view kOverview = R"MD(# overview

MCP server for Anlogic TangDynasty (TD) FPGA projects. Everything below is a
tool call, not prose; run the steps in order and pass `project` on every call
when the workspace holds more than one `.al`.

## Before anything else
1. `fpga_td_info` - is a TD installation detected at all.
2. `fpga_project_list` - the `.al` projects under the MCP root.
3. `fpga_project_info {"project":"<.al|dir|name>"}` - device, top module, TD
   summary, `warnings`, `runs`.

## Three routes
* Read/navigate: `fpga_project_files` -> `fpga_document_symbols`,
  `fpga_search_text`, `fpga_symbol_path`, `fpga_diagnostics` (topic: index).
* Build: `fpga_flow_run` -> `fpga_flow_wait` -> `fpga_project_reports`
  (topic: flow).
* On-board: `fpga_probe_config` -> rerun phy -> `fpga_probe_status` ->
  `fpga_probe_capture` (topics: probe, program).

## Standing rules
* Positions are 1-based; paths outside the MCP root are rejected.
* Read-only tools always work. Write tools need `--allow-write`, flow tools
  need `--allow-flow`. A gated tool is *absent* from `tools/list`, so
  "unknown tool" usually means "server started without the flag".
* MCP startup is ~0.1 s (TD discovery is a 40 ms directory scan, and its result
  is cached under %TEMP%/fpgalsp). phy takes 5-15 min. Keep a long run inside one
  session; exiting the process kills the TD children. Always close stdin after
  sending the `exit` notification, otherwise the server waits for EOF and your
  client looks like it hung for its whole timeout.
* Read `troubleshoot` before calling anything a failure: some TD `*-WARNING`
  lines are normal, and `ok:true` does not imply the artifact you want exists.
)MD";

constexpr std::string_view kProject = R"MD(# project

Discover and audit a project before touching the flow.

## Sequence
1. `fpga_project_list {}` - every `.al` under the root plus the selected one.
2. `fpga_project_info {"project":...}` - device, package, speed, top, `files`
   counts, `excluded`, `warnings`, `runs`.
3. `fpga_project_files {"kind":"hdl","include_excluded":true}` - **the build
   file set**. Files on disk are not necessarily compiled.
4. `fpga_device_info {"include_pins":true,"pin_filter":"..."}` - device and pin
   capability. May start a TD session; cached per process.
5. `fpga_constraints_status {}` then `fpga_constraint_audit
   {"include_device":true}` - ADC coverage, pin conflicts, SDC clocks.

## Preconditions
* A readable `.al` with `<Device>` and a top module. A missing device fails the
  flow at start with `PRJ-8001` - fix the `.al`, not the flow.
* Pin checks need the device database: the first such call takes ~30 s.

## Failure modes
* Several projects and no `project` argument: calls resolve to the *active*
  project, which may not be the one you mean. Pass `project` explicitly.
* `fpga_project_files` without `include_excluded:true` hides the excluded and
  missing files. "Module not found" almost always lives there.
* `kind":"all"` on a large project is big: filter by kind and use `limit`.
* `fpga_project_info` warning list is per-project trivia, not flow-blocking;
  compare it with `fpga_flow_issues` before blaming the toolchain.
)MD";

constexpr std::string_view kIndex = R"MD(# index

Symbols, search and diagnostics. All read-only; no TD session, no hardware.

## Sequence
1. `fpga_project_files {"kind":"hdl"}` - only build files are indexed.
2. `fpga_document_symbols {"file":"rtl/top.v"}` - module/port/signal tree.
3. `fpga_workspace_symbols {"query":"pwm","limit":200}` or
   `fpga_search_hierarchy {"query":"...","kind":"module"}` - find a name.
4. Position tools (`fpga_definition`, `fpga_references`, `fpga_hover`,
   `fpga_symbol_path`, `fpga_call_hierarchy`): pass `file` plus 1-based `line`
   and `column` pointing *at* the identifier.
5. `fpga_search_text {"query":"assign","regex":false,"glob":"*.v","limit":100}`
   - raw text when symbols are not enough.
6. `fpga_module_info {"module":"..."}`, `fpga_duplicate_modules {}`,
   `fpga_project_structure {"include_ports":true,"max_depth":8}` - structure.
7. `fpga_diagnostics {"sources":["hdl","adc","sdc"],"limit":2000}` - lint.

## Preconditions
* Files are read from disk: save first. MCP has no unsaved buffer and never
  receives `didOpen`/`didChange`.
* The index refreshes by (path, mtime, size); a just-written file is picked up,
  but a stale editor buffer is not.

## Failure modes
* Empty symbol tree or "not found": the file is not in the build set (see
  `fpga_project_files`), or the symbol sits in an excluded file.
* `duplicate_modules` non-empty: the same module is defined in two build files
  and TD silently picks one. Fix it before running the flow.
* Diagnostics tiers are independent: `hdl` never sees TD-level errors, and the
  `td` tier needs a previous flow run. Empty output is a result, not a failure.
* `column` past the end of the line yields no symbol, not an error.
)MD";

constexpr std::string_view kFlow = R"MD(# flow

Synthesis / place & route / bitstream, driven by TD.

## Sequence
1. `fpga_flow_run {"stages":["all"],"jobs":6}` - returns `runId` (alias `job`)
   and `async:true` immediately. Stages: `syn`, `phy` (P&R + bitstream),
   `route` (incremental route on an existing phy), `all` (syn+phy, the safe
   default).
2. `fpga_flow_wait {"timeout":3600,"interval":1}` - blocks, emits progress
   notifications, returns `flow` and `issues`. **Same MCP session**: the flow
   thread lives in this process, so exiting it kills the TD children and the
   next session sees `ok:true` with no flow.
3. `fpga_project_reports {"stage":"all","section":"summary","limit":200}` -
   QoR/timing/area/flow; use `section":"qor"`/`"timing"` for detail.
4. On failure: `fpga_flow_issues {"run_id":"flow_xxxx"}` then
   `fpga_flow_log {"run_id":...,"kind":"both","tail":400}`.
5. `fpga_job_status {"job":"flow_xxxx"}` / `fpga_job_cancel {"job":...}` -
   poll or cancel without blocking.
6. `fpga_run_diff {}` - QoR delta between the last two runs.

## Preconditions
* `--allow-flow` (or `fpgaLsp.mcp.allowFlow`), a TD install, a valid device and
  top module, and no other run active for that project.
* Artifacts: `<project>_Runs/syn_1`, `phy_1` (`.bit`, `cw/compiled.cwc`,
  reports). `fpga_project_reports` reads them from disk.

## Failure modes
* `ok:true` only means the *requested stages* finished. It is not a timing
  sign-off, DRC-clean proof, or evidence the probes are in the bit.
* RTL changed but only `["phy"]` or `["route"]` was run: the old netlist is
  reused. Always rerun `syn` after an RTL edit - `["all"]` is the reliable one.
* `PRJ-8806` / `PRJ-5303` are tolerated only when every expected artifact
  exists; any other `*-ERROR` fails the run.
* `flow_wait` returning `timedOut:true` leaves the run going: wait again or
  cancel, do not start a second flow.
)MD";

constexpr std::string_view kProbe = R"MD(# probe

On-chip logic capture (Anlogic ChipWatcher).

## Sequence
1. `fpga_probe_status {}` - what `chip_debugger/user.mdn` marks versus what the
   compiled watcher contains: `ok`, `ready`, `disabled`, and everything about the
   compiled cwc under `watcher` (`watcher.present`, `watcher.clock`,
   `watcher.sample_depth`, `watcher.net_count`, `watcher.stale`,
   `watcher.missing_in_bit`, `watcher.extra_in_bit`).
2. `fpga_probe_config {"signals":["WHG/pwm_reg0_i[15:0]"],"write":false}` - dry
   run listing `added`/`removed`, plus `bus_count`/`bit_count`/`budget`. Then
   `"write":true` to rewrite user.mdn. Ranges are expanded to per-bit nets.
   **No artificial cap.** Every device has its own ChipWatcher budget (on
   PH1A100SFG676 4 buses / 64 bits is what worked; 5 buses dropped the extra one
   and a very long list ended in `empty ChipWatcher configuration`). The tool
   reports the counts and lets TD raise the error at the phy stage, because the
   GUI can mark any signal and disabled entries do not consume debug resources.
   If TD errors, mark fewer signals and rerun `["all"]`.
3. `fpga_flow_run {"stages":["all"]}` + `fpga_flow_wait {}` - **`["all"]`, not
   `["phy"]`**: TD instantiates the ChipWatcher (clock/depth/debug hub/OOC
   synthesis) right AFTER *Optimize Gate*, i.e. in the syn run. phy-only reuses
   the old gate netlist and keeps the old (or no) watcher. Measured: with the
   gate step skipped, phy reports `KIT-2501 : No mark_debug exists in the
   netlist` and the bit ends up without probes.
4. `fpga_probe_verify {}` - per-bit answer: `signals{requested,embedded,missing}`,
   `buses[].{status,reason}`, `missing_nets`, `hints`. Use it to prove the
   probes really landed (`no-bitstream` = run the flow first;
   `dropped-by-td` = cross-domain or optimized away).
5. `fpga_probe_status {}` again - require `ready:true`, `watcher.present:true`,
   `watcher.stale:false`, `watcher.missing_in_bit:[]`, `watcher.net_count` > 0.
6. `fpga_probe_capture {"download":"auto","confirm":true,"trigger_now":true}` -
   captures over JTAG; returns per-sample values, `trigger_index`, CSV path.
   Measured: 17.4 s when the bitstream is already loaded, plus the SRAM load
   when `download` reloads it.

## Disabling entries (keeps them listed, no debug resources)
TD's ChipWatcher list can enable/disable an entry; a disabled entry is kept in the
list and in the `cwc` but consumes no debug resource (TD's own `cwc` carries
`is_enabled="no"` on the bus and `data="disable" trig="disable"` on its nets).
`chip_debugger/user.mdn` has only `mark_debug {...}` - no disable syntax - so the
backend can only express it through the `cwc` it hands to `insert_debugger -cwc`:
1. `fpga_probe_config {"signals":[...],"disabled":["WHG/e2e_hstat"],"write":true,
   "apply_build_config":true}` - `disabled` takes a bus base (`WHG/e2e_hstat`, all
   of its bits) or a single bit (`WHG/e2e_hstat[2]`); ranges are expanded. It is
   persisted as `disabled=...` in `.fpgalsp/probe/config.txt`. Check
   `enabled_bus_count` / `enabled_bit_count` (disabled bits excluded) and
   `unresolved_disabled` / `warning` for entries that are not in `signals`
   (a warning, not a failure).
2. `fpga_flow_run {"stages":["all"]}` + `fpga_flow_wait {}` - the watcher is
   rebuilt from the authored `cwc`; without `apply_build_config:true` the entries
   stay enabled in the bitstream.
3. `fpga_probe_status {}` - `disabled` / `disabled_count` show the request and
   `watcher.disabled_bus_count` / `disabled_net_count` what the compiled `cwc`
   actually marks; disabled nets do not take part in the bit order, the DIN count
   or `<logic_eram width>`.
Disabled entries are still written into the XML (the GUI and read-back expect
them), so `fpga_probe_verify` still counts them as embedded - use
`fpga_probe_status.disabled` for the enable/disable split.

## Preconditions
* `phy_1/cw/compiled.cwc` **and** `phy_1/cw/compiled.bit` exist (a probe build
  was done). A batch (`launch_runs`) phy run does **not** write `compiled.cwc`
  even when the bit really contains the watcher (only the TD GUI ChipWatcher
  saves it). If the file is missing but `syn_1/cw/auto_created_cwc*/` exists,
  the probes ARE embedded: copy any cwc that matches the current `user.mdn`
  (e.g. an earlier capture's `.fpgalsp/probe/trigger_*.cwc`) to that path, or
  save once from the TD GUI.
* A board on a cable, and no other hardware operation (programming or another
  capture) - the cable is leased per process, a conflict returns `busy`.
* `download:"auto"|"true"` needs `confirm:true` because it reprograms SRAM;
  `download:"false"` only reads the bitstream already loaded.

## Limits (compile-time; MCP cannot change them)
* Sample clock and sample depth come from the ChipWatcher build. TD offers no
  runtime command for them: mark a different clock/depth and rerun phy.
* Runtime-adjustable, applied by patching a copy of `compiled.cwc` at capture
  time: `trigger_position`, `window_num`, `window_depth`, `trigger_type`,
  `trigger_enabled`, `bus_op`. The bit still matches, so no rebuild is needed.

## Failure modes
* `watcher.stale:true` or non-empty `watcher.missing_in_bit`: user.mdn changed
  but phy was not rerun (an incremental route does not re-insert the debugger).
  Capture refuses unless `allow_stale:true` - and then you read the OLD probes.
* A successful flow is not evidence the probes reached the bit. Only
  `fpga_probe_status` is.
* Values are binary bit strings per bus, not integers; widths come from the
  `[msb:lsb]` header of each column.
)MD";

constexpr std::string_view kProgram = R"MD(# program

Programming the device. Read this before trying to "download the bitstream".

## What MCP has
* `fpga_cables {}` - read-only probe listing cables and online devices. It
  never programs anything.
* `fpga_probe_capture {"download":"true","confirm":true}` - the only MCP call
  that writes the device: it reloads `phy_1/cw/compiled.bit` into SRAM so the
  probes are live. It is part of a capture, not a programming API.
* There is **no** standalone program/flash tool in MCP. Do not look for one.

## Where programming actually lives
* The LSP side: command `fpgalsp.program` with `{mode:"sram"|"flash",
  confirm:true}`. In the editor it is the program command (extension
  `fpgaLsp.programSram` / `fpgaLsp.programFlash`, shown as `FPGA: Program SRAM
  (JTAG)`; also referred to as `FPGALSP: Program Device`).
* It refuses without an explicit `confirm:true`, with no cable detected, or on
  a device mismatch (`allowMismatch:true` does not bypass the device check).
* From an agent, either run that command through the editor or drive TD's own
  `download` in a TD session. Never fake it with `fpga_probe_capture`.

## Before programming
1. `fpga_project_reports {"section":"files"}` - the `.bit` exists and is newer
   than the last RTL edit.
2. `fpga_cables {}` - the board is visible. An empty list means cable, driver or
   power, not a broken bitstream.
3. Flash needs the right mode and a longer timeout; SRAM is lost on power-off.

## Failure modes
* phy finished but there is no `.bit`: the flow stopped after route. Rerun
  `stages:["all"]`.
* Programming succeeds and behaviour does not change: the device may still run
  an older image, or the flow reused an old netlist. Rerun `syn` first.
)MD";

constexpr std::string_view kTroubleshoot = R"MD(# troubleshoot

Known TD/MCP failure shapes and how to tell them apart.

## `KIT-2501` - no mark_debug
TD found no debugger marking, so `insert_debugger` inserts nothing. Symptom:
phy succeeds, `fpga_probe_status` says `present:false`, and no
`phy_1/cw/compiled.cwc` appears. Cause: `chip_debugger/user.mdn` is missing or
its `mark_debug {...}` is empty. Fix: `fpga_probe_config {"signals":[...],
"write":true}`, then rerun phy.

## `KIT-1004` - chipwatcher configuration imported
Success marker for `config_chipwatcher <compiled.cwc>`. The bare command
*returns success and does nothing* (no KIT-1004 output at all), so a missing
marker means no `.cwc` was imported, not a silent success. MCP always passes the
file; if capture reads nonsense, suspect the `.cwc`, not the cable.

## `KIT-8414` - watcher instance not found
`get_hw_cws` needs the explicit instance name (`auto_created_cwc1`); without it
it fails with KIT-8414. The handle it returns is an object: later commands must
use the handle, not the name string, otherwise `KIT-8000`.

## `PRJ-8806` / `PRJ-5303` - tolerated, only with artifacts
A run is accepted when **every** `ERROR` line is one of these AND the expected
artifacts exist; the log then says "completed despite TD PRJ-8806". Any other
`*-ERROR`, or a missing artifact, still fails.

## flow `ok:true` but no `compiled.cwc`
`ok` only reports the requested stages. If nothing was marked, or phy was
incremental, `phy_1/cw/` has no `compiled.cwc` / `compiled.bit`. Confirm with
`fpga_probe_status`, then rerun phy (not route).

## `probe_status` fields
Top level is `ok`, `ready`, `reason`, `chip_debugger`, `disabled`, `sampling`;
everything about the compiled watcher is under `watcher`:
* `watcher.present:false` - no readable `compiled.cwc`: probes were never
  compiled in.
* `watcher.stale:true` - user.mdn differs from the watcher in the bit; the bit
  predates the list, so a capture would read the old probes.
* `watcher.missing_in_bit:[...]` - signals you marked that the bit does not
  watch; `watcher.extra_in_bit:[...]` - signals in the bit that user.mdn no
  longer lists.
* `watcher.net_count` / `watcher.disabled_bus_count` / `disabled_net_count` -
  what the compiled cwc actually carries.
* `ready:false` plus `reason` - read it; it names the exact fix.

## Warnings that are NOT failures
`HDL-7225 CRITICAL-WARNING: 'x' is already implicitly declared` and similar
project-own `CRITICAL-WARNING` lines are normal in real RTL. Since TD 2026.2_SP1
they do not interrupt a flow, and MCP only fails on `ERROR`. Do not "fix" them
and do not report them as the cause of a failed build.
)MD";

// 顺序与 tools.cpp 里 fpga_guide 的 enum 一致（overview 是第一项 = 回落值）
constexpr std::array<TopicText, 7> kGuides{{
    {"overview", kOverview},
    {"project", kProject},
    {"index", kIndex},
    {"flow", kFlow},
    {"probe", kProbe},
    {"program", kProgram},
    {"troubleshoot", kTroubleshoot},
}};

}  // namespace

std::string_view ResolveGuideTopic(std::string_view topic) {
    for (const auto& [name, text] : kGuides) {
        (void)text;
        if (topic == name) return name;
    }
    // 未知 / 空：回落到 overview（绝不返回空正文 —— 那会让 agent 以为工具坏了）
    return kGuides.front().first;
}

std::string GuideText(std::string_view topic) {
    const std::string_view resolved = ResolveGuideTopic(topic);
    for (const auto& [name, text] : kGuides) {
        if (resolved == name) return std::string(text);
    }
    return std::string(kOverview);
}

}  // namespace fpgalsp::mcp