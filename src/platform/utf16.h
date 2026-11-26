// =============================================================================
//  src/platform/utf16.h —— LSP 位置换算（UTF-16 码元 <-> 字节/码点索引）
// =============================================================================
//  对应参考实现：%PYREF%\fpgalsp\lsp.py
//      utf16_len(text)              -> 码元数
//      py_to_utf16(text, index)     -> 字符索引推出 UTF-16 列
//      utf16_to_py(text, col)       -> UTF-16 列 clamp 到字符索引
//
//  ⚠ 本模块的核心契约（逐字复现 Python 版语义，改动前请先看测试）
//
//  1. utf16_column_to_index 的 clamp 规则
//     Python 版：
//         if used + w > col:  return i        # 落在代理对中间 -> 返回该字符之前
//         used += w
//         if used == col:     return i + 1    # 正好落在字符边界 -> 返回下一个
//     => 列号落在 emoji（2 码元）中间时，返回**前一个字符的索引**，不是四舍五入。
//     例：text = "😀"，(col=1) -> 0（不是 1）。
//
//  2. never-raise：非法 UTF-8 不抛异常，按 U+FFFD 替换处理（与 Python 的
//     errors="replace" 一致），保证 LSP 收到乱码也不会让服务崩溃。
//
//  3. 索引单位：对外 API 一律使用 **UTF-8 字节偏移**（与 C++ std::string 一致），
//     而不是 Python 的「码点索引」。两套 API 都提供，见下。
// =============================================================================

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace fpgalsp::platform {

// -----------------------------------------------------------------------------
// UTF-8 <-> UTF-16 转换
// -----------------------------------------------------------------------------

// 把 UTF-8 转成 UTF-16 码元序列（小端语义的 uint16_t 序列）。
// 非法 UTF-8 字节按 U+FFFD 替换，**不抛异常**。
// short_input_fast_path: 对短字符串走手写扫描（SIMD 在 <64B 时反而更慢）。
std::u16string utf8_to_utf16(std::string_view utf8);

// 把 UTF-16 码元序列转成 UTF-8。孤立代理项按 U+FFFD 替换，不抛异常。
std::string utf16_to_utf8(std::u16string_view utf16);

// UTF-8 字符串是否合法（供需要区分「替换过」与「原始就含 U+FFFD」的调用方使用）。
bool is_valid_utf8(std::string_view utf8);

// -----------------------------------------------------------------------------
// 码元 / 列换算
// -----------------------------------------------------------------------------

// 等价于 Python 的 utf16_len(text)：UTF-16 码元数。
// 注意：不是字节数，也不是码点数。
std::size_t utf16_len(std::string_view text);

// 等价于 Python 的 py_to_utf16(text, index)，但 index 是 **UTF-8 字节偏移**。
// 字节偏移落在字符中间时向下取整到该字符起始处（clamp，不抛）。
// 超出范围时 clamp 到 utf16_len(text)。
std::size_t byte_index_to_utf16_column(std::string_view text, std::size_t byte_index);

// 等价于 Python 的 utf16_to_py(text, col)，返回 **UTF-8 字节偏移**。
// 逐字复现上面的 clamp 规则：
//   - col <= 0            -> 0
//   - 列落在代理对中间     -> 该字符的起始字节偏移（= 前一个字符边界）
//   - 列正好在字符边界     -> 下一字符的起始字节偏移
//   - col 超过文本长度     -> text.size()
std::size_t utf16_column_to_byte_index(std::string_view text, std::size_t column);

// 码点索引版本的 UTF-16 列换算（对应 Python 的 py_to_utf16 直接语义）。
// 便于和 Python 参考实现做逐值对比测试。
std::size_t codepoint_index_to_utf16_column(std::string_view text, std::size_t codepoint_index);

// -----------------------------------------------------------------------------
// 行/列工具（对应 lsp.py 的 line_at / make_range）
// -----------------------------------------------------------------------------

// 取第 line 行（0-based），不含行尾的 "\n"；保留行尾可能存在的 "\r"。
// 行号越界返回空串（对应 Python 的 return ""）。
std::string_view line_at(std::string_view text, std::size_t line);

// 文本的总行数（按 '\n' 切分；空串算 1 行，与 Python "".split("\n") 一致）。
std::size_t line_count(std::string_view text);

// 0-based (line, character) -> UTF-8 字节偏移；越界自动 clamp，不抛。
std::size_t position_to_byte_index(std::string_view text, std::size_t line, std::size_t character);

}  // namespace fpgalsp::platform