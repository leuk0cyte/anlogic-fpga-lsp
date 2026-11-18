// =============================================================================
//  src/lsp/diagnostic.cpp
// =============================================================================

#include "lsp/diagnostic.h"

#include <string>
#include <utility>

namespace fpgalsp::lsp {
namespace {

// 从 json 安全取 int；缺失/类型不符返回 default
int ReadInt(const Json& obj, const char* key, int default_value) {
    if (!obj.is_object()) return default_value;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_number()) return default_value;
    try {
        return it->get<int>();
    } catch (...) {
        return default_value;
    }
}

std::string ReadString(const Json& obj, const char* key, std::string default_value = {}) {
    if (!obj.is_object()) return default_value;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return default_value;
    try {
        return it->get<std::string>();
    } catch (...) {
        return default_value;
    }
}

}  // namespace

// -----------------------------------------------------------------------------
// 构造辅助
// -----------------------------------------------------------------------------

Range MakeRange(int line, int start_char, int end_line, int end_char) {
    Range range;
    range.start.line = line;
    range.start.character = start_char;
    // 对应 Python: end_line if end_line is not None else line
    range.end.line = (end_line < 0) ? line : end_line;
    // 对应 Python: end_char if end_char is not None else start_char
    range.end.character = (end_char < 0) ? start_char : end_char;
    return range;
}

Diagnostic Diagnostic::AtLine(int line, int start_char, int end_char, int severity,
                              std::string code, std::string source, std::string message) {
    Diagnostic d;
    d.range = MakeRange(line, start_char, line, end_char);
    d.severity = severity;
    d.code = std::move(code);
    d.source = std::move(source);
    d.message = std::move(message);
    return d;
}

Diagnostic Diagnostic::Span(int start_line, int start_char, int end_line, int end_char,
                            int severity, std::string code, std::string source,
                            std::string message) {
    Diagnostic d;
    d.range = MakeRange(start_line, start_char, end_line, end_char);
    d.severity = severity;
    d.code = std::move(code);
    d.source = std::move(source);
    d.message = std::move(message);
    return d;
}

Diagnostic& Diagnostic::WithData(std::string key, Json value) {
    if (!data.is_object()) data = Json::object();
    data[std::move(key)] = std::move(value);
    return *this;
}

Diagnostic& Diagnostic::WithBackend(std::string_view backend) {
    return WithData("backend", Json(std::string(backend)));
}

// -----------------------------------------------------------------------------
// 序列化
// -----------------------------------------------------------------------------

Json ToJson(const Diagnostic& diagnostic) {
    Json out = Json::object();

    Json range = Json::object();
    Json start = Json::object();
    start["line"] = diagnostic.range.start.line;
    start["character"] = diagnostic.range.start.character;
    Json end = Json::object();
    end["line"] = diagnostic.range.end.line;
    end["character"] = diagnostic.range.end.character;
    range["start"] = std::move(start);
    range["end"] = std::move(end);

    out["range"] = std::move(range);
    out["severity"] = diagnostic.severity;
    out["code"] = diagnostic.code;
    out["source"] = diagnostic.source;
    out["message"] = diagnostic.message;

    // 对应 Python _sanitize: `if diag.get("data"): out["data"] = diag["data"]`
    // 即 data 为空(None/{}/[])时不输出该字段
    if (!diagnostic.data.is_null() &&
        !(diagnostic.data.is_object() && diagnostic.data.empty()) &&
        !(diagnostic.data.is_array() && diagnostic.data.empty())) {
        out["data"] = diagnostic.data;
    }
    return out;
}

Diagnostic FromJson(const Json& json) {
    Diagnostic d;
    if (!json.is_object()) return d;

    if (const auto it = json.find("range"); it != json.end() && it->is_object()) {
        if (const auto s = it->find("start"); s != it->end()) {
            d.range.start.line = ReadInt(*s, "line", 0);
            d.range.start.character = ReadInt(*s, "character", 0);
        }
        if (const auto e = it->find("end"); e != it->end()) {
            d.range.end.line = ReadInt(*e, "line", 0);
            d.range.end.character = ReadInt(*e, "character", 0);
        }
    }
    d.severity = ReadInt(json, "severity", kError);
    d.code = ReadString(json, "code");
    d.source = ReadString(json, "source");
    d.message = ReadString(json, "message");

    if (const auto it = json.find("data"); it != json.end()) {
        d.data = *it;
    }
    return d;
}

Json ToJson(const DiagnosticList& diagnostics) {
    Json out = Json::array();
    for (const auto& d : diagnostics) out.push_back(ToJson(d));
    return out;
}

// -----------------------------------------------------------------------------
// Sanitize（对应 server.py 的 _sanitize）
// -----------------------------------------------------------------------------

bool IsValidSeverity(int severity) { return severity >= kError && severity <= kHint; }

Diagnostic Sanitize(const Diagnostic& diagnostic, std::string_view source_default) {
    // never-raise：全部走"补齐"而不是报错
    Diagnostic out = diagnostic;

    // severity: Python `int(diag.get("severity") or 2)` —— 缺失或 0 都回落到 2
    if (out.severity == 0) {
        out.severity = kWarning;
    }
    if (!IsValidSeverity(out.severity)) {
        out.severity = kWarning;
    }

    // code: Python `str(diag.get("code") or "LSP")`
    if (out.code.empty()) {
        out.code = "LSP";
    }

    // source
    if (out.source.empty()) {
        out.source = std::string(source_default);
    }

    // range：Python 在 range 为 None 时给 {0,0}-{0,1}。
    // 这里判定"未设置"的启发式：start/end 全为 0。
    if (out.range.start.line == 0 && out.range.start.character == 0 &&
        out.range.end.line == 0 && out.range.end.character == 0) {
        out.range = MakeRange(0, 0, 0, 1);
    }

    // message：Python `str(diag.get("message") or "")` —— 空即空
    // （保持空串，不塞占位文本，避免与 Python 产生措辞差异）

    return out;
}

}  // namespace fpgalsp::lsp