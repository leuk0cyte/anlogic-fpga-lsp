// =============================================================================
//  src/platform/version.h —— 原生服务端对外自报的版本号（单一来源）
// =============================================================================
//  为什么需要这一处：`--help` 首行、LSP `initialize.serverInfo.version`、MCP
//  `initialize.serverInfo.version` 三处都要报同一个号，以前是三个字面量（而且
//  停留在 0.1.0，与扩展 package.json 的 0.2x 系列对不上，用户报 bug 时对不上号）。
//
//  发布纪律：与 `vscode-extension/package.json` 的 `version` 同步递增；改这里时
//  同时改 CMakeLists 的 `project(... VERSION ...)` 与扩展的 package.json。
//  `tools/audit/check_versions.py` 会核对这三处一致（见 doc/quality.md）。
// =============================================================================

#pragma once

namespace fpgalsp::platform {

inline constexpr const char* kServerVersion = "0.26.0";

}  // namespace fpgalsp::platform
