// =============================================================================
//  src/project/al_project.cpp
// =============================================================================

#include "project/al_project.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "platform/encoding.h"
#include "platform/path.h"
#include "project/xml_lite.h"

namespace fpgalsp::project {
namespace {

namespace plat = fpgalsp::platform;

constexpr const char* kVerilogExt[] = {".v", ".sv", ".vh", ".svh"};
constexpr const char* kVhdlExt[] = {".vhd", ".vhdl"};

std::string ToLower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string Trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

bool EndsWithAny(std::string_view s, const char* const* exts, std::size_t count) {
    const std::string lower = ToLower(s);
    for (std::size_t i = 0; i < count; ++i) {
        const std::string_view ext = exts[i];
        if (lower.size() >= ext.size() &&
            lower.compare(lower.size() - ext.size(), ext.size(), ext) == 0) {
            return true;
        }
    }
    return false;
}

FileKind KindOfTag(std::string_view tag) {
    const std::string local = LocalName(tag);
    if (local == "Verilog") return FileKind::Verilog;
    if (local == "VHDL") return FileKind::Vhdl;
    if (local == "ADC_FILE") return FileKind::Adc;
    if (local == "SDC_FILE") return FileKind::Sdc;
    if (local == "IP_FILE") return FileKind::Ip;
    if (local == "CWC_FILE") return FileKind::Cwc;
    return FileKind::Other;
}

// 对应 Python 的 _attrs(node)：递归收集所有后代里本地名为 Attr 的节点，
// 取 Name 作键、Val 作值（Name 为空则跳过）。
void CollectAttrs(const XmlNode& node,
                  std::vector<std::pair<std::string, std::string>>& out) {
    // 先序遍历：与 ElementTree 的 iter() 顺序一致
    if (LocalName(node.tag) == "Attr") {
        const std::string name = node.GetAttr("Name");
        if (!name.empty()) {
            out.emplace_back(name, node.GetAttr("Val", ""));
        }
    }
    for (const auto& child : node.children) {
        CollectAttrs(child, out);
    }
}

// 正则在文本中找 <Tag>...</Tag> 的内容并 strip（兜底路径用）
std::optional<std::string> RegexInnerText(std::string_view text, std::string_view tag) {
    const std::string open = "<" + std::string(tag) + ">";
    const std::string close = "</" + std::string(tag) + ">";
    const std::size_t p = text.find(open);
    if (p == std::string_view::npos) return std::nullopt;
    const std::size_t start = p + open.size();
    const std::size_t e = text.find(close, start);
    if (e == std::string_view::npos) return std::nullopt;
    return Trim(text.substr(start, e - start));
}

// 对应 Python: re.search(r"Last modified by ([A-Za-z0-9_. ]+)", text)
std::string FindModifiedBy(std::string_view text) {
    constexpr std::string_view kNeedle = "Last modified by ";
    const std::size_t p = text.find(kNeedle);
    if (p == std::string_view::npos) return {};

    std::size_t i = p + kNeedle.size();
    const std::size_t start = i;
    while (i < text.size()) {
        const char c = text[i];
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '.' || c == ' ';
        if (!ok) break;
        ++i;
    }
    if (i == start) return {};
    return Trim(text.substr(start, i - start));
}

bool IsAbsoluteDeclared(std::string_view rel) {
    if (rel.empty()) return false;
    // "Y:/..." 或 "/..."
    if (rel.size() >= 3 && std::isalpha(static_cast<unsigned char>(rel[0])) && rel[1] == ':' &&
        (rel[2] == '/' || rel[2] == '\\')) {
        return true;
    }
    if (rel[0] == '/' || rel[0] == '\\') return true;
    return false;
}

}  // namespace

// -----------------------------------------------------------------------------
// 枚举名
// -----------------------------------------------------------------------------

std::string_view FileKindName(FileKind kind) {
    switch (kind) {
        case FileKind::Verilog: return "verilog";
        case FileKind::Vhdl: return "vhdl";
        case FileKind::Adc: return "adc";
        case FileKind::Sdc: return "sdc";
        case FileKind::Ip: return "ip";
        case FileKind::Cwc: return "cwc";
        case FileKind::Other: return "other";
    }
    return "other";
}

std::string_view ResolvedViaName(ResolvedVia via) {
    switch (via) {
        case ResolvedVia::Project: return "project";
        case ResolvedVia::Declared: return "declared";
        case ResolvedVia::Absolute: return "absolute";
        case ResolvedVia::Missing: return "missing";
    }
    return "missing";
}

std::string AlFile::GetAttr(std::string_view name, std::string default_value) const {
    for (const auto& [key, value] : attrs) {
        if (key == name) return value;
    }
    return default_value;
}

// -----------------------------------------------------------------------------
// 路径解析
// -----------------------------------------------------------------------------

bool DefaultFileExists(std::string_view path) {
    if (path.empty()) return false;
    try {
        return std::filesystem::is_regular_file(std::filesystem::path(std::string(path)));
    } catch (...) {
        return false;
    }
}

ResolveResult ResolveRelative(std::string_view project_dir, std::string_view rel,
                              std::string_view declared_dir) {
    // 对应 Python：rel = (rel or "").replace("\\", "/").strip()
    std::string cleaned(rel);
    for (char& c : cleaned) {
        if (c == '\\') c = '/';
    }
    cleaned = Trim(cleaned);

    if (cleaned.empty()) {
        return {"", ResolvedVia::Missing};
    }

    if (IsAbsoluteDeclared(cleaned)) {
        const std::string path = plat::normalize_path(cleaned);
        return {path, DefaultFileExists(path) ? ResolvedVia::Absolute : ResolvedVia::Missing};
    }

    const std::string candidate = plat::normalize_path(plat::path_join(project_dir, cleaned));
    if (DefaultFileExists(candidate)) {
        return {candidate, ResolvedVia::Project};
    }

    if (!declared_dir.empty()) {
        const std::string fallback =
            plat::normalize_path(plat::path_join(declared_dir, cleaned));
        if (DefaultFileExists(fallback)) {
            return {fallback, ResolvedVia::Declared};
        }
    }

    return {candidate, ResolvedVia::Missing};
}

// -----------------------------------------------------------------------------
// Project 派生
// -----------------------------------------------------------------------------

std::vector<const AlFile*> Project::Group(std::initializer_list<FileKind> kinds,
                                         bool in_build_only) const {
    std::vector<const AlFile*> out;
    for (const auto& f : files) {
        const bool kind_match =
            std::find(kinds.begin(), kinds.end(), f.kind) != kinds.end();
        if (!kind_match) continue;
        if (in_build_only && !f.InBuild()) continue;
        out.push_back(&f);
    }
    std::sort(out.begin(), out.end(), [](const AlFile* a, const AlFile* b) {
        if (a->compile_order != b->compile_order) return a->compile_order < b->compile_order;
        return a->path < b->path;
    });
    return out;
}

std::vector<const AlFile*> Project::HdlFiles() const {
    return Group({FileKind::Verilog, FileKind::Vhdl});
}
std::vector<const AlFile*> Project::VerilogFiles() const { return Group({FileKind::Verilog}); }
std::vector<const AlFile*> Project::VhdlFiles() const { return Group({FileKind::Vhdl}); }
std::vector<const AlFile*> Project::AdcFiles() const { return Group({FileKind::Adc}); }
std::vector<const AlFile*> Project::SdcFiles() const { return Group({FileKind::Sdc}); }
std::vector<const AlFile*> Project::IpFiles() const { return Group({FileKind::Ip}); }

std::vector<const AlFile*> Project::NotInBuild() const {
    std::vector<const AlFile*> out;
    for (const auto& f : files) {
        if (!f.InBuild()) out.push_back(&f);
    }
    return out;
}

std::vector<std::string> Project::HdlPaths() const {
    std::vector<std::string> out;
    for (const AlFile* f : HdlFiles()) out.push_back(f->path);
    return out;
}

bool Project::PathMatches() const {
    if (declared_path.empty()) return true;
    return ToLower(plat::normalize_path(declared_path)) == ToLower(plat::normalize_path(dir));
}

std::string Project::Describe() const {
    return name + " [" + family + " " + device + "] top=" + top +
           " files=" + std::to_string(HdlFiles().size());
}

// -----------------------------------------------------------------------------
// 解析
// -----------------------------------------------------------------------------

Project ParseAlText(std::string_view al_path, std::string_view text) {
    Project proj;
    const std::string normalized_al = plat::normalize_path(al_path);
    proj.al_path = normalized_al;
    proj.dir = plat::path_dirname(normalized_al);

    // 1) 容错预处理：转义裸 &
    const std::string tolerant = TolerantXml(text);

    // 2) 解析
    XmlParseResult xml = ParseXml(tolerant);

    if (!xml.ok) {
        // ---- 正则兜底（复现 project.py）----
        proj.warnings.push_back("XML parse failed (.al may be locked or corrupted): " +
                                xml.error);
        if (auto v = RegexInnerText(text, "Device")) proj.device = *v;
        if (auto v = RegexInnerText(text, "Family")) proj.family = *v;
        if (auto v = RegexInnerText(text, "MODULE")) proj.top = *v;

        // name 兜底：文件名去扩展名
        const std::string base = plat::path_filename(normalized_al);
        const std::size_t dot = base.find_last_of('.');
        proj.name = (dot == std::string::npos) ? base : base.substr(0, dot);
        return proj;
    }

    const XmlNode& root = xml.root;
    proj.declared_path = root.GetAttr("Path", "");

    // 3) 遍历取 HardWare / TOP_MODULE / TD_Version
    //    用显式递归代替 Python 的 root.iter()（同样是先序）
    std::vector<const XmlNode*> all_nodes;
    {
        std::vector<const XmlNode*> stack{&root};
        while (!stack.empty()) {
            const XmlNode* node = stack.back();
            stack.pop_back();
            all_nodes.push_back(node);
            for (auto it = node->children.rbegin(); it != node->children.rend(); ++it) {
                stack.push_back(&(*it));
            }
        }
    }

    for (const XmlNode* node : all_nodes) {
        const std::string tag = LocalName(node->tag);
        if (tag == "HardWare") {
            for (const auto& child : node->children) {
                const std::string name = LocalName(child.tag);
                const std::string val = Trim(child.text);
                if (name == "Family") {
                    proj.family = val;
                } else if (name == "Device") {
                    proj.device = val;
                } else if (name == "Speed") {
                    // TD 写 "-2"，Tcl 命令要 "2"：去掉符号位
                    std::size_t k = 0;
                    while (k < val.size() && (val[k] == '+' || val[k] == '-')) ++k;
                    const std::string stripped = Trim(std::string_view(val).substr(k));
                    proj.speed = stripped.empty() ? std::nullopt
                                                  : std::optional<std::string>(stripped);
                }
            }
        } else if (tag == "TOP_MODULE") {
            for (const auto& child : node->children) {
                if (LocalName(child.tag) == "MODULE") {
                    proj.top = Trim(child.text);
                }
            }
        } else if (tag == "TD_Version") {
            proj.td_version = Trim(node->text);
        }
    }

    // 4) <Name> 必须是 <Project> 的直接子元素
    for (const auto& child : root.children) {
        if (LocalName(child.tag) == "Name") {
            proj.name = Trim(child.text);
        }
    }
    if (proj.name.empty()) {
        const std::string base = plat::path_filename(normalized_al);
        const std::size_t dot = base.find_last_of('.');
        proj.name = (dot == std::string::npos) ? base : base.substr(0, dot);
    }

    // 5) Last modified by（在原始文本里找，注释里也有）
    proj.modified_by = FindModifiedBy(text);

    // 6) Source_Files / 各 kind section 下的 <File>
    for (const XmlNode* section : all_nodes) {
        const FileKind kind = KindOfTag(section->tag);
        if (kind == FileKind::Other) continue;

        for (const auto& file_node : section->children) {
            if (LocalName(file_node.tag) != "File") continue;
            const std::string rel = file_node.GetAttr("Path", "");
            if (rel.empty()) continue;

            std::vector<std::pair<std::string, std::string>> attrs;
            CollectAttrs(file_node, attrs);

            auto attr_value = [&attrs](std::string_view key,
                                       std::string_view def) -> std::string {
                for (const auto& [k, v] : attrs) {
                    if (k == key) return v;
                }
                return std::string(def);
            };

            const bool enabled = ToLower(attr_value("Enable", "true")) != "false";
            const bool excluded = ToLower(attr_value("AutoExcluded", "false")) == "true";

            int order = 0;
            {
                const std::string raw = Trim(attr_value("CompileOrder", "0"));
                if (!raw.empty()) {
                    try {
                        std::size_t consumed = 0;
                        order = std::stoi(raw, &consumed);
                        if (consumed != raw.size()) order = 0;
                    } catch (...) {
                        order = 0;  // 对应 Python 的 except ValueError: order = 0
                    }
                }
            }

            ResolveResult resolved = ResolveRelative(proj.dir, rel, proj.declared_path);

            FileKind effective_kind = kind;
            // verilog section 里出现非 verilog 扩展名 -> 降级为 other
            if (kind == FileKind::Verilog &&
                !EndsWithAny(resolved.path, kVerilogExt,
                             sizeof(kVerilogExt) / sizeof(kVerilogExt[0]))) {
                effective_kind = FileKind::Other;
            }

            AlFile file;
            file.path = resolved.path;
            file.rel = rel;
            file.kind = effective_kind;
            file.compile_order = order;
            file.enabled = enabled;
            file.excluded = excluded;
            file.exists = DefaultFileExists(resolved.path);
            file.attrs = std::move(attrs);
            file.resolved_via = resolved.via;
            proj.files.push_back(std::move(file));
        }
    }

    // ---- 7) 结构化警告（对应 Python 的 structural warnings）----

    if (proj.top.empty()) {
        proj.warnings.push_back("Project has no top module (TOP_MODULE)");
    }
    if (proj.device.empty()) {
        proj.warnings.push_back("Project has no target device (HardWare/Device)");
    }
    if (!proj.declared_path.empty() && !proj.PathMatches()) {
        proj.warnings.push_back(
            "Project was created at " + proj.declared_path + " but the current directory is " +
            proj.dir +
            "; when TD cannot find a file it silently falls back to the original path "
            "(PRJ-6619)");
    }
    for (const auto& f : proj.files) {
        if (f.resolved_via == ResolvedVia::Declared) {
            proj.warnings.push_back(
                "File is outside the project directory and was found at the original path "
                "recorded in the .al (project moved/copied): " +
                f.rel);
        }
    }
    for (const auto& f : proj.files) {
        if (!f.exists) {
            proj.warnings.push_back("Missing build file: " + f.rel);
        }
    }
    if (proj.AdcFiles().empty()) {
        proj.warnings.push_back(
            "No ADC pin constraint file: bitgen will reject the design with BIT-8123 "
            "(projects with I/O must constrain pins)");
    }
    if (proj.SdcFiles().empty()) {
        proj.warnings.push_back(
            "No SDC timing constraint file: timing checks will have no constrained objects");
    }

    // HDL 文件在磁盘上但被排除且不在构建里
    std::set<std::string> hdl_present;
    for (const AlFile* f : proj.HdlFiles()) hdl_present.insert(f->path);
    for (const auto& f : proj.files) {
        if (f.kind == FileKind::Verilog && !f.InBuild() && f.exists &&
            hdl_present.find(f.path) == hdl_present.end()) {
            proj.warnings.push_back(
                "HDL file on disk is unregistered/excluded and not part of the build: " +
                f.rel);
        }
    }

    return proj;
}

Project ParseAl(std::string_view al_path) {
    const std::string normalized = plat::normalize_path(al_path);
    const plat::ReadFileResult file = plat::read_file_utf8(normalized);

    Project proj;
    proj.al_path = normalized;
    proj.dir = plat::path_dirname(normalized);

    if (!file.ok) {
        proj.warnings.push_back("Cannot read .al: " + normalized);
        return proj;
    }

    return ParseAlText(normalized, file.content);
}

// -----------------------------------------------------------------------------
// 工程发现
// -----------------------------------------------------------------------------

std::optional<std::string> FindAl(std::string_view start_path, int max_up) {
    const std::string path = plat::normalize_path(start_path);
    if (path.empty()) return std::nullopt;

    std::string directory;
    try {
        directory = std::filesystem::is_directory(std::filesystem::path(path))
                        ? path
                        : plat::path_dirname(path);
    } catch (...) {
        directory = plat::path_dirname(path);
    }

    for (int i = 0; i <= max_up; ++i) {
        if (directory.empty()) break;

        std::vector<std::string> entries;
        try {
            for (const auto& entry :
                 std::filesystem::directory_iterator(std::filesystem::path(directory))) {
                if (!entry.is_regular_file()) continue;
                const std::string name = entry.path().filename().string();
                if (ToLower(name).size() >= 3 &&
                    ToLower(name).compare(ToLower(name).size() - 3, 3, ".al") == 0) {
                    entries.push_back(name);
                }
            }
        } catch (...) {
            entries.clear();
        }

        if (entries.size() == 1) {
            return plat::normalize_path(plat::path_join(directory, entries[0]));
        }
        if (entries.size() > 1) {
            // 有歧义：优先选文件名 stem 与目录名相同者
            const std::string base = ToLower(plat::path_filename(directory));
            for (const auto& e : entries) {
                const std::size_t dot = e.find_last_of('.');
                const std::string stem = (dot == std::string::npos) ? e : e.substr(0, dot);
                if (ToLower(stem) == base) {
                    return plat::normalize_path(plat::path_join(directory, e));
                }
            }
            std::vector<std::string> sorted = entries;
            std::sort(sorted.begin(), sorted.end());
            return plat::normalize_path(plat::path_join(directory, sorted[0]));
        }

        const std::string parent = plat::path_dirname(directory);
        if (parent == directory || parent.empty()) break;
        directory = parent;
    }
    return std::nullopt;
}

std::vector<std::string> DiscoverProjects(std::string_view root, int max_depth,
                                          std::size_t limit) {
    std::vector<std::string> results;

    if (root.empty()) return results;

    try {
        if (!std::filesystem::is_directory(std::filesystem::path(std::string(root)))) {
            return results;
        }
    } catch (...) {
        return results;
    }

    static const std::set<std::string> kSkipDirs = {
        ".git", ".svn", ".hg", "node_modules", ".fpgalsp", "log", "logs", "obj",
        "dist", "build", "__pycache__", ".vs", ".idea", "out", "backup", "bak"};

    // 递归用显式栈，避免深目录把调用栈打爆
    struct Frame {
        std::string dir;
        int depth;
    };
    std::vector<Frame> stack{{plat::normalize_path(root), 0}};

    while (!stack.empty()) {
        if (results.size() >= limit) break;
        const Frame frame = stack.back();
        stack.pop_back();
        if (frame.depth > max_depth) continue;

        std::vector<std::string> entries;
        try {
            for (const auto& entry :
                 std::filesystem::directory_iterator(std::filesystem::path(frame.dir))) {
                entries.push_back(entry.path().filename().string());
            }
        } catch (...) {
            continue;
        }
        std::sort(entries.begin(), entries.end());

        // 先收 .al（对应 Python 先遍历文件名再遍历目录）
        for (const auto& name : entries) {
            const std::string lower = ToLower(name);
            if (lower.size() >= 3 && lower.compare(lower.size() - 3, 3, ".al") == 0) {
                results.push_back(plat::normalize_path(plat::path_join(frame.dir, name)));
                if (results.size() >= limit) break;
            }
        }

        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            const std::string& name = *it;
            const std::string full = plat::path_join(frame.dir, name);
            bool is_dir = false;
            try {
                is_dir = std::filesystem::is_directory(std::filesystem::path(full));
            } catch (...) {
                is_dir = false;
            }
            if (!is_dir) continue;

            const std::string lower = ToLower(name);
            if (kSkipDirs.count(lower) > 0) continue;
            if (!lower.empty() && lower[0] == '.') continue;
            if (lower.size() >= 5 && lower.compare(lower.size() - 5, 5, "_runs") == 0) continue;

            stack.push_back({full, frame.depth + 1});
        }
    }

    return results;
}

std::pair<int, std::size_t> Affinity(std::string_view al_path, std::string_view path) {
    const std::string project_dir = ToLower(plat::normalize_path(plat::path_dirname(al_path)));
    const std::string target = ToLower(plat::normalize_path(path));

    // 目标在工程目录内 -> tier 2，按目录长度决胜
    const std::string prefix = project_dir + "/";
    if (target.compare(0, prefix.size(), prefix) == 0) {
        return {2, project_dir.size()};
    }

    // 公共路径前缀长度
    std::size_t common = 0;
    std::size_t i = 0, j = 0;
    while (i < project_dir.size() && j < target.size()) {
        const std::size_t ni = project_dir.find('/', i);
        const std::size_t nj = target.find('/', j);
        const std::string_view pi = project_dir.substr(
            i, (ni == std::string::npos ? project_dir.size() : ni) - i);
        const std::string_view pj = target.substr(
            j, (nj == std::string::npos ? target.size() : nj) - j);
        if (pi != pj) break;
        ++common;
        if (ni == std::string::npos || nj == std::string::npos) break;
        i = ni + 1;
        j = nj + 1;
    }
    return {1, common};
}

std::optional<std::string> FindProjectAl(std::string_view root, std::string_view path,
                                        std::string_view configured_al) {
    // 1) 显式配置优先
    if (!configured_al.empty()) {
        const std::string al = plat::normalize_path(configured_al);
        if (DefaultFileExists(al)) return al;
        return std::nullopt;
    }

    // 2) 向上找
    if (auto al = FindAl(path)) return al;

    // 3) 工作区递归扫描 + 亲和度
    const std::string normalized_root = plat::normalize_path(root);
    if (normalized_root.empty()) return std::nullopt;

    const std::vector<std::string> candidates = DiscoverProjects(normalized_root);
    if (candidates.empty()) return std::nullopt;
    if (candidates.size() == 1) return candidates[0];

    std::string best = candidates[0];
    auto best_score = Affinity(best, path);
    for (std::size_t i = 1; i < candidates.size(); ++i) {
        const auto score = Affinity(candidates[i], path);
        if (score > best_score) {
            best = candidates[i];
            best_score = score;
        }
    }
    return best;
}

}  // namespace fpgalsp::project