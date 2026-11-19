// =============================================================================
//  src/platform/path.cpp
// =============================================================================
//  实现策略：
//   * 自己实现路径折叠（不依赖 std::filesystem 的改写行为），保证与 Python
//     os.path.abspath + normpath 的结果逐字一致，且不访问磁盘（不 resolve 符号
//     链接、不做真实存在性检查），避免在 LSP 里产生意外 I/O。
//   * never-raise：所有函数不抛异常。
// =============================================================================

#include "platform/path.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <cctype>
#include <string>
#include <vector>

#ifdef _MSC_VER
#include <direct.h>
#else
#include <unistd.h>
#endif

namespace fpgalsp::platform {
namespace {

char ascii_lower(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

char ascii_upper(char c) {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
}

bool is_sep(char c) { return c == '/' || c == '\\'; }

// 盘符大写化：形如 "y:/..." -> "Y:/..."
std::string uppercase_drive(std::string s) {
    if (s.size() >= 2 && std::isalpha(static_cast<unsigned char>(s[0])) && s[1] == ':') {
        s[0] = ascii_upper(s[0]);
    }
    return s;
}

// 把分隔符统一成 '/'，并折叠 '.' / '..'，去掉末尾 '/'（根除外）。
std::string collapse(std::string s) {
    // 1) 反斜杠 -> 正斜杠
    for (char& c : s) {
        if (c == '\\') c = '/';
    }

    // 2) UNC 前缀 "//server/share"：保留双斜杠
    bool unc = false;
    std::string prefix;
    if (s.size() >= 2 && s[0] == '/' && s[1] == '/') {
        unc = true;
    }
    // 3) 盘符前缀
    std::string drive;
    std::size_t pos = 0;
    if (s.size() >= 2 && std::isalpha(static_cast<unsigned char>(s[0])) && s[1] == ':') {
        drive = s.substr(0, 2);
        pos = 2;
    } else if (unc) {
        pos = 2;
    }

    const bool rooted = (pos < s.size() && s[pos] == '/');

    std::vector<std::string> parts;
    std::string cur;
    for (std::size_t i = pos; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '/') {
            if (!cur.empty()) {
                parts.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) parts.push_back(cur);

    // 折叠 "." 与 ".."
    std::vector<std::string> out;
    for (const auto& p : parts) {
        if (p == ".") {
            continue;
        }
        if (p == "..") {
            if (!out.empty() && out.back() != "..") {
                out.pop_back();
            } else if (!rooted) {
                out.push_back(p);  // 相对路径里保留前导 ".."
            }
            // 绝对路径的 ".." 在根之上直接丢弃
            continue;
        }
        out.push_back(p);
    }

    std::string result;
    if (!drive.empty()) {
        result += drive;
        result += '/';
    } else if (unc) {
        result += "//";
    } else if (rooted) {
        result += '/';
    }

    if (drive.empty() && !unc && !rooted) {
        // 相对路径：直接拼
        for (std::size_t i = 0; i < out.size(); ++i) {
            if (i) result += '/';
            result += out[i];
        }
        if (result.empty()) result = ".";
        return result;
    }

    for (std::size_t i = 0; i < out.size(); ++i) {
        if (i) result += '/';
        result += out[i];
    }
    return result;
}

// 是否为 drive-root 形式（"Y:/"），这种末尾斜杠要保留。
bool is_drive_root(const std::string& s) {
    return s.size() == 3 && std::isalpha(static_cast<unsigned char>(s[0])) && s[1] == ':' &&
           s[2] == '/';
}

std::string get_cwd() {
    // 取当前工作目录；失败返回 "."（never-throw）
#ifdef _MSC_VER
    std::array<char, 4096> buf{};
    if (::_getcwd(buf.data(), static_cast<int>(buf.size())) != nullptr) {
        return std::string(buf.data());
    }
#else
    std::array<char, 4096> buf{};
    if (::getcwd(buf.data(), buf.size()) != nullptr) {
        return std::string(buf.data());
    }
#endif
    return ".";
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string percent_encode(std::string_view in) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
        // unreserved 字符不需要编码（RFC 3986 2.3）
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
                                c == '~';
        // 路径分隔符与盘符冒号保持可读（LSP 客户端普遍接受，VS Code 也这么发）
        const bool keep = unreserved || c == '/' || c == ':';
        if (keep) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0x0F]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

std::string percent_decode(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            const int hi = hex_value(in[i + 1]);
            const int lo = hex_value(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(in[i]);
    }
    return out;
}

}  // namespace

// -----------------------------------------------------------------------------

std::string normalize_path(std::string_view path) {
    if (path.empty()) return {};  // 对应 Python: if not path: return path

    std::string s(path);

    // Unix 风格绝对路径在 Windows 下仍然按原样处理
    if (!is_absolute_path(s)) {
        s = get_cwd() + "/" + s;
    }

    s = collapse(s);
    s = uppercase_drive(s);

    // 去掉末尾分隔符（根除外）
    while (s.size() > 1 && s.back() == '/' && !is_drive_root(s) && s != "/" && s != "//") {
        s.pop_back();
    }
    return s;
}

bool path_equals_ignore_case(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    }
    return true;
}

bool path_less_ignore_case(std::string_view a, std::string_view b) {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const char ca = ascii_lower(a[i]);
        const char cb = ascii_lower(b[i]);
        if (ca != cb) return ca < cb;
    }
    return a.size() < b.size();
}

std::string path_key(std::string_view path) {
    std::string k(path);
    for (char& c : k) c = ascii_lower(c);
    return k;
}

bool is_absolute_path(std::string_view path) {
    if (path.empty()) return false;
    // 盘符绝对路径 "Y:/" 或 "Y:\"
    if (path.size() >= 3 && std::isalpha(static_cast<unsigned char>(path[0])) &&
        path[1] == ':' && is_sep(path[2])) {
        return true;
    }
    // UNC "//server/share" 或 "\\\\server\\share"
    if (path.size() >= 2 && is_sep(path[0]) && is_sep(path[1])) return true;
    // Unix 绝对路径 "/..."
    if (is_sep(path[0])) return true;
    return false;
}

std::string path_dirname(std::string_view path) {
    if (path.empty()) return {};
    std::string s(path);
    // 去掉末尾分隔符
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) {
        if (s.size() == 3 && s[1] == ':') break;  // "Y:/"
        s.pop_back();
    }
    const std::size_t pos = s.find_last_of("/\\");
    if (pos == std::string::npos) return {};
    if (pos == 0) return s.substr(0, 1);  // 根
    // "Y:/a" -> "Y:/"
    if (pos == 2 && s[1] == ':') return s.substr(0, 3);
    return s.substr(0, pos);
}

std::string path_filename(std::string_view path) {
    const std::size_t pos = path.find_last_of("/\\");
    if (pos == std::string_view::npos) return std::string(path);
    return std::string(path.substr(pos + 1));
}

std::string path_extension(std::string_view path) {
    const std::string name = path_filename(path);
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return {};
    return name.substr(dot);
}

std::string path_join(std::string_view a, std::string_view b) {
    if (a.empty()) return normalize_path(b);
    if (b.empty()) return normalize_path(a);
    if (is_absolute_path(b)) return normalize_path(b);
    return normalize_path(std::string(a) + "/" + std::string(b));
}

// -----------------------------------------------------------------------------
// 物理包含判定（B7 / S-04）
// -----------------------------------------------------------------------------
//  为什么需要：normalize_path 是**纯词法**的（本文件顶部 5-7 行的设计声明），
//  因此 "工程目录内放一个指向 C:\Windows 的 junction" 在词法上完全合法，
//  bridge 的 IsWithin 会放行。实测（doc/audit/_db/probe_mcp_root.py 用例 A3）
//  fpga_read_file 能读到 C:\Windows\System32\drivers\etc\hosts。
//
//  weakly_canonical 与 canonical 的区别：前者只要求"最长的存在前缀"能被解析，
//  尾部不存在的成分原样保留 —— 正好适合"文件可能还不存在（新建/写盘）"的场景。

namespace {

std::filesystem::path to_fs_path(std::string_view utf8) {
    // Windows 上 std::filesystem::path 的窄字符构造按 ACP 解释，必须显式走
    // UTF-8 -> char8_t 的路径，否则非 ASCII 用户名下的工程会被解析成乱码。
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()),
                                               utf8.size()));
}

}  // namespace

std::string canonical_path(std::string_view path) {
    const std::string normalized = normalize_path(path);
    if (normalized.empty()) return {};
    try {
        std::error_code ec;
        const std::filesystem::path resolved =
            std::filesystem::weakly_canonical(to_fs_path(normalized), ec);
        if (ec || resolved.empty()) return normalized;  // never-throw：退化为词法结果
        const std::u8string utf8 = resolved.u8string();
        std::string out(reinterpret_cast<const char*>(utf8.data()), utf8.size());
        for (char& c : out) {
            if (c == '\\') c = '/';
        }
        // weakly_canonical 保留末尾分隔符的情况极少，仍按 normalize 规则收一下
        while (out.size() > 1 && out.back() == '/' && !is_drive_root(out) && out != "/" &&
               out != "//") {
            out.pop_back();
        }
        return out;
    } catch (...) {
        return normalized;
    }
}

bool path_is_within_canonical(std::string_view path, std::string_view root) {
    const std::string target = path_key(canonical_path(path));
    const std::string base = path_key(canonical_path(root));
    if (base.empty()) return false;
    if (target == base) return true;
    if (target.size() <= base.size()) return false;
    if (target.compare(0, base.size(), base) != 0) return false;
    const char separator = target[base.size()];
    return separator == '/' || separator == '\\';
}

// -----------------------------------------------------------------------------
// URI
// -----------------------------------------------------------------------------

std::string path_to_uri(std::string_view path) {
    if (path.empty()) return {};
    std::string norm = normalize_path(path);
    // normalize_path 产出正斜杠绝对路径，例如 "Y:/proj/a b.sv"
    std::string encoded = percent_encode(norm);

    // "Y:/x" -> "file:///Y:/x"（三个斜杠：scheme 后面的空 authority + 根）
    if (encoded.size() >= 2 && encoded[1] == ':' && std::isalpha(static_cast<unsigned char>(encoded[0]))) {
        return "file:///" + encoded;
    }
    // UNC: "//server/share" -> "file://server/share"
    if (encoded.size() >= 2 && encoded[0] == '/' && encoded[1] == '/') {
        return "file:" + encoded;
    }
    // Unix 绝对路径 "/x" -> "file:///x"
    if (!encoded.empty() && encoded[0] == '/') {
        return "file://" + encoded;
    }
    return "file:///" + encoded;
}

std::string uri_to_path(std::string_view uri) {
    if (!uri_is_file(uri)) return {};

    std::string_view rest = uri.substr(5);  // 跳过 "file:"

    // 处理 "file:///Y:/..." 与 "file://server/share"
    std::string authority;
    if (rest.size() >= 2 && rest[0] == '/' && rest[1] == '/') {
        rest.remove_prefix(2);
        // authority 到下一个 '/' 为止
        const std::size_t slash = rest.find('/');
        if (slash == std::string_view::npos) {
            authority = std::string(rest);
            rest = {};
        } else {
            authority = std::string(rest.substr(0, slash));
            rest = rest.substr(slash);
        }
        // 常见的 "file:///Y:/..." 会因为去掉 "//" 后再见到一个 '/'，authority 为空
    }

    std::string decoded = percent_decode(rest);
    // 去掉 URI 里可能带上的查询/片段
    const std::size_t q = decoded.find_first_of("?#");
    if (q != std::string::npos) decoded.resize(q);

    if (!authority.empty()) {
        // "file://server/share/x" -> "//server/share/x"
        std::string unc = "//" + authority + decoded;
        return normalize_path(unc);
    }

    // "file:///Y:/x" -> decoded 是 "/Y:/x"，去掉前导斜杠得到 "Y:/x"
    if (decoded.size() >= 3 && decoded[0] == '/' &&
        std::isalpha(static_cast<unsigned char>(decoded[1])) && decoded[2] == ':') {
        decoded.erase(0, 1);
    } else if (decoded.size() >= 4 && decoded[0] == '/' && decoded[1] == '/' &&
               std::isalpha(static_cast<unsigned char>(decoded[2])) && decoded[3] == ':') {
        // "file:////server/share" 这种四斜杠形式
        decoded.erase(0, 2);
    }

    if (decoded.empty()) return {};
    return normalize_path(decoded);
}

bool uri_is_file(std::string_view uri) {
    if (uri.size() < 5) return false;
    return ascii_lower(uri[0]) == 'f' && ascii_lower(uri[1]) == 'i' && ascii_lower(uri[2]) == 'l' &&
           ascii_lower(uri[3]) == 'e' && uri[4] == ':';
}

// -----------------------------------------------------------------------------
//  状态目录（Wave 3 / W1）：实现见 path.h 的说明
// -----------------------------------------------------------------------------
namespace {

// 读 `FPGALSP_STATE_DIR`；未设置/空串返回 false。Windows 上走 _dupenv_s 避免 C4996。
bool ReadStateDirEnv(std::string* out) {
#if defined(_WIN32)
    char* buffer = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&buffer, &size, "FPGALSP_STATE_DIR") != 0 || buffer == nullptr) return false;
    std::string value(buffer);
    std::free(buffer);
    if (value.empty()) return false;
    *out = std::move(value);
    return true;
#else
    const char* value = std::getenv("FPGALSP_STATE_DIR");
    if (value == nullptr || *value == '\0') return false;
    *out = value;
    return true;
#endif
}

}  // namespace

std::string state_dir(std::string_view project_or_base) {
    std::string from_env;
    if (ReadStateDirEnv(&from_env)) return normalize_path(from_env);
    // 未设置环境变量：必须与 Wave 3 之前逐字节一致（CLI/MCP 直连、单测、Python 对拍都依赖它）。
    return path_join(project_or_base, ".fpgalsp");
}

bool state_dir_from_env() {
    std::string from_env;
    return ReadStateDirEnv(&from_env);
}

}  // namespace fpgalsp::platform