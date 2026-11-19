// =============================================================================
//  probe_smoke.cpp —— 探针骨架自检（模板，新增探针时复制改名）
// =============================================================================
//  build/bin/perf_smoke.exe --iters 50
//  只验证：perf_common.h 可用、链接上全部 fpga_lsp 静态库、计时链路正常。
// =============================================================================
#include <cstdio>
#include <string>

#include "perf_common.h"

int main(int argc, char** argv) {
  const perf::Cli cli = perf::ParseCli(argc, argv);
  perf::Banner("smoke");

  // 一个足够无聊但真会分配的量，用来确认计时与分位数在纳秒~微秒级也稳定。
  auto st = perf::Bench(
      "string_append_1k", cli.iters,
      [] {
        std::string s;
        s.reserve(4096);
        for (int i = 0; i < 1000; ++i) s += "abcdefgh";
        if (s.size() == 0) std::printf("unreachable\n");
      },
      cli.warmup);
  st.Print();
  st.PrintCsv();

  std::printf("\nworking_set=%.2f MB peak_working_set=%.2f MB\n",
              perf::Mb(perf::CurrentWorkingSetBytes()),
              perf::Mb(perf::PeakWorkingSetBytes()));
  return 0;
}