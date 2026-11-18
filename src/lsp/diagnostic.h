// =============================================================================
//  src/lsp/diagnostic.h —— LSP Diagnostic 基础类型
// =============================================================================
//  ⚠⚠ 这是**所有诊断来源的唯一形状**。ADC / SDC / Verible lint / HDL 索引
//      都必须产出本文件的 `Diagnostic`，不要各自发明结构。
//
//  为什么要有这个约束：
//    如果三个来源各有各的 Diagnostic 结构，最终必然要写转换层。转换层会引入
//    新的差异点（字段名、默认值、data 里放什么），而这些差异恰恰是最难与
//    Python 版逐字段对照的东西。把形状从诞生起就固定在一个地方，转换层就不存在。
//
//  ---- 字段语义 ----
//    range.start/end.line      0-based 行号（LSP 规范）
//    range.start/end.character 0-based UTF-16 列号（见下方"列号"说明）
//    severity                  1=Error 2=Warning 3=Information 4=Hint
//    code                      诊断代码字符串，如 "HDL-7144" / "LSP-PATH-SPACE"
//    source                    来源标识，固定前缀 "fpga-lsp:xxx"
//                              （"fpga-lsp:td" / "fpga-lsp:verible" / "fpga-lsp"）
//    message                   人类可读消息
//    data                      可选的结构化附加信息，无附加信息时为 null
//
//  ---- data 里允许放什么（约定，保持统一） ----
//    {"backend": "<来源内部后端名>"}      如 {"backend": "read_hdl"}
//    {"origin": "td|verible|adc|sdc"}     来源大类
//    {"tdSeverity": "CRITICAL-WARNING"}   TD 原始 severity 名（便于追溯）
//    其余键请在实现前先在此登记，避免各来源随意发明。
//
//  ---- 列号：TD 诊断恒为 0 ----
//    TD 的诊断粒度是「模块级 + 文件行号」，**没有列号**（实测确认）。
//    因此 TD 产出的 Diagnostic 的 character 恒为 0，range 是
//        {line,0} - {line,1}   （1 列宽，使编辑器能画出可见波浪线）
//    （对应 Python `_diag()` 的 end_char 默认值 = char + 1）
//    若将来要用本地 HDL 索引补出精确列号，那是"增强"而非"修正"。
// =============================================================================

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace fpgalsp::lsp {

using Json = nlohmann::json;

// LSP DiagnosticSeverity（数值与协议一致，不要改）
inline constexpr int kError = 1;
inline constexpr int kWarning = 2;
inline constexpr int kInfo = 3;
inline constexpr int kHint = 4;

// 0-based 位置。character 是 UTF-16 码元偏移（LSP 规范）。
struct Position {
    int line = 0;
    int character = 0;
};

struct Range {
    Position start;
    Position end;
};

struct Diagnostic {
    Range range;
    int severity = kError;
    std::string code;
    std::string source;
    std::string message;
    Json data = nullptr;  // 默认 null（不是空对象）

    // 便捷构造：同一行的 range（end_char 默认 start_char + 1）
    static Diagnostic AtLine(int line, int start_char, int end_char, int severity,
                             std::string code, std::string source, std::string message);

    // 便捷构造：跨行 range
    static Diagnostic Span(int start_line, int start_char, int end_line, int end_char,
                           int severity, std::string code, std::string source,
                           std::string message);

    // 设置 data 的常用键（返回自身便于链式调用）
    Diagnostic& WithData(std::string key, Json value);
    Diagnostic& WithBackend(std::string_view backend);
};

// 对应 Python 的 make_range(line, start_char, end_line=None, end_char=None)
// end_line 缺省 = line；end_char 缺省 = start_char
Range MakeRange(int line, int start_char, int end_line = -1, int end_char = -1);

// 序列化 / 反序列化（对应 LSP wire format）
Json ToJson(const Diagnostic& diagnostic);
Diagnostic FromJson(const Json& json);

// 诊断数组
using DiagnosticList = std::vector<Diagnostic>;
Json ToJson(const DiagnosticList& diagnostics);

// 对应 server.py 的 _sanitize：把任意诊断补齐成合法 LSP 形状。
// 默认值逐字复现 Python：
//   range   缺失 -> {line:0,character:0}-{line:0,character:1}
//   severity 缺失/0 -> 2
//   code     缺失 -> "LSP"
//   source   缺失 -> source_default
//   message  缺失 -> ""
//   data     为空则不输出该字段
//
// 关键：**不抛异常**。任何畸形输入都被补齐成合法形状
// （CONTRACT.md：畸形文件必须产生诊断，不能异常）。
Diagnostic Sanitize(const Diagnostic& diagnostic, std::string_view source_default = "fpga-lsp");

// 判断严重度是否合法（1..4）
bool IsValidSeverity(int severity);

}  // namespace fpgalsp::lsp