// =============================================================================
//  src/project/al_project.h —— Anlogic TangDynasty 工程模型（.al）
// =============================================================================
//  对应参考实现：Y:\fpga-lsp\fpgalsp\project.py
//
//  .al 是「哪些文件参与构建 / 顶层模块是哪个 / 用哪颗器件」的唯一权威来源。
//  本模块不调用 TD，纯静态解析（Python 版约 1 ms），使 LSP 能在打开文件时
//  立即给出答案。
//
//  已实测的 .al 结构（TD 2026.2）：
//      <Project Version Minor Path="...">
//        <TD_Version/><Name/>
//        <HardWare><Family/><Device/><Speed/></HardWare>
//        <TOP_MODULE><MODULE/></TOP_MODULE>
//        <Source_Files>
//          <Verilog|VHDL|ADC_FILE|SDC_FILE|IP_FILE|CWC_FILE>
//            <File Path="..."><FileInfo><Attr Name="..." Val="..."/></FileInfo></File>
//        </Source_Files>
//      </Project>
// =============================================================================

#pragma once

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fpgalsp::project {

// 文件种类
enum class FileKind {
    Verilog,
    Vhdl,
    Adc,
    Sdc,
    Ip,
    Cwc,
    Other,
};

// 对应 Python 的 kind 字符串
std::string_view FileKindName(FileKind kind);

// 路径是怎么解析出来的
enum class ResolvedVia {
    Project,   // 相对 .al 所在目录（正常情况）
    Declared,  // 只在 .al 里记录的原始路径下找到（工程被拷走，TD 会静默回退，PRJ-6619）
    Absolute,  // declared 是绝对路径
    Missing,   // 找不到
};

std::string_view ResolvedViaName(ResolvedVia via);

// .al 里的一条 <File> 记录
struct AlFile {
    std::string path;  // 绝对路径，正斜杠
    std::string rel;   // 按 .al 里原样记录的路径
    FileKind kind = FileKind::Other;
    int compile_order = 0;
    bool enabled = true;
    bool excluded = false;
    bool exists = true;
    // <Attr Name=.. Val=../> 平铺成 map（保序，用 vector）
    std::vector<std::pair<std::string, std::string>> attrs;
    ResolvedVia resolved_via = ResolvedVia::Project;

    // 是否参与构建（对应 Python 的 in_build property）
    [[nodiscard]] bool InBuild() const { return enabled && !excluded && exists; }

    // 取属性值
    [[nodiscard]] std::string GetAttr(std::string_view name, std::string default_value = {}) const;
};

struct Project {
    std::string dir;       // .al 所在目录（绝对，正斜杠）
    std::string al_path;   // .al 的绝对路径
    std::string name;
    std::string family;
    std::string device;
    std::optional<std::string> speed;  // 已去掉正负号；空 <Speed/> -> nullopt
    std::string td_version;
    std::string modified_by;
    std::string declared_path;  // 根节点 Path 属性
    std::string top;            // TOP_MODULE/MODULE
    std::vector<AlFile> files;
    std::vector<std::string> warnings;

    // ---- 派生分组（对应 Python 的 _group / *_files）----
    // 按 (compile_order, path) 排序
    [[nodiscard]] std::vector<const AlFile*> Group(std::initializer_list<FileKind> kinds,
                                     bool in_build_only = true) const;

    [[nodiscard]] std::vector<const AlFile*> HdlFiles() const;
    [[nodiscard]] std::vector<const AlFile*> VerilogFiles() const;
    [[nodiscard]] std::vector<const AlFile*> VhdlFiles() const;
    [[nodiscard]] std::vector<const AlFile*> AdcFiles() const;
    [[nodiscard]] std::vector<const AlFile*> SdcFiles() const;
    [[nodiscard]] std::vector<const AlFile*> IpFiles() const;
    [[nodiscard]] std::vector<const AlFile*> NotInBuild() const;

    // 该工程里参与构建的 hdl 文件的绝对路径
    [[nodiscard]] std::vector<std::string> HdlPaths() const;

    // declared_path 与当前目录是否一致（对应 Python 的 path_matches）
    [[nodiscard]] bool PathMatches() const;

    // 单行描述（对应 Python 的 describe）
    [[nodiscard]] std::string Describe() const;
};

// -----------------------------------------------------------------------------
// 解析
// -----------------------------------------------------------------------------

// 解析 .al 文件。never-throw：
//   * 文件读不到 -> warnings 里记一条，返回只有 dir/al_path 的空工程
//   * XML 结构损坏 -> warnings 里记一条，走正则兜底取 Device/Family/top
//   * 裸 '&' 等字符级非法 -> 先经 TolerantXml 处理，正常解析
Project ParseAl(std::string_view al_path);

// 从内存文本解析（测试/已打开缓冲区用）。al_path 仅用于推导 dir 与 name。
Project ParseAlText(std::string_view al_path, std::string_view text);

// 把 .al 里的一条 <File Path> 解析成绝对路径。
// 返回 (绝对路径, 解析方式)，对应 Python 的 resolve_relative。
struct ResolveResult {
    std::string path;
    ResolvedVia via = ResolvedVia::Missing;
};
ResolveResult ResolveRelative(std::string_view project_dir, std::string_view rel,
                              std::string_view declared_dir = {});

// 判断文件是否存在（可注入，便于测试）
bool DefaultFileExists(std::string_view path);

// -----------------------------------------------------------------------------
// 工程发现
// -----------------------------------------------------------------------------

// 从 start_path（文件或目录）向上最多 max_up 层找含唯一 .al 的目录。
// 找到返回 .al 绝对路径；没有返回 nullopt。
std::optional<std::string> FindAl(std::string_view start_path, int max_up = 6);

// 递归扫描 root 下的 *.al（深度 max_depth，跳过 VCS/构建/缓存目录）。
std::vector<std::string> DiscoverProjects(std::string_view root, int max_depth = 6,
                                          std::size_t limit = 500);

// 计算 .al 所在工程目录与目标路径的亲和度（对应 Python 的 _affinity）。
// 返回 (tier, score)，tier 2 = 目标在工程目录内，1 = 部分路径前缀匹配。
std::pair<int, std::size_t> Affinity(std::string_view al_path, std::string_view path);

// 为某个文件挑工程：配置优先 > 向上查找 > 工作区递归扫描 + 亲和度。
std::optional<std::string> FindProjectAl(std::string_view root, std::string_view path,
                                         std::string_view configured_al = {});

}  // namespace fpgalsp::project