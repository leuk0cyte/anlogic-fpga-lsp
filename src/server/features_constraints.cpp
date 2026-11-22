// =============================================================================
//  src/server/features_constraints.cpp —— .adc/.sdc 的符号 / hover / completion
// =============================================================================
//  OWNER: task B10 (parity-symbols)。
//
//  本文件是**协议 JSON 的唯一出口**：src/constraints/{adc,sdc}.cpp 里只有
//  「文本 -> JSON」的纯函数，扩展名判定、文本装载后的分派都在这里。
//
//  与 Python 的对齐口径（探针 parity-lsp/raw.json boundaries[adc:*, sdc:*]）：
//    · documentSymbol  pin.adc -> 47 条（kind 8，detail = LOCATION）
//                      top.sdc  -> 2 条（kind 3，detail = 参数拼接前 60 字）
//    · hover           非 null；命令/整行属性都是 markdown
//    · completion      {"isIncomplete":false,"items":[…]}
//
//  为什么 hover/completion 是自由函数而不是 `LspServer::Feature*`：
//    见 constraints_features.h 的说明（B1 只固定了 FeatureConstraintSymbols 一个签名，
//    B10 不改 lsp_server.h）。
// =============================================================================

#include "server/constraints_features.h"

#include <string>
#include <string_view>
#include <vector>

#include "constraints/adc.h"
#include "constraints/sdc.h"
#include "server/lsp_server.h"

#include "platform/text.h"
namespace fpgalsp::server {
namespace {

using platform::LowerAscii;

using platform::EndsWith;

enum class ConstraintKind { kNone, kAdc, kSdc };

ConstraintKind KindOf(std::string_view path) {
    const std::string lowered = LowerAscii(path);
    if (EndsWith(lowered, ".adc")) return ConstraintKind::kAdc;
    if (EndsWith(lowered, ".sdc")) return ConstraintKind::kSdc;
    return ConstraintKind::kNone;
}

}  // namespace

bool IsConstraintDocument(std::string_view path) {
    return KindOf(path) != ConstraintKind::kNone;
}

// B-B1：约束诊断（编辑器 Problems 与 MCP fpga_diagnostics 走同一批纯函数）。
lsp::DiagnosticList ConstraintDiagnostics(std::string_view path, std::string_view text,
                                          const std::vector<constraints::Port>& ports) {
    lsp::DiagnosticList out;
    const auto ends_with = [](std::string_view value, std::string_view suffix) {
        if (value.size() < suffix.size()) return false;
        for (std::size_t i = 0; i < suffix.size(); ++i) {
            const char left = static_cast<char>(
                std::tolower(static_cast<unsigned char>(value[value.size() - suffix.size() + i])));
            if (left != suffix[i]) return false;
        }
        return true;
    };
    if (ends_with(path, ".adc")) {
        const std::vector<constraints::adc::Entry> entries = constraints::adc::Parse(text);
        // B-B5：顺便给出"未约束 IO"的覆盖率概览（一条 info；全部约束时不产生）。
        constraints::adc::DiagnosticsOptions options;
        // A-6：DiagnosticsOptions::text 现在有所有权（std::string），这里必须显式拷贝。
        // 以前是 string_view 直接指向上面的 text，语义等价但类型上不表达生命周期。
        options.text = std::string(text);
        options.include_coverage = true;
        for (const lsp::Diagnostic& diagnostic :
             constraints::adc::Diagnostics(entries, ports, options)) {
            // 落在顶层端口声明行的 bit 宽度提示由 HDL 层发布，这里不重复。
            if (diagnostic.code == "BIT-8123") continue;
            out.push_back(diagnostic);
        }
    } else if (ends_with(path, ".sdc")) {
        const std::vector<constraints::sdc::Command> commands = constraints::sdc::Parse(text);
        for (const lsp::Diagnostic& diagnostic :
             constraints::sdc::Diagnostics(commands, ports, nullptr)) {
            out.push_back(diagnostic);
        }
    }
    return out;
}

Json ConstraintDocumentSymbols(std::string_view path, std::string_view text) {
    switch (KindOf(path)) {
        case ConstraintKind::kAdc:
            return constraints::adc::DocumentSymbols(text);
        case ConstraintKind::kSdc:
            return constraints::sdc::DocumentSymbols(text);
        case ConstraintKind::kNone:
            break;
    }
    return Json::array();
}

Json ConstraintHover(std::string_view path, std::string_view text, int line, int character,
                     const std::vector<constraints::Port>& ports) {
    switch (KindOf(path)) {
        case ConstraintKind::kAdc:
            return constraints::adc::Hover(text, line, character, ports);
        case ConstraintKind::kSdc:
            return constraints::sdc::Hover(text, line, character, ports);
        case ConstraintKind::kNone:
            break;
    }
    return Json(nullptr);
}

Json ConstraintCompletion(std::string_view path, std::string_view text, int line, int character,
                          const std::vector<constraints::Port>& ports) {
    switch (KindOf(path)) {
        case ConstraintKind::kAdc:
            return constraints::adc::Completion(text, line, character, ports);
        case ConstraintKind::kSdc:
            return constraints::sdc::LspCompletion(text, line, character, ports);
        case ConstraintKind::kNone:
            break;
    }
    return Json::object();
}

// -----------------------------------------------------------------------------
//  LspServer::FeatureConstraintSymbols —— B1 固定的唯一约束符号入口
// -----------------------------------------------------------------------------
//  只读**文本**（缓冲区优先）；工程/索引一概不碰：`.adc` 的 documentSymbol 与
//  顶层端口表无关，这样才能在"工程还没索引完 / 单文件打开"时也给出符号。
Json LspServer::FeatureConstraintSymbols(std::string_view path) {
    if (path.empty()) return Json::array();
    if (!IsConstraintDocument(path)) return Json::array();
    const std::optional<std::string> text = LoadText(std::string(path));
    const std::string_view view =
        text.has_value() ? std::string_view(*text) : std::string_view();
    return ConstraintDocumentSymbols(path, view);
}

}  // namespace fpgalsp::server