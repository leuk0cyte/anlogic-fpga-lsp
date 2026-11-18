// =============================================================================
//  src/platform/encoding.h —— 文件读写（UTF-8，never-raise）
// =============================================================================
//  对应参考实现：
//    Y:\fpga-lsp\fpgalsp\reports.py 读外部文件时用 encoding="utf-8",
//    errors="replace" —— 非法字节替换成 U+FFFD 而不是抛异常。
//
//  额外约定：
//    * 读：自动剥离 UTF-8 BOM（否则第一行第一个字符会变成 U+FEFF，位置全部错位）
//    * 写：UTF-8 **无 BOM**，换行不做转换（不把 \n 改成 \r\n）
// =============================================================================

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace fpgalsp::platform {

struct ReadFileResult {
    bool ok = false;
    std::string content;       // UTF-8，已剥离 BOM
    bool had_invalid_bytes = false;  // 是否发生过 U+FFFD 替换
    bool had_bom = false;
};

// 读整个文件为 UTF-8 字符串。
// 非法 UTF-8 字节按 U+FFFD 替换（不抛、不失败）。
// 只有「文件打不开」才 ok=false。
ReadFileResult read_file_utf8(std::string_view path);

// 便捷版：失败返回 nullopt。
std::optional<std::string> try_read_file_utf8(std::string_view path);

// 写 UTF-8 文本到文件（无 BOM）。成功返回 true；失败返回 false，不抛。
bool write_file_utf8(std::string_view path, std::string_view content);

// 确保目录存在（递归创建）。已存在返回 true。
bool ensure_directory(std::string_view path);

// 把字节序列强制转成合法 UTF-8：非法字节替换为 U+FFFD。
std::string sanitize_utf8(std::string_view bytes);

// 去掉开头的 UTF-8 BOM（EF BB BF）。
std::string_view strip_utf8_bom(std::string_view bytes);

// 该字节序列是否带 UTF-8 BOM。
bool has_utf8_bom(std::string_view bytes);

}  // namespace fpgalsp::platform