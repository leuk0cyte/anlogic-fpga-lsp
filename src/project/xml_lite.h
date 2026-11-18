// =============================================================================
//  src/project/xml_lite.h —— 只够解析 .al 的容错 XML 解析器
// =============================================================================
//  为什么不引入 pugixml/tinyxml：任务明确要求不新增依赖，而 .al 的结构极其有限。
//
//  支持（覆盖 .al 实际用到的全部语法）：
//    * 元素、嵌套、自闭合 <X/>
//    * 属性（单/双引号）、实体引用解码
//    * 文本节点、CDATA（原样保留）
//    * 注释 <!-- -->、处理指令 <?xml ... ?>、DOCTYPE（跳过）
//
//  不支持（.al 里不出现）：
//    * 命名空间语义（标签名按原样保留，包括 "a:b" 形式）
//    * DTD 实体定义、外部实体
//
//  ⚠ 本解析器对「标签不闭合 / 属性引号不配对」等**结构性损坏**返回 ok=false，
//    由调用方走正则兜底（复现 project.py 的行为）。
//    但对于「裸 &」这类**字符级非法**，输入应当先经过 TolerantXml() 预处理。
// =============================================================================

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fpgalsp::project {

struct XmlNode {
    std::string tag;   // 标签名（去掉了 XML 声明的命名空间前缀修饰，见 .cpp）
    std::string text;  // 直接文本内容（不含子元素文本）
    std::vector<std::pair<std::string, std::string>> attributes;  // 保序
    std::vector<XmlNode> children;

    // 取属性；不存在返回 default_value
    std::string GetAttr(std::string_view name, std::string default_value = {}) const;

    bool HasAttr(std::string_view name) const;

    // 直接子元素里第一个 tag 匹配的（不递归）
    const XmlNode* FindChild(std::string_view tag_name) const;

    // 所有直接子元素
    const std::vector<XmlNode>& Children() const { return children; }
};

struct XmlParseResult {
    bool ok = false;
    XmlNode root;            // ok 时有效
    std::string error;       // ok=false 时的原因（供日志/警告使用）
    std::size_t error_offset = 0;  // 出错位置，便于定位损坏的 .al
};

// 解析 XML 文本。never-throw：任何问题都通过 result.ok / result.error 反馈。
XmlParseResult ParseXml(std::string_view xml);

// -----------------------------------------------------------------------------
// 容错预处理（逐字复现 project.py 的 _tolerant_xml）
// -----------------------------------------------------------------------------
//  Python 原文：
//      re.sub(r"&(?!(?:#\d+|#x[0-9A-Fa-f]+|[A-Za-z][A-Za-z0-9]*);)", "&amp;", text)
//
//  只转义「不属于字符引用 / 命名实体引用」的裸 '&'。
//  负向前瞻的三个分支缺一不可：
//     #\d+                     十进制字符引用  &#38;
//     #x[0-9A-Fa-f]+           十六进制字符引用 &#x26;
//     [A-Za-z][A-Za-z0-9]*     命名实体        &amp; &lt; &gt; &quot; &apos;
//  漏掉任何一个都会把合法引用二次转义成 &amp;amp; / &amp;#38;。
std::string TolerantXml(std::string_view text);

// 带命名空间前缀的标签名取本地名（"ns:Tag" -> "Tag"；"Tag" -> "Tag"）
// 对应 Python 的 _local()。
std::string LocalName(std::string_view tag);

}  // namespace fpgalsp::project