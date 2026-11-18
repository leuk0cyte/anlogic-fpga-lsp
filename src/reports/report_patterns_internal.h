// =============================================================================
//  src/reports/report_patterns_internal.h —— reports 内部的正则自检钩子
// =============================================================================
//  仅给 report_text.cpp 的 AllReportPatternsValid 用。不是公开 API。
//
//  存在理由：re2 编译失败**不会抛异常**，只会让该模式永远匹配不到。对报告解析
//  来说那就是"换了个器件族/换了个 TD 版本后静默返回空"，是最难查的一类 bug。
//  所以每个解析器都把自己的模式表交出来，由单元测试一次性验证全部可编译。
// =============================================================================

#pragma once

#include <string>

namespace fpgalsp::reports::detail {

bool ValidateQorPatterns(std::string* error);
bool ValidateTimingPatterns(std::string* error);
bool ValidateAreaPatterns(std::string* error);
bool ValidateFlowStatusPatterns(std::string* error);

}  // namespace fpgalsp::reports::detail