// =============================================================================
//  src/hdl/hdl_index.h —— HDL 符号索引（Verible 词法 + hdl.py 的结构识别算法）
// =============================================================================
//  对应参考实现：Y:\fpga-lsp\fpgalsp\hdl.py（1839 行，自研 tokenizer + 结构识别）
//
//  本实现的取舍（务必先读这段再改代码）：
//
//  1. **token 来自 Verible，结构识别照 hdl.py 的算法**
//     hdl.py 自己写了 tokenizer 再在上面做结构识别。我们不再重写 tokenizer
//     （那是 Verible 的活，且它更准），但**保留 hdl.py 的结构识别算法**，
//     因为验收标准是"与 Python 逐字段对比一致" —— 结构算法一致才谈得上逐字段对齐。
//
//  2. **只用 Verible 的 filtered token 流，并自己再滤一遍预处理行**
//     `GetTokenStreamView()` 已去掉空白/注释，并做了宏展开与 ifdef 分支过滤。
//     Python 的 tokenizer 是"整行丢弃预处理指令"，这里用同样规则（DropPreprocessorLines），
//     使同一份源码两边的 token 序列尽量一致。
//     ⚠ 两处**有意为之**的差异（只会更准，不会漏符号）：
//       * Python 保留 `ifdef 的**两个分支**（会重复统计模块）；我们只留激活分支
//       * Python 把宏调用当 id 保留；Verible 在 filtered 流里已展开
//
//  3. **位置一律 UTF-16 列**
//     Verible 只给**字节偏移**。这里换算成 (行, UTF-16 列)，与 LSP 完全一致
//     （复用 platform::byte_index_to_utf16_column）。含中文注释/字符串不会错位。
//
//  4. **VHDL 不走 Verible**
//     Verible 不支持 VHDL。保留 Python 的做法：只抓 `entity <name> is` 造"合成模块"
//     （ports 为空），使层次树里 VHDL 实体不是黑盒。
//     ⚠ 与 Python 的一处差异：Python 那里把**字节偏移当列号**用（含中文的 .vhd 会错位），
//        我们换算成 UTF-16 列（更正确）。
//
//  5. **不抛异常**：任何解析失败都进 `parse_errors`，文件仍部分可用（never-raise）。
// =============================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "project/al_project.h"

namespace fpgalsp::hdl {

// -----------------------------------------------------------------------------
// 位置
// -----------------------------------------------------------------------------

// 同一行上的一段（对应 Python 的 _span）
struct Span {
    int line = 0;
    int start_character = 0;
    int end_character = 0;
};

// 跨行位置（对应 LSP Location 的 range）
struct Location {
    std::string path;
    int line = 0;
    int character = 0;
    int end_line = 0;
    int end_character = 0;
};

// -----------------------------------------------------------------------------
// 符号（字段名与 hdl.py 的 dict 对齐，便于逐字段对比）
// -----------------------------------------------------------------------------

struct Port {
    std::string name;
    std::string direction;  // input / output / inout / ref / ""
    std::string net;        // wire / reg / ...（hdl.py 的 "net"，不进 LSP）
    std::optional<int> msb;
    std::optional<int> lsb;
    int width = 1;
    int line = 0;
    int character = 0;
    std::string file;
};

struct Param {
    std::string name;
    std::string value;  // 已去掉空白的拼接文本
    bool local = false;
    int line = 0;
    int character = 0;
    std::string file;
    std::string module;
};

struct Signal {
    std::string name;
    std::string type;  // 声明关键字：wire/reg/logic/...
    std::optional<int> msb;
    std::optional<int> lsb;
    int width = 1;
    int line = 0;
    int character = 0;
    std::string file;
    std::string module;
};

struct Instance {
    std::string module;  // 被例化的模块名
    std::string name;    // 实例名（无名例化时为空）
    std::string file;
    int line = 0;        // 实例名位置（无名时退化为模块名位置）
    int character = 0;
    std::map<std::string, std::string> connections;  // 端口名 -> 拼接文本
    std::map<std::string, std::string> params;       // 参数名 -> 拼接文本

    // 供 definition / hover / inlayHint 用（Python 存在 _inst_extra 里）
    int module_line = 0;
    int module_character = 0;
    int start_line = 0;  // 例化语句起始（模块名）
    int start_character = 0;
    int end_line = 0;    // 到分号
    int end_character = 0;
    // (.port(...) 里端口名的位置 -> 端口名)
    std::vector<std::pair<std::pair<int, int>, std::string>> connection_ports;
};

struct Module {
    std::string name;
    std::string file;
    int line = 0;  // 模块名的位置（= hdl.py module["line"]）
    int character = 0;
    int end_line = 0;  // endmodule 所在行；缺失时为最后一行
    std::vector<Port> ports;
    std::vector<std::string> parameters;  // 参数名列表
    bool ansi_ports = false;
    bool vhdl = false;  // VHDL 合成模块（ports 为空）

    // 整块范围（documentSymbol 的 range 用）
    int keyword_line = 0;
    int keyword_character = 0;
    int end_character = 0;  // endmodule 结束列
};

struct ParseError {
    int line = 0;
    int character = 0;
    std::string message;
};

// 与 hdl.py 的 token 等价：(text, kind, byte offset, line, utf16 column, end column)
// kind: 'i' = 标识符/关键字, 'n' = 数字, 's' = 字符串, 'o' = 运算符
struct Token {
    std::string text;
    char kind = 'o';
    std::size_t offset = 0;
    int line = 0;
    int character = 0;
    int end_character = 0;
};

// 模块内部位置索引（对应 Python _modinfo 里的 *_pos 字典）
struct ModulePositions {
    std::map<std::pair<int, int>, std::size_t> port_pos;    // -> Module::ports
    std::map<std::pair<int, int>, std::size_t> port_alias;  // 非 ANSI：header 里的名字
    std::map<std::pair<int, int>, std::size_t> signal_pos;  // -> ModuleInfo::signals
    std::map<std::pair<int, int>, std::size_t> param_pos;   // -> ModuleInfo::params
    std::map<std::pair<int, int>, std::size_t> inst_name_pos;  // -> ParsedFile::instances
    std::map<std::pair<int, int>, std::string> modref_pos;     // -> 被例化模块名
    std::map<std::pair<int, int>, std::pair<std::string, std::string>> conn_port_pos;
};

struct ModuleInfo {
    Module module;
    std::vector<Param> params;
    std::vector<Signal> signals;
    ModulePositions positions;
};

struct ParsedFile {
    std::string path;
    std::vector<ModuleInfo> modules;
    std::vector<Instance> instances;
    std::vector<ParseError> parse_errors;
    std::vector<Token> tokens;

    bool ok() const { return parse_errors.empty(); }
};

// -----------------------------------------------------------------------------
// 单文件解析（纯函数，never-raise）
// -----------------------------------------------------------------------------

// Verilog/SystemVerilog：用 Verible 取 token，再按 hdl.py 的算法识别结构。
ParsedFile ParseVerilog(std::string_view path, std::string_view text);

// VHDL：只提取 `entity <name> is ... end [entity] <name>` 造合成模块（ports 为空）。
ParsedFile ParseVhdlEntities(std::string_view path, std::string_view text);

// 供测试/调试：Verible 词法结果 + 我们的 kind 分类（不建结构）
std::vector<Token> LexWithVerible(std::string_view path, std::string_view text,
                                  std::vector<ParseError>* errors);

bool IsVerilogPath(std::string_view path);
bool IsVhdlPath(std::string_view path);

// 是否是 Verilog 内建门/开关原语（and/or/nand/buf/...）。
// moduleTree 默认不递归这些（对应 hdl.py 的 PRIMITIVES）。
bool IsPrimitiveGate(std::string_view name);

// 按扩展名分流解析
ParsedFile ParseFile(std::string_view path, std::string_view text);

// -----------------------------------------------------------------------------
// 解析缓存（按 path + 内容哈希）
// -----------------------------------------------------------------------------
//  Phase 2 / A1：冷构建改为**多线程并行解析**，所以缓存必须线程安全。
//  策略：一把互斥量保护 entries_ 与计数器（Get/Put 都只做一次哈希 + 一次查表，
//  临界区 ~100 ns；真正的解析在锁外做，因此不存在"大锁"). 若将来出现更高并发，
//  再按哈希分片即可，接口不变。
class ParseCache {
public:
    ParseCache() = default;
    ParseCache(const ParseCache&) = delete;
    ParseCache& operator=(const ParseCache&) = delete;

    // 命中返回旧结果；未命中返回 nullptr（调用方解析后 Put）
    std::shared_ptr<const ParsedFile> Get(std::string_view path, std::string_view text);
    void Put(std::string_view path, std::string_view text, std::shared_ptr<ParsedFile> file);
    void Clear();
    std::size_t size() const;
    std::size_t hits() const;
    std::size_t misses() const;

    // 容量上限（超出时整体清空 —— 缓存只是加速，丢了不影响正确性）
    std::size_t capacity = 4096;

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<ParsedFile>> entries_;
    std::size_t hits_ = 0;
    std::size_t misses_ = 0;
};

// FNV-1a 64（缓存键用；不需要密码学强度）
std::uint64_t ContentHash(std::string_view text);

// -----------------------------------------------------------------------------
// 查询结果
// -----------------------------------------------------------------------------

// 光标处符号（对应 hdl.py 的 _module_symbol/_port_symbol/_signal_symbol/...）
struct SymbolAtResult {
    std::string kind;  // module / port / signal / parameter / instance
    std::string name;
    std::string detail;  // "input [7:0]" / "module (25 ports)"
    std::string file;
    Span span;  // 声明处 name 的范围
    std::optional<int> msb;
    std::optional<int> lsb;
    std::optional<int> width;
    std::string module;
};

// LSP DocumentSymbol 的中间表示（由 server 层序列化）
//   kind: 2=Module 8=Field(端口) 13=Variable(信号) 14=Constant(参数)
struct DocSymbol {
    std::string name;
    std::string detail;
    int kind = 13;
    int range_start_line = 0;
    int range_start_character = 0;
    int range_end_line = 0;
    int range_end_character = 0;
    Span selection;
    std::vector<DocSymbol> children;
};

// workspace/symbol 的中间表示
struct WorkspaceSymbol {
    std::string name;
    std::string container;  // 所属模块名
    std::string detail;
    int kind = 13;
    std::string path;
    Span span;
};

// -----------------------------------------------------------------------------
// 索引
// -----------------------------------------------------------------------------
class HdlIndex {
public:
    HdlIndex() = default;

    // B8 / PERF-03：按共享所有权加入，**不再深拷贝** ParsedFile。
    // 解析缓存（ParseCache）里本来就存着 shared_ptr<const ParsedFile>，索引直接共享同一份；
    // 200 文件工程的峰值内存因此少掉一整份 token 表（实测 16.2 MB → 12.4 MB）。
    void AddFile(std::shared_ptr<const ParsedFile> file);

    // 兼容重载（单元测试/一次性用法）：把值 move 进 shared_ptr，**本身也不做深拷贝**。
    // 热路径（BuildIndex）一律走上面的共享版。
    void AddFile(ParsedFile file) {
        AddFile(std::make_shared<const ParsedFile>(std::move(file)));
    }

    // ---- 增量更新（Phase 2 / A1）---------------------------------------------
    //  给 IndexService 用：改一个文件时不再全量 BuildIndex，而是
    //      HdlIndex next = current.Clone();      // O(N) shared_ptr 指针拷贝
    //      next.ReplaceFile(parsed);             // O(模块数) 重建模块索引
    //  这些方法都**不深拷贝 ParsedFile**（只搬 shared_ptr），也不重新解析别的文件。

    // 深拷贝"索引结构"（files_ 里是 shared_ptr，拷贝的是指针）——供写者做 COW。
    HdlIndex Clone() const;

    // 同路径替换（路径不存在时追加，语义等于 Upsert）。会重建 modules_by_key_。
    void ReplaceFile(std::shared_ptr<const ParsedFile> file);
    void UpsertFile(std::shared_ptr<const ParsedFile> file) { ReplaceFile(std::move(file)); }

    // 移除一个文件；返回是否真的存在并移除了。
    // 会重建 by_lower_path_（下标整体左移）与 modules_by_key_。
    bool RemoveFile(std::string_view path);

    // 重建 modules_by_key_ / module_order_（O(模块数)）。
    // 语义与"按 files_ 顺序重新 AddFile 一遍"完全相同：
    //   模块名首次出现在哪个文件就以哪个为准，module_order_ 保持首次出现顺序。
    void RebuildModuleIndex();

    // 按路径取文件；大小写不敏感回退（Windows 语义）
    const ParsedFile* File(std::string_view path) const;

    // 按名字取模块定义（第一个）
    const Module* ModuleByName(std::string_view name) const;

    // 所有模块名（原始大小写，按首次出现顺序）
    std::vector<std::string> ModuleNames() const;

    std::vector<const ModuleInfo*> ModulesOfFile(std::string_view path) const;
    std::vector<const Instance*> InstancesOfFile(std::string_view path) const;
    std::vector<const Instance*> AllInstances() const;
    std::vector<std::string> Paths() const;

    std::size_t FileCount() const { return files_.size(); }
    std::size_t ModuleCount() const { return module_order_.size(); }

    // (line, character) 处的 token
    static const Token* TokenAt(const ParsedFile& file, int line, int character);
    // 包含该行的模块（对应 hdl.py _module_at）
    static const ModuleInfo* ModuleAt(const ParsedFile& file, int line);

    // 光标处符号 / 种类 / 跳转定义
    std::optional<SymbolAtResult> SymbolAt(std::string_view path, int line,
                                           int character) const;
    std::string KindAt(std::string_view path, int line, int character) const;
    std::optional<Location> DefinitionOf(std::string_view path, int line, int character) const;

    std::vector<DocSymbol> DocumentSymbols(std::string_view path) const;
    std::vector<WorkspaceSymbol> SearchSymbols(std::string_view query) const;

    // 文本级引用搜索（对应 server.py on_textDocument_references 的语义）：
    // 先看光标处 token 的名字，再在**所有已索引文件**的 token 里找同名标识符。
    // 用 token 而不是正则，所以注释/字符串里的同名文本天然不会命中。
    std::vector<Location> References(std::string_view path, int line, int character) const;

private:
    void RebuildPathIndex();  // RemoveFile 后下标左移：重建 path -> 下标

    std::vector<std::shared_ptr<const ParsedFile>> files_;
    std::map<std::string, std::size_t> by_lower_path_;                          // lower(path) -> 下标
    std::map<std::string, std::pair<std::size_t, std::size_t>> modules_by_key_;  // lower name -> (file, module)
    std::vector<std::string> module_order_;
};

// -----------------------------------------------------------------------------
// 工程级构建
// -----------------------------------------------------------------------------

// IP 生成的 HDL（对应 Python td.ip_generated_hdl：`.ipc` 同名的 .v/.vhd 在旁边）。
// ⚠ 与 Python 的关键差异（PYTHON-BUGS P-1）：这里**按扩展名分流**，.vhd 不会被当成
//   Verilog 喂给 Verible/read_hdl。Python 把两者混成一个列表，导致 elaborate 整体失败。
struct IpGeneratedHdl {
    std::vector<std::string> verilog;
    std::vector<std::string> vhdl;
};
IpGeneratedHdl IpGeneratedHdlFor(const project::Project& project);

// 参与 Verilog 索引的文件（.al 的 Verilog 文件 + IP 生成的 .v）
std::vector<std::string> VerilogFilesFor(const project::Project& project);
// 参与 VHDL entity 提取的文件（.al 的 VHDL 文件 + IP 生成的 .vhd/.vhdl）
std::vector<std::string> VhdlFilesFor(const project::Project& project);

// 读文本（未保存缓冲区优先，落盘回落）由调用方注入
using TextLoader = std::function<std::optional<std::string>(const std::string& path)>;

struct BuildOptions {
    ParseCache* cache = nullptr;
    std::size_t max_files = 2000;              // 文件数预算（防异常工程）
    std::size_t max_total_tokens = 4'000'000;  // token 总预算

    // 并行解析线程数（Phase 2 / A1）：
    //   0（默认）或 1 = 顺序（B8 之前的原路径，行为逐字节一致）；
    //   >=2        = 多线程解析，**保序合并**（结果与顺序完全一致）。
    // 用 RecommendedParseThreads() 取"自动"值：min(8, hardware_concurrency()-1)。
    std::size_t parse_threads = 0;
};

// 冷构建并行的默认线程数：min(8, hw-1)，下限 1（= 顺序）。
std::size_t RecommendedParseThreads();

struct BuildResult {
    HdlIndex index;
    bool truncated = false;  // 因预算被截断
    std::size_t files_indexed = 0;
    std::size_t parse_error_count = 0;
};

BuildResult BuildIndex(const project::Project& project, const TextLoader& load,
                       const BuildOptions& options = {});

}  // namespace fpgalsp::hdl