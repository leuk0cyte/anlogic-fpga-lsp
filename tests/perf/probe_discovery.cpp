// =============================================================================
//  tests/perf/probe_discovery.cpp —— 工程发现（FindProjectAl / ProjectForPath）成本
// =============================================================================
//  背景（perf-index 报告 §6 指出的缺口）：
//      LspServer::ProjectForPath() 在真正干活之前，先要回答"这个文件属于哪个 .al"。
//      它的缓存是 (configured|root|path, 5 s) —— **按 path 分键**，所以：
//        * 同一个文件连续请求：命中（便宜）；
//        * 不同文件交替请求（编辑器常态）：**每次都 miss**，每次都重做工程发现。
//      其他 5 条线测的"每请求固定开销"都不含这段 → 他们的数字是**下界**。
//
//  本探针只调 `project::FindProjectAl / FindAl / DiscoverProjects / Affinity`
//  （ProjectForPath 内部用的就是它们，见 src/server/lsp_server.cpp:1379-1396），
//  用同一套缓存键策略做**模拟**比较，不改产品代码。
//
//  构建： pwsh -File tools\perf\build.ps1 -Target perf_discovery
//  运行： build\bin\perf_discovery.exe [--out <dir>] [--iters 200]
//
//  输出：stdout 的 markdown 表 + csv 行；--out 目录下 discovery.csv / summary.json
// =============================================================================

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "platform/path.h"
#include "project/al_project.h"

namespace fs = std::filesystem;

namespace {

double NowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

struct Stats {
    std::vector<double> v;
    void Add(double ms) { v.push_back(ms); }
    double Pct(double p) const {
        if (v.empty()) return 0.0;
        std::vector<double> s = v;
        std::sort(s.begin(), s.end());
        const size_t idx = static_cast<size_t>(p * (s.size() - 1) + 0.5);
        return s[std::min(idx, s.size() - 1)];
    }
    double Mean() const {
        if (v.empty()) return 0.0;
        double sum = 0;
        for (double x : v) sum += x;
        return sum / static_cast<double>(v.size());
    }
};

const char* kAlTemplate =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<Project Version=\"2\" Path=\"%s\">\n"
    "  <Name>%s</Name>\n"
    "  <HardWare><Family>EF2</Family><Device>EF2L45LG144B</Device><Speed>-2</Speed></HardWare>\n"
    "  <TOP_MODULE><MODULE>top</MODULE></TOP_MODULE>\n"
    "  <Source_Files><Verilog>\n"
    "    <File Path=\"rtl/top.v\"><FileInfo><Attr Name=\"CompileOrder\" Val=\"1\"/></FileInfo></File>\n"
    "  </Verilog></Source_Files>\n"
    "</Project>\n";

const char* kVerilog = "module top(input wire clk, output reg [7:0] q);\n"
                       "  always @(posedge clk) q <= q + 8'h1;\nendmodule\n";

// 建一个工作区：na 个工程，每个 <root>/p<i>/proj<i>.al + rtl/top.v
// nested=false 时把 .al 放在**工作区根的兄弟目录**，让文件的 FindAl 向上查不到（逼出 DiscoverProjects）
void MakeWorkspace(const std::string& root, int na, bool nested) {
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    for (int i = 0; i < na; ++i) {
        const fs::path proj = fs::path(root) / (nested ? "p" + std::to_string(i)
                                                       : "al_dir_" + std::to_string(i));
        fs::create_directories(proj / "rtl", ec);
        const std::string name = "proj" + std::to_string(i);
        char buf[2048];
        std::snprintf(buf, sizeof(buf), kAlTemplate, proj.generic_string().c_str(), name.c_str());
        std::ofstream(proj / (name + ".al"), std::ios::binary) << buf;
        for (int f = 0; f < 20; ++f) {
            std::ofstream(proj / "rtl" / ("m" + std::to_string(f) + ".v"), std::ios::binary) << kVerilog;
        }
    }
    if (!nested) {
        // 文件放在独立的 files/ 目录：向上 6 层没有 .al -> 只能递归扫描工作区
        fs::create_directories(fs::path(root) / "src", ec);
        for (int f = 0; f < 200; ++f) {
            std::ofstream(fs::path(root) / "src" / ("f" + std::to_string(f) + ".v"), std::ios::binary)
                << kVerilog;
        }
    }
    // 预热 page cache（README §6.4）
    for (const auto& e : fs::recursive_directory_iterator(root, ec)) {
        if (ec) break;
        if (e.is_regular_file()) {
            std::ifstream in(e.path(), std::ios::binary);
            char tmp[4096];
            while (in.read(tmp, sizeof(tmp))) {
            }
        }
    }
}

// -----------------------------------------------------------------------------
// 缓存策略模拟（只统计 miss 次数；用实测的 miss 成本换算总时间）
// -----------------------------------------------------------------------------
struct CacheSim {
    // 0 = 当前实现：key=(configured|root|path)，TTL 5 s，>64 条整体清空
    // 1 = 按目录：key=(root|dirname(path))，TTL 5 s
    // 2 = 按工程根：key=(root)，TTL 5 s
    int policy = 0;
    std::map<std::string, double> entries;
    long long hits = 0;
    long long misses = 0;
    long long clears = 0;

    std::string Key(const std::string& root, const std::string& path) const {
        if (policy == 2) return root;
        if (policy == 1) return root + "|" + std::string(fpgalsp::platform::path_dirname(path));
        return root + "|" + path;
    }
    void Access(const std::string& root, const std::string& path, double now_ms) {
        const std::string key = Key(root, path);
        const auto it = entries.find(key);
        if (it != entries.end() && now_ms - it->second < 5000.0) {
            ++hits;
            return;
        }
        ++misses;
        if (entries.size() >= 64) {  // 与 lsp_server.cpp:1391-1393 一致
            entries.clear();
            ++clears;
        }
        entries[key] = now_ms;
    }
};

struct Row {
    std::string mode;    // "FindAl-hit" / "Discover+Affinity"
    int projects = 0;
    std::string trace;   // "same-file" / "alternate"
    Stats cost;
};

double Pct(double p, const Stats& s) { return s.Pct(p); }

}  // namespace

int main(int argc, char** argv) {
    std::string out_dir = "tools/perf/discovery/out";
    int iters = 200;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) out_dir = argv[++i];
        else if (a == "--iters" && i + 1 < argc) iters = std::atoi(argv[++i]);
    }
    std::error_code ec;
    fs::create_directories(out_dir, ec);

    const fs::path base = fs::temp_directory_path(ec) / "fpgalsp-perf" / "discovery";
    fs::create_directories(base, ec);

    std::printf("============================================================\n");
    std::printf(" 工程发现成本探针（FindProjectAl / ProjectForPath）\n");
    std::printf("  workspace: %s\n", base.string().c_str());
    std::printf("  iters=%d\n", iters);
    std::printf("============================================================\n\n");

    std::vector<Row> rows;
    std::vector<std::string> csv{"csv,mode,projects,trace,n,p50_ms,p95_ms,mean_ms"};

    for (const int na : {1, 10, 50}) {
        for (const bool nested : {true, false}) {
            const std::string root =
                (base / (std::string("ws_") + std::to_string(na) + (nested ? "_nested" : "_flat"))).string();
            MakeWorkspace(root, na, nested);

            // 目标文件：nested=true -> 每个工程的 rtl/top.v（FindAl 能命中，向上 2 层）
            //           nested=false -> files/src/f*.v（FindAl 命中不了，逼出 DiscoverProjects）
            std::vector<std::string> targets;
            if (nested) {
                for (int i = 0; i < na; ++i) {
                    targets.push_back((fs::path(root) / ("p" + std::to_string(i)) / "rtl" / "top.v").string());
                }
            } else {
                for (int f = 0; f < 50; ++f) {
                    targets.push_back((fs::path(root) / "src" / ("f" + std::to_string(f) + ".v")).string());
                }
            }

            const std::string mode = nested ? "FindAl-hit" : "Discover+Affinity";

            // ---- trace 1：同一个文件重复请求 ----
            {
                Row row;
                row.mode = mode;
                row.projects = na;
                row.trace = "same-file";
                for (int i = 0; i < iters; ++i) {
                    const double t0 = NowMs();
                    (void)fpgalsp::project::FindProjectAl(root, targets[0], "");
                    row.cost.Add(NowMs() - t0);
                }
                rows.push_back(row);
            }

            // ---- trace 2：不同文件交替（编辑器常态）----
            {
                Row row;
                row.mode = mode;
                row.projects = na;
                row.trace = "alternate";
                for (int i = 0; i < iters; ++i) {
                    const std::string& target = targets[static_cast<size_t>(i) % targets.size()];
                    const double t0 = NowMs();
                    (void)fpgalsp::project::FindProjectAl(root, target, "");
                    row.cost.Add(NowMs() - t0);
                }
                rows.push_back(row);
            }

            // ---- trace 3：模拟"每次按键"（同一文件、间隔 6 s 的场景用时间戳模拟） ----
            //   真实 5 s TTL 无法在探针里等，所以用 CacheSim 的时间轴模拟：
            //   键 = path 时同一文件在 5 s 内命中、超过 5 s 重新发现。
            //   这里只报"一次 miss 的成本"，乘法交给报告。
            (void)0;
        }
    }

    std::printf("| 模式 | .al 数 | trace | p50 (µs) | p95 (µs) | mean (µs) |\n");
    std::printf("|---|---|---|---|---|---|\n");
    for (const Row& r : rows) {
        std::printf("| %s | %d | %s | %.1f | %.1f | %.1f |\n", r.mode.c_str(), r.projects,
                    r.trace.c_str(), r.cost.Pct(0.5) * 1000.0, r.cost.Pct(0.95) * 1000.0,
                    r.cost.Mean() * 1000.0);
        char buf[256];
        std::snprintf(buf, sizeof(buf), "csv,%s,%d,%s,%zu,%.4f,%.4f,%.4f", r.mode.c_str(),
                      r.projects, r.trace.c_str(), r.cost.v.size(), r.cost.Pct(0.5),
                      r.cost.Pct(0.95), r.cost.Mean());
        csv.push_back(buf);
    }

    // ---------------- 成本分解（常态 vs 边缘） ----------------
    // 常态：文件在某个 .al 目录之下 -> FindAl 向上 2 层就命中（与 .al 数量无关）
    // 边缘：文件不在任何 .al 之下（monorepo 共享文件 / 工作区根下的脚本）
    //       -> 只能 DiscoverProjects 递归扫 + 对每个候选算 Affinity
    std::printf("\n### 成本分解（FindAl 单项 / DiscoverProjects 单项）\n\n");
    std::printf("| 操作 | 规模 | p50 (µs) | p95 (µs) |\n|---|---|---|---|\n");
    {
        const std::string root50 = (base / "ws_50_nested").string();
        const std::string file50 = (fs::path(root50) / "p0" / "rtl" / "m0.v").string();
        Stats findal;
        for (int i = 0; i < iters; ++i) {
            const double t0 = NowMs();
            (void)fpgalsp::project::FindAl(file50);
            findal.Add(NowMs() - t0);
        }
        std::printf("| FindAl(path) | 50 工程/1000 文件 | %.1f | %.1f |\n", findal.Pct(0.5) * 1000,
                    findal.Pct(0.95) * 1000);
        char buf[256];
        std::snprintf(buf, sizeof(buf), "csv,FindAl,50,%,%d,%.4f,%.4f,%.4f", iters, findal.Pct(0.5),
                      findal.Pct(0.95), findal.Mean());
        csv.push_back(buf);

        Stats disc;
        for (int i = 0; i < 20; ++i) {
            const double t0 = NowMs();
            const auto cands = fpgalsp::project::DiscoverProjects(root50, 6);
            (void)cands;
            disc.Add(NowMs() - t0);
        }
        std::printf("| DiscoverProjects(root) | 50 工程/1000 文件 | %.1f | %.1f |\n",
                    disc.Pct(0.5) * 1000, disc.Pct(0.95) * 1000);
        std::snprintf(buf, sizeof(buf), "csv,DiscoverProjects,50,%d,20,%.4f,%.4f,%.4f", 0,
                      disc.Pct(0.5), disc.Pct(0.95), disc.Mean());
        csv.push_back(buf);

        Stats aff;
        const std::string al = (fs::path(root50) / "p0" / "proj0.al").string();
        for (int i = 0; i < iters; ++i) {
            const double t0 = NowMs();
            (void)fpgalsp::project::Affinity(al, file50);
            aff.Add(NowMs() - t0);
        }
        std::printf("| Affinity(al, path) | 单项 | %.1f | %.1f |\n", aff.Pct(0.5) * 1000,
                    aff.Pct(0.95) * 1000);
        std::snprintf(buf, sizeof(buf), "csv,Affinity,1,%d,%.4f,%.4f,%.4f", iters, aff.Pct(0.5),
                      aff.Pct(0.95), aff.Mean());
        csv.push_back(buf);
    }

    // ---------------- 缓存键策略模拟（两条真实 trace） ----------------
    // trace A（单工程内切换）：20 个文件都在同一个 rtl/ 目录，200 次访问、100 ms 间隔
    // trace B（多工程切换）：50 个工程各 4 个文件，200 次访问、100 ms 间隔
    std::printf("\n### 缓存键策略模拟（命中/未命中，200 次访问、100 ms 间隔）\n\n");
    struct Trace {
        const char* name;
        std::vector<std::string> paths;
        double miss_cost_ms;
    };
    std::vector<Trace> traces;
    {
        const std::string root = (base / "ws_1_nested").string();
        Trace a{"A 单工程同目录", {}, 0.0};
        for (int i = 0; i < 20; ++i) {
            a.paths.push_back((fs::path(root) / "p0" / "rtl" / ("m" + std::to_string(i) + ".v")).string());
        }
        for (const Row& r : rows) {
            if (r.mode == "FindAl-hit" && r.projects == 1 && r.trace == "alternate") {
                a.miss_cost_ms = r.cost.Mean();
            }
        }
        traces.push_back(a);

        const std::string root50 = (base / "ws_50_nested").string();
        Trace b{"B 多工程切换", {}, 0.0};
        for (int i = 0; i < 50; ++i) {
            for (int f = 0; f < 4; ++f) {
                b.paths.push_back((fs::path(root50) / ("p" + std::to_string(i)) / "rtl" /
                                   ("m" + std::to_string(f) + ".v")).string());
            }
        }
        for (const Row& r : rows) {
            if (r.mode == "FindAl-hit" && r.projects == 50 && r.trace == "alternate") {
                b.miss_cost_ms = r.cost.Mean();
            }
        }
        traces.push_back(b);
    }

    std::printf("| trace | 策略 | 命中 | 未命中 | 清空 | 总发现成本 (ms) |\n|---|---|---|---|---|---|\n");
    for (const Trace& trace : traces) {
        for (int policy = 0; policy <= 2; ++policy) {
            CacheSim sim;
            sim.policy = policy;
            for (int i = 0; i < 200; ++i) {
                const std::string& path = trace.paths[static_cast<size_t>(i) % trace.paths.size()];
                sim.Access(policy == 2 ? std::string("ROOT") : std::string("ROOT"), path,
                           static_cast<double>(i) * 100.0);
            }
            const double ms = static_cast<double>(sim.misses) * trace.miss_cost_ms;
            const char* name = policy == 0 ? "按 path（当前）" : (policy == 1 ? "按目录" : "按工程根");
            std::printf("| %s | %s | %lld | %lld | %lld | %.1f |\n", trace.name, name, sim.hits,
                        sim.misses, sim.clears, ms);
            char buf[256];
            std::snprintf(buf, sizeof(buf), "csv,cache,%s,%d,%lld,%lld,%lld,%.2f", trace.name, policy,
                          sim.hits, sim.misses, sim.clears, ms);
            csv.push_back(buf);
        }
    }

    // ---------------- 落盘 ----------------
    {
        std::ofstream out(fs::path(out_dir) / "discovery.csv", std::ios::binary);
        for (const std::string& line : csv) out << line << "\n";
    }
    {
        std::ofstream out(fs::path(out_dir) / "summary.json", std::ios::binary);
        out << "{\n  \"iters\": " << iters << ",\n  \"rows\": [\n";
        for (size_t i = 0; i < rows.size(); ++i) {
            const Row& r = rows[i];
            out << "    {\"mode\": \"" << r.mode << "\", \"projects\": " << r.projects
                << ", \"trace\": \"" << r.trace << "\", \"p50_ms\": " << r.cost.Pct(0.5)
                << ", \"p95_ms\": " << r.cost.Pct(0.95) << ", \"mean_ms\": " << r.cost.Mean() << "}";
            out << (i + 1 < rows.size() ? ",\n" : "\n");
        }
        out << "  ]\n}\n";
    }
    std::printf("\n[probe] wrote %s/discovery.csv + summary.json\n", out_dir.c_str());
    return 0;
}