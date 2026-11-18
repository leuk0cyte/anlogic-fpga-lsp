// =============================================================================
//  src/platform/path.h —— 路径规范化与 URI 互转
// =============================================================================
//  对应参考实现：
//    Y:\fpga-lsp\fpgalsp\project.py :: norm()
//        "Absolute path, forward slashes, no trailing separator."
//    LSP 的 file URI（file:///Y:/path/to/file.sv，需百分号编码）
//
//  Windows 语义要点：
//   * 分隔符统一正斜杠，但**保留原始大小写**用于显示（大小写不敏感只用于比较）
//   * 盘符统一大写（Y: 而不是 y:），避免同一文件被当成两个文档
//   * 末尾分隔符去除，但盘符根（"Y:/"）保留
// =============================================================================

#pragma once

#include <string>
#include <string_view>

namespace fpgalsp::platform {

// 复现 project.py 的 norm()：
//   * 转绝对路径（相对路径相对于当前工作目录）
//   * 反斜杠 -> 正斜杠
//   * 折叠重复分隔符、消解 "." 与 ".."
//   * 去掉末尾分隔符（盘符根 "Y:/" 等根路径除外）
//   * 盘符大写
//   * 空串原样返回空串（对应 Python 的 `if not path: return path`）
// 全程 never-raise；无法解析的输入尽量原样返回而不抛。
std::string normalize_path(std::string_view path);

// 大小写不敏感比较（Windows 语义）。用于判断两个路径是否指向同一文件。
bool path_equals_ignore_case(std::string_view a, std::string_view b);

// 大小写不敏感的字典序比较（用于有序容器排序时保证稳定）。
bool path_less_ignore_case(std::string_view a, std::string_view b);

// 把已规范化的路径转成小写键（用于 std::map/unordered_map 的索引键）。
std::string path_key(std::string_view path);

// 判断是否绝对路径（含盘符绝对路径与 UNC 路径）。
bool is_absolute_path(std::string_view path);

// 取目录部分（不含末尾分隔符）；无目录返回空串。
std::string path_dirname(std::string_view path);

// 取文件名（含扩展名）。
std::string path_filename(std::string_view path);

// 取扩展名（含点，例如 ".sv"）；无扩展名返回空串。
std::string path_extension(std::string_view path);

// 拼接路径：(a) 之后接上相对路径 (b)，结果已规范化。
std::string path_join(std::string_view a, std::string_view b);

// -----------------------------------------------------------------------------
// URI <-> 路径
// -----------------------------------------------------------------------------

// 路径 -> file URI。
//   "Y:/proj/a b.sv"  ->  "file:///Y:/proj/a%20b.sv"
// 按 RFC 3986 对非 unreserved 字符做百分号编码；盘符规范化成大写。
std::string path_to_uri(std::string_view path);

// file URI -> 路径（已规范化）。
//   "file:///Y:/proj/a%20b.sv"  ->  "Y:/proj/a b.sv"
// 对非 file scheme、畸形输入返回空串（never-raise）。
std::string uri_to_path(std::string_view uri);

// URI 是否以 "file:" 开头（大小写不敏感）。
bool uri_is_file(std::string_view uri);

}  // namespace fpgalsp::platform