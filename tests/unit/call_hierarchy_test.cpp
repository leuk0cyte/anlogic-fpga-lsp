// =============================================================================
//  tests/unit/call_hierarchy_test.cpp —— B4：call hierarchy in/out + 区间格式化
// =============================================================================
//  契约来自 A2 的 wire 实测（audit/2026-11-18/parity-lsp/raw.json matrix[12..16]）：
//    prepareCallHierarchy(top.v @ sys_rstn 实例) ->
//        [{name:"sys_rstn", kind:2, uri:<sys_rstn.v>, range/selectionRange 0:7-0:15,
//          data:{module:"sys_rstn", defined:true}}]
//    outgoingCalls(top)  -> 12 条，首条是 pll_gen（fromRanges 65:9-65:16）
//    incomingCalls(sys_rstn) -> 1 条，from=top（fromRanges 72:11-72:19）
//
//  这些测试直接驱动 LspServer::Feature*（不经子进程），用的是
//  tests/gate-corpus/corpus_alpha 这份与真实工程逐字节相同的语料。
// =============================================================================

#include <gtest/gtest.h>

#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "hdl/hdl_index.h"
#include "jsonrpc/transport.h"
#include "platform/path.h"
#include "server/features_range_format.h"
#include "server/lsp_server.h"

#if defined(FPGA_LSP_HAVE_VERIBLE_BRIDGE)
#include "verible_bridge/format_text.h"
#endif

namespace {

using Json = fpgalsp::server::Json;
using fpgalsp::jsonrpc::TransportBaseCommon;
using fpgalsp::jsonrpc::TransportMode;
using fpgalsp::server::LspServer;

const std::string kProject = std::string(FPGA_LSP_SOURCE_DIR) + "/tests/gate-corpus/corpus_alpha";
const std::string kTop = kProject + "/src/top.v";

std::string ReadFile(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.good()) return {};
    return std::string((std::istreambuf_iterator<char>(stream)),
                       std::istreambuf_iterator<char>());
}

// 按 "第一个含 token 的行 + 该行内 token 的位置" 找锚点（0-based 列），
// 避免把行号/列号硬编码进测试。
struct Anchor {
    int line = 0;
    int character = 0;
    bool found = false;
};

Anchor FindAnchor(const std::string& text, const std::string& line_needle,
                  const std::string& token) {
    Anchor anchor;
    std::istringstream stream(text);
    std::string row;
    int line = 0;
    while (std::getline(stream, row)) {
        if (row.find(line_needle) != std::string::npos) {
            const std::size_t at = row.find(token);
            if (at != std::string::npos) {
                anchor.line = line;
                anchor.character = static_cast<int>(at) + 1;  // 落在标识符内部
                anchor.found = true;
                return anchor;
            }
        }
        ++line;
    }
    return anchor;
}

// 最小 LspServer 夹具：本地 stringstream 充当传输，只要 initialize 的 rootUri
class Fixture {
public:
    Fixture()
        : transport_(fpgalsp::jsonrpc::MakeTransport(TransportMode::Lsp, input_, output_)),
          server_(*transport_) {
        Json params = Json::object();
        params["rootUri"] = fpgalsp::platform::path_to_uri(kProject);
        server_.FeatureInitialize(params);
    }

    LspServer& server() { return server_; }

private:
    std::istringstream input_;
    std::ostringstream output_;
    std::unique_ptr<TransportBaseCommon> transport_;
    LspServer server_;
};

Json MakeItem(const std::string& module_name, const std::string& uri) {
    Json item = Json::object();
    item["name"] = module_name;
    item["kind"] = 2;
    item["uri"] = uri;
    Json data = Json::object();
    data["module"] = module_name;
    data["defined"] = true;
    item["data"] = std::move(data);
    return item;
}

std::string ItemName(const Json& item) {
    return item.is_object() && item.contains("name") ? item["name"].get<std::string>() : "";
}

int ItemLine(const Json& item) {
    return item["range"]["start"]["line"].get<int>();
}

int ItemStart(const Json& item) {
    return item["range"]["start"]["character"].get<int>();
}

}  // namespace

// -----------------------------------------------------------------------------
// 1. prepareCallHierarchy
// -----------------------------------------------------------------------------
TEST(CallHierarchyPrepare, OnInstanceNameYieldsModule) {
    const std::string text = ReadFile(kTop);
    ASSERT_FALSE(text.empty());
    const Anchor anchor = FindAnchor(text, "sys_rstn  sys_rstn", "sys_rstn(");
    ASSERT_TRUE(anchor.found);

    Fixture fixture;
    const Json items = fixture.server().FeatureCallHierarchyPrepare(kTop, anchor.line,
                                                                   anchor.character);
    ASSERT_TRUE(items.is_array());
    ASSERT_EQ(items.size(), 1u) << "Python: 实例名 -> 1 个 CallHierarchyItem";
    const Json& item = items[0];
    EXPECT_EQ(ItemName(item), "sys_rstn");
    EXPECT_EQ(item["kind"].get<int>(), 2);
    EXPECT_EQ(item["uri"].get<std::string>(),
              fpgalsp::platform::path_to_uri(kProject + "/src/system/sys_rstn.v"));
    EXPECT_EQ(ItemLine(item), 0);
    EXPECT_EQ(ItemStart(item), 7);
    EXPECT_EQ(item["range"]["end"]["character"].get<int>(), 15);
    EXPECT_EQ(item["selectionRange"]["start"]["character"].get<int>(), 7);
    EXPECT_EQ(item["selectionRange"]["end"]["character"].get<int>(), 15);
    EXPECT_EQ(item["data"]["module"].get<std::string>(), "sys_rstn");
    EXPECT_TRUE(item["data"]["defined"].get<bool>());
}

TEST(CallHierarchyPrepare, OnModuleDefinitionYieldsSameModule) {
    const std::string text = ReadFile(kTop);
    ASSERT_FALSE(text.empty());
    const Anchor anchor = FindAnchor(text, "module top(", "top(");
    ASSERT_TRUE(anchor.found);

    Fixture fixture;
    const Json items = fixture.server().FeatureCallHierarchyPrepare(kTop, anchor.line,
                                                                   anchor.character);
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(ItemName(items[0]), "top");
    EXPECT_EQ(ItemLine(items[0]), 14);
    EXPECT_EQ(ItemStart(items[0]), 7);
}

TEST(CallHierarchyPrepare, OnKeywordOrCommentIsEmpty) {
    Fixture fixture;
    // 行 0 是注释；行 14 第 0 列是 `module` 关键字
    EXPECT_TRUE(fixture.server().FeatureCallHierarchyPrepare(kTop, 0, 0).empty());
    EXPECT_TRUE(fixture.server().FeatureCallHierarchyPrepare(kTop, 14, 0).empty());
    // 越界位置不该崩
    EXPECT_TRUE(fixture.server().FeatureCallHierarchyPrepare(kTop, 100000, 0).empty());
}

TEST(CallHierarchyPrepare, UnknownPathIsEmpty) {
    Fixture fixture;
    EXPECT_TRUE(fixture.server().FeatureCallHierarchyPrepare("", 0, 0).empty());
    EXPECT_TRUE(fixture.server()
                    .FeatureCallHierarchyPrepare(kProject + "/nope.v", 0, 0)
                    .empty());
}

// -----------------------------------------------------------------------------
// 2. outgoingCalls
// -----------------------------------------------------------------------------
TEST(CallHierarchyOutgoing, TopHasTwelveCallees) {
    Fixture fixture;
    const std::string top_uri = fpgalsp::platform::path_to_uri(kTop);
    const Json calls = fixture.server().FeatureCallHierarchyOutgoing(MakeItem("top", top_uri));
    ASSERT_TRUE(calls.is_array());
    ASSERT_EQ(calls.size(), 12u) << "Python outgoingCalls(top) = 12 条（按目标模块名分组）";

    // 首条：pll_gen（fromRanges 65:9-65:16，to 指向 al_ip/pll_gen.v）
    const Json& first = calls[0];
    EXPECT_EQ(ItemName(first["from"]), "top");
    EXPECT_EQ(ItemName(first["to"]), "pll_gen");
    EXPECT_EQ(first["fromRanges"].size(), 1u);
    EXPECT_EQ(first["fromRanges"][0]["start"]["line"].get<int>(), 65);
    EXPECT_EQ(first["fromRanges"][0]["start"]["character"].get<int>(), 9);
    EXPECT_EQ(first["fromRanges"][0]["end"]["character"].get<int>(), 16);
    EXPECT_EQ(first["to"]["uri"].get<std::string>(),
              fpgalsp::platform::path_to_uri(kProject + "/al_ip/pll_gen.v"));
    EXPECT_EQ(first["to"]["range"]["start"]["line"].get<int>(), 22);
    EXPECT_TRUE(first["to"]["data"]["defined"].get<bool>());

    // 每条 call 的 from 都是 top 的定义位置（14:7-14:10）
    for (const Json& call : calls) {
        EXPECT_EQ(ItemName(call["from"]), "top");
        EXPECT_EQ(call["from"]["range"]["start"]["line"].get<int>(), 14);
        EXPECT_EQ(call["from"]["range"]["start"]["character"].get<int>(), 7);
        EXPECT_EQ(call["from"]["range"]["end"]["character"].get<int>(), 10);
        EXPECT_FALSE(ItemName(call["to"]).empty());
        EXPECT_FALSE(call["fromRanges"].empty());
    }

    // 被例化的目标里必然有 sys_rstn / trans，且不重复（分组语义）
    std::vector<std::string> targets;
    for (const Json& call : calls) targets.push_back(ItemName(call["to"]));
    EXPECT_NE(std::find(targets.begin(), targets.end(), "sys_rstn"), targets.end());
    EXPECT_NE(std::find(targets.begin(), targets.end(), "trans"), targets.end());
    for (std::size_t i = 0; i < targets.size(); ++i) {
        for (std::size_t j = i + 1; j < targets.size(); ++j) {
            EXPECT_NE(targets[i], targets[j]) << "同一目标模块只能出现一次（分组）";
        }
    }
}

TEST(CallHierarchyOutgoing, UnknownModuleIsEmpty) {
    Fixture fixture;
    const Json calls = fixture.server().FeatureCallHierarchyOutgoing(
        MakeItem("no_such_module_xyz", fpgalsp::platform::path_to_uri(kTop)));
    EXPECT_TRUE(calls.empty());
    // data 里没有 module 也返回空，不抛
    Json empty_item = Json::object();
    EXPECT_TRUE(fixture.server().FeatureCallHierarchyOutgoing(empty_item).empty());
}

// -----------------------------------------------------------------------------
// 3. incomingCalls
// -----------------------------------------------------------------------------
TEST(CallHierarchyIncoming, SysRstnIsInstantiatedByTop) {
    Fixture fixture;
    const std::string uri = fpgalsp::platform::path_to_uri(kProject + "/src/system/sys_rstn.v");
    const Json calls = fixture.server().FeatureCallHierarchyIncoming(MakeItem("sys_rstn", uri));
    ASSERT_TRUE(calls.is_array());
    ASSERT_EQ(calls.size(), 1u) << "Python incomingCalls(sys_rstn) = 1 条";
    const Json& call = calls[0];
    EXPECT_EQ(ItemName(call["from"]), "top");
    EXPECT_EQ(ItemLine(call["from"]), 14);
    EXPECT_EQ(call["from"]["range"]["start"]["character"].get<int>(), 7);
    EXPECT_EQ(call["from"]["range"]["end"]["character"].get<int>(), 10);
    EXPECT_EQ(ItemName(call["to"]), "sys_rstn");
    EXPECT_EQ(ItemLine(call["to"]), 0);
    ASSERT_EQ(call["fromRanges"].size(), 1u);
    EXPECT_EQ(call["fromRanges"][0]["start"]["line"].get<int>(), 72);
    EXPECT_EQ(call["fromRanges"][0]["start"]["character"].get<int>(), 11);
    EXPECT_EQ(call["fromRanges"][0]["end"]["character"].get<int>(), 19);
}

TEST(CallHierarchyIncoming, TopHasNoParent) {
    Fixture fixture;
    const std::string uri = fpgalsp::platform::path_to_uri(kTop);
    EXPECT_TRUE(fixture.server().FeatureCallHierarchyIncoming(MakeItem("top", uri)).empty());
    EXPECT_TRUE(fixture.server()
                    .FeatureCallHierarchyIncoming(MakeItem("no_such_module_xyz", uri))
                    .empty());
}

// -----------------------------------------------------------------------------
// 4. 区间格式化（纯函数，不依赖 verible）
// -----------------------------------------------------------------------------
TEST(RangeFormattingEdits, PerLineWhenLineCountMatches) {
    const std::string text =
        "module m;\n"
        "  wire  x;\n"
        "endmodule\n";
    const std::string formatted =
        "module m;\n"
        "  wire x;\n"
        "endmodule\n";
    const Json edits = fpgalsp::server::RangeFormattingEdits(text, formatted, 1, 1);
    ASSERT_EQ(edits.size(), 1u);
    EXPECT_EQ(edits[0]["range"]["start"]["line"].get<int>(), 1);
    EXPECT_EQ(edits[0]["range"]["start"]["character"].get<int>(), 0);
    EXPECT_EQ(edits[0]["range"]["end"]["line"].get<int>(), 1);
    EXPECT_EQ(edits[0]["range"]["end"]["character"].get<int>(),
              static_cast<int>(std::string("  wire  x;").size()));
    EXPECT_EQ(edits[0]["newText"].get<std::string>(), "  wire x;");
}

TEST(RangeFormattingEdits, LineCountChangeFallsBackToWholeDocument) {
    const std::string text =
        "module m;\n"
        "  assign y =\n"
        "      a && b;\n"
        "endmodule\n";
    const std::string formatted =
        "module m;\n"
        "  assign y = a && b;\n"
        "endmodule\n";
    const Json edits = fpgalsp::server::RangeFormattingEdits(text, formatted, 1, 1);
    // Python: 行数变了 -> 回落到整篇格式化（单条覆盖全文的编辑）
    ASSERT_EQ(edits.size(), 1u);
    EXPECT_EQ(edits[0]["range"]["start"]["line"].get<int>(), 0);
    EXPECT_EQ(edits[0]["range"]["start"]["character"].get<int>(), 0);
    EXPECT_EQ(edits[0]["range"]["end"]["line"].get<int>(), 4);  // 最后一个空行
    EXPECT_EQ(edits[0]["range"]["end"]["character"].get<int>(), 0);
    EXPECT_EQ(edits[0]["newText"].get<std::string>(), formatted);
}

TEST(RangeFormattingEdits, NoChangeProducesNoEdits) {
    const std::string text = "module m;\nendmodule\n";
    EXPECT_TRUE(fpgalsp::server::RangeFormattingEdits(text, text, 0, 2).empty());
    // 区间外的改动不出编辑
    const std::string formatted = "module  m;\nendmodule\n";
    EXPECT_TRUE(fpgalsp::server::RangeFormattingEdits(text, formatted, 1, 1).empty());
}

// -----------------------------------------------------------------------------
// 5. 区间格式化：真实 Verible 桥 + top.v 语料
// -----------------------------------------------------------------------------
#if defined(FPGA_LSP_HAVE_VERIBLE_BRIDGE)
TEST(RangeFormattingVerible, TopCorpusProducesWholeDocumentEdit) {
    const std::string text = ReadFile(kTop);
    ASSERT_FALSE(text.empty());
    const auto formatted = fpgalsp::verible_bridge::FormatText(text, kTop);
    ASSERT_TRUE(formatted.ok) << formatted.error;

    const Json edits = fpgalsp::server::RangeFormattingEdits(text, formatted.text, 10, 25);
    ASSERT_EQ(edits.size(), 1u) << "Verible 改写了行结构 -> 单条覆盖全文的编辑";
    EXPECT_EQ(edits[0]["range"]["start"]["line"].get<int>(), 0);
    EXPECT_EQ(edits[0]["range"]["start"]["character"].get<int>(), 0);
    EXPECT_EQ(edits[0]["range"]["end"]["line"].get<int>(), 501);  // top.v 有 502 行（末尾空行）
    EXPECT_EQ(edits[0]["range"]["end"]["character"].get<int>(), 0);
    EXPECT_EQ(edits[0]["newText"].get<std::string>(), formatted.text);
    EXPECT_NE(formatted.text, text);
}
#endif