// =============================================================================
//  probe_latency.cpp —— 端到端 LSP 延迟的服务端分段（task-28 / perf-latency）
// =============================================================================
//  用途：客户端只能量到"didChange 发出 → 诊断推送 → 响应到达"这三段；
//        本探针把服务端内部再拆细，回答"键按下去以后，CPU 花在哪个函数"。
//
//  测的东西（全部用内部 API 复刻真实 handler 做的事，不改产品代码）：
//    1. 编辑链路：DocumentStore::Change / Snapshot / GetText / IndexService::OnFileChanged
//    2. 查询链路：features::<Method>() 的**构造 JSON** 成本 vs jsonrpc::Serialize() 成本
//       —— 这两段在线上请求里是混在一起的，客户端根本分不开。
//    3. 每个阶段的占比用 perf::PhaseLedger 打出来（一次典型 completion 请求）。
//
//  用法：
//      build\bin\perf_latency.exe --project %TEMP%\fpgalsp-perf\latency\scale200
//      （工程由 tools/perf/latency/latency_driver.py 生成；也可以用 %WORKSPACE%\CorpusAlpha）
// =============================================================================
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "perf_common.h"

#include "hdl/hdl_index.h"
#include "jsonrpc/json_util.h"
#include "lsp/document_store.h"
#include "project/al_project.h"
#include "server/features.h"
#include "server/index_service.h"

// 本探针在全局作用域里直接用内部 API：统一引进 fpgalsp 命名空间，少写前缀。
using namespace fpgalsp;  // NOLINT(google-build-using-namespace)

namespace {

std::string Uri(const std::string& path) {
    return "file:///" + path;
}

// 与 tools/perf/latency/latency_driver.py 的取点保持一致：
// 模块声明行的名字列 + 一个模块体内的补全位置。
void FindAnchors(const std::string& text, int* decl_line, int* decl_col,
                 int* body_line) {
    *decl_line = 0;
    *decl_col = 0;
    *body_line = 0;
    std::size_t pos = 0;
    int line = 0;
    while (pos <= text.size()) {
        const std::size_t end = text.find('\n', pos);
        const std::string_view row(text.data() + pos,
                                   (end == std::string::npos ? text.size() : end) - pos);
        if (row.find("module synth_m0001") != std::string_view::npos) {
            const std::size_t at = row.find("module synth_m0001");
            *decl_line = line;
            *decl_col = static_cast<int>(at + 7);
        }
        if (row.find("acc <= acc + din;") != std::string_view::npos) *body_line = line;
        if (end == std::string::npos) break;
        pos = end + 1;
        ++line;
    }
}

}  // namespace

int main(int argc, char** argv) {
    const perf::Cli cli = perf::ParseCli(argc, argv);
    perf::Banner("latency");

    std::string project_dir = cli.extra;
    if (project_dir.empty() || !std::filesystem::is_directory(project_dir)) {
        std::printf("usage: perf_latency.exe --iters N --project <synthetic project dir>\n");
        return 2;
    }
    std::string al_path;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(project_dir, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".al") {
            al_path = entry.path().string();
            break;
        }
    }
    if (al_path.empty()) {
        std::printf("no .al in %s\n", project_dir.c_str());
        return 2;
    }

    project::Project project = project::ParseAl(al_path);
    std::printf("project: %s  files=%zu\n", project.name.c_str(), project.HdlFiles().size());

    // ---- 索引（与线上一致：IndexService + 读盘 loader）----------------------
    server::IndexService index_service;
    const auto t_build0 = perf::Clock::now();
    index_service.SetProject(project);
    const server::IndexService::Snapshot snapshot = index_service.Get();
    const double build_ms = perf::MsSince(t_build0);
    std::printf("index build: %.1f ms  files=%zu modules=%zu\n\n",
                build_ms, snapshot->FileCount(), snapshot->ModuleCount());

    // 选一个"中等大小"的文件做编辑/查询锚点
    const std::string target = project.HdlFiles().empty()
                                   ? std::string()
                                   : project.HdlFiles().front()->path;
    std::string aligned_target;
    for (const auto* file : project.HdlFiles()) {
        if (file->path.find("synth_m0001") != std::string::npos) aligned_target = file->path;
    }
    if (!aligned_target.empty()) {
        // 与 python 驱动用的同一个文件
    }
    const std::string path = aligned_target.empty() ? target : aligned_target;
    const std::string text0 = perf::ReadFile(path);
    int decl_line = 0, decl_col = 0, body_line = 0;
    FindAnchors(text0, &decl_line, &decl_col, &body_line);
    std::printf("anchor file: %s  (%zu bytes) decl=%d:%d body=%d\n\n", path.c_str(),
                text0.size(), decl_line, decl_col, body_line);

    // ---- 1. 编辑链路的分段 -------------------------------------------------
    lsp::DocumentStore docs;
    docs.Open(Uri(path), text0, 1, "verilog", path);
    lsp::ContentChange change;
    change.text = text0;  // 全文替换（与驱动脚本的 didChange 一致）
    change.has_range = false;

    auto st_change = perf::Bench("edit: DocumentStore::Change(fullext)", cli.iters, [&] {
        docs.Change(Uri(path), {change}, 2);
    }, cli.warmup);
    st_change.Print();
    st_change.PrintCsv();

    auto st_snapshot = perf::Bench("edit: DocumentStore::Snapshot", cli.iters, [&] {
        const lsp::DocumentSnapshot snap = docs.Snapshot(Uri(path));
        if (!snap.valid()) std::printf("invalid\n");
    }, cli.warmup);
    st_snapshot.Print();
    st_snapshot.PrintCsv();

    auto st_gettext = perf::Bench("edit: DocumentStore::GetText", cli.iters, [&] {
        const auto t = docs.GetText(Uri(path));
        if (!t.has_value()) std::printf("missing\n");
    }, cli.warmup);
    st_gettext.Print();
    st_gettext.PrintCsv();

    auto st_onfile = perf::Bench("edit: IndexService::OnFileChanged", cli.iters, [&] {
        index_service.OnFileChanged(path, text0);
    }, cli.warmup);
    st_onfile.Print();
    st_onfile.PrintCsv();

    // ---- 1b. 读盘：completion 每次走 LoadText(path) —— 未打开的文档直接读盘 ----
    //  历史 HOTSPOTS §4.3 的指控：DocumentStore 没有文本缓存。这里分别量"小文件/100 KB"。
    {
        const std::filesystem::path tmp = std::filesystem::temp_directory_path() /
                                          "fpgalsp-perf-latency-readcost.v";
        std::string big = text0;
        big.reserve(120 * 1024);
        while (big.size() < 100 * 1024) big += "// padding line for read-cost probe\n";
        std::FILE* fp = nullptr;
        if (_wfopen_s(&fp, tmp.wstring().c_str(), L"wb") == 0 && fp != nullptr) {
            std::fwrite(big.data(), 1, big.size(), fp);
            std::fclose(fp);
        }
        const std::string big_path = tmp.string();
        std::printf("\nread-cost probe file: %zu bytes (%s)\n", big.size(), big_path.c_str());
        auto st_small = perf::Bench("read: GetText(closed, small file)", cli.iters, [&] {
            const auto t = docs.GetText(Uri(path));
            if (!t.has_value()) std::printf("missing\n");
        }, cli.warmup);
        st_small.Print();
        st_small.PrintCsv();
        auto st_big = perf::Bench("read: GetText(closed, ~100KB)", cli.iters, [&] {
            const auto t = docs.GetText(Uri(big_path));
            if (!t.has_value()) std::printf("missing\n");
        }, cli.warmup);
        st_big.Print();
        st_big.PrintCsv();
        std::error_code rm_ec;
        std::filesystem::remove(tmp, rm_ec);
    }

    // ---- 2. 查询链路：构造 JSON vs 序列化 --------------------------------
    const hdl::HdlIndex& index = *snapshot;
    struct Case {
        std::string label;
        std::function<jsonrpc::Json()> build;
    };
    const std::vector<Case> cases = {
        {"completion", [&] {
             return server::features::Completion(index, path, text0, body_line, 8);
         }},
        {"documentSymbol", [&] { return server::features::DocumentSymbols(index, path); }},
        {"hover", [&] { return server::features::Hover(index, path, decl_line, decl_col); }},
        {"definition", [&] { return server::features::Definition(index, path, decl_line, decl_col); }},
        {"references", [&] {
             return server::features::References(index, path, decl_line, decl_col, true);
         }},
        {"inlayHint", [&] { return server::features::InlayHints(index, path); }},
        {"workspaceSymbol", [&] { return server::features::WorkspaceSymbols(index, "synth_m"); }},
        {"moduleTree", [&] { return server::features::ModuleTree(index, project, false); }},
    };

    std::printf("\n%-16s %12s %12s %10s %12s\n", "method", "feature_ms", "serialize_ms",
                "payload", "feature%");
    for (const Case& item : cases) {
        // 先跑一次拿到 payload 大小，之后两段分别计时。
        jsonrpc::Json value = item.build();
        const std::size_t payload = jsonrpc::Serialize(value, false).size();

        auto st_feature = perf::Bench("feature:" + item.label, cli.iters, [&] {
            const jsonrpc::Json fresh = item.build();
            if (fresh.is_null()) std::printf("null\n");
        }, cli.warmup);
        auto st_serialize = perf::Bench("serialize:" + item.label, cli.iters, [&] {
            const std::string body = jsonrpc::Serialize(value, false);
            if (body.empty()) std::printf("empty\n");
        }, cli.warmup);
        const double share = st_feature.P50() + st_serialize.P50() > 0
                                 ? 100.0 * st_feature.P50() /
                                       (st_feature.P50() + st_serialize.P50())
                                 : 0.0;
        std::printf("%-16s %12.3f %12.3f %10zu %11.1f%%\n", item.label.c_str(),
                    st_feature.P50(), st_serialize.P50(), payload, share);
        std::printf("csv,feature:%s,%zu,%.4f,%.4f,%.4f,%.4f\n", item.label.c_str(),
                    st_feature.N(), st_feature.P50(), st_feature.P95(), st_feature.Max(),
                    st_feature.Mean());
        std::printf("csv,serialize:%s,%zu,%.4f,%.4f,%.4f,%.4f\n", item.label.c_str(),
                    st_serialize.N(), st_serialize.P50(), st_serialize.P95(),
                    st_serialize.Max(), st_serialize.Mean());
    }

    // ---- 3. 一次典型 completion 请求的阶段账本 ---------------------------
    perf::PhaseLedger ledger;
    for (int i = 0; i < 10; ++i) {
        auto t = perf::Clock::now();
        docs.Change(Uri(path), {change}, 10 + i);
        ledger.Add("1 docs.Change", perf::MsSince(t));
        t = perf::Clock::now();
        index_service.OnFileChanged(path, text0);
        ledger.Add("2 index.OnFileChanged", perf::MsSince(t));
        t = perf::Clock::now();
        const server::IndexService::Snapshot snap = index_service.Get();
        ledger.Add("3 IndexService::Get", perf::MsSince(t));
        t = perf::Clock::now();
        const jsonrpc::Json value =
            server::features::Completion(*snap, path, text0, body_line, 8);
        ledger.Add("4 features::Completion", perf::MsSince(t));
        t = perf::Clock::now();
        const std::string body = jsonrpc::Serialize(value, false);
        ledger.Add("5 Serialize", perf::MsSince(t));
        if (body.empty()) std::printf("empty\n");
    }
    ledger.Print("one completion request (10 samples summed)");

    std::printf("\nworking_set=%.2f MB peak_working_set=%.2f MB\n",
                perf::Mb(perf::CurrentWorkingSetBytes()),
                perf::Mb(perf::PeakWorkingSetBytes()));
    return 0;
}