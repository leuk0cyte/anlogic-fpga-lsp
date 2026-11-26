// =============================================================================
//  src/server/features_range_format.h —— 区间格式化（B4）
// =============================================================================
//  为什么单独一个头文件：`FeatureRangeFormatting` 的实现与 call hierarchy 放在
//  同一个 TU（features_call_hierarchy.cpp）里，避免为 B4 新增 .cpp 再动 CMake
//  （CMake 归 Lead）。这里只声明**纯函数**，方便以后拆成独立 TU 或直接单测。
//
//  对应参考实现：%PYREF%\fpgalsp\server.py:1646-1667（on_textDocument_rangeFormatting）
//    * 只对 .v/.sv/.vh/.svh 生效，其它扩展名返回 []
//    * 先用 Verible 格式化**全文**；行数与原文不同的场合**回落到全文档格式化**
//      （Python 的 if len(old_rows) != len(new_rows): return on_textDocument_formatting(params)）
//    * 行数一致时，只对 [start.line, end.line] 里"内容变了且不是空行"的行出单行编辑
// =============================================================================

#pragma once

#include <string>
#include <string_view>

#include "jsonrpc/json_util.h"  // nlohmann::json

namespace fpgalsp::server {

// 把 Verible 的全文格式化结果折成 rangeFormatting 的编辑数组。
//   text        原文（UTF-8，保留原行尾）
//   formatted   格式化后的全文
//   first_line / last_line  请求的行区间（0-based，闭区间）
// 返回 LSP TextEdit 数组；无改动返回空数组。
nlohmann::json RangeFormattingEdits(std::string_view text, std::string_view formatted,
                                    int first_line, int last_line);

}  // namespace fpgalsp::server