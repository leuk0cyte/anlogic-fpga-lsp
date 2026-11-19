// =============================================================================
//  tests/perf/probe_index.cpp —— 索引与解析的真实成本账本（task-29）
// =============================================================================
//  回答四件事：
//    1. 单文件解析到底多贵（1 KB / 13 KB / 100 KB，µs 与 MB/s）；
//    2. 冷索引（打开工程一次）在 N=50/200/1000 下的总时间与**每文件固定开销**；
//    3. 增量更新（每次编辑）是不是真的增量：Clone / Parse / Replace / Get 分阶段，
//       并跨 N 比较斜率（O(1) 还是 O(N)）；
//    4. `.al` 解析与工程签名在 0/50/200/400 条目下的单次成本，以及**现在还有没有
//       每请求都做**（对照 `src/server/lsp_server.cpp` 的调用点）。
//
//  这个探针**直接调内部 API**（hdl / project / server::IndexService），不走协议，
//  所以测出来的就是"引擎里的那一层"，不含 JSON-RPC 传输。
//
//  用法：
//      perf_index.exe                      # 全部场景（默认 iters=30）
//      perf_index.exe --iters 20 --warmup 3
//      perf_index.exe --scenario parse     # parse|build|incremental|al|cache|mem|all
//      perf_index.exe --scenario mem --size 200
//
//  合成数据全部落在 %TEMP%\fpgalsp-perf\index\（不碰仓库、不碰真实工程）。
// =============================================================================

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "perf_common.h"

// windows.h 会把 ReplaceFile 映射成 ReplaceFileW/A（宏），把 HdlIndex::ReplaceFile 打坏。
#if defined(ReplaceFile)
#undef ReplaceFile
#endif

#include "hdl/hdl_index.h"
#include "platform/encoding.h"
#include "platform/path.h"
#include "project/al_project.h"
#include "server/index_service.h"

namespace fs = std::filesystem;

namespace {

using fpgalsp::hdl::BuildIndex;
using fpgalsp::hdl::BuildOptions;
using fpgalsp::hdl::HdlIndex;
using fpgalsp::hdl::ParsedFile;
using fpgalsp::hdl::ParseCache;
using fpgalsp::hdl::ParseFile;
using fpgalsp::project::Project;
using fpgalsp::server::IndexService;

// -----------------------------------------------------------------------------
// 系统负载（README §6.2 要求记录；GetSystemTimes 差分）
// -----------------------------------------------------------------------------
struct CpuTimes {
  std::uint64_t idle = 0;
  std::uint64_t kernel = 0;
  std::uint64_t user = 0;
};

CpuTimes ReadCpuTimes() {
  CpuTimes t;
  FILETIME idle{}, kernel{}, user{};
  if (GetSystemTimes(&idle, &kernel, &user)) {
    t.idle = (static_cast<std::uint64_t>(idle.dwHighDateTime) << 32) | idle.dwLowDateTime;
    t.kernel = (static_cast<std::uint64_t>(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime;
    t.user = (static_cast<std::uint64_t>(user.dwHighDateTime) << 32) | user.dwLowDateTime;
  }
  return t;
}

// 0..1。kernel 时间里包含 idle，所以 busy = (kernel+user-idle)/(kernel+user)。
double CpuBusy(const CpuTimes& a, const CpuTimes& b) {
  const double idle = static_cast<double>(b.idle - a.idle);
  const double total = static_cast<double>((b.kernel - a.kernel) + (b.user - a.user));
  if (total <= 0.0) return 0.0;
  return std::max(0.0, std::min(1.0, (total - idle) / total));
}

// -----------------------------------------------------------------------------
// 合成工程
// -----------------------------------------------------------------------------
const fs::path& PerfRoot() {
  static const fs::path root = fs::temp_directory_path() / "fpgalsp-perf" / "index";
  return root;
}

void WriteText(const fs::path& path, std::string_view text) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::optional<std::string> DiskLoader(const std::string& path) {
  return fpgalsp::platform::try_read_file_utf8(path);
}

// 一个"够真实"的 Verilog 文件：ANSI 端口 + wire/assign + 例化 + always 块。
// 大小由 target_bytes 控制（按行堆到超过目标，再补 endmodule）。
std::string MakeVerilog(const std::string& module, std::size_t target_bytes,
                        int* out_signals = nullptr) {
  std::string text;
  text.reserve(target_bytes + 512);
  text += "module " + module + " (\n";
  text += "  input  wire        clk,\n";
  text += "  input  wire        rst_n,\n";
  text += "  input  wire [31:0] din,\n";
  text += "  output reg  [31:0] dout\n";
  text += ");\n";
  int index = 0;
  char line[192];
  while (text.size() < target_bytes) {
    std::snprintf(line, sizeof(line),
                  "  wire [31:0] w%04d;  assign w%04d = din ^ {16'b0, dout[15:0]};\n",
                  index, index);
    text += line;
    if (index % 8 == 3) {
      std::snprintf(line, sizeof(line),
                    "  sub_mod u%04d (.clk(clk), .rst_n(rst_n), .din(w%04d), .dout());\n",
                    index, index);
      text += line;
    }
    if (index % 16 == 5) {
      std::snprintf(line, sizeof(line),
                    "  always @(posedge clk) begin if (!rst_n) dout[%d] <= 1'b0; else dout[%d] "
                    "<= w%04d[%d]; end\n",
                    index % 32, index % 32, index, index % 32);
      text += line;
    }
    ++index;
  }
  text += "endmodule\n";
  if (out_signals != nullptr) *out_signals = index;
  return text;
}

std::string AlXml(const fs::path& dir, const std::vector<std::string>& rel_files,
                  const std::string& top) {
  std::string xml;
  xml += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  xml += "<Project Version=\"3\" Minor=\"2\" Path=\"" + dir.generic_string() + "\">\n";
  xml += "    <TD_Version>6.2.3.210850</TD_Version>\n";
  xml += "    <Name>perf_index</Name>\n";
  xml += "    <HardWare>\n        <Family>EF2</Family>\n"
         "        <Device>EF2L45LG144B</Device>\n        <Speed></Speed>\n    </HardWare>\n";
  xml += "    <Source_Files>\n        <Verilog>\n";
  int order = 1;
  for (const std::string& rel : rel_files) {
    xml += "            <File Path=\"" + rel + "\">\n                <FileInfo>\n";
    xml += "                    <Attr Name=\"UsedInSyn\" Val=\"true\"/>\n";
    xml += "                    <Attr Name=\"InP&R\" Val=\"true\"/>\n";
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

// 一个 N 文件的合成工程：每文件 ~640 B（与历史基准同规模，便于和 44 KB/文件 对照）
struct Synthetic {
  fs::path dir;
  std::string al;
  std::vector<std::string> abs_paths;
  std::vector<std::string> texts;  // 与 abs_paths 一一对应（供 loader 复用）
  Project project;
  std::uint64_t source_bytes = 0;
};

Synthetic MakeProject(const std::string& name, int files, std::size_t file_bytes = 640) {
  Synthetic out;
  out.dir = PerfRoot() / name;
  std::error_code ec;
  fs::remove_all(out.dir, ec);
  fs::create_directories(out.dir / "src", ec);

  std::vector<std::string> rel;
  rel.reserve(static_cast<std::size_t>(files));
  out.abs_paths.reserve(static_cast<std::size_t>(files));
  out.texts.reserve(static_cast<std::size_t>(files));

  for (int i = 1; i <= files; ++i) {
    char module[32];
    std::snprintf(module, sizeof(module), "m%04d", i);
    std::string text;
    if (i == 1) {
      text = "module m0001 (input wire clk, output wire q);\n  assign q = clk;\nendmodule\n";
      // 让 m0001 也被别处例化：这里只是保证它存在
    } else {
      text = MakeVerilog(module, file_bytes);
    }
    const std::string rel_path = "src/" + std::string(module) + ".v";
    WriteText(out.dir / rel_path, text);
    rel.push_back(rel_path);
    out.abs_paths.push_back(fpgalsp::platform::normalize_path(
        (out.dir / rel_path).generic_string()));
    out.texts.push_back(std::move(text));
    out.source_bytes += out.texts.back().size();
  }

  out.al = (out.dir / "perf.al").generic_string();
  WriteText(out.dir / "perf.al", AlXml(out.dir, rel, "m0001"));
  out.project = fpgalsp::project::ParseAl(out.al);

  // 预热 page cache（README §6.4）：先把所有文件读一遍
  for (const std::string& path : out.abs_paths) (void)DiskLoader(path);
  return out;
}

// 生成"只有 .al、条目不指向真实文件"的工程（隔离 ParseAl 的纯解析成本用）
Synthetic MakeAlOnly(const std::string& name, int entries) {
  Synthetic out;
  out.dir = PerfRoot() / name;
  std::error_code ec;
  fs::remove_all(out.dir, ec);
  fs::create_directories(out.dir, ec);

  std::vector<std::string> rel;
  rel.reserve(static_cast<std::size_t>(entries));
  for (int i = 1; i <= entries; ++i) {
    char module[32];
    std::snprintf(module, sizeof(module), "e%04d", i);
    rel.push_back("src/" + std::string(module) + ".v");  // 不创建：ResolveRelative 会走 missing
  }
  out.al = (out.dir / "perf.al").generic_string();
  WriteText(out.dir / "perf.al", AlXml(out.dir, rel, "e0001"));
  return out;
}

// -----------------------------------------------------------------------------
// 小工具
// -----------------------------------------------------------------------------
void PrintThroughput(const perf::Stats& st, std::size_t bytes, const char* label) {
  const double mb = static_cast<double>(bytes) / 1e6;
  const double p50 = st.P50() / 1000.0;  // ms -> s
  const double mbs = p50 > 0.0 ? mb / p50 : 0.0;
  std::printf("%-28s %8zu B  p50=%9.1f us  p95=%9.1f us  %8.1f MB/s\n", label, bytes,
              st.P50() * 1000.0, st.P95() * 1000.0, mbs);
}

std::string ReadRealFile(const std::string& path) {
  return perf::ReadFile(fs::path(path));
}

// 在 text 里改 1 个 token（把第一个 "w0000" 换成别的名字）——只挪一个标识符
std::string ChangeOneToken(const std::string& text, int counter) {
  std::string out = text;
  const std::string from = "w0000";
  const std::size_t at = out.find(from);
  if (at != std::string::npos) {
    out.replace(at, from.size(), "z" + std::to_string(1000 + counter));
  }
  return out;
}

// 改 10 行（在文件里插 10 行 wire/assign）
std::string ChangeTenLines(const std::string& text, int counter) {
  std::string out = text;
  const std::size_t at = out.rfind("endmodule");
  std::string block;
  for (int i = 0; i < 10; ++i) {
    block += "  wire [7:0] extra_" + std::to_string(counter) + "_" + std::to_string(i) +
             ";  assign extra_" + std::to_string(counter) + "_" + std::to_string(i) +
             " = 8'h00;\n";
  }
  if (at != std::string::npos) {
    out.insert(at, block);
  } else {
    out += block;
  }
  return out;
}

// 改整文件：重新生成一份（长度可能不同）
std::string ChangeWholeFile(const std::string& module, int counter) {
  return MakeVerilog(module + "_v" + std::to_string(counter), 640);
}

// -----------------------------------------------------------------------------
// 场景 0（头号）：watcher 缺失路径 —— 每个 Get() 都做目录 stamp 校验
// -----------------------------------------------------------------------------
//  `IndexService` 默认 `watcher_active = false`，而 `SetWatcherActive(true)` 全仓唯一
//  调用点在 `lsp_server.cpp:424`（`HandleDidChangeWatchedFiles` 内）。服务端
//  `initialize` 没有动态注册 `workspace/didChangeWatchedFiles`（`lsp_server.cpp:344-346`
//  的 workspace 能力只有 workspaceFolders），所以正规客户端不会发那条通知 ——
//  于是 `Get()` 每次都走 `ValidateStampsLocked()`：一次目录枚举 + N 次
//  「路径规范化 + 小写 + 两次 map 查找」。
//
//  本场景给出三样东西（M 级）：
//    1. `Get()` 在 watcher 未激活 / 已激活下的 p50 差（= 能省多少）；
//    2. 随 N 的斜率（证明 O(N)）；
//    3. 分解：目录枚举 vs 路径键 vs 比较，各占多少。
void ScenarioStamps(int iters, int warmup) {
  std::printf("\n=== [0] watcher 缺失路径：Get() 每请求目录 stamp 校验 ===\n");
  std::printf("%-40s %7s %10s %10s %12s\n", "case", "files", "p50(ms)", "p95(ms)", "us/file");

  for (const int files : {50, 200, 1000}) {
    Synthetic synthetic = MakeProject("stamp_" + std::to_string(files), files);
    IndexService service(DiskLoader);
    service.SetProject(synthetic.project);
    // 默认 watcher_active == false：冷构建后会立刻建立 stamp 基线
    const IndexService::Snapshot cold = service.Get();
    if (cold == nullptr) {
      std::printf("!! 构建失败 N=%d\n", files);
      continue;
    }

    const auto inactive = perf::Bench(
        "stamp_get_inactive_" + std::to_string(files), iters,
        [&] {
          const IndexService::Snapshot s = service.Get();
          if (s == nullptr) std::printf("unreachable\n");
        },
        warmup);
    std::printf("%-40s %7d %10.3f %10.3f %12.2f\n", "Get(watcher 未激活 = 默认)", files,
                inactive.P50(), inactive.P95(), inactive.P50() * 1000.0 / files);
    inactive.PrintCsv();

    // 对照：手工激活 watcher（不改产品代码，只改运行时状态）
    service.SetWatcherActive(true);
    const auto active = perf::Bench(
        "stamp_get_active_" + std::to_string(files), iters,
        [&] {
          const IndexService::Snapshot s = service.Get();
          if (s == nullptr) std::printf("unreachable\n");
        },
        warmup);
    std::printf("%-40s %7d %10.3f %10.3f %12.2f\n", "Get(watcher 激活 = 只取指针)", files,
                active.P50(), active.P95(), active.P50() * 1000.0 / files);
    active.PrintCsv();

    const double delta_ms = inactive.P50() - active.P50();
    std::printf("%-40s %7d %10.3f %10.3f %12.2f   <-- ValidateStampsLocked\n",
                "  差值 = stamp 校验净成本", files, delta_ms, inactive.P95() - active.P95(),
                delta_ms * 1000.0 / files);

    // ---- 分解 a：目录枚举（复刻 index_service.cpp 的 DirStamps::LoadDir）----
    const std::string dir = (synthetic.dir / "src").generic_string();
    const auto enumerate = perf::Bench(
        "stamp_dir_enum_" + std::to_string(files), iters,
        [&] {
          std::map<std::string, std::pair<std::uintmax_t, std::int64_t>> entries;
          std::error_code ec;
          fs::directory_iterator iter{fs::path(dir), ec};
          if (ec) return;
          const fs::directory_iterator end;
          for (; iter != end; iter.increment(ec)) {
            if (ec) break;
            std::error_code entry_ec;
            const auto size = iter->file_size(entry_ec);
            if (entry_ec) continue;
            const auto write_time = iter->last_write_time(entry_ec);
            if (entry_ec) continue;
            const std::u8string u8 = iter->path().filename().u8string();
            std::string name(reinterpret_cast<const char*>(u8.data()), u8.size());
            entries.emplace(fpgalsp::platform::path_key(name),
                            std::make_pair(size, static_cast<std::int64_t>(
                                                     write_time.time_since_epoch().count())));
          }
          if (entries.empty()) std::printf("unreachable\n");
        },
        warmup);
    std::printf("%-40s %7d %10.3f %10.3f %12.2f   (%.0f%% of 差值)\n",
                "  分解 a: 目录枚举(1 次/N 文件)", files, enumerate.P50(), enumerate.P95(),
                enumerate.P50() * 1000.0 / files,
                delta_ms > 0.0 ? 100.0 * enumerate.P50() / delta_ms : 0.0);
    enumerate.PrintCsv();

    // ---- 分解 b：N 次 路径键（normalize_path + 小写）----
    const auto keys = perf::Bench(
        "stamp_path_keys_" + std::to_string(files), iters,
        [&] {
          std::size_t total = 0;
          for (const std::string& path : synthetic.abs_paths) {
            total += fpgalsp::platform::path_key(fpgalsp::platform::normalize_path(path)).size();
          }
          if (total == 0) std::printf("unreachable\n");
        },
        warmup);
    std::printf("%-40s %7d %10.3f %10.3f %12.2f   (%.0f%% of 差值)\n",
                "  分解 b: N 次路径键(normalize+lower)", files, keys.P50(), keys.P95(),
                keys.P50() * 1000.0 / files,
                delta_ms > 0.0 ? 100.0 * keys.P50() / delta_ms : 0.0);
    keys.PrintCsv();

    // ---- 分解 b1/b2：把「路径键」拆成 normalize_path 与 小写 ----
    const auto normalize_only = perf::Bench(
        "stamp_normalize_" + std::to_string(files), iters,
        [&] {
          std::size_t total = 0;
          for (const std::string& path : synthetic.abs_paths) {
            total += fpgalsp::platform::normalize_path(path).size();
          }
          if (total == 0) std::printf("unreachable\n");
        },
        warmup);
    std::printf("%-40s %7d %10.3f %10.3f %12.2f   (%.0f%% of 路径键)\n",
                "  分解 b1: normalize_path", files, normalize_only.P50(), normalize_only.P95(),
                normalize_only.P50() * 1000.0 / files,
                keys.P50() > 0.0 ? 100.0 * normalize_only.P50() / keys.P50() : 0.0);
    normalize_only.PrintCsv();

    const auto lower_only = perf::Bench(
        "stamp_lower_" + std::to_string(files), iters,
        [&] {
          std::size_t total = 0;
          for (const std::string& path : synthetic.abs_paths) {
            total += fpgalsp::platform::path_key(path).size();
          }
          if (total == 0) std::printf("unreachable\n");
        },
        warmup);
    std::printf("%-40s %7d %10.3f %10.3f %12.2f   (%.0f%% of 路径键)\n",
                "  分解 b2: path_key(仅小写)", files, lower_only.P50(), lower_only.P95(),
                lower_only.P50() * 1000.0 / files,
                keys.P50() > 0.0 ? 100.0 * lower_only.P50() / keys.P50() : 0.0);
    lower_only.PrintCsv();

    // ---- 分解 c：N 次 map 查找 + stamp 比较（键预计算，排除 b 的干扰）----
    std::vector<std::string> precomputed;
    precomputed.reserve(synthetic.abs_paths.size());
    for (const std::string& path : synthetic.abs_paths) {
      precomputed.push_back(fpgalsp::platform::path_key(fpgalsp::platform::normalize_path(path)));
    }
    std::map<std::string, IndexService::FileStamp> fake;
    for (std::size_t i = 0; i < precomputed.size(); ++i) {
      IndexService::FileStamp stamp;
      stamp.exists = true;
      stamp.size = 640;
      stamp.mtime = static_cast<std::int64_t>(i);
      fake.emplace(precomputed[i], stamp);
    }
    const auto compares = perf::Bench(
        "stamp_compare_" + std::to_string(files), iters,
        [&] {
          std::size_t hits = 0;
          for (const std::string& key : precomputed) {
            const auto it = fake.find(key);
            if (it != fake.end() && it->second.exists) ++hits;
          }
          if (hits == 0) std::printf("unreachable\n");
        },
        warmup);
    std::printf("%-40s %7d %10.3f %10.3f %12.2f   (%.0f%% of 差值)\n",
                "  分解 c: N 次 map 查找+比较", files, compares.P50(), compares.P95(),
                compares.P50() * 1000.0 / files,
                delta_ms > 0.0 ? 100.0 * compares.P50() / delta_ms : 0.0);
    compares.PrintCsv();

    const double sum = enumerate.P50() + keys.P50() + compares.P50();
    std::printf("%-40s %7d %10.3f %10.3f %12.2f   (分解和 / 差值 = %.2fx)\n",
                "  分解合计", files, sum, 0.0, sum * 1000.0 / files,
                delta_ms > 0.0 ? sum / delta_ms : 0.0);
  }
}

// -----------------------------------------------------------------------------
// 场景 1：单文件解析吞吐
// -----------------------------------------------------------------------------
void ScenarioParse(int iters, int warmup) {
  std::printf("\n=== [1] 单文件解析（ParseFile = Verible 词法 + 结构识别）===\n");

  struct Case {
    const char* label;
    std::string text;
    std::string path;
  };
  std::vector<Case> cases;

  // 1 KB：#!/ 合成
  {
    std::string text = MakeVerilog("small_mod", 1024);
    cases.push_back({"parse 1KB(synthetic)", std::move(text), "Y:/perf/small.v"});
  }
  // 13 KB：真实语料的 top.v（13297 B，501 行，含例化/参数/always）
  {
    const std::string path =
        std::string(FPGA_LSP_SOURCE_DIR) + "/tests/gate-corpus/corpus_alpha/src/top.v";
    std::string text = ReadRealFile(path);
    if (text.empty()) {
      text = MakeVerilog("mid_mod", 13312);
      cases.push_back({"parse 13KB(synthetic)", std::move(text), "Y:/perf/mid.v"});
    } else {
      cases.push_back({"parse 13KB(real top.v)", std::move(text), path});
    }
  }
  // 100 KB：合成
  {
    std::string text = MakeVerilog("large_mod", 100 * 1024);
    cases.push_back({"parse 100KB(synthetic)", std::move(text), "Y:/perf/large.v"});
  }
  // 1 MB：附加点（判断"是否线性"）
  {
    std::string text = MakeVerilog("huge_mod", 1024 * 1024);
    cases.push_back({"parse 1MB(synthetic)", std::move(text), "Y:/perf/huge.v"});
  }

  for (const Case& c : cases) {
    // token 密度差异很大（真实代码 ~100 token/KB，合成密集表达式 ~200 token/KB），
    // 所以除了 MB/s 还要给 µs/token —— 解析成本主要跟 token 数走。
    std::size_t token_count = 0;
    {
      const ParsedFile probe = ParseFile(c.path, c.text);
      token_count = probe.tokens.size();
    }
    const double tokens_per_kb =
        c.text.size() > 0 ? static_cast<double>(token_count) / (c.text.size() / 1024.0) : 0.0;

    const auto st = perf::Bench(
        std::string(c.label), iters,
        [&] {
          const ParsedFile file = ParseFile(c.path, c.text);
          if (file.path.empty()) std::printf("unreachable\n");
        },
        warmup);
    PrintThroughput(st, c.text.size(), c.label);
    std::printf("%-28s %8zu tokens (%.0f/KB)  %6.3f us/token\n", "  token 数",
                token_count, tokens_per_kb,
                token_count > 0 ? st.P50() * 1000.0 / static_cast<double>(token_count) : 0.0);
    st.PrintCsv();

    // 拆一下：全文 FNV（ParseCache 的键）+ Verible 词法
    const auto hash = perf::Bench(
        std::string(c.label) + "/ContentHash", iters,
        [&] {
          const std::uint64_t h = fpgalsp::hdl::ContentHash(c.text);
          if (h == 0) std::printf("unreachable\n");
        },
        warmup);
    std::printf("%-28s %8zu B  p50=%9.1f us  (占 ParseFile %.1f%%)\n", "  ContentHash(FNV-1a)",
                c.text.size(), hash.P50() * 1000.0,
                st.P50() > 0.0 ? 100.0 * hash.P50() / st.P50() : 0.0);
    hash.PrintCsv();

    std::vector<fpgalsp::hdl::ParseError> errors;
    const auto lex = perf::Bench(
        std::string(c.label) + "/LexWithVerible", iters,
        [&] {
          const auto tokens = fpgalsp::hdl::LexWithVerible(c.path, c.text, &errors);
          if (tokens.empty() && !errors.empty()) std::printf("unreachable\n");
        },
        warmup);
    std::printf("%-28s %8zu B  p50=%9.1f us  (占 ParseFile %.1f%%)\n", "  LexWithVerible",
                c.text.size(), lex.P50() * 1000.0,
                st.P50() > 0.0 ? 100.0 * lex.P50() / st.P50() : 0.0);
    lex.PrintCsv();
  }
}

// -----------------------------------------------------------------------------
// 场景 2：建索引（冷路径）
// -----------------------------------------------------------------------------
void ScenarioBuild(int iters, int warmup) {
  std::printf("\n=== [2] 建索引（冷路径：打开工程那一次）===\n");
  std::printf("%-30s %6s %10s %10s %10s %10s %10s\n", "case", "files", "p50(ms)", "p95(ms)",
              "us/file", "sourceKB", "MB/s");

  for (const int files : {50, 200, 1000}) {
    Synthetic synthetic = MakeProject("build_" + std::to_string(files), files);
    const Project& project = synthetic.project;

    // 2.1 纯顺序（B8 之前的行为）
    const auto seq = perf::Bench(
        "build_seq_" + std::to_string(files), iters,
        [&] {
          ParseCache cache;  // 每次都是冷缓存 = 真实"打开工程"
          BuildOptions options;
          options.cache = &cache;
          const auto built = BuildIndex(project, DiskLoader, options);
          if (built.files_indexed == 0) std::printf("unreachable\n");
        },
        warmup);
    std::printf("%-30s %6d %10.3f %10.3f %10.1f %10.1f %10.2f\n",
                "build_seq(cold cache)", files, seq.P50(), seq.P95(),
                seq.P50() * 1000.0 / files, synthetic.source_bytes / 1024.0,
                (static_cast<double>(synthetic.source_bytes) / 1e6) / (seq.P50() / 1000.0));
    seq.PrintCsv();

    // 2.2 并行（IndexService 默认：RecommendedParseThreads）
    const auto par = perf::Bench(
        "build_par_" + std::to_string(files), iters,
        [&] {
          ParseCache cache;
          BuildOptions options;
          options.cache = &cache;
          options.parse_threads = fpgalsp::hdl::RecommendedParseThreads();
          const auto built = BuildIndex(project, DiskLoader, options);
          if (built.files_indexed == 0) std::printf("unreachable\n");
        },
        warmup);
    std::printf("%-30s %6d %10.3f %10.3f %10.1f %10.1f %10.2f\n",
                "build_par(cold cache)", files, par.P50(), par.P95(),
                par.P50() * 1000.0 / files, synthetic.source_bytes / 1024.0,
                (static_cast<double>(synthetic.source_bytes) / 1e6) / (par.P50() / 1000.0));
    par.PrintCsv();

    // 2.3 IndexService 冷构建（服务层：SetProject + 首次 Get）
    const auto service_cold = perf::Bench(
        "indexservice_cold_" + std::to_string(files), std::max(5, iters / 2),
        [&] {
          IndexService service(DiskLoader);
          service.SetProject(project);
          const IndexService::Snapshot snapshot = service.Get();
          if (snapshot == nullptr || snapshot->FileCount() == 0) std::printf("unreachable\n");
        },
        std::max(1, warmup / 2));
    std::printf("%-30s %6d %10.3f %10.3f %10.1f %10.1f %10.2f\n",
                "IndexService::Get(cold)", files, service_cold.P50(), service_cold.P95(),
                service_cold.P50() * 1000.0 / files, synthetic.source_bytes / 1024.0,
                (static_cast<double>(synthetic.source_bytes) / 1e6) /
                    (service_cold.P50() / 1000.0));
    service_cold.PrintCsv();

    // 2.4 热缓存重建（= "热路径里的全量重建"，ParseCache 全命中）
    {
      ParseCache cache;
      BuildOptions options;
      options.cache = &cache;
      (void)BuildIndex(project, DiskLoader, options);  // 预热 cache
      const auto warm = perf::Bench(
          "build_warm_" + std::to_string(files), iters,
          [&] {
            const auto built = BuildIndex(project, DiskLoader, options);
            if (built.files_indexed == 0) std::printf("unreachable\n");
          },
          warmup);
      std::printf("%-30s %6d %10.3f %10.3f %10.1f %10.1f %10.2f\n", "build_warm(cache hit)",
                  files, warm.P50(), warm.P95(), warm.P50() * 1000.0 / files,
                  synthetic.source_bytes / 1024.0,
                  (static_cast<double>(synthetic.source_bytes) / 1e6) / (warm.P50() / 1000.0));
      warm.PrintCsv();

      // 2.5 热重建的成本拆解：N 次读盘 / N 次 AddFile（cache 命中时都还要付）
      const auto read_only = perf::Bench(
          "build_read_only_" + std::to_string(files), std::min(10, iters),
          [&] {
            std::size_t total = 0;
            for (const std::string& path : synthetic.abs_paths) {
              const auto text = DiskLoader(path);
              total += text.has_value() ? text->size() : 0;
            }
            if (total == 0) std::printf("unreachable\n");
          },
          std::max(1, warmup / 2));
      std::printf("%-30s %6d %10.3f %10.3f %10.1f\n", "  N 次读盘(仅 loader)", files,
                  read_only.P50(), read_only.P95(), read_only.P50() * 1000.0 / files);
      read_only.PrintCsv();

      std::vector<std::shared_ptr<const ParsedFile>> pre_parsed;
      pre_parsed.reserve(synthetic.abs_paths.size());
      for (const std::string& path : synthetic.abs_paths) {
        pre_parsed.push_back(cache.Get(path, *DiskLoader(path)));
      }
      const auto add_only = perf::Bench(
          "build_addfile_only_" + std::to_string(files), iters,
          [&] {
            HdlIndex index;
            for (const auto& parsed : pre_parsed) index.AddFile(parsed);
            if (index.FileCount() == 0) std::printf("unreachable\n");
          },
          warmup);
      std::printf("%-30s %6d %10.3f %10.3f %10.1f\n", "  N 次 AddFile(仅)", files,
                  add_only.P50(), add_only.P95(), add_only.P50() * 1000.0 / files);
      add_only.PrintCsv();
    }
  }
}

// -----------------------------------------------------------------------------
// 场景 3：增量更新（热路径）
// -----------------------------------------------------------------------------
void ScenarioIncremental(int iters, int warmup) {
  std::printf("\n=== [3] 增量更新（热路径：每次编辑）===\n");
  std::printf("%-34s %6s %10s %10s %12s\n", "case", "files", "p50(ms)", "p95(ms)", "us/edit");

  for (const int files : {50, 200, 1000}) {
    Synthetic synthetic = MakeProject("inc_" + std::to_string(files), files);
    const Project& project = synthetic.project;

    IndexService service(DiskLoader);
    service.SetProject(project);
    service.SetWatcherActive(true);  // 编辑器声明了 watcher：Get 不做目录枚举
    const IndexService::Snapshot snapshot = service.Get();
    if (snapshot == nullptr) {
      std::printf("!! 索引构建失败 N=%d\n", files);
      continue;
    }
    const IndexService::Snapshot stable = snapshot;

    const std::string target = synthetic.abs_paths[1];
    const std::string baseline_text = synthetic.texts[1];

    // 3.1 分阶段：Clone / Parse / Replace
    const auto clone = perf::Bench(
        "inc_clone_" + std::to_string(files), iters,
        [&] {
          const HdlIndex copy = stable->Clone();
          if (copy.FileCount() == 0) std::printf("unreachable\n");
        },
        warmup);
    std::printf("%-34s %6d %10.3f %10.3f %12.2f\n", "  Clone()", files, clone.P50(),
                clone.P95(), clone.P50() * 1000.0);

    std::vector<std::shared_ptr<const ParsedFile>> parsed_cache;  // 防优化掉
    const auto parse = perf::Bench(
        "inc_parse_" + std::to_string(files), iters,
        [&] {
          parsed_cache.push_back(
              std::make_shared<const ParsedFile>(ParseFile(target, baseline_text)));
        },
        warmup);
    std::printf("%-34s %6d %10.3f %10.3f %12.2f\n", "  ParseFile(640B)", files, parse.P50(),
                parse.P95(), parse.P50() * 1000.0);

    {
      HdlIndex copy = stable->Clone();
      auto shared = std::make_shared<const ParsedFile>(ParseFile(target, baseline_text));
      const auto replace = perf::Bench(
          "inc_replace_" + std::to_string(files), iters,
          [&] { copy.ReplaceFile(shared); },
          warmup);
      std::printf("%-34s %6d %10.3f %10.3f %12.2f\n", "  ReplaceFile(同路径)", files,
                  replace.P50(), replace.P95(), replace.P50() * 1000.0);
    }

    // 3.2 OnFileChanged：三种改动幅度
    struct Edit {
      const char* label;
      std::function<std::string(int)> make;
    };
    const std::vector<Edit> edits = {
        {"OnFileChanged(改 1 token)",
         [&](int i) { return ChangeOneToken(baseline_text, i); }},
        {"OnFileChanged(改 10 行)",
         [&](int i) { return ChangeTenLines(baseline_text, i); }},
        {"OnFileChanged(改整文件)",
         [&](int i) { return ChangeWholeFile("m0002", i); }},
    };

    for (const Edit& edit : edits) {
      int counter = 0;
      const auto st = perf::Bench(
          std::string("inc_") + edit.label + "_" + std::to_string(files), iters,
          [&] {
            const std::string text = edit.make(counter++);
            service.OnFileChanged(target, text);
            const IndexService::Snapshot after = service.Get();
            if (after == nullptr) std::printf("unreachable\n");
          },
          warmup);
      std::printf("%-34s %6d %10.3f %10.3f %12.2f\n", edit.label, files, st.P50(), st.P95(),
                  st.P50() * 1000.0);
      st.PrintCsv();
    }

    // 3.3 对照：全量重建（ParseCache 热 —— 现状里"重建"的最便宜形态）
    {
      ParseCache cache;
      BuildOptions options;
      options.cache = &cache;
      (void)BuildIndex(project, DiskLoader, options);
      const auto full = perf::Bench(
          "inc_full_rebuild_" + std::to_string(files), std::max(5, iters / 2),
          [&] {
            const auto built = BuildIndex(project, DiskLoader, options);
            if (built.files_indexed == 0) std::printf("unreachable\n");
          },
          std::max(1, warmup / 2));
      std::printf("%-34s %6d %10.3f %10.3f %12.2f\n", "对照:全量重建(cache 热)", files,
                  full.P50(), full.P95(), full.P50() * 1000.0);
      full.PrintCsv();
    }

    // 3.4 Get()：watcher 活跃 / 未激活（目录 stamp 校验）
    const auto get_active = perf::Bench(
        "inc_get_active_" + std::to_string(files), iters,
        [&] {
          const IndexService::Snapshot s = service.Get();
          if (s == nullptr) std::printf("unreachable\n");
        },
        warmup);
    std::printf("%-34s %6d %10.3f %10.3f %12.2f\n", "Get(watcher 活跃)", files,
                get_active.P50(), get_active.P95(), get_active.P50() * 1000.0);

    service.SetWatcherActive(false);
    (void)service.Get();  // 建立 stamp 基线
    const auto get_inactive = perf::Bench(
        "inc_get_inactive_" + std::to_string(files), iters,
        [&] {
          const IndexService::Snapshot s = service.Get();
          if (s == nullptr) std::printf("unreachable\n");
        },
        warmup);
    std::printf("%-34s %6d %10.3f %10.3f %12.2f\n", "Get(watcher 未激活:stamp 校验)", files,
                get_inactive.P50(), get_inactive.P95(), get_inactive.P50() * 1000.0);
    get_inactive.PrintCsv();
  }
}

// -----------------------------------------------------------------------------
// 场景 4：`.al` 解析 / 工程缓存命中 / 每请求的固定开销
// -----------------------------------------------------------------------------
void ScenarioAl(int iters, int warmup) {
  std::printf("\n=== [4] .al 解析与工程签名（历史 P0 复核）===\n");
  std::printf("%-34s %7s %10s %10s %12s\n", "case", "entries", "p50(ms)", "p95(ms)",
              "us/entry");

  for (const int entries : {0, 50, 200, 400}) {
    Synthetic al_only = MakeAlOnly("al_" + std::to_string(entries), entries);

    // 4.1 纯 ParseAl（冷：读盘 + XML + 每条目 resolve/exists）
    const auto parse = perf::Bench(
        "al_parse_" + std::to_string(entries), std::max(5, iters),
        [&] {
          const Project project = fpgalsp::project::ParseAl(al_only.al);
          if (project.al_path.empty()) std::printf("unreachable\n");
        },
        warmup);
    std::printf("%-34s %7d %10.3f %10.3f %12.2f\n", "project::ParseAl(冷)", entries,
                parse.P50(), parse.P95(),
                entries > 0 ? parse.P50() * 1000.0 / entries : 0.0);
    parse.PrintCsv();

    // 4.2 (mtime,size) 命中路径：2 次 stat + 缓存查表 + Project 拷贝
    //     = lsp_server.cpp:1323-1355 的 ParseAlCached 命中分支
    const Project cached = fpgalsp::project::ParseAl(al_only.al);
    const std::string al_path = al_only.al;
    const auto cache_hit = perf::Bench(
        "al_cache_hit_" + std::to_string(entries), std::max(5, iters),
        [&] {
          std::error_code ec;
          const fs::path path{al_path};
          const std::uintmax_t size = fs::file_size(path, ec);
          const auto write_time = fs::last_write_time(path, ec);
          const std::int64_t mtime =
              static_cast<std::int64_t>(write_time.time_since_epoch().count());
          // 命中时返回的是 Project 的**副本**（lsp_server.cpp:1340 的 `return it->second.project`）
          Project copy = cached;
          if (size == 0 || mtime == 0 || copy.al_path.empty()) std::printf("unreachable\n");
        },
        warmup);
    std::printf("%-34s %7d %10.4f %10.4f %12.3f\n", "ParseAlCached 命中(2 stat+拷贝)",
                entries, cache_hit.P50(), cache_hit.P95(),
                entries > 0 ? cache_hit.P50() * 1000.0 / entries : 0.0);
    cache_hit.PrintCsv();

    // 4.3 Project 拷贝单独测（判断 4.2 里"拷贝"占多少）
    const auto copy_only = perf::Bench(
        "al_project_copy_" + std::to_string(entries), std::max(5, iters),
        [&] {
          Project copy = cached;
          if (copy.files.empty() && entries > 0) std::printf("unreachable\n");
        },
        warmup);
    std::printf("%-34s %7d %10.4f %10.4f %12.3f\n", "Project 拷贝(仅)", entries,
                copy_only.P50(), copy_only.P95(),
                entries > 0 ? copy_only.P50() * 1000.0 / entries : 0.0);
    copy_only.PrintCsv();
  }

  // 4.4 热请求的固定开销（用真实文件工程，N=50/200/400）
  // C1 之后真实链路是：ParseAlCachedShared 命中（缓存条目是 shared_ptr<const Project>，
  // 命中只拷指针 = 0 份 Project 深拷贝）-> SetProject(const Project&)（只做
  // state_.project 的 1 份拷贝）-> Get()。旧版本这里手抄的 `Project copy = cached;`
  // 复刻的是"cache → optional → 按值参数"的两份拷贝，已不代表现状。
  std::printf("\n--- 热请求固定开销（真实链路：ParseAlCachedShared 命中 + "
              "SetProject + Get）---\n");
  std::printf("%-34s %7s %10s %10s\n", "case", "files", "p50(ms)", "p95(ms)");
  for (const int files : {50, 200, 400}) {
    Synthetic synthetic = MakeProject("req_" + std::to_string(files), files);
    IndexService service(DiskLoader);
    service.SetProject(synthetic.project);
    service.SetWatcherActive(true);
    const auto built = service.Get();
    if (built == nullptr) continue;
    const std::string al_path = synthetic.al;
    // 模拟缓存条目：修复后缓存存的是 shared_ptr<const Project>，命中时只拷指针。
    const std::shared_ptr<const Project> cached =
        std::make_shared<const Project>(synthetic.project);

    const auto per_request = perf::Bench(
        "req_fixed_" + std::to_string(files), iters,
        [&] {
          // (a) ParseAlCached 命中：2 次 stat（命中只拷 shared_ptr，不拷 Project）
          std::error_code ec;
          const fs::path path{al_path};
          (void)fs::file_size(path, ec);
          (void)fs::last_write_time(path, ec);
          // (b) IndexService：工程未变 -> O(N) 比较 + state_.project 的唯一一次拷贝
          service.SetProject(*cached);
          // (c) 快照（watcher 活跃 -> 只取指针）
          const IndexService::Snapshot s = service.Get();
          if (s == nullptr) std::printf("unreachable\n");
        },
        warmup);
    std::printf("%-34s %7d %10.3f %10.3f\n", "每请求固定开销(watcher 活跃)", files,
                per_request.P50(), per_request.P95());
    per_request.PrintCsv();

    // watcher 未激活：再加上目录 stamp 校验
    service.SetWatcherActive(false);
    (void)service.Get();
    const auto per_request_slow = perf::Bench(
        "req_fixed_no_watcher_" + std::to_string(files), iters,
        [&] {
          // 同上：缓存命中 0 份 + state_.project 1 份，另加 stamp 校验
          service.SetProject(*cached);
          const IndexService::Snapshot s = service.Get();
          if (s == nullptr) std::printf("unreachable\n");
        },
        warmup);
    std::printf("%-34s %7d %10.3f %10.3f\n", "每请求固定开销(watcher 未激活)", files,
                per_request_slow.P50(), per_request_slow.P95());
    per_request_slow.PrintCsv();
    service.SetWatcherActive(true);
  }
}

// -----------------------------------------------------------------------------
// 场景 5：解析缓存行为（键计算占比 / 命中成本 / 共享证据）
// -----------------------------------------------------------------------------
void ScenarioCache(int iters, int warmup) {
  std::printf("\n=== [5] ParseCache：键计算成本与「是否深拷贝」===\n");

  struct Case {
    const char* label;
    std::string text;
    std::string path;
  };
  std::vector<Case> cases;
  {
    std::string text = MakeVerilog("c1k", 1024);
    cases.push_back({"cache 1KB", std::move(text), "Y:/perf/c1k.v"});
  }
  {
    std::string text = MakeVerilog("c100k", 100 * 1024);
    cases.push_back({"cache 100KB", std::move(text), "Y:/perf/c100k.v"});
  }

  for (const Case& c : cases) {
    ParseCache cache;
    cache.Put(c.path, c.text, std::make_shared<ParsedFile>(ParseFile(c.path, c.text)));

    const auto miss = perf::Bench(
        std::string(c.label) + "/Get(miss)", iters,
        [&] {
          ParseCache fresh;
          (void)fresh.Get(c.path, c.text);
        },
        warmup);
    const auto hit = perf::Bench(
        std::string(c.label) + "/Get(hit)", iters,
        [&] {
          const auto found = cache.Get(c.path, c.text);
          if (found == nullptr) std::printf("unreachable\n");
        },
        warmup);
    std::printf("%-28s %8zu B  miss_p50=%7.2f us  hit_p50=%7.2f us\n", c.label,
                c.text.size(), miss.P50() * 1000.0, hit.P50() * 1000.0);
    miss.PrintCsv();
    hit.PrintCsv();
  }

  // 共享证据：索引里的 ParsedFile 是否就是缓存里的那一份（B8 之前是深拷贝）
  std::printf("\n--- 深拷贝复核（BuildIndex 后 cache 与 index 是否共享同一份 ParsedFile）---\n");
  Synthetic synthetic = MakeProject("share", 50);
  ParseCache cache;
  BuildOptions options;
  options.cache = &cache;
  const auto built = BuildIndex(synthetic.project, DiskLoader, options);
  std::size_t shared = 0;
  std::size_t not_shared = 0;
  std::size_t use_count_ok = 0;
  for (std::size_t i = 0; i < synthetic.abs_paths.size(); ++i) {
    const std::string& path = synthetic.abs_paths[i];
    const auto from_cache = cache.Get(path, synthetic.texts[i]);
    const ParsedFile* in_index = built.index.File(path);
    if (from_cache == nullptr || in_index == nullptr) continue;
    if (from_cache.get() == in_index) {
      ++shared;
      if (from_cache.use_count() == 3) ++use_count_ok;  // cache + 索引 + 本函数局部
    } else {
      ++not_shared;
    }
  }
  std::printf("shared_parsedfile=%zu not_shared=%zu use_count==3(cache+index+local)=%zu\n",
              shared, not_shared, use_count_ok);
  std::printf("csv,share_check,%zu,%zu,%zu,0,0\n", shared, not_shared, use_count_ok);
}

// -----------------------------------------------------------------------------
// 场景 6：内存（每文件 MB / 峰值）—— 需要单独进程，避免互相污染峰值
// -----------------------------------------------------------------------------
void ScenarioMem(int files, int /*iters*/, int /*warmup*/) {
  std::printf("\n=== [6] 内存（N=%d）===\n", files);
  const std::size_t baseline_ws = perf::CurrentWorkingSetBytes();

  Synthetic synthetic = MakeProject("mem_" + std::to_string(files), files);
  const std::size_t after_generate = perf::CurrentWorkingSetBytes();

  IndexService service(DiskLoader);
  service.SetProject(synthetic.project);
  const auto snapshot = service.Get();
  const std::size_t after_build = perf::CurrentWorkingSetBytes();

  // 再跑一遍全量重建（cache 热），看"重复构建"会不会再涨一份
  IndexService second(DiskLoader);
  second.SetProject(synthetic.project);
  (void)second.Get();
  const std::size_t after_second = perf::CurrentWorkingSetBytes();

  // 关键对照（历史指控 §3 的证伪实验）：
  //   (a) cache + index（生产形态：索引与 ParseCache 共享同一份 ParsedFile）
  //   (b) 只有 index（BuildIndex 不给 cache：索引自己持有一份）
  //   若两者 WS 接近，说明"索引里又深拷贝了一份"确实不存在了。
  const std::size_t ws_before_nocache = perf::CurrentWorkingSetBytes();
  BuildOptions no_cache_options;  // cache == nullptr
  const auto no_cache_build = BuildIndex(synthetic.project, DiskLoader, no_cache_options);
  const std::size_t ws_after_nocache = perf::CurrentWorkingSetBytes();
  const std::size_t nocache_bytes = ws_after_nocache - ws_before_nocache;

  const std::size_t peak = perf::PeakWorkingSetBytes();
  const double mb_per_file = files > 0
                                 ? (perf::Mb(after_build) - perf::Mb(baseline_ws)) /
                                       static_cast<double>(files)
                                 : 0.0;
  const double kb_per_file = mb_per_file * 1024.0;
  const double src_mb = static_cast<double>(synthetic.source_bytes) / (1024.0 * 1024.0);
  std::printf("files=%d source=%.2f MB indexed_files=%.0f modules=%.0f\n", files, src_mb,
              snapshot != nullptr ? static_cast<double>(snapshot->FileCount()) : 0.0,
              snapshot != nullptr ? static_cast<double>(snapshot->ModuleCount()) : 0.0);
  std::printf("ws baseline=%.2f MB after_al=%.2f MB after_build=%.2f MB after_second_build=%.2f "
              "MB peak=%.2f MB\n",
              perf::Mb(baseline_ws), perf::Mb(after_generate), perf::Mb(after_build),
              perf::Mb(after_second), perf::Mb(peak));
  std::printf("delta_build=%.2f MB  KB/file=%.1f  (源码 %.1f KB/file, 比例 %.1fx)\n",
              perf::Mb(after_build) - perf::Mb(baseline_ws), kb_per_file,
              files > 0 ? synthetic.source_bytes / 1024.0 / files : 0.0,
              files > 0 && synthetic.source_bytes > 0
                  ? (static_cast<double>(after_build - baseline_ws) / files) /
                        (static_cast<double>(synthetic.source_bytes) / files)
                  : 0.0);
  std::printf("no_cache_build: files=%zu index_bytes=%.2f MB (%.1f KB/file) "
              "[对照: cache+index 共享 = %.1f KB/file]\n",
              no_cache_build.index.FileCount(), perf::Mb(nocache_bytes),
              files > 0 ? static_cast<double>(nocache_bytes) / files / 1024.0 : 0.0,
              kb_per_file);
  std::printf("csv,mem_%d,%d,%.3f,%.3f,%.3f,%.3f\n", files, after_build - baseline_ws,
              perf::Mb(after_build), perf::Mb(peak), kb_per_file);
}

}  // namespace

int main(int argc, char** argv) {
  std::string scenario = "all";
  int mem_size = 200;
  int iters = 30;
  int warmup = 3;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--scenario" && i + 1 < argc) {
      scenario = argv[++i];
    } else if (arg == "--size" && i + 1 < argc) {
      mem_size = std::atoi(argv[++i]);
    } else if (arg == "--iters" && i + 1 < argc) {
      iters = std::atoi(argv[++i]);
    } else if (arg == "--warmup" && i + 1 < argc) {
      warmup = std::atoi(argv[++i]);
    } else if (arg == "--help" || arg == "-h") {
      std::printf("usage: perf_index [--scenario all|stamps|parse|build|incremental|al|cache|mem] "
                  "[--size N] [--iters N] [--warmup N]\n");
      return 0;
    }
  }

  perf::Banner("index");
  const CpuTimes load_before = ReadCpuTimes();

  std::printf("scenario=%s iters=%d warmup=%d mem_size=%d temp=%s\n\n", scenario.c_str(),
              iters, warmup, mem_size, PerfRoot().generic_string().c_str());

  if (scenario == "all" || scenario == "stamps") ScenarioStamps(iters, warmup);
  if (scenario == "all" || scenario == "parse") ScenarioParse(iters, warmup);
  if (scenario == "all" || scenario == "build") ScenarioBuild(iters, warmup);
  if (scenario == "all" || scenario == "incremental") ScenarioIncremental(iters, warmup);
  if (scenario == "all" || scenario == "al") ScenarioAl(iters, warmup);
  if (scenario == "all" || scenario == "cache") ScenarioCache(iters, warmup);
  // 内存场景必须单独进程跑（峰值 working set 会被前面场景污染）：
  //     perf_index.exe --scenario mem --size 200
  if (scenario == "mem") ScenarioMem(mem_size, iters, warmup);

  const CpuTimes load_after = ReadCpuTimes();
  std::printf("\nsystem_cpu_busy=%.3f (before/after GetSystemTimes diff)\n",
              CpuBusy(load_before, load_after));
  std::printf("process_peak_ws=%.2f MB\n", perf::Mb(perf::PeakWorkingSetBytes()));
  std::printf("csv,system_cpu_busy,1,%.4f,0,0,0\n", CpuBusy(load_before, load_after));
  return 0;
}