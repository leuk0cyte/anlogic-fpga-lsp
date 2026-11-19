// =============================================================================
//  tests/unit/index_service_test.cpp —— IndexService（增量索引 + 不可变快照）
// =============================================================================
//  任务：task-21（Phase 2 / A1）
//
//  要证明的几件事（按测试分组）：
//    1. HdlIndex 的增量原语（Clone / ReplaceFile / RemoveFile / RebuildModuleIndex）
//       与"重建一遍"结果等价：文件顺序、模块首次出现顺序、alias 都一致。
//    2. IndexService：改一个文件只解析/安装那一个文件（rebuilds 不涨、file_updates 涨），
//       未改动的文件**复用同一个 ParsedFile 实例**（shared_ptr 指针不变 = 没有重解析）。
//    3. 删除 / 重命名 / .al 变更（全量重建）语义正确。
//    4. 快照不可变：更新之后，老快照仍能完整查询（读不会被写撕裂）。
//    5. 并发：多个线程同时 Get()（每个请求都在锁外跑查询）与单写者 Update 并发无撕裂、
//       无崩溃、generation 单调。
//    6. watcher 缺失：磁盘上改了文件，Get() 用目录 stamp 懒校验捡到变化；
//       缓冲区未保存的改动不会被磁盘内容覆盖回去。
//    7. 冷构建并行（BuildOptions.parse_threads）与顺序构建**结果完全一致**。
//    8. 基准（env 门控）：200 / 1000 文件合成工程，didChange→completion 的 before/after。
//
//  临时目录：%TEMP%\fpgalsp-index-service-test\<name>，每个用例自己清理。
// =============================================================================

#include "server/index_service.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "hdl/hdl_index.h"
#include "platform/encoding.h"
#include "platform/path.h"
#include "project/al_project.h"
#include "server/features.h"

namespace {

namespace fs = std::filesystem;

using fpgalsp::hdl::BuildIndex;
using fpgalsp::hdl::BuildOptions;
using fpgalsp::hdl::HdlIndex;
using fpgalsp::hdl::ParsedFile;
using fpgalsp::hdl::ParseCache;
using fpgalsp::hdl::ParseVerilog;
using fpgalsp::project::Project;
using fpgalsp::server::IndexService;

// -----------------------------------------------------------------------------
// 临时工程夹具
// -----------------------------------------------------------------------------

fs::path TempRoot(const std::string& name) {
    const fs::path root = fs::temp_directory_path() / "fpgalsp-index-service-test" / name;
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    return root;
}

void WriteText(const fs::path& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string ReadText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// 最小可用 .al（结构与 TD 2026.2 的 <Project><Source_Files><Verilog><File Path=...> 一致）
std::string AlXml(const fs::path& dir, const std::vector<std::string>& rel_files,
                  const std::string& top = "m1") {
    std::string xml;
    xml += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    xml += "<Project Version=\"3\" Minor=\"2\" Path=\"" + dir.generic_string() + "\">\n";
    xml += "    <TD_Version>6.2.3.210850</TD_Version>\n";
    xml += "    <Name>synthetic</Name>\n";
    xml += "    <HardWare>\n        <Family>EF2</Family>\n"
           "        <Device>EF2L45LG144B</Device>\n        <Speed></Speed>\n    </HardWare>\n";
    xml += "    <Source_Files>\n        <Verilog>\n";
    int order = 1;
    for (const std::string& rel : rel_files) {
        xml += "            <File Path=\"" + rel + "\">\n                <FileInfo>\n";
        xml += "                    <Attr Name=\"UsedInSyn\" Val=\"true\"/>\n";
        xml += "                    <Attr Name=\"CompileOrder\" Val=\"" + std::to_string(order++) +
               "\"/>\n";
        xml += "                </FileInfo>\n            </File>\n";
    }
    xml += "        </Verilog>\n    </Source_Files>\n";
    xml += "    <TOP_MODULE>\n        <LABEL>" + top + "</LABEL>\n        <MODULE>" + top +
           "</MODULE>\n    </TOP_MODULE>\n";
    xml += "</Project>\n";
    return xml;
}

// 单文件 Verilog 源码：模块名 + 一个可区分的端口名，便于断言"内容真的换了"
std::string VerilogModule(const std::string& module, const std::string& marker) {
    return "module " + module + " (input clk, input " + marker + ", output q);\n"
           "  assign q = clk & " + marker + ";\n"
           "endmodule\n";
}

// 一个 3 文件工程（m1 例化 m2 / m3）
struct TinyProject {
    fs::path dir;
    std::string al;
    std::vector<std::string> rel_files = {"src/m1.v", "src/m2.v", "src/m3.v"};

    explicit TinyProject(const std::string& name) : dir(TempRoot(name)) {
        WriteText(dir / "src/m1.v",
                  "module m1 (input clk, output q);\n  m2 u2 (.clk(clk));\n"
                  "  m3 u3 (.clk(clk));\n  assign q = clk;\nendmodule\n");
        WriteText(dir / "src/m2.v",
                  "module m2 (input clk);\n  wire a;  assign a = clk;\nendmodule\n");
        WriteText(dir / "src/m3.v",
                  "module m3 (input clk);\n  wire b;  assign b = clk;\nendmodule\n");
        al = (dir / "p.al").generic_string();
        WriteText(dir / "p.al", AlXml(dir, rel_files));
    }

    std::string File(int index) const { return (dir / rel_files[index]).generic_string(); }

    Project Parse() const { return fpgalsp::project::ParseAl(al); }
};

// 从磁盘读（= IndexService 的默认 loader，但显式注入便于与"现状"路径共用同一份实现）
std::optional<std::string> DiskLoader(const std::string& path) {
    return fpgalsp::platform::try_read_file_utf8(path);
}

// -----------------------------------------------------------------------------
// HdlIndex 增量原语
// -----------------------------------------------------------------------------

// 构造一个"按顺序 AddFile 三份"的索引，作为对照基准
HdlIndex BuildByAdd(const std::vector<std::string>& paths, const std::vector<std::string>& texts) {
    HdlIndex index;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        index.AddFile(ParseVerilog(paths[i], texts[i]));
    }
    return index;
}

// 与 AddFile(值重载) 同语义，但显式包成 shared_ptr（ReplaceFile 只吃 shared_ptr）
std::shared_ptr<const ParsedFile> ParseShared(const std::string& path, const std::string& text) {
    return std::make_shared<const ParsedFile>(ParseVerilog(path, text));
}

void ExpectSameIndex(const HdlIndex& a, const HdlIndex& b) {
    EXPECT_EQ(a.FileCount(), b.FileCount());
    EXPECT_EQ(a.Paths(), b.Paths());
    EXPECT_EQ(a.ModuleNames(), b.ModuleNames());
    EXPECT_EQ(a.ModuleCount(), b.ModuleCount());
    for (const std::string& name : a.ModuleNames()) {
        const fpgalsp::hdl::Module* left = a.ModuleByName(name);
        const fpgalsp::hdl::Module* right = b.ModuleByName(name);
        ASSERT_NE(left, nullptr);
        ASSERT_NE(right, nullptr);
        EXPECT_EQ(left->file, right->file);
        EXPECT_EQ(left->line, right->line);
        EXPECT_EQ(left->ports.size(), right->ports.size());
    }
}

const char* kPathA = "Y:/idx/a.v";
const char* kPathB = "Y:/idx/b.v";
const char* kPathC = "Y:/idx/c.v";

TEST(HdlIndexIncremental, CloneIsIndependentCopy) {
    const HdlIndex original =
        BuildByAdd({kPathA}, {"module a (input clk); endmodule\n"});
    HdlIndex clone = original.Clone();
    ASSERT_EQ(clone.FileCount(), 1u);

    // 改 clone 不影响 original，且 ParsedFile 是**共享**的（同一指针）
    const fpgalsp::hdl::ParsedFile* before = original.File(kPathA);
    ASSERT_NE(before, nullptr);
    clone.ReplaceFile(ParseShared(kPathA, "module a2 (input clk); endmodule\n"));
    EXPECT_EQ(original.ModuleByName("a2"), nullptr);
    EXPECT_NE(original.ModuleByName("a"), nullptr);
    EXPECT_NE(clone.ModuleByName("a2"), nullptr);
    EXPECT_EQ(original.File(kPathA), before);  // 原索引的 ParsedFile 未被动过
}

TEST(HdlIndexIncremental, CloneSharesParsedFiles) {
    // "O(N) 指针拷贝"的可观测证据：Clone 后未改动文件的 ParsedFile 指针完全相同。
    HdlIndex original = BuildByAdd({kPathA, kPathB}, {"module a; endmodule\n",
                                                      "module b; endmodule\n"});
    HdlIndex clone = original.Clone();
    ASSERT_EQ(clone.FileCount(), 2u);
    EXPECT_EQ(clone.File(kPathA), original.File(kPathA));
    EXPECT_EQ(clone.File(kPathB), original.File(kPathB));
}

TEST(HdlIndexIncremental, ReplaceFileEqualsFullRebuild) {
    // 改 b.v 的模块名：增量结果必须与"重新 AddFile 一遍"逐字段一致
    const std::string text_a = "module a (input clk); endmodule\n";
    const std::string text_c = "module c (input rst); endmodule\n";
    const std::string old_b = "module b (input clk); endmodule\n";
    const std::string new_b = "module b_new (input clk, output q); endmodule\n";

    HdlIndex incremental = BuildByAdd({kPathA, kPathB, kPathC}, {text_a, old_b, text_c});
    incremental.ReplaceFile(ParseShared(kPathB, new_b));

    const HdlIndex full = BuildByAdd({kPathA, kPathB, kPathC}, {text_a, new_b, text_c});
    ExpectSameIndex(incremental, full);
    EXPECT_EQ(incremental.ModuleByName("b"), nullptr);
    EXPECT_NE(incremental.ModuleByName("b_new"), nullptr);
}

TEST(HdlIndexIncremental, ReplaceFileAddingNewPathAppends) {
    HdlIndex incremental = BuildByAdd({kPathA}, {"module a; endmodule\n"});
    incremental.ReplaceFile(ParseShared(kPathB, "module b; endmodule\n"));
    const HdlIndex full = BuildByAdd({kPathA, kPathB}, {"module a; endmodule\n",
                                                       "module b; endmodule\n"});
    ExpectSameIndex(incremental, full);
    EXPECT_EQ(incremental.Paths()[1], fpgalsp::platform::normalize_path(kPathB));
}

TEST(HdlIndexIncremental, RemoveFileShiftsIndices) {
    const std::string text_a = "module a; endmodule\n";
    const std::string text_b = "module b; endmodule\n";
    const std::string text_c = "module c; endmodule\n";

    HdlIndex incremental = BuildByAdd({kPathA, kPathB, kPathC}, {text_a, text_b, text_c});
    ASSERT_TRUE(incremental.RemoveFile(kPathA));
    EXPECT_FALSE(incremental.RemoveFile(kPathA));  // 已经不在里面

    const HdlIndex full = BuildByAdd({kPathB, kPathC}, {text_b, text_c});
    ExpectSameIndex(incremental, full);
    // 下标左移后查找仍然正确（by_lower_path_ 已重建）
    EXPECT_NE(incremental.File(kPathB), nullptr);
    EXPECT_NE(incremental.ModuleByName("c"), nullptr);
    EXPECT_EQ(incremental.ModuleByName("a"), nullptr);
}

TEST(HdlIndexIncremental, DuplicateModuleKeepsFirstOccurrence) {
    // 两个文件都有 module dup：全量构建取**先出现**的那个；增量替换后仍应如此
    const std::string first = "module dup (input a); endmodule\n";
    const std::string second = "module dup (input a, input b); endmodule\n";
    HdlIndex incremental = BuildByAdd({kPathA, kPathB}, {first, second});
    ASSERT_NE(incremental.ModuleByName("dup"), nullptr);
    EXPECT_EQ(incremental.ModuleByName("dup")->ports.size(), 1u);

    // 把"先出现"的文件换成没有 dup 的模块 -> 模块表必须落到第二份
    incremental.ReplaceFile(ParseShared(kPathA, "module other; endmodule\n"));
    ASSERT_NE(incremental.ModuleByName("dup"), nullptr);
    EXPECT_EQ(incremental.ModuleByName("dup")->ports.size(), 2u);

    const HdlIndex full =
        BuildByAdd({kPathA, kPathB}, {"module other; endmodule\n", second});
    ExpectSameIndex(incremental, full);
}

// -----------------------------------------------------------------------------
// IndexService：基本增量语义
// -----------------------------------------------------------------------------

TEST(IndexServiceTest, ColdBuildThenSingleFileUpdate) {
    TinyProject tiny("single_file_update");
    IndexService service(DiskLoader);
    service.SetProject(tiny.Parse());

    const IndexService::Snapshot first = service.Get();
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->FileCount(), 3u);
    EXPECT_EQ(service.stats().rebuilds, 1u);
    EXPECT_EQ(service.stats().file_updates, 0u);
    const std::uint64_t generation0 = service.stats().generation;

    // didChange：m2.v 内容变了（缓冲区文本）
    const std::string new_text = VerilogModule("m2", "marker_b");
    service.OnFileChanged(tiny.File(1), new_text);

    const IndexService::Stats stats = service.stats();
    EXPECT_EQ(stats.rebuilds, 1u) << "改一个文件不应触发全量重建";
    EXPECT_EQ(stats.file_updates, 1u);
    EXPECT_EQ(stats.generation, generation0 + 1);

    const IndexService::Snapshot second = service.Get();
    ASSERT_NE(second, nullptr);
    EXPECT_NE(second->ModuleByName("m2"), nullptr);
    const fpgalsp::hdl::Module* m2 = second->ModuleByName("m2");
    ASSERT_NE(m2, nullptr);
    // 端口 marker_b 出现 = 新内容生效
    bool has_marker = false;
    for (const fpgalsp::hdl::Port& port : m2->ports) {
        if (port.name == "marker_b") has_marker = true;
    }
    EXPECT_TRUE(has_marker) << "增量更新没有装上新的 ParsedFile";

    // 未改动的文件复用同一个 ParsedFile 实例（指针不变 = 没有重新解析）
    EXPECT_EQ(second->File(tiny.File(0)), first->File(tiny.File(0)));
    EXPECT_EQ(second->File(tiny.File(2)), first->File(tiny.File(2)));
    EXPECT_EQ(second->FileCount(), 3u);
}

TEST(IndexServiceTest, UpdateForUnknownFileIsIgnored) {
    TinyProject tiny("unknown_file");
    IndexService service(DiskLoader);
    service.SetProject(tiny.Parse());
    ASSERT_NE(service.Get(), nullptr);
    const std::uint64_t generation = service.stats().generation;

    const fs::path stranger = tiny.dir / "src" / "stranger.v";
    WriteText(stranger, "module stranger; endmodule\n");
    service.OnFileChanged(stranger.generic_string(), std::string("module stranger2; endmodule\n"));

    EXPECT_EQ(service.stats().generation, generation) << "非工程文件不应改变快照";
    EXPECT_EQ(service.stats().file_updates, 0u);
}

TEST(IndexServiceTest, SetProjectWithSameFilesKeepsSnapshot) {
    TinyProject tiny("same_project");
    IndexService service(DiskLoader);
    service.SetProject(tiny.Parse());
    const IndexService::Snapshot first = service.Get();
    ASSERT_NE(first, nullptr);

    // 同一个 .al 再设一遍（集成方每请求都会调 SetProject）：不得重建
    service.SetProject(tiny.Parse());
    const IndexService::Snapshot second = service.Get();
    EXPECT_EQ(first.get(), second.get());
    EXPECT_EQ(service.stats().rebuilds, 1u);
}

// C1：SetProject 改为 `const project::Project&` 接收（每请求只留 state_.project 那一份拷贝）。
// 集成方每请求都会以"同一工程"再调一次（LspServer 的 didOpen/didChange/didSave 与
// IndexForProject 都是），这条 lvalue 路径必须仍是空操作：不重建、不换快照。
TEST(IndexServiceTest, SetProjectByConstRefWithSameProjectKeepsSnapshot) {
    TinyProject tiny("const_ref_same_project");
    IndexService service(DiskLoader);

    const Project project = tiny.Parse();
    service.SetProject(project);  // lvalue -> const& 路径（热路径的实际形态）
    const IndexService::Snapshot first = service.Get();
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(service.tracked_files(), 3u);

    const IndexService::Stats baseline = service.stats();
    ASSERT_EQ(baseline.rebuilds, 1u);
    ASSERT_EQ(baseline.file_updates, 0u);

    // 模拟连续两个请求各设一次同一工程
    service.SetProject(project);
    service.SetProject(project);

    EXPECT_EQ(service.Get().get(), first.get()) << "同一工程不得换快照";
    const IndexService::Stats after = service.stats();
    EXPECT_EQ(after.rebuilds, baseline.rebuilds) << "SetProject 不得触发全量重建";
    EXPECT_EQ(after.generation, baseline.generation) << "SetProject 不得产生新快照";
    EXPECT_EQ(after.file_updates, baseline.file_updates);
    EXPECT_EQ(service.al_path(), project.al_path);
    EXPECT_EQ(service.tracked_files(), 3u);
}

TEST(IndexServiceTest, SnapshotIsImmutableAcrossUpdates) {
    TinyProject tiny("snapshot_immutable");
    IndexService service(DiskLoader);
    service.SetProject(tiny.Parse());
    const IndexService::Snapshot old_snapshot = service.Get();
    ASSERT_NE(old_snapshot, nullptr);
    const fpgalsp::hdl::ParsedFile* old_file = old_snapshot->File(tiny.File(1));
    ASSERT_NE(old_file, nullptr);

    // 写者连续更新两次
    service.OnFileChanged(tiny.File(1), VerilogModule("m2", "marker_one"));
    service.OnFileChanged(tiny.File(1), VerilogModule("m2", "marker_two"));

    // 老快照完全不受影响
    EXPECT_EQ(old_snapshot->FileCount(), 3u);
    EXPECT_EQ(old_snapshot->File(tiny.File(1)), old_file);
    EXPECT_NE(old_snapshot->ModuleByName("m2"), nullptr);
}

// -----------------------------------------------------------------------------
// IndexService：删除 / 重命名
// -----------------------------------------------------------------------------

TEST(IndexServiceTest, DeleteViaWatchedFiles) {
    TinyProject tiny("delete");
    IndexService service(DiskLoader);
    service.SetProject(tiny.Parse());
    ASSERT_EQ(service.Get()->FileCount(), 3u);

    std::error_code ec;
    fs::remove(fs::path(tiny.File(1)), ec);
    service.OnWatchedFiles({}, {tiny.File(1)});

    const IndexService::Snapshot after = service.Get();
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->FileCount(), 2u);
    EXPECT_EQ(after->File(tiny.File(1)), nullptr);
    EXPECT_EQ(after->ModuleByName("m2"), nullptr);
    EXPECT_NE(after->ModuleByName("m1"), nullptr);
    EXPECT_EQ(service.stats().file_updates, 1u);
    EXPECT_EQ(service.stats().rebuilds, 1u);
}

TEST(IndexServiceTest, RenameViaAlRewriteRebuilds) {
    TinyProject tiny("rename");
    IndexService service(DiskLoader);
    service.SetProject(tiny.Parse());
    ASSERT_NE(service.Get(), nullptr);
    const std::uint64_t generation = service.stats().generation;

    // 重命名 m3.v -> renamed.v：磁盘改名 + .al 改写（.al 是权威来源）
    const std::string old_path = tiny.File(2);
    std::error_code ec;
    fs::rename(fs::path(old_path), tiny.dir / "src" / "renamed.v", ec);
    ASSERT_FALSE(ec);
    tiny.rel_files[2] = "src/renamed.v";
    WriteText(tiny.dir / "p.al", AlXml(tiny.dir, tiny.rel_files));

    service.OnWatchedFiles({tiny.al}, {});
    const IndexService::Snapshot after = service.Get();
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(service.stats().rebuilds, 2u) << ".al 变更必须全量重建";
    EXPECT_EQ(service.stats().generation, generation + 1);
    EXPECT_EQ(after->File(old_path), nullptr);
    EXPECT_NE(after->File((tiny.dir / "src" / "renamed.v").generic_string()), nullptr);
    EXPECT_EQ(after->FileCount(), 3u);
    EXPECT_NE(after->ModuleByName("m3"), nullptr);
}

TEST(IndexServiceTest, AlChangeAddingFileRebuildsFully) {
    TinyProject tiny("al_add");
    IndexService service(DiskLoader);
    service.SetProject(tiny.Parse());
    ASSERT_NE(service.Get(), nullptr);
    ASSERT_EQ(service.Get()->FileCount(), 3u);

    WriteText(tiny.dir / "src/m4.v", "module m4 (input clk); endmodule\n");
    tiny.rel_files.push_back("src/m4.v");
    WriteText(tiny.dir / "p.al", AlXml(tiny.dir, tiny.rel_files));

    service.OnWatchedFiles({tiny.al}, {});
    const IndexService::Snapshot after = service.Get();
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->FileCount(), 4u);
    EXPECT_NE(after->ModuleByName("m4"), nullptr);
    EXPECT_EQ(service.stats().rebuilds, 2u);
}

// -----------------------------------------------------------------------------
// IndexService：watcher 缺失时的目录 stamp 懒校验
// -----------------------------------------------------------------------------

TEST(IndexServiceTest, WatcherMissingDetectsDiskChange) {
    TinyProject tiny("stamp_change");
    IndexService service(DiskLoader);
    service.SetProject(tiny.Parse());
    service.SetWatcherActive(false);
    ASSERT_NE(service.Get(), nullptr);  // 建立 stamp 基线
    ASSERT_FALSE(service.watcher_active());
    const std::uint64_t generation = service.stats().generation;

    // 外部改盘（模拟"编辑器没有 watcher"）：内容变化 + 明确把 mtime 推到未来，
    // 避免同一时钟刻度内"改了但 stamp 看起来没变"。
    const fs::path target(tiny.File(0));
    WriteText(target, VerilogModule("m1", "marker_disk"));
    std::error_code ec;
    fs::last_write_time(target, fs::file_time_type::clock::now() + std::chrono::seconds(5), ec);
    ASSERT_FALSE(ec);

    // F2/R3：默认 TTL（250 ms）内不会重复校验 —— 这是有意的最终一致语义
    // （外部改动最迟 250 ms 可见）。这里把间隔归零，验证"校验本身仍能捡到变化"。
    service.SetStampValidationInterval(std::chrono::milliseconds(0));

    const IndexService::Snapshot after = service.Get();
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(service.stats().generation, generation + 1) << "目录 stamp 没有捡到磁盘变化";
    EXPECT_EQ(service.stats().file_updates, 1u);
    EXPECT_EQ(service.stats().rebuilds, 1u) << "只应增量更新，不做全量重建";

    const fpgalsp::hdl::Module* m1 = after->ModuleByName("m1");
    ASSERT_NE(m1, nullptr);
    bool has_marker = false;
    for (const fpgalsp::hdl::Port& port : m1->ports) {
        if (port.name == "marker_disk") has_marker = true;
    }
    EXPECT_TRUE(has_marker);
}

TEST(IndexServiceTest, WatcherMissingKeepsBufferOverlay) {
    TinyProject tiny("stamp_buffer");
    IndexService service(DiskLoader);
    service.SetProject(tiny.Parse());
    service.SetWatcherActive(false);
    ASSERT_NE(service.Get(), nullptr);

    // 未保存的缓冲区改动（磁盘上还是旧内容）
    service.OnFileChanged(tiny.File(1), VerilogModule("m2", "marker_buffer"));
    const IndexService::Snapshot after_buffer = service.Get();
    const fpgalsp::hdl::Module* m2 = after_buffer->ModuleByName("m2");
    ASSERT_NE(m2, nullptr);
    bool has_buffer_marker = false;
    for (const fpgalsp::hdl::Port& port : m2->ports) {
        if (port.name == "marker_buffer") has_buffer_marker = true;
    }
    EXPECT_TRUE(has_buffer_marker);
    EXPECT_EQ(service.stats().file_updates, 1u);

    // 再 Get 几次：磁盘没变 -> 不能把缓冲区内容覆盖回旧内容
    for (int i = 0; i < 3; ++i) {
        const IndexService::Snapshot again = service.Get();
        ASSERT_NE(again, nullptr);
        EXPECT_EQ(again.get(), after_buffer.get()) << "磁盘没变时 Get 不应产生新快照";
    }
    EXPECT_EQ(service.stats().file_updates, 1u);
}

TEST(IndexServiceTest, WatcherActiveSkipsStampValidation) {
    TinyProject tiny("watcher_active");
    IndexService service(DiskLoader);
    service.SetProject(tiny.Parse());
    service.SetWatcherActive(true);
    ASSERT_NE(service.Get(), nullptr);
    const std::uint64_t generation = service.stats().generation;

    // 磁盘变了但 watcher 说没变（没收到通知）-> 不捡变化（信任 watcher）
    const fs::path target(tiny.File(0));
    WriteText(target, VerilogModule("m1", "marker_unnotified"));
    const IndexService::Snapshot after = service.Get();
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(service.stats().generation, generation);
}

// -----------------------------------------------------------------------------
// IndexService：并发（多读 + 单写）
// -----------------------------------------------------------------------------

TEST(IndexServiceTest, ConcurrentGetWhileUpdating) {
    TinyProject tiny("concurrent");
    IndexService service(DiskLoader);
    service.SetProject(tiny.Parse());
    service.SetWatcherActive(true);  // 只测快照并发，不掺目录枚举
    ASSERT_NE(service.Get(), nullptr);

    constexpr int kReaders = 6;
    constexpr int kUpdates = 40;
    std::atomic<bool> stop{false};
    std::atomic<int> reads{0};
    std::atomic<int> bad_snapshots{0};

    std::vector<std::thread> readers;
    readers.reserve(kReaders);
    for (int i = 0; i < kReaders; ++i) {
        readers.emplace_back([&]() {
            while (!stop.load(std::memory_order_relaxed)) {
                const IndexService::Snapshot snapshot = service.Get();
                if (snapshot == nullptr) {
                    bad_snapshots.fetch_add(1);
                    continue;
                }
                // 一直在锁外跑查询：读到的必须是完整一致的索引
                const std::size_t files = snapshot->FileCount();
                if (files != 3) bad_snapshots.fetch_add(1);
                if (snapshot->ModuleByName("m1") == nullptr) bad_snapshots.fetch_add(1);
                // 深一点的遍历（会碰到 vector/map）
                (void)snapshot->DocumentSymbols(tiny.File(0));
                (void)snapshot->SearchSymbols("m");
                (void)snapshot->AllInstances();
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (int i = 0; i < kUpdates; ++i) {
        const std::string text = VerilogModule("m2", "marker_" + std::to_string(i));
        service.OnFileChanged(tiny.File(1), text);
        if (i % 7 == 0) {
            service.OnWatchedFiles({}, {});
        }
    }
    stop.store(true, std::memory_order_relaxed);
    for (std::thread& reader : readers) reader.join();

    EXPECT_EQ(bad_snapshots.load(), 0);
    EXPECT_GT(reads.load(), 0);
    const IndexService::Stats stats = service.stats();
    EXPECT_EQ(stats.rebuilds, 1u);
    EXPECT_EQ(stats.file_updates, static_cast<std::size_t>(kUpdates));
    EXPECT_EQ(stats.generation, 1u + static_cast<std::uint64_t>(kUpdates));

    const IndexService::Snapshot final_snapshot = service.Get();
    ASSERT_NE(final_snapshot, nullptr);
    EXPECT_EQ(final_snapshot->FileCount(), 3u);
}

// -----------------------------------------------------------------------------
// 并行冷构建：结果与顺序构建完全一致
// -----------------------------------------------------------------------------

namespace {

// 一个合成工程：N 个文件，m0001 被其余模块例化
struct SyntheticProject {
    fs::path dir;
    std::string al;
    std::vector<std::string> rel_files;

    explicit SyntheticProject(const std::string& name, int files) : dir(TempRoot(name)) {
        rel_files.reserve(static_cast<std::size_t>(files));
        for (int i = 1; i <= files; ++i) {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "m%04d", i);
            const std::string module = buffer;
            std::string text;
            if (i == 1) {
                text = "module m0001 (input clk, output q);\n  assign q = clk;\nendmodule\n";
            } else {
                text = "module " + module + " (input clk, output q);\n"
                       "  m0001 u_core (.clk(clk), .q(q));\n"
                       "  assign q = clk;\nendmodule\n";
            }
            const std::string rel = "src/" + module + ".v";
            WriteText(dir / rel, text);
            rel_files.push_back(rel);
        }
        al = (dir / "p.al").generic_string();
        WriteText(dir / "p.al", AlXml(dir, rel_files, "m0001"));
    }

    Project Parse() const { return fpgalsp::project::ParseAl(al); }
};

}  // namespace

TEST(IndexServiceTest, ParallelBuildMatchesSequential) {
    SyntheticProject synthetic("parallel_equiv", 60);
    const Project project = synthetic.Parse();
    ASSERT_EQ(fpgalsp::hdl::VerilogFilesFor(project).size(), 60u);

    BuildOptions sequential;
    ParseCache sequential_cache;
    sequential.cache = &sequential_cache;

    BuildOptions parallel;
    ParseCache parallel_cache;
    parallel.cache = &parallel_cache;
    parallel.parse_threads = fpgalsp::hdl::RecommendedParseThreads();
    ASSERT_GE(parallel.parse_threads, 1u);

    const auto seq = BuildIndex(project, DiskLoader, sequential);
    const auto par = BuildIndex(project, DiskLoader, parallel);

    EXPECT_EQ(seq.files_indexed, par.files_indexed);
    EXPECT_EQ(seq.parse_error_count, par.parse_error_count);
    EXPECT_EQ(seq.truncated, par.truncated);
    ExpectSameIndex(seq.index, par.index);
    EXPECT_EQ(seq.index.Paths(), par.index.Paths());
    EXPECT_EQ(par.index.FileCount(), 60u);
}

TEST(IndexServiceTest, ColdBuildUsesParallelOptionAndIndexesEverything) {
    SyntheticProject synthetic("parallel_cold", 80);
    IndexService service(DiskLoader);
    service.SetProject(synthetic.Parse());

    const IndexService::Snapshot snapshot = service.Get();
    ASSERT_NE(snapshot, nullptr);
    EXPECT_EQ(snapshot->FileCount(), 80u);
    EXPECT_EQ(snapshot->ModuleCount(), 80u);
    EXPECT_EQ(service.stats().rebuilds, 1u);
    EXPECT_GE(fpgalsp::hdl::RecommendedParseThreads(), 1u);
}

// -----------------------------------------------------------------------------
// 真实语料等价性：增量快照 vs 全量重建（documentSymbol 逐字段）
// -----------------------------------------------------------------------------

TEST(IndexServiceTest, MatchesFullBuildOnRealCorpus) {
    const std::string al = std::string(FPGA_LSP_SOURCE_DIR) +
                           "/tests/gate-corpus/corpus_alpha/demo_project.al";
    if (!std::filesystem::exists(al)) {
        GTEST_SKIP() << "语料缺失: " << al;
    }
    const Project project = fpgalsp::project::ParseAl(al);
    ASSERT_FALSE(project.al_path.empty()) << "语料工程解析失败";
    ASSERT_GE(fpgalsp::hdl::VerilogFilesFor(project).size(), 10u);

    // 缓冲区优先的 loader：模拟"某个文件在编辑器里被改过"
    std::string edited_path;
    std::string edited_text;
    const auto load = [&](const std::string& path) -> std::optional<std::string> {
        if (!edited_path.empty() && path == edited_path) return edited_text;
        return DiskLoader(path);
    };

    IndexService service(load);
    service.SetProject(project);
    service.SetWatcherActive(true);
    const IndexService::Snapshot cold = service.Get();
    ASSERT_NE(cold, nullptr);

    const auto expect_matches_full_build = [&](const IndexService::Snapshot& snapshot,
                                               const char* stage) {
        const auto fresh = BuildIndex(project, load);
        EXPECT_EQ(snapshot->FileCount(), fresh.index.FileCount()) << stage;
        EXPECT_EQ(snapshot->Paths(), fresh.index.Paths()) << stage;
        EXPECT_EQ(snapshot->ModuleNames(), fresh.index.ModuleNames()) << stage;
        for (const std::string& path : snapshot->Paths()) {
            const std::string incremental =
                fpgalsp::server::features::DocumentSymbols(*snapshot, path).dump();
            const std::string rebuilt =
                fpgalsp::server::features::DocumentSymbols(fresh.index, path).dump();
            EXPECT_EQ(incremental, rebuilt) << stage << " @ " << path;
        }
    };

    expect_matches_full_build(cold, "冷构建");

    // 改一个真实文件（只在缓冲区里）：增量结果必须与"用同一份文本全量重建"一致
    const std::vector<std::string> verilog = fpgalsp::hdl::VerilogFilesFor(project);
    ASSERT_FALSE(verilog.empty());
    edited_path = verilog.front();
    const std::optional<std::string> original = DiskLoader(edited_path);
    ASSERT_TRUE(original.has_value());
    edited_text = *original + "\n// A1 增量等价性探针\nmodule a1_probe_module (input wire probe_in);\nendmodule\n";

    service.OnFileChanged(edited_path, edited_text);
    const IndexService::Snapshot updated = service.Get();
    ASSERT_NE(updated, nullptr);
    EXPECT_EQ(service.stats().rebuilds, 1u);
    EXPECT_NE(updated->ModuleByName("a1_probe_module"), nullptr);
    expect_matches_full_build(updated, "单文件增量后");

    // 再来一次（覆盖同一个文件的第二个版本），确认不是"一次性巧合"
    edited_text = *original + "\nmodule a1_probe_module (input wire probe_in, output probe_out);\nendmodule\n";
    service.OnFileChanged(edited_path, edited_text);
    expect_matches_full_build(service.Get(), "第二次增量后");
    EXPECT_EQ(service.stats().file_updates, 2u);
}

// -----------------------------------------------------------------------------
// 基准（env 门控）：didChange→completion 的 before / after
// -----------------------------------------------------------------------------
//  运行：set FPGALSP_BENCH_INDEX=1 & unit_index_service --gtest_filter=*Bench*
//  可选：FPGALSP_BENCH_SIZES=200,1000  FPGALSP_BENCH_ITERATIONS=30
//
//  "before" = Phase B 之后、A1 之前的真实路径（LspServer::IndexForProject）：
//       ProjectSignature(每请求目录枚举 + 拼串) + BuildIndex(全量，ParseCache 命中)
//       + features::Completion
//  "after"  = 本任务：OnFileChanged(单文件) + Get() + features::Completion
//  两者用同一份合成工程、同一份 loader（缓冲区优先）、同一处 completion 调用。

namespace {

using Clock = std::chrono::steady_clock;

double MsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

double Percentile(std::vector<double> samples, double p) {
    if (samples.empty()) return 0.0;
    std::sort(samples.begin(), samples.end());
    const std::size_t index = static_cast<std::size_t>(
        std::min<double>(static_cast<double>(samples.size()) - 1.0,
                         std::ceil(p * static_cast<double>(samples.size())) - 1.0));
    return samples[index];
}

// 复刻 LspServer::ProjectSignature 的成本（B8 之后的实现：每目录一次枚举 + 拼串）
std::string CurrentProjectSignature(const Project& project,
                                    const std::map<std::string, std::string>& buffers) {
    std::string signature = project.al_path;
    std::vector<std::string> files = fpgalsp::hdl::VerilogFilesFor(project);
    for (const std::string& path : fpgalsp::hdl::VhdlFilesFor(project)) files.push_back(path);
    signature.reserve(signature.size() + files.size() * 48);

    // 每个目录只枚举一次，带回整目录的 (size, mtime)
    std::vector<std::string> dirs;
    for (const std::string& path : files) {
        const std::string dir = fpgalsp::platform::path_dirname(path);
        if (std::find(dirs.begin(), dirs.end(), dir) == dirs.end()) dirs.push_back(dir);
    }
    std::map<std::string, std::pair<std::uintmax_t, std::int64_t>> stamps;  // key -> (size, mtime)
    for (const std::string& dir : dirs) {
        std::error_code ec;
        std::filesystem::directory_iterator iter{std::filesystem::path(dir), ec};
        if (ec) continue;
        const std::filesystem::directory_iterator end;
        for (; iter != end; iter.increment(ec)) {
            if (ec) break;
            const std::filesystem::directory_entry& entry = *iter;
            std::error_code entry_ec;
            const auto size = entry.file_size(entry_ec);
            if (entry_ec) continue;
            const auto write_time = entry.last_write_time(entry_ec);
            if (entry_ec) continue;
            const std::string full = fpgalsp::platform::path_key(
                fpgalsp::platform::normalize_path(entry.path().generic_string()));
            stamps[full] = {size,
                            static_cast<std::int64_t>(write_time.time_since_epoch().count())};
        }
    }

    for (const std::string& path : files) {
        signature += '\n';
        signature += path;
        signature += '|';
        if (const auto it = buffers.find(path); it != buffers.end()) {
            signature += 'v';
            signature += std::to_string(it->second.size());
            continue;
        }
        const std::string key = fpgalsp::platform::path_key(
            fpgalsp::platform::normalize_path(path));
        const auto stamp = stamps.find(key);
        if (stamp == stamps.end()) {
            signature += "missing";
            continue;
        }
        signature += std::to_string(stamp->second.first);
        signature += ':';
        signature += std::to_string(stamp->second.second);
    }
    return signature;
}

}  // namespace

TEST(IndexServiceBench, DidChangeToCompletion) {
    const char* enabled = std::getenv("FPGALSP_BENCH_INDEX");
    if (enabled == nullptr || std::string(enabled) != "1") {
        GTEST_SKIP() << "设置 FPGALSP_BENCH_INDEX=1 运行基准";
    }

    int iterations = 30;
    if (const char* text = std::getenv("FPGALSP_BENCH_ITERATIONS")) {
        iterations = std::max(1, std::atoi(text));
    }
    std::vector<int> sizes{200, 1000};
    if (const char* text = std::getenv("FPGALSP_BENCH_SIZES")) {
        sizes.clear();
        std::string spec(text);
        std::size_t begin = 0;
        while (begin <= spec.size()) {
            const std::size_t end = spec.find(',', begin);
            const std::string item =
                spec.substr(begin, (end == std::string::npos ? spec.size() : end) - begin);
            if (!item.empty()) sizes.push_back(std::atoi(item.c_str()));
            if (end == std::string::npos) break;
            begin = end + 1;
        }
    }

    for (const int files : sizes) {
        if (files <= 0) continue;
        SyntheticProject synthetic("bench_" + std::to_string(files), files);
        const Project project = synthetic.Parse();
        const std::string edited_path = (synthetic.dir / synthetic.rel_files[1]).generic_string();

        // 预热 OS page cache（BENCH.md §9.6 的教训：先跑的一侧白捡预热）
        for (const std::string& rel : synthetic.rel_files) {
            (void)ReadText(synthetic.dir / rel);
        }

        // 缓冲区：didChange 之后只有这一个文件的内容与磁盘不同
        std::map<std::string, std::string> buffers;

        // ---- before：ProjectSignature + 全量 BuildIndex（ParseCache 已预热） ----
        ParseCache cache;
        BuildOptions options;
        options.cache = &cache;
        const auto load_before = [&](const std::string& path) -> std::optional<std::string> {
            if (const auto it = buffers.find(path); it != buffers.end()) return it->second;
            return DiskLoader(path);
        };
        const auto cold_before_start = Clock::now();
        (void)BuildIndex(project, load_before, options);
        const double cold_before = MsSince(cold_before_start);

        std::vector<double> before;
        before.reserve(static_cast<std::size_t>(iterations));
        for (int i = 0; i < iterations; ++i) {
            buffers[edited_path] = VerilogModule("m0002", "marker_" + std::to_string(i));
            const auto start = Clock::now();
            const std::string signature = CurrentProjectSignature(project, buffers);
            const auto build = BuildIndex(project, load_before, options);
            const auto symbols = fpgalsp::server::features::Completion(
                build.index, edited_path, buffers[edited_path], 0, 8);
            before.push_back(MsSince(start));
            EXPECT_FALSE(signature.empty());
            EXPECT_TRUE(symbols.contains("items"));
        }

        // ---- after：IndexService 增量 ----
        buffers.clear();
        IndexService service(DiskLoader);
        service.SetProject(project);
        service.SetWatcherActive(true);  // 有 watcher：Get 不做目录枚举
        const auto cold_after_start = Clock::now();
        IndexService::Snapshot snapshot = service.Get();
        const double cold_after = MsSince(cold_after_start);
        ASSERT_NE(snapshot, nullptr);
        ASSERT_EQ(snapshot->FileCount(), static_cast<std::size_t>(files));

        std::vector<double> after;
        after.reserve(static_cast<std::size_t>(iterations));
        for (int i = 0; i < iterations; ++i) {
            const std::string text = VerilogModule("m0002", "marker_" + std::to_string(i));
            const auto start = Clock::now();
            // 集成后的真实形状：ProjectForPath 每次都会给出 Project（可能同一份），
            // IndexService 对"工程文件集合没变"的 SetProject 是空操作。
            service.SetProject(project);
            service.OnFileChanged(edited_path, text);
            snapshot = service.Get();
            const auto symbols =
                fpgalsp::server::features::Completion(*snapshot, edited_path, text, 0, 8);
            after.push_back(MsSince(start));
            EXPECT_TRUE(symbols.contains("items"));
        }

        // ---- watcher 缺失时的回退成本（一次目录枚举 + N 次比对，无变化） ----
        service.SetWatcherActive(false);
        (void)service.Get();  // 建立基线
        std::vector<double> fallback;
        fallback.reserve(static_cast<std::size_t>(iterations));
        for (int i = 0; i < iterations; ++i) {
            const auto start = Clock::now();
            snapshot = service.Get();
            fallback.push_back(MsSince(start));
        }

        const auto stats = service.stats();
        std::printf(
            "[bench] files=%d iterations=%d cold_before=%.2fms cold_after=%.2fms "
            "before_p50=%.3fms before_p95=%.3fms after_p50=%.3fms after_p95=%.3fms "
            "speedup=%.2fx watcher_missing_get_p50=%.3fms stats={generation:%llu rebuilds:%zu "
            "file_updates:%zu}\n",
            files, iterations, cold_before, cold_after, Percentile(before, 0.50),
            Percentile(before, 0.95), Percentile(after, 0.50), Percentile(after, 0.95),
            Percentile(after, 0.50) > 0.0 ? Percentile(before, 0.50) / Percentile(after, 0.50)
                                         : 0.0,
            Percentile(fallback, 0.50),
            static_cast<unsigned long long>(stats.generation), stats.rebuilds,
            stats.file_updates);
        std::fflush(stdout);
    }
}

}  // namespace
// =============================================================================
//  P-C1：indexChanged 的变化摘要（客户端据此就地打补丁）
// =============================================================================
TEST(IndexServiceTest, ChangeSetReportsPatchedModules) {
    TinyProject tiny("changeset_patch");
    IndexService service(DiskLoader);
    service.SetProject(tiny.Parse());
    ASSERT_NE(service.Get(), nullptr);

    // 同一文件、同一模块名，只改端口集合 -> 可以就地打补丁（full=false）
    service.OnFileChanged(tiny.File(1), VerilogModule("m2", "marker_b"));

    const IndexService::ChangeSet changes = service.LastChanges();
    ASSERT_EQ(changes.paths.size(), 1u);
    EXPECT_FALSE(changes.full) << "模块名集合没变，不该要求整树重取";
    ASSERT_EQ(changes.modules.size(), 1u);
    EXPECT_EQ(changes.modules[0].module, "m2");
    EXPECT_TRUE(changes.modules[0].defined);
    // VerilogModule 声明了 clk / marker_b / q 三个端口
    EXPECT_EQ(changes.modules[0].port_count, 3);
}

TEST(IndexServiceTest, ChangeSetMarksModuleRenameAsFull) {
    TinyProject tiny("changeset_rename");
    IndexService service(DiskLoader);
    service.SetProject(tiny.Parse());
    ASSERT_NE(service.Get(), nullptr);

    // 同一文件里 m2 改名 -> 模块集合变化，客户端必须整树重取
    service.OnFileChanged(tiny.File(1), VerilogModule("m2_renamed", "clk"));

    const IndexService::ChangeSet changes = service.LastChanges();
    EXPECT_TRUE(changes.full) << "模块消失/改名必须整树重取，否则树上留幽灵节点";
}

TEST(IndexServiceTest, ChangeSetMarksModuleAdditionAsFull) {
    TinyProject tiny("changeset_addition");
    IndexService service(DiskLoader);
    service.SetProject(tiny.Parse());
    ASSERT_NE(service.Get(), nullptr);

    // 同一文件里多定义一个模块 -> 新节点插不进"就地补丁"，必须整树重取
    const std::string two_modules = VerilogModule("m1", "clk") + VerilogModule("m4", "clk");
    service.OnFileChanged(tiny.File(0), two_modules);

    const IndexService::ChangeSet changes = service.LastChanges();
    EXPECT_TRUE(changes.full) << "新增模块必须整树重取";
}

// =============================================================================
//  B-B2：时序端点 -> 源码（服务端注入 HDL 索引后重算）
// =============================================================================
TEST(IndexServiceTest, ResolvesTimingEndpointsAgainstTheIndex) {
    TinyProject tiny("timing_endpoints");
    IndexService service(DiskLoader);
    service.SetProject(tiny.Parse());
    const IndexService::Snapshot index = service.Get();
    ASSERT_NE(index, nullptr);

    // m1 = top，例化了 m2/u2 与 m3/u3（见 TinyProject 夹具）
    const fpgalsp::project::Project project = tiny.Parse();
    ASSERT_EQ(project.top, "m1");

    nlohmann::json data = nlohmann::json::object();
    nlohmann::json timing = nlohmann::json::object();
    nlohmann::json paths = nlohmann::json::array();
    nlohmann::json path = nlohmann::json::object();
    // 解析得到：m2 的端口 clk（u2 是 m1 里的实例名）
    path["begin_at"] = {{"signal", "clk"}, {"hierarchy", {"u2"}}, {"resolved", false},
                        {"reason", "hdl-index-unavailable"}};
    // 解析不到：m1 里没有这个信号
    path["end_at"] = {{"signal", "no_such_signal"}, {"hierarchy", nlohmann::json::array()},
                      {"resolved", false}, {"reason", "hdl-index-unavailable"}};
    paths.push_back(path);
    timing["paths"] = paths;
    data["timing"] = timing;

    fpgalsp::server::features::ResolveTimingEndpoints(*index, project, data);

    const nlohmann::json& patched = data["timing"]["paths"][0];
    EXPECT_TRUE(patched["begin_at"]["resolved"].get<bool>()) << patched["begin_at"].dump();
    EXPECT_EQ(patched["begin_at"]["module"].get<std::string>(), "m2");
    EXPECT_FALSE(patched["begin_at"]["file"].is_null());
    EXPECT_FALSE(patched["begin_at"]["reason"].is_string()) << "解析成功时不应再带 reason";

    EXPECT_FALSE(patched["end_at"]["resolved"].get<bool>());
    EXPECT_EQ(patched["end_at"]["reason"].get<std::string>(), "signal-not-in-module");

    const nlohmann::json& counts = data["timing"]["begin_end_resolved"];
    EXPECT_EQ(counts["total"].get<int>(), 2);
    EXPECT_EQ(counts["resolved"].get<int>(), 1);
}
