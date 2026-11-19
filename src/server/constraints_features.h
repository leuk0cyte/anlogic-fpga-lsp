// =============================================================================
//  src/server/constraints_features.h —— .adc / .sdc 的 LSP 符号类入口
// =============================================================================
//  为什么单独一个头：
//    `LspServer` 只有 `FeatureConstraintSymbols(path)` 一个约束符号入口（B1 固定的
//    签名，B10 不许改 lsp_server.h），但 textDocument/{documentSymbol,hover,completion}
//    三个方法都要分派到约束文档。这里用**自由函数**补齐缺口，实现全部落在
//    `features_constraints.cpp`（协议 JSON 的唯一出口），hdl_handlers.cpp 只做
//    「按扩展名分派 + 传文本/端口表」。
//
//  分层（与 B-PLAN §1 一致）：
//      src/constraints/{adc,sdc}.cpp        纯函数：文本 -> 文档符号 JSON / hover / 补全
//      src/server/features_constraints.cpp  这里：扩展名判定 + 文本装载后的唯一出口
//      src/server/hdl_handlers.cpp          只分派，不含语义
// =============================================================================

#pragma once

#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "constraints/port.h"

namespace fpgalsp::server {

using Json = nlohmann::json;

// 路径是否为约束文档（`.adc` / `.sdc`，大小写不敏感 —— 对应 Python 的
// `lower = doc.path.lower()`）。
bool IsConstraintDocument(std::string_view path);

// 以下三个函数的 `text` 是**已经装载好的**文档文本（缓冲区优先，见 LoadText）。
// 都不抛异常；`path` 不是约束文档时返回 Python 的"该方法不支持"形态
// （documentSymbol/补全 -> 空数组/空 items，hover -> null）。
Json ConstraintDocumentSymbols(std::string_view path, std::string_view text);
Json ConstraintHover(std::string_view path, std::string_view text, int line, int character,
                     const std::vector<constraints::Port>& ports);
Json ConstraintCompletion(std::string_view path, std::string_view text, int line, int character,
                          const std::vector<constraints::Port>& ports);

}  // namespace fpgalsp::server