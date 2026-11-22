// =============================================================================
//  src/project/xml_lite.cpp
// =============================================================================

#include "project/xml_lite.h"

#include <cctype>
#include <string>
#include <string_view>
#include <utility>

namespace fpgalsp::project {
namespace {

bool IsNameStart(char c) {
    const auto u = static_cast<unsigned char>(c);
    return std::isalpha(u) || c == '_' || c == ':' || u >= 0x80;
}

bool IsNameChar(char c) {
    const auto u = static_cast<unsigned char>(c);
    return std::isalnum(u) || c == '_' || c == ':' || c == '-' || c == '.' || u >= 0x80;
}

bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// 去掉 XML 声明头可能留下的 BOM
std::string_view StripBom(std::string_view s) {
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB && static_cast<unsigned char>(s[2]) == 0xBF) {
        s.remove_prefix(3);
    }
    return s;
}

// 解码实体引用。返回替换后的文本。
std::string DecodeEntities(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());

    for (std::size_t i = 0; i < raw.size();) {
        if (raw[i] != '&') {
            out.push_back(raw[i++]);
            continue;
        }

        const std::size_t semi = raw.find(';', i);
        if (semi == std::string_view::npos || semi - i > 12) {
            out.push_back('&');  // 不是实体引用，原样保留
            ++i;
            continue;
        }

        const std::string_view body = raw.substr(i + 1, semi - i - 1);

        if (!body.empty() && body[0] == '#') {
            bool ok = false;
            char32_t cp = 0;
            if (body.size() > 1 && (body[1] == 'x' || body[1] == 'X')) {
                // 十六进制
                for (std::size_t k = 2; k < body.size(); ++k) {
                    const char c = body[k];
                    int d = -1;
                    if (c >= '0' && c <= '9') d = c - '0';
                    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                    if (d < 0) { ok = false; break; }
                    cp = cp * 16 + static_cast<char32_t>(d);
                    ok = true;
                }
            } else {
                for (std::size_t k = 1; k < body.size(); ++k) {
                    const char c = body[k];
                    if (c < '0' || c > '9') { ok = false; break; }
                    cp = cp * 10 + static_cast<char32_t>(c - '0');
                    ok = true;
                }
            }

            if (ok && cp <= 0x10FFFF && (cp < 0xD800 || cp > 0xDFFF)) {
                // 编码成 UTF-8
                if (cp < 0x80) {
                    out.push_back(static_cast<char>(cp));
                } else if (cp < 0x800) {
                    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                } else if (cp < 0x10000) {
                    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                } else {
                    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
                i = semi + 1;
                continue;
            }
            out.push_back('&');
            ++i;
            continue;
        }

        // 命名实体
        if (body == "amp") { out.push_back('&'); i = semi + 1; continue; }
        if (body == "lt") { out.push_back('<'); i = semi + 1; continue; }
        if (body == "gt") { out.push_back('>'); i = semi + 1; continue; }
        if (body == "quot") { out.push_back('"'); i = semi + 1; continue; }
        if (body == "apos") { out.push_back('\''); i = semi + 1; continue; }

        out.push_back('&');  // 未知实体：原样保留而不是报错
        ++i;
    }
    return out;
}

class Parser {
public:
    explicit Parser(std::string_view xml) : s_(StripBom(xml)) {}

    XmlParseResult Run() {
        XmlParseResult result;

        // B7 / S-05：体积极限。放在最前面，避免对明显不是 .al 的输入做任何扫描
        // （ParseAl 侧还会在**读文件之前**用 file_size 挡一次，见 al_project.cpp）。
        if (s_.size() > kMaxXmlDocumentBytes) {
            result.error = "document too large (" + std::to_string(s_.size()) + " bytes > " +
                           std::to_string(kMaxXmlDocumentBytes) + ")";
            result.error_offset = 0;
            return result;
        }

        // 跳过根元素之前的杂项（声明 / 注释 / DOCTYPE / 空白）
        if (!SkipMisc(&result.error, &result.error_offset)) {
            return result;
        }

        if (i_ >= s_.size() || s_[i_] != '<') {
            result.error = "no root element";
            result.error_offset = i_;
            return result;
        }

        if (!ParseElement(result.root, &result.error, &result.error_offset)) {
            return result;
        }

        // 根元素之后允许注释/空白
        if (!SkipMisc(&result.error, &result.error_offset)) {
            return result;
        }

        result.ok = true;
        return result;
    }

private:
    // 跳过空白 / 注释 / PI / DOCTYPE
    bool SkipMisc(std::string* error, std::size_t* error_offset) {
        while (i_ < s_.size()) {
            if (IsSpace(s_[i_])) {
                ++i_;
                continue;
            }
            if (s_.compare(i_, 4, "<!--") == 0) {
                const std::size_t end = s_.find("-->", i_ + 4);
                if (end == std::string_view::npos) {
                    *error = "unterminated comment";
                    *error_offset = i_;
                    return false;
                }
                i_ = end + 3;
                continue;
            }
            if (s_.compare(i_, 2, "<?") == 0) {
                const std::size_t end = s_.find("?>", i_ + 2);
                if (end == std::string_view::npos) {
                    *error = "unterminated processing instruction";
                    *error_offset = i_;
                    return false;
                }
                i_ = end + 2;
                continue;
            }
            if (s_.compare(i_, 9, "<!DOCTYPE") == 0) {
                const std::size_t end = s_.find('>', i_);
                if (end == std::string_view::npos) {
                    *error = "unterminated DOCTYPE";
                    *error_offset = i_;
                    return false;
                }
                i_ = end + 1;
                continue;
            }
            break;
        }
        return true;
    }

    std::string ReadName() {
        const std::size_t start = i_;
        if (i_ < s_.size() && IsNameStart(s_[i_])) {
            ++i_;
            while (i_ < s_.size() && IsNameChar(s_[i_])) ++i_;
        }
        return std::string(s_.substr(start, i_ - start));
    }

    void SkipSpaces() {
        while (i_ < s_.size() && IsSpace(s_[i_])) ++i_;
    }

    // 解析开始标签的属性直到 '>' 或 "/>"
    // 返回 false 表示结构损坏
    bool ParseAttributes(XmlNode& node, bool* self_closing, std::string* error,
                         std::size_t* error_offset) {
        *self_closing = false;
        while (true) {
            SkipSpaces();
            if (i_ >= s_.size()) {
                *error = "unexpected end of input in start tag";
                *error_offset = i_;
                return false;
            }
            if (s_[i_] == '>') {
                ++i_;
                return true;
            }
            if (s_[i_] == '/' && i_ + 1 < s_.size() && s_[i_ + 1] == '>') {
                *self_closing = true;
                i_ += 2;
                return true;
            }

            const std::string attr_name = ReadName();
            if (attr_name.empty()) {
                *error = "invalid attribute name";
                *error_offset = i_;
                return false;
            }
            SkipSpaces();
            if (i_ >= s_.size() || s_[i_] != '=') {
                *error = "expected '=' after attribute name";
                *error_offset = i_;
                return false;
            }
            ++i_;
            SkipSpaces();
            if (i_ >= s_.size() || (s_[i_] != '"' && s_[i_] != '\'')) {
                *error = "attribute value must be quoted";
                *error_offset = i_;
                return false;
            }
            const char quote = s_[i_];
            ++i_;
            const std::size_t value_start = i_;
            const std::size_t value_end = s_.find(quote, value_start);
            if (value_end == std::string_view::npos) {
                *error = "unterminated attribute value";
                *error_offset = i_;
                return false;
            }
            node.attributes.emplace_back(attr_name,
                                         DecodeEntities(s_.substr(value_start, value_end - value_start)));
            i_ = value_end + 1;
        }
    }

    bool ParseElement(XmlNode& node, std::string* error, std::size_t* error_offset) {
        // B7 / S-05：深度上限。这里必须在**递归之前**判断，否则第 513 层已经进了栈。
        // 超限返回结构化错误（ok=false + 原因），绝不崩溃。
        if (depth_ >= kMaxXmlDepth) {
            *error = "element nesting too deep (limit " + std::to_string(kMaxXmlDepth) + ")";
            *error_offset = i_;
            return false;
        }
        struct DepthGuard {
            int& depth;
            explicit DepthGuard(int& d) : depth(d) { ++depth; }
            // A-2：持有的是 depth_ 的引用，拷贝/移动会让两个 guard 各减一次 depth，
            // 让深度计数变成负数（后续元素就绕过了 512 层上限）。因此一律禁止。
            DepthGuard(const DepthGuard&) = delete;
            DepthGuard& operator=(const DepthGuard&) = delete;
            DepthGuard(DepthGuard&&) = delete;
            DepthGuard& operator=(DepthGuard&&) = delete;
            ~DepthGuard() { --depth; }
        } depth_guard(depth_);

        if (i_ >= s_.size() || s_[i_] != '<') {
            *error = "expected '<'";
            *error_offset = i_;
            return false;
        }
        ++i_;  // 跳过 '<'

        node.tag = ReadName();
        if (node.tag.empty()) {
            *error = "invalid element name";
            *error_offset = i_;
            return false;
        }

        bool self_closing = false;
        if (!ParseAttributes(node, &self_closing, error, error_offset)) {
            return false;
        }
        if (self_closing) {
            return true;
        }

        // 解析内容直到 </tag>
        while (true) {
            if (i_ >= s_.size()) {
                *error = "unexpected end of input, missing </" + node.tag + ">";
                *error_offset = i_;
                return false;
            }

            if (s_[i_] != '<') {
                // 文本节点：一直取到下一个 '<'
                const std::size_t next = s_.find('<', i_);
                const std::size_t end = (next == std::string_view::npos) ? s_.size() : next;
                node.text += DecodeEntities(s_.substr(i_, end - i_));
                i_ = end;
                continue;
            }

            // CDATA
            if (s_.compare(i_, 9, "<![CDATA[") == 0) {
                const std::size_t end = s_.find("]]>", i_ + 9);
                if (end == std::string_view::npos) {
                    *error = "unterminated CDATA";
                    *error_offset = i_;
                    return false;
                }
                node.text += std::string(s_.substr(i_ + 9, end - i_ - 9));
                i_ = end + 3;
                continue;
            }

            // 注释
            if (s_.compare(i_, 4, "<!--") == 0) {
                const std::size_t end = s_.find("-->", i_ + 4);
                if (end == std::string_view::npos) {
                    *error = "unterminated comment";
                    *error_offset = i_;
                    return false;
                }
                i_ = end + 3;
                continue;
            }

            // 处理指令
            if (s_.compare(i_, 2, "<?") == 0) {
                const std::size_t end = s_.find("?>", i_ + 2);
                if (end == std::string_view::npos) {
                    *error = "unterminated processing instruction";
                    *error_offset = i_;
                    return false;
                }
                i_ = end + 2;
                continue;
            }

            // 结束标签
            if (s_.compare(i_, 2, "</") == 0) {
                i_ += 2;
                const std::string close_name = ReadName();
                SkipSpaces();
                if (i_ >= s_.size() || s_[i_] != '>') {
                    *error = "malformed closing tag";
                    *error_offset = i_;
                    return false;
                }
                ++i_;
                if (close_name != node.tag) {
                    *error = "mismatched closing tag: expected </" + node.tag + "> got </" +
                             close_name + ">";
                    *error_offset = i_;
                    return false;
                }
                return true;
            }

            // 子元素
            node.children.emplace_back();
            if (!ParseElement(node.children.back(), error, error_offset)) {
                return false;
            }
        }
    }

    std::string_view s_;
    std::size_t i_ = 0;
    int depth_ = 0;  // B7 / S-05：当前递归深度
};

}  // namespace

// -----------------------------------------------------------------------------
// XmlNode
// -----------------------------------------------------------------------------

std::string XmlNode::GetAttr(std::string_view name, std::string default_value) const {
    for (const auto& [key, value] : attributes) {
        if (key == name) return value;
    }
    return default_value;
}

bool XmlNode::HasAttr(std::string_view name) const {
    for (const auto& [key, value] : attributes) {
        if (key == name) return true;
    }
    return false;
}

const XmlNode* XmlNode::FindChild(std::string_view tag_name) const {
    for (const auto& child : children) {
        if (child.tag == tag_name) return &child;
    }
    return nullptr;
}

// -----------------------------------------------------------------------------
// 对外接口
// -----------------------------------------------------------------------------

XmlParseResult ParseXml(std::string_view xml) {
    XmlParseResult result;
    try {
        Parser parser(xml);
        result = parser.Run();
    } catch (...) {
        // never-throw：解析器内部的任何意外都转成 ok=false
        result.ok = false;
        result.error = "internal parser error";
    }
    return result;
}

std::string TolerantXml(std::string_view text) {
    // 逐字复现：
    //   re.sub(r"&(?!(?:#\d+|#x[0-9A-Fa-f]+|[A-Za-z][A-Za-z0-9]*);)", "&amp;", text)
    //
    // 对每个 '&'，检查紧随其后是否构成下面三种引用之一（必须以 ';' 收尾）：
    //   1) #\d+                    十进制字符引用
    //   2) #x[0-9A-Fa-f]+          十六进制字符引用（注意：只认小写 x）
    //   3) [A-Za-z][A-Za-z0-9]*    命名实体
    // 只有三者都不匹配时才替换成 "&amp;"。
    std::string out;
    out.reserve(text.size() + text.size() / 16);

    const std::size_t n = text.size();
    for (std::size_t i = 0; i < n; ++i) {
        const char c = text[i];
        if (c != '&') {
            out.push_back(c);
            continue;
        }

        bool is_reference = false;
        std::size_t k = i + 1;

        if (k < n && text[k] == '#') {
            ++k;
            if (k < n && text[k] == 'x') {
                // 2) 十六进制：#x[0-9A-Fa-f]+
                // 注意只认小写 'x'（Python 正则写的是 #x，不忽略大小写）
                ++k;
                const std::size_t digits_start = k;
                while (k < n && ((text[k] >= '0' && text[k] <= '9') ||
                                 (text[k] >= 'A' && text[k] <= 'F') ||
                                 (text[k] >= 'a' && text[k] <= 'f'))) {
                    ++k;
                }
                is_reference = (k > digits_start) && k < n && text[k] == ';';
            } else {
                // 1) 十进制：#\d+（\d 在 Python 字符串里默认只匹配 ASCII 0-9）
                const std::size_t digits_start = k;
                while (k < n && text[k] >= '0' && text[k] <= '9') ++k;
                is_reference = (k > digits_start) && k < n && text[k] == ';';
            }
        } else if (k < n && ((text[k] >= 'A' && text[k] <= 'Z') ||
                             (text[k] >= 'a' && text[k] <= 'z'))) {
            // 3) 命名实体：首字符是字母，后续是字母或数字
            ++k;
            while (k < n && ((text[k] >= 'A' && text[k] <= 'Z') ||
                             (text[k] >= 'a' && text[k] <= 'z') ||
                             (text[k] >= '0' && text[k] <= '9'))) {
                ++k;
            }
            is_reference = k < n && text[k] == ';';
        }

        if (is_reference) {
            out.push_back('&');
        } else {
            out += "&amp;";
        }
    }
    return out;
}

std::string LocalName(std::string_view tag) {
    const std::size_t colon = tag.rfind(':');
    if (colon == std::string_view::npos) return std::string(tag);
    return std::string(tag.substr(colon + 1));
}

}  // namespace fpgalsp::project