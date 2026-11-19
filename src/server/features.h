// =============================================================================
//  src/server/features.h —— 符号类查询的**唯一实现**（LSP handler 与 MCP 工具共用）
// =============================================================================
//  为什么要有这一层：
//    * LSP handler（textDocument/documentSymbol 等）需要它
//    * MCP 工具（task-18 阶段二 A）也需要同样的东西
//    如果两边各写一份"索引 -> JSON"的转换，**迟早漂移**（字段名/边界条件/kind 编号）。
//    所以把纯转换函数抽到这里，两边只负责"怎么拿到 index"（缓冲区优先 vs 磁盘）。
//
//  分层：
//    src/hdl    —— 词法/结构解析 + 索引（不认识 LSP 也不认识 JSON 契约）
//    features.h —— index -> LSP JSON 形状（**本层**；不持有任何状态）
//    lsp_server —— LSP 传输/线程/缓存；handler 只是 3 行包装
//
//  约定：
//    * 全部输入不可信：path/行列越界都返回空结果，不抛异常
//    * 返回的 JSON 已经是协议要求的最终形状（handler 不再加工）
// =============================================================================

#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "hdl/hdl_index.h"
#include "project/al_project.h"

namespace fpgalsp::server::features {

using Json = nlohmann::json;

// textDocument/documentSymbol
//   模块(kind 2) + 端口(8) + 参数(14) + 信号(13)
Json DocumentSymbols(const hdl::HdlIndex& index, std::string_view path);

// workspace/symbol（query 为空 = 全部；大小写不敏感子串）
Json WorkspaceSymbols(const hdl::HdlIndex& index, std::string_view query);

// textDocument/definition（返回 Location 或 null）
Json Definition(const hdl::HdlIndex& index, std::string_view path, int line, int character);

// textDocument/hover（返回 Hover 或 null）
Json Hover(const hdl::HdlIndex& index, std::string_view path, int line, int character);

// textDocument/references
Json References(const hdl::HdlIndex& index, std::string_view path, int line, int character,
                bool include_declaration);

// textDocument/inlayHint（例化名后 `: 模块名`）
Json InlayHints(const hdl::HdlIndex& index, std::string_view path);

// textDocument/completion
//   text 是**当前文档内容**（缓冲区优先），用于判断"光标是否在行首"与缩进/实例名避让。
Json Completion(const hdl::HdlIndex& index, std::string_view path, std::string_view text,
                int line, int character);

// fpgaLsp/moduleChildren（P-C2）：只回 parent 的直接子节点（带 hasChildren，不带 children），
// offset/limit 分页。parent 为空时用 project.top。
Json ModuleChildren(const hdl::HdlIndex& index, const project::Project& project,
                    std::string_view module, int offset, int limit, bool include_primitives);

// fpgaLsp/moduleTree（root=project.top；IP/VHDL 在索引里 -> 非黑盒）
Json ModuleTree(const hdl::HdlIndex& index, const project::Project& project,
                bool include_primitives);

}  // namespace fpgalsp::server::features