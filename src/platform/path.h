// =============================================================================
//  src/platform/path.h —— 路径规范化与 URI 互转
// =============================================================================
//  对应参考实现：
//    %PYREF%\fpgalsp\project.py :: norm()
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
// 物理包含判定（B7 / S-04）
// -----------------------------------------------------------------------------
//  normalize_path 只做**词法**折叠：不 resolve 符号链接、不碰磁盘（见 .cpp 顶部
//  的设计说明）。因此 "工程内一个指向 C:\\Windows 的 junction" 会通过纯词法的
//  IsWithin —— 这不是理论问题，实测可读出 hosts 文件。
//
//  canonical_path() 用 std::filesystem::weakly_canonical 做**物理**解析
//  （展开 symlink/junction、消解 . / ..、规范大小写与短名），失败时退化为
//  normalize_path 的结果（never-throw）。
//
//  path_is_within_canonical(path, root) 在物理路径上做"大小写不敏感 + 分隔符
//  边界"的包含判定。它与 bridge 里的 IsWithin 词法版本互补：
//  **词法先行（便宜、无 I/O），物理复核（贵、但防符号链接）**。
std::string canonical_path(std::string_view path);
bool path_is_within_canonical(std::string_view path, std::string_view root);

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

// -----------------------------------------------------------------------------
// 状态目录（Wave 3 / W1）：`.fpgalsp` 可以外移到系统临时目录
// -----------------------------------------------------------------------------
//  用户在 2026-11-19 要求"插件的状态文件不要落在工作目录"。**C++ 侧只认环境变量**，
//  目录由调用方（插件）算好并通过 `FPGALSP_STATE_DIR` 传下来 —— 这样两侧不会各算一套。
//
//  解析顺序（唯一事实源）：
//    1) 环境变量 `FPGALSP_STATE_DIR` 非空  -> 用它（normalize_path 之后返回）
//    2) 否则                              -> `<project_or_base>/.fpgalsp`
//
//  ⚠ 第 2 条必须与 Wave 3 之前**逐字节相同**：CLI/MCP 直连、单元测试、Python 对拍
//     都依赖 `<project>/.fpgalsp/...` 这个语义。所以未设环境变量时本函数只是
//     `path_join(base, ".fpgalsp")`，不做别的加工。
//
//  子目录约定（调用方用 path_join(state_dir(dir), "<sub>") 拼）：
//    <state>/logs   <state>/run   <state>/cache   <state>/flow   <state>/analysis
//
//  ⚠ 本函数**不建目录**：很多调用点只"读缓存/试读日志"，不存在就当未命中。
//     提前 create_directories 会让从没跑过 flow 的工程凭空多出目录，破坏 e2e 的
//     "语料里不出现 .fpgalsp" 断言。建目录由真正要写的调用点负责。
std::string state_dir(std::string_view project_or_base);

// 当前是否由 `FPGALSP_STATE_DIR` 决定（仅供日志/诊断/测试断言用）。
bool state_dir_from_env();

}  // namespace fpgalsp::platform