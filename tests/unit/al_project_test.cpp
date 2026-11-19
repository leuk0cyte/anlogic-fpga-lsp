// =============================================================================
//  tests/unit/al_project_test.cpp
// =============================================================================
//  重点：
//   1. _tolerant_xml 的负向前瞻三个分支（裸 & / 十进制 / 十六进制 / 命名实体）
//   2. 结构损坏时走正则兜底且不抛异常
//   3. 真实语料 tests/gate-corpus/corpus_alpha/demo_project.al（含裸 &）
// =============================================================================

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "project/al_project.h"
#include "project/xml_lite.h"

namespace {

using namespace fpgalsp::project;

// -----------------------------------------------------------------------------
// TolerantXml：逐字复现 project.py 的 _tolerant_xml
// -----------------------------------------------------------------------------

TEST(TolerantXml, EscapesBareAmpersand) {
    EXPECT_EQ(TolerantXml("UsedInP&R"), "UsedInP&amp;R");
    EXPECT_EQ(TolerantXml("a & b"), "a &amp; b");
    EXPECT_EQ(TolerantXml("&"), "&amp;");
}

TEST(TolerantXml, PreservesNamedEntities) {
    // 三个分支之一：命名实体不能被二次转义
    EXPECT_EQ(TolerantXml("&amp;"), "&amp;");
    EXPECT_EQ(TolerantXml("&lt;"), "&lt;");
    EXPECT_EQ(TolerantXml("&gt;"), "&gt;");
    EXPECT_EQ(TolerantXml("&quot;"), "&quot;");
    EXPECT_EQ(TolerantXml("&apos;"), "&apos;");
    // 自定义命名实体形态也保留（Python 正则不校验实体名是否存在）
    EXPECT_EQ(TolerantXml("&foo;"), "&foo;");
    EXPECT_EQ(TolerantXml("&a1;"), "&a1;");
}

TEST(TolerantXml, PreservesDecimalCharRef) {
    // 分支之二
    EXPECT_EQ(TolerantXml("&#38;"), "&#38;");
    EXPECT_EQ(TolerantXml("&#x26;"), "&#x26;");
    EXPECT_EQ(TolerantXml("&#0;"), "&#0;");
    EXPECT_EQ(TolerantXml("&#123456;"), "&#123456;");
}

TEST(TolerantXml, PreservesHexCharRef) {
    // 分支之三
    EXPECT_EQ(TolerantXml("&#xAB;"), "&#xAB;");
    EXPECT_EQ(TolerantXml("&#xFF;"), "&#xFF;");
    EXPECT_EQ(TolerantXml("&#xdeadbeef;"), "&#xdeadbeef;");
}

TEST(TolerantXml, MixedContent) {
    // 同时含裸 &、命名实体、字符引用
    const std::string in = "UsedInP&R &amp; &#38; &#x26; & &lt;";
    const std::string out = TolerantXml(in);
    EXPECT_EQ(out, "UsedInP&amp;R &amp; &#38; &#x26; &amp; &lt;");
}

TEST(TolerantXml, AmpersandAtEnd) {
    EXPECT_EQ(TolerantXml("abc&"), "abc&amp;");
}

TEST(TolerantXml, NoAmpersandUnchanged) {
    EXPECT_EQ(TolerantXml("plain text"), "plain text");
    EXPECT_EQ(TolerantXml(""), "");
}

TEST(TolerantXml, SemicolonWithoutAmpersand) {
    // 分号本身不是转义对象
    EXPECT_EQ(TolerantXml("a;b"), "a;b");
}

TEST(TolerantXml, UppercaseXIsNotHexRef) {
    // Python 正则写的是小写 'x'，所以 &#X26; 不算合法十六进制引用：
    // 负向前瞻的 "&#\d+" 也匹配不上（X 不是数字），故整个 & 会被转义。
    EXPECT_EQ(TolerantXml("&#X26;"), "&amp;#X26;");
}

TEST(TolerantXml, IdempotentOnAlreadyEscaped) {
    const std::string once = TolerantXml("UsedInP&R &amp; x");
    const std::string twice = TolerantXml(once);
    EXPECT_EQ(once, twice);  // 再跑一次不应二次转义
}

// -----------------------------------------------------------------------------
// 最小 XML 解析器
// -----------------------------------------------------------------------------

TEST(XmlLite, ParsesSimpleDocument) {
    auto r = ParseXml("<a x=\"1\"><b>hi</b></a>");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.root.tag, "a");
    EXPECT_EQ(r.root.GetAttr("x"), "1");
    ASSERT_EQ(r.root.children.size(), 1u);
    EXPECT_EQ(r.root.children[0].tag, "b");
    EXPECT_EQ(r.root.children[0].text, "hi");
}

TEST(XmlLite, SelfClosingAndAttributes) {
    auto r = ParseXml("<a><b y='2'/><c/></a>");
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_EQ(r.root.children.size(), 2u);
    EXPECT_EQ(r.root.children[0].GetAttr("y"), "2");
}

TEST(XmlLite, DecodesEntities) {
    auto r = ParseXml("<a attr=\"x&amp;y\">p&lt;q</a>");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.root.GetAttr("attr"), "x&y");
    EXPECT_EQ(r.root.text, "p<q");
}

TEST(XmlLite, SkipsCommentsAndDeclaration) {
    auto r = ParseXml("<?xml version=\"1.0\"?><!-- c --><a><!-- inner --><b/></a>");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.root.tag, "a");
    EXPECT_EQ(r.root.children.size(), 1u);
}

TEST(XmlLite, HandlesCdata) {
    auto r = ParseXml("<a><![CDATA[raw <&> text]]></a>");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.root.text, "raw <&> text");
}

TEST(XmlLite, MismatchedTagIsError) {
    auto r = ParseXml("<a><b></c></a>");
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.empty());
}

TEST(XmlLite, UnterminatedIsError) {
    auto r = ParseXml("<a><b>text");
    EXPECT_FALSE(r.ok);
}

TEST(XmlLite, EmptyInputIsError) {
    auto r = ParseXml("");
    EXPECT_FALSE(r.ok);
}

TEST(XmlLite, BareAmpersandAfterToleranceParses) {
    // 裸 & 经 TolerantXml 后可正常解析
    auto r = ParseXml(TolerantXml("<a n=\"UsedInP&R\"/>"));
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.root.GetAttr("n"), "UsedInP&R");
}

TEST(XmlLite, NeverThrowsOnGarbage) {
    for (const char* garbage : {"<", "<<<>>>", "<a b=", "<a b=>", "<a></a><b></b>",
                                "not xml at all", "<a><<<</a>", "\x00\x01"}) {
        EXPECT_NO_THROW({
            auto r = ParseXml(garbage);
            (void)r;
        }) << "input: " << garbage;
    }
}

TEST(XmlLite, LocalName) {
    EXPECT_EQ(LocalName("Tag"), "Tag");
    EXPECT_EQ(LocalName("ns:Tag"), "Tag");
    EXPECT_EQ(LocalName("a:b:c"), "c");
}

// -----------------------------------------------------------------------------
// 解析辅助
// -----------------------------------------------------------------------------

class AlProjectTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() / "fpgalsp_al_test";
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
        std::filesystem::create_directories(root_);
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

    std::string Write(const std::string& rel, const std::string& content) const {
        const auto p = root_ / rel;
        std::filesystem::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary);
        out << content;
        out.close();
        return p.string();
    }

    std::filesystem::path root_;
};

// 一个结构完整的 .al
std::string MakeAl(const std::string& extra_files = "", const std::string& speed = "-2",
                   const std::string& path_attr = "") {
    const std::string path_line =
        path_attr.empty() ? "" : " Path=\"" + path_attr + "\"";
    return std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n") +
           "<!-- Last modified by TangDynasty Release_2026.2 -->\n" +
           "<Project Version=\"3\" Minor=\"2\"" + path_line + ">\n" +
           "  <TD_Version>6.2.3.210850</TD_Version>\n" +
           "  <Name>demo</Name>\n" +
           "  <HardWare>\n"
           "    <Family>EF2</Family>\n"
           "    <Device>EF2L45LG144B</Device>\n"
           "    <Speed>" + speed + "</Speed>\n"
           "  </HardWare>\n"
           "  <TOP_MODULE><MODULE>top</MODULE></TOP_MODULE>\n"
           "  <Source_Files>\n"
           "    <Verilog>\n"
           "      <File Path=\"src/a.v\"><FileInfo>\n"
           "        <Attr Name=\"UsedInP&R\" Val=\"true\"/>\n"
           "        <Attr Name=\"CompileOrder\" Val=\"2\"/>\n"
           "      </FileInfo></File>\n"
           "    </Verilog>\n"
           + extra_files +
           "  </Source_Files>\n"
           "</Project>\n";
}

// -----------------------------------------------------------------------------
// 正常解析
// -----------------------------------------------------------------------------

TEST_F(AlProjectTest, ParsesBasicFields) {
    Write("src/a.v", "module a; endmodule\n");
    const std::string al = Write("demo.al", MakeAl());

    const Project p = ParseAl(al);
    EXPECT_EQ(p.name, "demo");
    EXPECT_EQ(p.family, "EF2");
    EXPECT_EQ(p.device, "EF2L45LG144B");
    EXPECT_EQ(p.top, "top");
    EXPECT_EQ(p.td_version, "6.2.3.210850");
    EXPECT_EQ(p.modified_by, "TangDynasty Release_2026.2");
    ASSERT_TRUE(p.speed.has_value());
    EXPECT_EQ(*p.speed, "2");  // "-2" 去符号
}

TEST_F(AlProjectTest, SpeedStripsPlusToo) {
    Write("src/a.v", "x");
    const std::string al = Write("demo.al", MakeAl("", "+3"));
    EXPECT_EQ(*ParseAl(al).speed, "3");
}

TEST_F(AlProjectTest, EmptySpeedIsNullopt) {
    Write("src/a.v", "x");
    const std::string al = Write("demo.al", MakeAl("", ""));
    EXPECT_FALSE(ParseAl(al).speed.has_value());
}

TEST_F(AlProjectTest, BareAmpersandInAttributeIsTolerated) {
    // 这是 TD 真实写出的形态：UsedInP&R 是裸 &
    Write("src/a.v", "x");
    const std::string al = Write("demo.al", MakeAl());

    Project p;
    EXPECT_NO_THROW(p = ParseAl(al));
    ASSERT_EQ(p.files.size(), 1u);
    EXPECT_EQ(p.files[0].GetAttr("UsedInP&R"), "true");
    // 没有 XML 解析失败警告
    for (const auto& w : p.warnings) {
        EXPECT_EQ(w.find("XML parse failed"), std::string::npos) << w;
    }
}

TEST_F(AlProjectTest, AmpersandAndSemicolonInAttrValue) {
    // 任务提醒：属性值里可能含 & 与 ;
    const std::string extra =
        "    <Verilog>\n"
        "      <File Path=\"src/a.v\"><FileInfo>\n"
        "        <Attr Name=\"Note\" Val=\"a &amp; b; c &amp;&amp; d\"/>\n"
        "        <Attr Name=\"Raw\" Val=\"x&y\"/>\n"
        "      </FileInfo></File>\n"
        "    </Verilog>\n";
    Write("src/a.v", "x");
    const std::string al = Write("demo.al", MakeAl(extra));

    Project p;
    EXPECT_NO_THROW(p = ParseAl(al));
    // 第一个 <Verilog> 段里是 a.v；第二个 Verilog 段也是 a.v（同一文件两条记录）
    ASSERT_GE(p.files.size(), 2u);
    EXPECT_EQ(p.files[1].GetAttr("Note"), "a & b; c && d");
    EXPECT_EQ(p.files[1].GetAttr("Raw"), "x&y");
}

TEST_F(AlProjectTest, FileKindsAndCompileOrder) {
    const std::string extra =
        "    <VHDL><File Path=\"src/b.vhd\"><FileInfo>"
        "<Attr Name=\"CompileOrder\" Val=\"7\"/></FileInfo></File></VHDL>\n"
        "    <ADC_FILE><File Path=\"src/c.adc\"><FileInfo>"
        "<Attr Name=\"CompileOrder\" Val=\"1\"/></FileInfo></File></ADC_FILE>\n"
        "    <SDC_FILE><File Path=\"src/d.sdc\"><FileInfo/>"
        "</File></SDC_FILE>\n"
        "    <IP_FILE><File Path=\"src/e.ip\"><FileInfo/>"
        "</File></IP_FILE>\n"
        "    <CWC_FILE><File Path=\"src/f.cwc\"><FileInfo/>"
        "</File></CWC_FILE>\n";
    for (const char* f : {"src/a.v", "src/b.vhd", "src/c.adc", "src/d.sdc", "src/e.ip",
                          "src/f.cwc"}) {
        Write(f, "x");
    }
    const std::string al = Write("demo.al", MakeAl(extra));
    const Project p = ParseAl(al);

    EXPECT_EQ(p.VerilogFiles().size(), 1u);
    EXPECT_EQ(p.VhdlFiles().size(), 1u);
    EXPECT_EQ(p.AdcFiles().size(), 1u);
    EXPECT_EQ(p.SdcFiles().size(), 1u);
    EXPECT_EQ(p.IpFiles().size(), 1u);
    EXPECT_EQ(p.HdlFiles().size(), 2u);

    // CompileOrder 排序：adc(1) 在 verilog(2) 之前
    ASSERT_EQ(p.files.size(), 6u);
}

TEST_F(AlProjectTest, VerilogSectionWithNonVerilogExtDowngradesToOther) {
    const std::string extra =
        "    <Verilog><File Path=\"src/readme.txt\"><FileInfo/></File></Verilog>\n";
    Write("src/readme.txt", "x");
    const std::string al = Write("demo.al", MakeAl(extra));
    const Project p = ParseAl(al);

    // readme.txt 在 Verilog 段里但不是 verilog 扩展名 -> other
    const auto& f = p.files.back();
    EXPECT_EQ(FileKindName(f.kind), "other");
}

// -----------------------------------------------------------------------------
// 损坏 / 缺文件
// -----------------------------------------------------------------------------

TEST_F(AlProjectTest, TruncatedXmlFallsBackToRegex) {
    // 截断在中间：结构损坏 -> 兜底取 Device/Family/top
    const std::string truncated =
        "<?xml version=\"1.0\"?>\n<Project>\n<HardWare>\n"
        "<Family>EF3</Family>\n<Device>EF3L90</Device>\n</HardWare>\n"
        "<TOP_MODULE><MODULE>mytop</MODULE></TOP_MODULE>\n"
        "<Source_Files><Verilog><File Path=\"x.v\">\n";  // 未闭合

    const std::string al = Write("broken.al", truncated);
    Project p;
    EXPECT_NO_THROW(p = ParseAl(al));

    EXPECT_EQ(p.device, "EF3L90");
    EXPECT_EQ(p.family, "EF3");
    EXPECT_EQ(p.top, "mytop");
    // 必须有解析失败警告
    bool has_warning = false;
    for (const auto& w : p.warnings) {
        if (w.find("XML parse failed") != std::string::npos) has_warning = true;
    }
    EXPECT_TRUE(has_warning);
}

TEST_F(AlProjectTest, MissingAlFileDoesNotThrow) {
    Project p;
    EXPECT_NO_THROW(p = ParseAl((root_ / "nope.al").string()));
    EXPECT_TRUE(p.name.empty());
    ASSERT_FALSE(p.warnings.empty());
    EXPECT_NE(p.warnings[0].find("Cannot read .al"), std::string::npos);
}

TEST_F(AlProjectTest, MissingReferencedFileWarns) {
    const std::string al = Write("demo.al", MakeAl());  // 不创建 src/a.v
    const Project p = ParseAl(al);
    ASSERT_EQ(p.files.size(), 1u);
    EXPECT_FALSE(p.files[0].exists);
    EXPECT_EQ(p.files[0].resolved_via, ResolvedVia::Missing);

    bool warned = false;
    for (const auto& w : p.warnings) {
        if (w.find("Missing build file: src/a.v") != std::string::npos) warned = true;
    }
    EXPECT_TRUE(warned);
}

// -----------------------------------------------------------------------------
// 路径解析
// -----------------------------------------------------------------------------

TEST_F(AlProjectTest, RelativePathResolvesAgainstAlDir) {
    Write("src/a.v", "x");
    const std::string al = Write("demo.al", MakeAl());
    const Project p = ParseAl(al);
    ASSERT_EQ(p.files.size(), 1u);
    EXPECT_EQ(p.files[0].resolved_via, ResolvedVia::Project);
    EXPECT_TRUE(p.files[0].path.find("src/a.v") != std::string::npos);
    EXPECT_EQ(p.files[0].path.find('\\'), std::string::npos);  // 正斜杠
}

TEST_F(AlProjectTest, BackslashPathNormalized) {
    const std::string extra =
        "    <Verilog><File Path=\"src\\sub\\a.v\"><FileInfo/></File></Verilog>\n";
    Write("src/sub/a.v", "x");
    const std::string al = Write("demo.al", MakeAl(extra));
    const Project p = ParseAl(al);
    ASSERT_GE(p.files.size(), 1u);
    EXPECT_EQ(p.files.back().path.find('\\'), std::string::npos);
    EXPECT_EQ(p.files.back().resolved_via, ResolvedVia::Project);
}

TEST_F(AlProjectTest, AbsolutePathUsedDirectly) {
    const std::string abs_file = Write("abs/target.v", "x");
    std::string abs_fwd = abs_file;
    for (char& c : abs_fwd) if (c == '\\') c = '/';

    const std::string extra = "    <Verilog><File Path=\"" + abs_fwd +
                              "\"><FileInfo/></File></Verilog>\n";
    const std::string al = Write("demo.al", MakeAl(extra));
    const Project p = ParseAl(al);
    ASSERT_GE(p.files.size(), 1u);
    EXPECT_EQ(p.files.back().resolved_via, ResolvedVia::Absolute);
}

TEST_F(AlProjectTest, DeclaredFallbackWhenProjectMoved) {
    // 工程被拷到别处：文件只在 .al 记录的原始目录下存在 -> resolved_via = declared
    const std::string declared_dir = (root_ / "original").string();
    std::filesystem::create_directories(std::filesystem::path(declared_dir));
    std::ofstream(std::filesystem::path(declared_dir) / "moved.v") << "x";

    const std::string moved_root = (root_ / "moved").string();
    std::filesystem::create_directories(moved_root);

    std::string declared_fwd = declared_dir;
    for (char& c : declared_fwd) if (c == '\\') c = '/';

    const std::string extra =
        "    <Verilog><File Path=\"moved.v\"><FileInfo/></File></Verilog>\n";
    const std::string al = (std::filesystem::path(moved_root) / "demo.al").string();
    std::ofstream(al) << MakeAl(extra, "-2", declared_fwd);

    const Project p = ParseAl(al);
    ASSERT_GE(p.files.size(), 1u);
    EXPECT_EQ(p.files.back().resolved_via, ResolvedVia::Declared);

    bool warned = false;
    for (const auto& w : p.warnings) {
        if (w.find("original path recorded in the .al") != std::string::npos) warned = true;
    }
    EXPECT_TRUE(warned);
}

TEST_F(AlProjectTest, PathWithSemicolonAndAmpersandOnDisk) {
    // 任务提醒：路径/属性值里可能出现 ';' 和 '&'。
    // 这里验证真实落盘的文件名同时含 ';'、'&'、'#'、'%' 时仍能正确定位。
    const std::string tricky = "src/we ird;a&b#c%d.v";
    Write(tricky, "x");

    const std::string extra =
        "    <Verilog><File Path=\"" + tricky + "\"><FileInfo>\n"
        "      <Attr Name=\"UsedInP&R\" Val=\"true\"/>\n"
        "    </FileInfo></File></Verilog>\n";

    const std::string al = Write("demo.al", MakeAl(extra));
    const Project p = ParseAl(al);

    ASSERT_GE(p.files.size(), 1u);
    const AlFile& f = p.files.back();
    EXPECT_EQ(f.rel, tricky);
    EXPECT_TRUE(f.exists) << "path=" << f.path;
    EXPECT_EQ(f.resolved_via, ResolvedVia::Project);
    EXPECT_EQ(f.GetAttr("UsedInP&R"), "true");
}

TEST_F(AlProjectTest, ResolveRelativeUnit) {
    const std::string dir = root_.string();
    Write("a.v", "x");

    auto r1 = ResolveRelative(dir, "a.v");
    EXPECT_EQ(r1.via, ResolvedVia::Project);

    auto r2 = ResolveRelative(dir, "");
    EXPECT_EQ(r2.via, ResolvedVia::Missing);
    EXPECT_TRUE(r2.path.empty());

    auto r3 = ResolveRelative(dir, "missing.v");
    EXPECT_EQ(r3.via, ResolvedVia::Missing);
}

// -----------------------------------------------------------------------------
// 结构警告
// -----------------------------------------------------------------------------

TEST_F(AlProjectTest, WarnsOnMissingTopAndDevice) {
    const std::string minimal = "<Project><HardWare></HardWare></Project>";
    const std::string al = Write("min.al", minimal);
    const Project p = ParseAl(al);

    bool top_warn = false, dev_warn = false;
    for (const auto& w : p.warnings) {
        if (w.find("no top module") != std::string::npos) top_warn = true;
        if (w.find("no target device") != std::string::npos) dev_warn = true;
    }
    EXPECT_TRUE(top_warn);
    EXPECT_TRUE(dev_warn);
}

TEST_F(AlProjectTest, WarnsOnNoAdcAndNoSdc) {
    Write("src/a.v", "x");
    const std::string al = Write("demo.al", MakeAl());
    const Project p = ParseAl(al);

    bool adc = false, sdc = false;
    for (const auto& w : p.warnings) {
        if (w.find("No ADC pin constraint") != std::string::npos) adc = true;
        if (w.find("No SDC timing constraint") != std::string::npos) sdc = true;
    }
    EXPECT_TRUE(adc);
    EXPECT_TRUE(sdc);
}

TEST_F(AlProjectTest, NameFallsBackToFilename) {
    const std::string minimal = "<Project><HardWare><Device>D</Device>"
                                "</HardWare><TOP_MODULE><MODULE>t</MODULE>"
                                "</TOP_MODULE></Project>";
    const std::string al = Write("nosuchname.al", minimal);
    EXPECT_EQ(ParseAl(al).name, "nosuchname");
}

// -----------------------------------------------------------------------------
// 真实语料
// -----------------------------------------------------------------------------

TEST(AlRealCorpus, SixiangxianContainsBareAmpersandAndParses) {
    const std::string al =
        std::string(FPGA_LSP_SOURCE_DIR) +
        "/tests/gate-corpus/corpus_alpha/demo_project.al";

    if (!std::filesystem::exists(al)) {
        GTEST_SKIP() << "corpus not found: " << al;
    }

    Project p;
    EXPECT_NO_THROW(p = ParseAl(al));

    // 字段
    EXPECT_EQ(p.name, "demo_project");
    EXPECT_EQ(p.family, "EF2");
    EXPECT_EQ(p.device, "EF2L45LG144B");
    EXPECT_EQ(p.top, "top");
    EXPECT_FALSE(p.speed.has_value());  // <Speed></Speed> 为空
    EXPECT_EQ(p.td_version, "6.2.3.210850");
    EXPECT_EQ(p.declared_path, "%WORKSPACE%/CorpusAlpha");

    // 裸 & 被正确解析（不是走兜底）
    ASSERT_EQ(p.files.size(), 23u);
    EXPECT_EQ(p.files[0].GetAttr("UsedInP&R"), "true");

    // 种类分布
    EXPECT_EQ(p.VerilogFiles().size(), 20u);
    EXPECT_EQ(p.AdcFiles().size(), 1u);
    EXPECT_EQ(p.SdcFiles().size(), 1u);
    EXPECT_EQ(p.IpFiles().size(), 1u);
    EXPECT_EQ(p.HdlFiles().size(), 20u);

    // 因为 declared_path 与实际目录不同，应有 PRJ-6619 警告
    bool prj = false;
    for (const auto& w : p.warnings) {
        if (w.find("PRJ-6619") != std::string::npos) prj = true;
    }
    EXPECT_TRUE(prj);
}

// -----------------------------------------------------------------------------
// 工程发现
// -----------------------------------------------------------------------------

TEST_F(AlProjectTest, FindAlWalksUp) {
    Write("proj/demo.al", MakeAl());
    const std::string deep = Write("proj/src/deep/x.v", "x");

    auto found = FindAl(deep);
    ASSERT_TRUE(found.has_value());
    EXPECT_NE(found->find("proj/demo.al"), std::string::npos);
}

TEST_F(AlProjectTest, FindAlPrefersStemMatchingDirectory) {
    Write("proj/other.al", "<Project/>");
    Write("proj/proj.al", "<Project/>");
    const std::string file = Write("proj/x.v", "x");

    auto found = FindAl(file);
    ASSERT_TRUE(found.has_value());
    EXPECT_NE(found->find("proj.al"), std::string::npos);
    EXPECT_EQ(found->find("other.al"), std::string::npos);
}

TEST_F(AlProjectTest, FindAlReturnsNulloptWhenNone) {
    const std::string file = Write("nodir/x.v", "x");
    EXPECT_FALSE(FindAl(file).has_value());
}

TEST_F(AlProjectTest, DiscoverFindsNestedProjects) {
    Write("a/p1.al", "<Project/>");
    Write("b/c/p2.al", "<Project/>");
    const auto found = DiscoverProjects(root_.string());
    EXPECT_EQ(found.size(), 2u);
}

TEST_F(AlProjectTest, DiscoverSkipsBuildAndVcsDirs) {
    Write("live/p1.al", "<Project/>");
    Write("build/p2.al", "<Project/>");
    Write(".git/p3.al", "<Project/>");
    Write("node_modules/p4.al", "<Project/>");
    Write("x_runs/p5.al", "<Project/>");
    Write(".hidden/p6.al", "<Project/>");

    const auto found = DiscoverProjects(root_.string());
    ASSERT_EQ(found.size(), 1u);
    EXPECT_NE(found[0].find("live/p1.al"), std::string::npos);
}

TEST_F(AlProjectTest, DiscoverRespectsDepth) {
    Write("d1/d2/d3/d4/deep.al", "<Project/>");
    EXPECT_TRUE(DiscoverProjects(root_.string(), 1).empty());
    EXPECT_EQ(DiscoverProjects(root_.string(), 6).size(), 1u);
}

TEST_F(AlProjectTest, AffinityPrefersContainingProject) {
    const std::string al_in = (root_ / "proj/p.al").string();
    const std::string al_out = (root_ / "other/p.al").string();

    const auto inside = Affinity(al_in, (root_ / "proj/src/a.v").string());
    const auto outside = Affinity(al_out, (root_ / "proj/src/a.v").string());

    EXPECT_EQ(inside.first, 2);   // 在工程目录内
    EXPECT_EQ(outside.first, 1);  // 只是公共前缀
    EXPECT_GT(inside, outside);
}

TEST_F(AlProjectTest, FindProjectAlUsesAffinity) {
    Write("projA/a.al", MakeAl());
    Write("projB/b.al", MakeAl());
    const std::string target = Write("projB/src/target.v", "x");

    auto chosen = FindProjectAl(root_.string(), target);
    ASSERT_TRUE(chosen.has_value());
    EXPECT_NE(chosen->find("projB"), std::string::npos);
}

TEST_F(AlProjectTest, FindProjectAlConfiguredWins) {
    Write("projA/a.al", MakeAl());
    Write("projB/b.al", MakeAl());
    const std::string target = Write("projB/src/target.v", "x");
    const std::string configured = (root_ / "projA/a.al").string();

    auto chosen = FindProjectAl(root_.string(), target, configured);
    ASSERT_TRUE(chosen.has_value());
    EXPECT_NE(chosen->find("projA"), std::string::npos);
}

TEST_F(AlProjectTest, FindProjectAlConfiguredMissingReturnsNullopt) {
    const std::string target = Write("x.v", "x");
    EXPECT_FALSE(
        FindProjectAl(root_.string(), target, (root_ / "nope.al").string()).has_value());
}

// =============================================================================
//  B7 / S-05：xml_lite 的深度与体积上限
// =============================================================================
//  修复前 ParseElement 是**无上限**的递归下降，而 .al 的大小同样不受限：
//  一个 700 KB、十万层嵌套的 .al 会让进程 0xC00000FD（栈溢出）——
//  在 LSP 里表现为"语言服务器崩溃并反复重启"。
// =============================================================================

TEST(XmlLiteDepth, RealWorldDepthParses) {
    // 真实 .al：Project/Source_Files/Verilog/File/FileInfo/Attr ≈ 6 层
    const std::string xml =
        "<Project><Name>p</Name><Source_Files><Verilog><File Path=\"a.v\">"
        "<FileInfo><Attr Name=\"CompileOrder\" Val=\"1\"/></FileInfo></File></Verilog>"
        "</Source_Files></Project>";
    const XmlParseResult result = ParseXml(xml);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.root.tag, "Project");
}

TEST(XmlLiteDepth, DeepNestingIsRejectedInsteadOfCrashing) {
    const int depth = kMaxXmlDepth + 64;
    std::string xml;
    xml.reserve(static_cast<std::size_t>(depth) * 7);
    for (int i = 0; i < depth; ++i) xml += "<a>";
    for (int i = 0; i < depth; ++i) xml += "</a>";

    XmlParseResult result;
    EXPECT_NO_THROW(result = ParseXml(xml));
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("too deep"), std::string::npos) << result.error;
}

TEST(XmlLiteDepth, JustBelowLimitStillParses) {
    const int depth = kMaxXmlDepth - 1;
    std::string xml;
    for (int i = 0; i < depth; ++i) xml += "<a>";
    for (int i = 0; i < depth; ++i) xml += "</a>";
    const XmlParseResult result = ParseXml(xml);
    EXPECT_TRUE(result.ok) << result.error;
}

TEST(XmlLiteSize, OversizeDocumentRejectedBeforeScanning) {
    // 8 MiB + 1 的空文档：不是 .al，必须在扫描前就被拒
    const std::string xml(kMaxXmlDocumentBytes + 1, ' ');
    const XmlParseResult result = ParseXml(xml);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("too large"), std::string::npos) << result.error;
}

TEST(XmlLiteSize, ParseAlDoesNotReadAbsurdlyLargeFile) {
    // ParseAl 侧先用 file_size 挡一道，避免把 4 GB 的"文件"读进内存
    std::error_code ec;
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path(ec) / "fpgalsp-b7-huge-al";
    if (ec) GTEST_SKIP() << "no temp dir";
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path path = dir / "huge.al";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.seekp(static_cast<std::streamoff>(kMaxXmlDocumentBytes) + 1);
        out.put('<');  // 稀疏文件，只占很少磁盘
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || static_cast<std::size_t>(size) <= kMaxXmlDocumentBytes) {
        std::filesystem::remove_all(dir, ec);
        GTEST_SKIP() << "filesystem does not support sparse size here";
    }
    Project proj;
    EXPECT_NO_THROW(proj = ParseAl(path.string()));
    ASSERT_FALSE(proj.warnings.empty());
    EXPECT_NE(proj.warnings.front().find("too large"), std::string::npos)
        << proj.warnings.front();
    std::filesystem::remove_all(dir, ec);
}

// =============================================================================
//  B7 / S-06：Affinity 里的 string_view 悬垂（UB）回归
// =============================================================================
//  al_project.cpp:635-638 曾经把 `project_dir.substr(...)` 返回的**临时 string**
//  绑到 string_view 上，下一条语句就悬垂比较（clang-tidy bugprone-dangling-handle）。
//  这段路径决定"为某个文件挑哪个工程"，错乱会导致选错工程。
// =============================================================================

TEST(AlAffinity, ProjectMemberIsTier2) {
    const auto inside = Affinity("Y:/proj/demo.al", "Y:/proj/src/a.v");
    EXPECT_EQ(inside.first, 2);
}

TEST(AlAffinity, SiblingDirectoryIsTier1) {
    const auto sibling = Affinity("Y:/proj/demo.al", "Y:/proj_other/a.v");
    EXPECT_EQ(sibling.first, 1);
    // 公共路径段只有盘符 "Y:" -> score 1（悬垂视图时代这里是不可预测值）
    EXPECT_EQ(sibling.second, 1u);
}

TEST(AlAffinity, LongPathsDoNotDangle) {
    // 深路径会被分配到堆上，正是悬垂 string_view 最容易"看起来正常"的场景
    const std::string dir = "Y:/workspace/aaaaaaaaaaaaaaaaaaaa/bbbbbbbbbbbbbbbbbbbb/cccccccccccccccccccc";
    const auto inside = Affinity(dir + "/demo.al", dir + "/rtl/a.v");
    EXPECT_EQ(inside.first, 2);
    const auto outside = Affinity(dir + "/demo.al", dir + "x/rtl/a.v");
    EXPECT_EQ(outside.first, 1);
}

}  // namespace