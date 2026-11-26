// =============================================================================
//  src/server/ide_features.h —— IDE 类 executeCommand 的**纯逻辑**（无状态、无传输）
// =============================================================================
//  为什么单独一层：
//    * LSP 侧：workspace/executeCommand 的 7 条命令（B1a）需要它
//    * MCP 侧：instantiation / duplicate_modules / symbol_path /
//      search_hierarchy / project_rename / project_add_source 也需要同样的东西
//      （B6 会把 bridge.cpp 里的重复实现改成调用这里）
//    两份"索引 -> JSON"一旦各写一份必然漂移（字段名/0-based/大小写/排序），
//    所以把**唯一实现**放在这里，features_commands.cpp 只负责解包 params。
//
//  参考实现：%PYREF%\fpgalsp\ide.py 与 server.py::_instantiation_text /
//            on_workspace_executeCommand（fpgalsp.* 分支）。
//
//  约定：
//    * 输入（路径/行号/JSON）一律不可信，返回 `{ok:false,error}` 而不抛异常
//    * 返回的 JSON 就是 Python executeCommand 的字段形状，**不新增字段**
//    * 写 .al 的文件名/字段口径与 Python 一致；唯一有意的差异是**原子写 + 回滚**
//      （Python 是直接覆盖写、失败会留下半截文件）与**保留原文件行尾**
//      （Python 的 open(newline="") 会把 CRLF 统一成 LF），见 B1a-REPORT.md
// =============================================================================

#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "hdl/hdl_index.h"
#include "project/al_project.h"

namespace fpgalsp::server::ide {

using Json = nlohmann::json;

// -----------------------------------------------------------------------------
//  Python server.py::_instantiation_text 的逐行移植
// -----------------------------------------------------------------------------
struct InstantiationText {
    std::string text;      // 含首行缩进与结尾换行（与 Python 完全一致）
    std::string instance;  // 缺省实例名 "u_<module>"
};

InstantiationText BuildInstantiationText(const hdl::Module& module, std::string_view indent);

// fpgalsp.instantiate
//   module 为空 -> {ok:true, modules:[...]}（Python 的"列出可选模块"分支）
//   否则        -> {ok, module, instanceName, text, portCount}
Json Instantiation(const hdl::HdlIndex& index, std::string_view module_name,
                   std::string_view indent);

// fpgalsp.copyInstantiation
//   当前文件里**第一个** module（按 line 升序，Python ide.modules_in_file）
//   没有 module -> {ok:false, error:"The current file has no module definition"}
//   否则        -> {ok, module, text, portCount}
Json CopyInstantiation(const hdl::HdlIndex& index, std::string_view path);

// fpgalsp.symbolPath（line/character 是 **0-based**）
//   -> {ok, name, path, instancePath, module} 或 {ok:false, error}
Json SymbolPath(const hdl::HdlIndex& index, const project::Project& project,
                std::string_view path, int line, int character);

// fpgalsp.duplicateModules -> {ok:true, duplicates:[{name, definitions:[{file,line,end_line}]}]}
//   ⚠ line / end_line 是 **0-based**（Python 原样透传，不要"顺手"改成 1-based）
Json DuplicateModules(const hdl::HdlIndex& index);

// fpgalsp.searchHierarchy -> {ok:true, matches:[{kind,name,file,line,module,instance[,parent]}]}
//   limit 缺省 200（Python ide.search_hierarchy 的默认参数）
Json SearchHierarchy(const hdl::HdlIndex& index, std::string_view query, int limit = 200);

// fpgalsp.addSourceFile -> {ok, added, rel, compileOrder}
//   已存在（大小写不敏感、斜杠归一）-> {ok:true, added:false, rel, compileOrder:null}
Json AddSourceFile(std::string_view al_path, std::string_view source_path);

// fpgalsp.setProjectName -> {ok, changed, name}
Json SetProjectName(std::string_view al_path, std::string_view name);

// -----------------------------------------------------------------------------
//  供测试与复用的小工具（不属于协议）
// -----------------------------------------------------------------------------
// 原子写：同目录临时文件 -> 替换；任一步失败都保持原文件不变（返回 false）。
bool WriteTextFileAtomic(const std::string& path, std::string_view content);

// Python ide.ProjectRelative：相对工程目录、正斜杠（不同盘符 -> 原样归一化路径）
std::string ProjectRelativePath(const std::string& path, const std::string& project_dir);

}  // namespace fpgalsp::server::ide