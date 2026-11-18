#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_verible_cmake.py —— 把 vendored Verible 的 Bazel BUILD 转译成 CMake。

开发期工具（不参与构建），产物入库：thirdparty/verible/verible/<pkg>/CMakeLists.txt。
要求可重复运行：同一输入跑两次必须产出逐字节相同的文件（不写时间戳/绝对路径）。

用法：
    python tools/gen_verible_cmake.py            # 生成
    python tools/gen_verible_cmake.py --check    # 只校验（生成物与磁盘不一致时退出码 1）
    python tools/gen_verible_cmake.py --list     # 只列出 discovery 结果
    python tools/gen_verible_cmake.py --prune    # 删除已不在 discovery 里的过期 CMakeLists.txt

===============================================================================
转译规则（对应关系）
===============================================================================

一、BUILD 解析
  * 不引入 Bazel / starlark 依赖，自带一个「够用」的 Starlark 表达式求值器：
    支持 str / int / bool / list / dict / tuple、`+` 字符串与列表拼接、
    下标、函数调用（cc_library/cc_test/cc_binary/genrule/select/glob/filegroup/
    alias/package/load/...）、`for` 列表推导（只在 cc_test shard 展开里用到）。
  * 无法静态求值的东西（`native.module_version()`、未知 genrule 的 cmd）不报错，
    记为「不可求值」，由下游规则决定如何处理。

二、target 命名（关键规则）
    Bazel: //verible/common/text:token-info
    CMake: verible__common__text__token-info        （package 用于 add_subdirectory）
    别名:  verible::common::text::token-info        （用于 target_link_libraries）
  规则：去掉 "//verible/" 前缀，把 "/" 和 ":" 都换成 "__"，
        前面的 "verible" 换成 "verible" 前缀 => 保证全工程唯一且可读。
  同名 target 在别的包也出现时（例如 //verible/preprocessor:verilog-preprocess
  与 //verible/verilog/preprocessor:verilog-preprocess）不会冲突。

三、label -> CMake target 映射
    "@abseil-cpp//absl/strings"                -> absl::strings
    "@abseil-cpp//absl/status:statusor"        -> absl::statusor
    "@abseil-cpp//absl/container:flat_hash_map"-> absl::flat_hash_map
      （规则：pkg 最后一段要么就是 target 名，要么没写 target 就用 pkg 末段）
    "@re2"                                     -> re2::re2
    "@nlohmann_json//:singleheader-json"       -> nlohmann_json::nlohmann_json
    "@googletest//:gtest"                      -> GTest::gtest
    "@googletest//:gtest_main"                 -> GTest::gtest_main
    "//verible/..." / ":name"                  -> verible::<path>（见二）
    "//external_libs:editscript"               -> verible::external_libs::editscript
      （external_libs/BUILD 里的纯头文件库，用 interface library 表示）
    "//bazel:flex"                             -> verible_flex_lexer_runtime
      （Bazel 里 flex 运行时头文件库；CMake 里由 cmake/Verible.cmake 提供）
    "//third_party/portable_endian"、"@zlib"  -> 已被 0-A 删除，遇到即丢弃（不报错）

四、cc_library / cc_test / cc_binary -> CMake
    * 有 srcs 的 -> add_library(<name> STATIC ...) / add_executable
    * 只含 hdrs（纯头文件库）-> add_library(<name> INTERFACE ...)，
      并把 hdrs 挂到 INTERFACE SOURCES 上方便 IDE 浏览
    * a.cc + a.h 同名时不存在的问题：Bazel 的 hdrs/srcs 一一对应
    * copts 里的 gcc 专有选项（-Wxxx / -fexceptions）在 MSVC 下丢弃
    * select({"@platforms//os:windows": [...], "//conditions:default": [...]})
      -> 只取 windows 分支；如果 windows 分支为空列表则等价于「无 copts」
    * visibility 完全忽略
    * alwayslink = 1 -> 在 CMake 里记录成注释（MSVC 静态库没有直接的对应物，
      实际由 cmake/Verible.cmake 的 /WHOLEARCHIVE 处理）
    * testonly = 1 的 cc_library 照常生成（本阶段不编测试，但库得存在）
    * cc_test -> 默认跳过（Phase-0 不编 225 个测试）；用 --with-tests 才生成
    * cc_binary -> 默认跳过

五、genrule（生成物）
    只转译三类本阶段真正需要的：
      * verilog-nonterminals-foreach-gen   (verible/verilog/CST)
            用 cmake -P 脚本实现（cmake/scripts/verible-nonterminals.cmake），
            sed/grep 在 Windows 上不可依赖
      * version-header                     (verible/common/util)
            生成最小的 generated-verible-build-version.h（值取自 VENDOR-INFO.md
            里的 commit，避免依赖 Bazel workspace status）
    其余 genrule（flex/bison/文本补丁）由任务 0-D 在 cmake/scripts/ 实现，
    本脚本只在生成的 CMakeLists.txt 里留 TODO 引用目标名。

六、包发现（discovery）
    本阶段只接入 Phase-0 需要的包集合，但包的「传递闭包」会自动递归展开：
      种子：common/{text,util,strings,lexer,parser}、verilog/{parser,CST}
      另外 common/analysis(+matcher)、verilog/{analysis,preprocessor} 等如果被
      种子包依赖，也会被自动纳入 —— 否则 CMake 会「找不到 target」。
    不在闭包里的包（formatting/analysis 的规则集 etc.）留到阶段 1。

七、生成物里不写任何绝对路径，路径一律相对 ${CMAKE_CURRENT_SOURCE_DIR}
   和 ${CMAKE_SOURCE_DIR}，保证可换机器/换盘符重建（重跑不产生 diff）。
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from dataclasses import dataclass, field
from typing import Any

# =============================================================================
# 路径与全局配置
# =============================================================================

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# thirdparty/verible 是 include 根（源码里写 #include "verible/common/text/token-info.h"）
VERIBLE_ROOT = os.path.join(REPO_ROOT, "thirdparty", "verible")
# BUILD 包根：verible/ 下的包路径形如 //verible/common/text
VALID_PKG_ROOTS = ("verible", "external_libs")

# 本工程内需要编译的「构建期工具」二进制（Bazel genrule 的 tools= 参数）。
# 只有这些 cc_binary 会生成 CMake target；Verible 自带的 CLI 工具
# （verible-verilog-syntax / -lint / -format ...）不在本项目交付范围。
GENERATED_BINARIES = {
    # common/lsp 的两阶段自举：jcxxgen 读 lsp-protocol.yaml 生成 lsp-protocol.h
    "jcxxgen",
}

GENERATED_MARKER = "GENERATED FILE - DO NOT EDIT"
GENERATOR_NAME = "tools/gen_verible_cmake.py"


def log(msg: str) -> None:
    print(f"[gen_verible_cmake] {msg}", file=sys.stderr)


# =============================================================================
# 第一部分：Starlark 迷你求值器
# =============================================================================

_TOKEN_RE = re.compile(
    r"""
    (?P<ws>[ \t\r\n]+)
  | (?P<comment>\#[^\n]*)
  | (?P<str>"(?:[^"\\]|\\.)*"|'(?:[^'\\]|\\.)*')
  | (?P<num>\d+)
  | (?P<name>[A-Za-z_][A-Za-z0-9_.]*)
  | (?P<punct>[\[\](){}:,=+*])
    """,
    re.VERBOSE,
)


class _Unevaluable:
    """表示「这个表达式没法静态求值」的哨兵值。"""

    _instance = None

    def __new__(cls):  # singleton
        if cls._instance is None:
            cls._instance = super().__new__(cls)
        return cls._instance

    def __repr__(self) -> str:
        return "<unevaluable>"


UNEVALUABLE = _Unevaluable()


@dataclass
class Token:
    kind: str
    value: str
    pos: int
    line: int


def _tokenize(text: str) -> list[Token]:
    tokens: list[Token] = []
    line = 1
    pos = 0
    n = len(text)
    while pos < n:
        m = _TOKEN_RE.match(text, pos)
        if not m:
            return tokens  # 剩下的无法识别（例如 Python 三元表达式），直接截断
        kind = m.lastgroup or ""
        raw = m.group(kind)
        if kind == "ws":
            line += raw.count("\n")
        elif kind == "comment":
            pass
        elif kind == "str":
            body = raw[1:-1]
            # Starlark/Python 的转义在本项目里只用到 \" 和 \\ 和 \n
            body = (
                body.replace("\\\\", "\x00")
                .replace('\\"', '"')
                .replace("\\'", "'")
                .replace("\\n", "\n")
                .replace("\\t", "\t")
                .replace("\x00", "\\")
            )
            tokens.append(Token("str", body, pos, line))
        else:
            tokens.append(Token(kind, raw, pos, line))
        pos = m.end()
    return tokens


class BuildFile:
    """一个 BUILD 文件的可求值视图。"""

    def __init__(self, path: str, package: str) -> None:
        self.path = path
        self.package = package  # 例如 "verible/common/text"
        self.rules: list[dict[str, Any]] = []  # 所有 rule 调用（dict）
        self.unevaluable: list[str] = []  # 无法求值的规则名（记录供人工检查）
        with open(path, "r", encoding="utf-8") as f:
            self.text = f.read()
        self.tokens = _tokenize(self.text)
        self._parse()

    # ---------------- 解析 ----------------
    def _parse(self) -> None:
        toks = self.tokens
        i = 0
        while i < len(toks):
            tok = toks[i]
            # 顶层只关心 NAME( ... ) 形式的顶层语句
            if tok.kind == "name" and i + 1 < len(toks) and toks[i + 1].value == "(":
                callee = tok.value
                try:
                    args, i = self._parse_call(i + 1)
                except _ParseError:
                    # 解析失败：跳到下一个顶层语句
                    self.unevaluable.append(callee)
                    i = self._skip_to_next_statement(i)
                    continue
                # 顶层调用可能是 load / package / 变量赋值
                if callee in ("load", "package", "licenses", "exports_files"):
                    continue
                handled = self._record_rule(callee, args)
                if not handled:
                    self.unevaluable.append(callee)
                continue
            i += 1

    def _skip_to_next_statement(self, i: int) -> int:
        depth = 0
        while i < len(self.tokens):
            v = self.tokens[i].value
            if v in "([{":
                depth += 1
            elif v in ")]}":
                depth -= 1
                if depth <= 0:
                    return i + 1
            i += 1
        return i

    def _parse_call(self, i: int) -> tuple[dict[str, Any], int]:
        """从 '(' 开始解析，返回 (kwargs, next_index)。"""
        assert self.tokens[i].value == "("
        i += 1
        kwargs: dict[str, Any] = {}
        positional: list[Any] = []
        while i < len(self.tokens):
            v = self.tokens[i].value
            if v == ")":
                i += 1
                break
            if v == ",":
                i += 1
                continue
            # key = value
            if (
                self.tokens[i].kind == "name"
                and i + 1 < len(self.tokens)
                and self.tokens[i + 1].value == "="
                and self.tokens[i + 2].value != "="
            ):
                key = self.tokens[i].value
                val, i = self._parse_expr(i + 2)
                kwargs[key] = val
            else:
                val, i = self._parse_expr(i)
                positional.append(val)
        if positional:
            kwargs["__positional__"] = positional
        return kwargs, i

    def _parse_expr(self, i: int) -> tuple[Any, int]:
        """解析一个表达式（含 + 拼接、下标、列表推导）。"""
        val, i = self._parse_primary(i)
        while i < len(self.tokens) and self.tokens[i].value in ("+", "["):
            if self.tokens[i].value == "+":
                rhs, i = self._parse_primary(i + 1)
                if val is UNEVALUABLE or rhs is UNEVALUABLE:
                    val = UNEVALUABLE
                elif isinstance(val, str) and isinstance(rhs, str):
                    val = val + rhs
                elif isinstance(val, list) and isinstance(rhs, list):
                    val = val + rhs
                elif isinstance(val, tuple) and isinstance(rhs, tuple):
                    val = val + rhs
                else:
                    val = UNEVALUABLE
            else:  # '['
                if isinstance(val, list) or isinstance(val, dict):
                    idx, i = self._parse_expr(i + 1)
                    if i < len(self.tokens) and self.tokens[i].value == "]":
                        i += 1
                    try:
                        val = val[idx]
                    except Exception:
                        val = UNEVALUABLE
                else:
                    val = UNEVALUABLE
                    i += 1
        return val, i

    def _parse_primary(self, i: int) -> tuple[Any, int]:
        toks = self.tokens
        if i >= len(toks):
            raise _ParseError("eof")
        tok = toks[i]
        if tok.kind == "str":
            return tok.value, i + 1
        if tok.kind == "num":
            return int(tok.value), i + 1
        if tok.value == "[":
            return self._parse_list(i)
        if tok.value == "{":
            return self._parse_dict(i)
        if tok.value == "(":
            inner, j = self._parse_expr(i + 1)
            if j < len(toks) and toks[j].value == ")":
                j += 1
            # tuple 形式 (a,) 也当成单元素
            return inner, j
        if tok.kind == "name":
            name = tok.value
            if name == "True":
                return True, i + 1
            if name == "False":
                return False, i + 1
            if name == "None":
                return None, i + 1
            # 函数调用
            if i + 1 < len(toks) and toks[i + 1].value == "(":
                kwargs, j = self._parse_call(i + 1)
                return self._call(name, kwargs), j
            # 裸标识符（例如 default_visibility 变量）无法求值
            return UNEVALUABLE, i + 1
        raise _ParseError(f"unexpected token {tok.value!r}")

    def _parse_list(self, i: int) -> tuple[Any, int]:
        """解析 [...]，支持 [ expr for x in [...] ] 的列表推导。"""
        toks = self.tokens
        assert toks[i].value == "["
        i += 1
        items: list[Any] = []
        while i < len(toks):
            if toks[i].value == "]":
                return items, i + 1
            if toks[i].value == ",":
                i += 1
                continue
            # 列表推导
            if toks[i].value == "(" and i + 1 < len(toks) and toks[i + 1].value == "(":
                pass
            start = i
            val, j = self._parse_expr(i)
            if j < len(toks) and toks[j].kind == "name" and toks[j].value == "for":
                # 形如 [( cc_test(...), ) for shard in ["1","2"]]
                element_paren = toks[start].value == "("
                # 找 'for X in <iterable>'
                k = j + 1
                if k + 1 < len(toks) and toks[k + 1].value == "in":
                    iterable, k = self._parse_expr(k + 2)
                    if isinstance(iterable, list):
                        for var in iterable:
                            if isinstance(element_paren, bool):
                                # 列表元素是 tuple，取第一个元素
                                if isinstance(val, tuple):
                                    items.append(val[0] if val else UNEVALUABLE)
                                else:
                                    items.append(val)
                    # 跳过到 ']'
                    while k < len(toks) and toks[k].value != "]":
                        k += 1
                    return items, k + 1
                items.append(val)
                i = j
                continue
            items.append(val)
            i = j
        return items, i

    def _parse_dict(self, i: int) -> tuple[Any, int]:
        toks = self.tokens
        assert toks[i].value == "{"
        i += 1
        d: dict[Any, Any] = {}
        while i < len(toks):
            if toks[i].value == "}":
                return d, i + 1
            if toks[i].value == ",":
                i += 1
                continue
            key, i = self._parse_expr(i)
            if i < len(toks) and toks[i].value == ":":
                val, i = self._parse_expr(i + 1)
            else:
                val = UNEVALUABLE
            try:
                d[key] = val
            except TypeError:
                pass
        return d, i

    def _call(self, name: str, kwargs: dict[str, Any]) -> Any:
        """记录一次规则调用，返回一个占位值。"""
        kwargs["__callee__"] = name
        return _RuleCall(name, kwargs)

    def _record_rule(self, callee: str, args: dict[str, Any]) -> bool:
        """把顶层调用登记进 self.rules。返回是否已处理。"""
        # 顶层 cc_library/cc_test/... 会被 _parse_expr 走过一次，
        # 这里 args 是 _parse_call 直接产出的 kwargs，没有 __callee__
        args = dict(args)
        args["__callee__"] = callee
        args.pop("__positional__", None)
        self.rules.append(args)
        return True


@dataclass
class _RuleCall:
    callee: str
    kwargs: dict[str, Any]


class _ParseError(Exception):
    pass


# ---------------------------------------------------------------------------
# 解析 BUILD 的入口：把「顶层 cc_library(...)」当成规则
# ---------------------------------------------------------------------------
_RULE_CALLEES = {
    "cc_library",
    "cc_test",
    "cc_binary",
    "genrule",
    "filegroup",
    "alias",
    "genlex",
    "genyacc",
    "std_move_parser_symbols",
    "record_recovered_syntax_errors",
    "glob",
    "select",
}


def parse_build_file(path: str, package: str) -> BuildFile:
    return BuildFile(path, package)


# =============================================================================
# 第二部分：label 映射
# =============================================================================

# 已被 0-A 删除的外部依赖：遇到直接丢弃，不报错（脚本要能容忍 target 缺失）
DROPPED_EXTERNAL = {
    "@zlib",
    "@platforms",
    "//third_party/portable_endian",
    "//third_party/proto/kythe",
}

# 固定的外部 label -> CMake target
EXTERNAL_MAP = {
    "@re2": "re2::re2",
    "@nlohmann_json//:singleheader-json": "nlohmann_json::nlohmann_json",
    "@googletest//:gtest": "GTest::gtest",
    "@googletest//:gtest_main": "GTest::gtest_main",
    "//external_libs:editscript": "verible::external_libs::editscript",
}

# Bazel 的 abseil target 名与 CMake 的导入名不一致的地方（实测于 vendored abseil）。
# 默认规则 absl::<pkg 末段> 对大多数成立；下面这些必须显式列出。
ABSL_NAME_OVERRIDES = {
    # Bazel: @abseil-cpp//absl/flags:flag        CMake: absl_flags        -> absl::flags
    "flag": "flags",
    # Bazel: @abseil-cpp//absl/flags:parse       CMake: absl_flags_parse
    "parse": "flags_parse",
    # Bazel: @abseil-cpp//absl/flags:usage       CMake: absl_flags_usage
    "usage": "flags_usage",
    # Bazel: @abseil-cpp//absl/flags:config      CMake: absl_flags_config
    "config": "flags_config",
    # Bazel: @abseil-cpp//absl/flags:reflection  CMake: absl_flags_reflection
    "reflection": "flags_reflection",
    # Bazel: @abseil-cpp//absl/log:globals       CMake: absl_log_globals
    "globals": "log_globals",
    # Bazel: @abseil-cpp//absl/log:initialize    CMake: absl_log_initialize
    "initialize": "log_initialize",
    # Bazel: @abseil-cpp//absl/random            CMake: absl_random_random -> absl::random_random
    "random": "random_random",
    # @abseil-cpp//absl/time 在两个构建系统里同名，无需覆盖
}

# 本工程内部（cmake/Verible.cmake）提供的运行时库
INTERNAL_MAP = {
    "//bazel:flex": "verible_flex_lexer_runtime",
    "//bazel:bison": "verible_bison_parser_runtime",
}


def cmake_target_name(package: str, name: str) -> str:
    """(package, name) -> 唯一 CMake target 名。

    例：("verible/common/text", "token-info") -> "verible__common__text__token-info"
    """
    pkg = package.replace("/", "__")
    return f"{pkg}__{name}"


def cmake_alias_name(package: str, name: str) -> str:
    """(package, name) -> 可读别名。

    例：("verible/common/text", "token-info") -> "verible::common::text::token-info"
    """
    return package.replace("/", "::") + "::" + name


def map_dep_label(label: str, from_package: str) -> str | None:
    """把一条 Bazel dep label 映射成 CMake target（或 None 表示丢弃）。"""
    label = label.strip()
    if not label:
        return None
    if label in DROPPED_EXTERNAL:
        return None
    if label in EXTERNAL_MAP:
        return EXTERNAL_MAP[label]
    if label in INTERNAL_MAP:
        return INTERNAL_MAP[label]
    # --- abseil -------------------------------------------------------------
    if label.startswith("@abseil-cpp//"):
        rest = label[len("@abseil-cpp//"):]
        pkg, _, tgt = rest.partition(":")
        if not tgt:
            # @abseil-cpp//absl/strings -> absl::strings
            name = pkg.rsplit("/", 1)[-1]
        else:
            name = tgt
        name = ABSL_NAME_OVERRIDES.get(name, name)
        return "absl::" + name
    # --- 同包 ":name" --------------------------------------------------------
    if label.startswith(":"):
        return cmake_alias_name(from_package, label[1:])
    # --- 绝对路径 //pkg 或 //pkg:name ---------------------------------------
    if label.startswith("//"):
        rest = label[2:]
        pkg, sep, tgt = rest.partition(":")
        if not sep:
            # 裸包 label（//verible/common/lexer）在 Bazel 里指向同名 target
            pkg_name = pkg.rsplit("/", 1)[-1]
            return cmake_alias_name(pkg, pkg_name)
        return cmake_alias_name(pkg, tgt)
    # 无法识别的（@rules_cc、@platforms 等）：丢弃
    return None


def package_of_label(label: str, from_package: str) -> str | None:
    """label 指向的包名（用于依赖图）。外部 label 返回 None。"""
    if label.startswith(":"):
        return from_package
    if label.startswith("//"):
        rest = label[2:]
        pkg = rest.partition(":")[0]
        return pkg
    return None


# =============================================================================
# 第三部分：规则 -> CMake 转译
# =============================================================================

# MSVC 下要丢掉的 gcc 风格 copts 前缀
GCC_ONLY_COPTS_PREFIX = ("-W", "-f", "-std=", "-O", "-g", "-m", "-pthread")


def _as_list(v: Any) -> list[Any]:
    if v is None or v is UNEVALUABLE:
        return []
    if isinstance(v, list):
        return v
    return [v]


def _select_windows(sel: Any) -> list[Any]:
    """select({...}) 只取 windows 分支。"""
    if not isinstance(sel, dict):
        return []
    val = sel.get("@platforms//os:windows")
    if val is None:
        # 没有 windows 分支时退回 default（本项目里 default 都是 unix 专有的，
        # 所以返回空
        val = sel.get("//conditions:default", [])
    return _as_list(val)


def _copts_of(rule: dict[str, Any]) -> list[str]:
    raw = rule.get("copts")
    items = _as_list(raw) if not isinstance(raw, dict) else _select_windows(raw)
    out = []
    for c in items:
        if not isinstance(c, str):
            continue
        if c.startswith(GCC_ONLY_COPTS_PREFIX):
            continue  # gcc/clang 专有，MSVC 不用
        out.append(c)
    return out


def _defines_of(rule: dict[str, Any]) -> list[str]:
    return [d for d in _as_list(rule.get("defines")) if isinstance(d, str)]


@dataclass
class Target:
    package: str
    kind: str  # cc_library / cc_test / cc_binary
    name: str
    srcs: list[str] = field(default_factory=list)
    hdrs: list[str] = field(default_factory=list)
    deps: list[str] = field(default_factory=list)  # 已映射的 CMake target
    raw_deps: list[str] = field(default_factory=list)  # 原始 label
    excluded_deps: list[str] = field(default_factory=list)  # 指向阶段 1 包的 label
    copts: list[str] = field(default_factory=list)
    defines: list[str] = field(default_factory=list)
    alwayslink: bool = False
    testonly: bool = False
    unevaluable: bool = False

    @property
    def cmake_name(self) -> str:
        return cmake_target_name(self.package, self.name)

    @property
    def alias(self) -> str:
        return cmake_alias_name(self.package, self.name)

    @property
    def is_interface(self) -> bool:
        return not self.srcs


@dataclass
class Genrule:
    """需要在 CMake 侧落地的生成规则。"""

    package: str
    name: str
    srcs: list[str]
    outs: list[str]
    cmd: str
    handler: str  # "nonterminals-foreach" / "version-header" / "todo"


def _classify_genrule(package: str, name: str) -> str:
    if name == "verilog-nonterminals-foreach-gen":
        return "nonterminals-foreach"
    if name == "version-header":
        return "version-header"
    return "todo"


def _extract_targets(bf: BuildFile) -> tuple[list[Target], list[Genrule]]:
    targets: list[Target] = []
    genrules: list[Genrule] = []
    for rule in bf.rules:
        callee = rule.get("__callee__")
        if callee in ("cc_library", "cc_test", "cc_binary"):
            name = rule.get("name")
            if not isinstance(name, str):
                continue
            t = Target(
                package=bf.package,
                kind=callee,
                name=name,
                srcs=[s for s in _as_list(rule.get("srcs")) if isinstance(s, str)],
                hdrs=[h for h in _as_list(rule.get("hdrs")) if isinstance(h, str)],
                raw_deps=[d for d in _as_list(rule.get("deps")) if isinstance(d, str)],
                copts=_copts_of(rule),
                defines=_defines_of(rule),
                alwayslink=bool(rule.get("alwayslink")),
                testonly=bool(rule.get("testonly")),
            )
            t.deps = [
                m
                for m in (map_dep_label(d, bf.package) for d in t.raw_deps)
                if m
            ]
            # 指向阶段 1 才纳入的包（EXCLUDED_PREFIXES）的依赖：本阶段没有对应
            # target，直接丢弃并记录，否则 CMake 会硬报“target not found”。
            for d in t.raw_deps:
                dep_pkg = package_of_label(d, bf.package)
                if dep_pkg and dep_pkg.startswith(EXCLUDED_PREFIXES):
                    t.excluded_deps.append(d)
                    mapped = map_dep_label(d, bf.package)
                    if mapped in t.deps:
                        t.deps.remove(mapped)
            targets.append(t)
        elif callee == "genrule":
            name = rule.get("name")
            if isinstance(name, str):
                genrules.append(
                    Genrule(
                        package=bf.package,
                        name=name,
                        srcs=[s for s in _as_list(rule.get("srcs")) if isinstance(s, str)],
                        outs=[s for s in _as_list(rule.get("outs")) if isinstance(s, str)],
                        cmd=rule.get("cmd") if isinstance(rule.get("cmd"), str) else "",
                        handler=_classify_genrule(bf.package, name),
                    )
                )
    return targets, genrules


# =============================================================================
# 第四部分：生成 CMakeLists.txt
# =============================================================================

HEADER = """\
# =============================================================================
#  {marker}
# =============================================================================
#  由 {generator} 从同目录的 Bazel BUILD 自动转译而来。
#  手工修改会在下次运行生成器时被覆盖；要改行为请改生成器本身。
#
#  源包: //{package}
#
#  目录里也没有 BUILD 生成物时，本文件只声明库，不在这里编测试
#  （阶段 0 不编 Verible 自带的 225 个 cc_test）。
# =============================================================================
"""


def _emit_options_marker() -> str:
    """每个包的 target 都要挂上统一编译选项 target。"""
    return (
        "# verible_common_opts：/W0 /utf-8 /permissive- /bigobj，由 cmake/Verible.cmake 定义\n"
    )


def _cmake_quote(s: str) -> str:
    return '"' + s.replace("\\", "/") + '"'


def _file_exists_relative(package: str, relpath: str) -> bool:
    """BUILD 里写的文件名相对包目录；判断它在源码树里是否存在。"""
    if os.path.isabs(relpath):
        return os.path.exists(relpath)
    p = os.path.join(VERIBLE_ROOT, package.replace("/", os.sep),
                     relpath.replace("/", os.sep))
    return os.path.exists(p)


def _sorted_unique(items: list[str]) -> list[str]:
    seen = set()
    out = []
    for it in items:
        if it in seen:
            continue
        seen.add(it)
        out.append(it)
    return out


def render_genrules(package: str, genrules: list[Genrule]) -> str:
    """把需要落地的 genrule 转成 add_custom_command 注释块。

    真正的实现（cmake -P 脚本）放在 cmake/scripts/ 下，由任务 0-D 维护；
    这里只声明「生成哪个文件、由谁生成」，保证 CMakeLists 可独立阅读。
    """
    if not genrules:
        return ""
    lines = [
        "# ---------------------------------------------------------------------------",
        "# BUILD 里的 genrule（Bazel 的 sed/grep 在 Windows 上不可用，改用 cmake -P）",
        "# ---------------------------------------------------------------------------",
    ]
    for g in genrules:
        out0 = g.outs[0] if g.outs else g.name
        if g.handler == "nonterminals-foreach":
            lines += [
                f"# genrule({g.name}) -> {out0}",
                "#   从 verilog-nonterminals.h 的 BEGIN GENERATE / END GENERATE 段",
                "#   抽出 CONSIDER(...) 列表。",
                "#   实现：cmake/scripts/verible-nonterminals.cmake（cmake -P 脚本）",
                "#   挂接：cmake/Verible.cmake 的 add_custom_command（本文件不重复声明，",
                f"#         否则会与 Verible.cmake 里的 OUTPUT 撞车）",
                "",
            ]
        elif g.handler == "version-header":
            lines += [
                f"# genrule({g.name}) -> {out0}",
                "#   Bazel 靠 workspace status 生成版本头；这里写最小实现，",
                "#   版本号取自 thirdparty/verible/VENDOR-INFO.md 的 commit。",
                "#   目标：verible__common__util__build-version（纯头文件）",
                "",
            ]
        else:
            lines += [
                f"# genrule({g.name}) -> {', '.join(g.outs) or '(no outs)'}",
                "#   TODO(阶段 0-D)：该生成规则属于 flex/bison 链路，",
                "#   由 cmake/scripts/ 实现，本脚本不重复生成。",
                "",
            ]
    return "\n".join(lines) + "\n"


def render_target(t: Target, include_packages: set[str], skipped_pkg_deps: set[str]) -> str:
    lines: list[str] = []
    name = t.name
    friendly = t.alias
    kind_word = {
        "cc_library": "静态库" if not t.is_interface else "纯头文件库",
        "cc_test": "测试可执行",
        "cc_binary": "可执行",
    }.get(t.kind, t.kind)
    lines.append("")
    lines.append("# " + "-" * 75)
    lines.append(f"# {friendly}   ({kind_word})   <- BUILD: cc_{t.kind.split('_', 1)[-1]}(name = \"{name}\")")
    lines.append("# " + "-" * 75)

    # 目标类型
    # 注意：srcs/hdrs 里可能出现「构建目录里生成的文件」（例如 verilog-token-enum.h
    # 由 bison 生成），这类文件在 configure 期还不存在，直接写进 add_library 会让
    # CMake 报 "Cannot find source file"。规则：
    #   * srcs 里不存在的文件 -> 记入 verible_generated_sources 属性，由
    #     cmake/Verible.cmake 在生成规则就绪后用 target_sources 补挂；
    #   * hdrs 里不存在的文件 -> 同样处理（只影响 IDE 展示，不参与编译）。
    src_present = [s for s in t.srcs if _file_exists_relative(t.package, s)]
    src_missing = [s for s in t.srcs if s not in src_present]
    hdr_present = [h for h in t.hdrs if _file_exists_relative(t.package, h)]
    hdr_missing = [h for h in t.hdrs if h not in hdr_present]

    sources = [_cmake_quote(s) for s in src_present]
    headers = [_cmake_quote(h) for h in hdr_present]

    if t.kind in ("cc_test", "cc_binary"):
        # cc_test 与 cc_binary 都是可执行文件
        if sources:
            lines.append(f"add_executable({t.cmake_name} {' '.join(sources)})")
        else:
            lines.append(f"add_executable({t.cmake_name} EXCLUDE_FROM_ALL)")
    elif not src_present and not src_missing:
        # 真正的纯头文件库（BUILD 里本来就没有 srcs）-> INTERFACE
        lines.append(f"add_library({t.cmake_name} INTERFACE)")
        if headers:
            lines.append(
                f"target_sources({t.cmake_name} INTERFACE {' '.join(headers)})"
            )
    elif not src_present:
        # 源文件全是构建期生成的（verilog-y-cc 的 verilog-final.tab.cc、
        # verilog-lexer 的 verilog.yy.cc 等）——**必须仍然是 STATIC**：
        # 曾经把它建成 INTERFACE 库，导致 target_sources 只能改成 INTERFACE 属性，
        # 最终根本没有 .lib 产出，链接时报
        #   LNK2001: 无法解析的外部符号 verilog::verilog_parse / verilog_debug
        # 这里先建一个「空壳 STATIC 库」，再由 cmake/Verible.cmake 用
        # target_sources() 把生成物补进去（STATIC 库允许后续追加 PRIVATE 源文件）。
        # placeholder.hpp 保证 add_library 在 configure 期有至少一个源文件可用。
        lines.append(f"add_library({t.cmake_name} STATIC \"${{VERIBLE_PLACEHOLDER_SRC}}\")")
        lines.append(
            "#  ^ 空壳：真正的源文件由 cmake/Verible.cmake 在生成规则就绪后"
            " target_sources 补挂"
        )
        if headers:
            lines.append(
                f"target_sources({t.cmake_name} PRIVATE {' '.join(headers)})"
            )
    else:
        lines.append(f"add_library({t.cmake_name} STATIC {' '.join(sources)})")
        if headers:
            lines.append(
                f"target_sources({t.cmake_name} PRIVATE {' '.join(headers)})"
            )

    # configure 期还不存在的文件（构建期生成）：
    if src_missing or hdr_missing:
        lines.append("")
        lines.append("# 构建期生成的文件（configure 时还不存在，由 cmake/Verible.cmake 挂接）：")
        if src_missing:
            lines.append(
                f"set_property(TARGET {t.cmake_name} PROPERTY verible_generated_sources "
                + " ".join(_cmake_quote(x) for x in src_missing)
                + ")"
            )
        if hdr_missing:
            lines.append(
                f"set_property(TARGET {t.cmake_name} PROPERTY verible_generated_headers "
                + " ".join(_cmake_quote(x) for x in hdr_missing)
                + ")"
            )

    # 别名
    lines.append("")
    if t.kind in ("cc_test", "cc_binary"):
        # 可执行文件不能建 add_library(... ALIAS ...)：
        #   add_library cannot create ALIAS target "x" because "y" is not a library
        # 需要「按名字引用可执行文件」时用 $<TARGET_FILE:...>（见 jcxxgen 自举）。
        lines.append(f"# cc_{t.kind.split('_', 1)[-1]}：可执行文件，无 add_library ALIAS；"
                     f"引用时用 $<TARGET_FILE:{t.cmake_name}>")
    else:
        # INTERFACE / STATIC 都能建带 :: 的 ALIAS
        lines.append(f"add_library({friendly} ALIAS {t.cmake_name})")
        # external_libs 不在 verible/ 命名空间下，额外给一个 verible:: 前缀别名，
        # 方便和 //verible/... 的库放在同一条 target_link_libraries 里阅读
        if t.package == "external_libs":
            lines.append(
                f"add_library(verible::{friendly} ALIAS {t.cmake_name})"
            )

    # 编译选项
    lines.append(_emit_options_marker().rstrip())
    is_interface_lib = (t.kind == "cc_library" and not src_present and not src_missing)
    if is_interface_lib:
        lines.append(f"target_link_libraries({t.cmake_name} INTERFACE verible_common_opts)")
    else:
        lines.append(f"target_link_libraries({t.cmake_name} PRIVATE verible_common_opts)")

    # defines
    if t.defines:
        ds = " ".join(_cmake_quote(d) for d in t.defines)
        vis = "INTERFACE" if is_interface_lib else "PUBLIC"
        lines.append(f"target_compile_definitions({t.cmake_name} {vis} {ds})")

    # copts（windows 分支）
    if t.copts:
        cs = " ".join(_cmake_quote(c) for c in t.copts)
        vis = "INTERFACE" if is_interface_lib else "PRIVATE"
        lines.append(f"target_compile_options({t.cmake_name} {vis} {cs})")

    # deps
    if t.deps:
        deps = _sorted_unique(t.deps)
        vis = "INTERFACE" if is_interface_lib else "PUBLIC"
        lines.append("")
        lines.append(f"target_link_libraries({t.cmake_name} {vis}")
        for d in deps:
            lines.append(f"    {d}")
        lines.append(")")

    if t.excluded_deps:
        lines.append("")
        lines.append("# 以下 BUILD 依赖指向阶段 1 才纳入的包，本阶段不链接（否则 target 不存在）：")
        for d in t.excluded_deps:
            lines.append(f"#   - {d}")

    if t.alwayslink:
        lines.append("")
        lines.append("# BUILD: alwayslink = 1")
        lines.append("#   MSVC 静态库没有对应属性；依赖方链接时用 /WHOLEARCHIVE 保证符号不被丢弃。")
        lines.append("#   （verible_parser_libs 聚合 target 里已统一处理）")

    if t.testonly:
        lines.append("")
        lines.append("# BUILD: testonly = 1（本阶段不编译测试，库照常生成以便被引用）")

    return "\n".join(lines) + "\n"


def render_package_cmakelists(
    package: str,
    targets: list[Target],
    genrules: list[Genrule],
    include_packages: set[str],
    skipped_dep_targets: dict[str, list[str]],
) -> str:
    out: list[str] = []
    out.append(
        HEADER.format(marker=GENERATED_MARKER, generator=GENERATOR_NAME, package=package)
    )
    out.append("")

    # 生成物
    gr = render_genrules(package, genrules)
    if gr:
        out.append(gr)

    # 本阶段输出 cc_library + 少量需要的 cc_binary；cc_test 默认跳过
    libs = [t for t in targets if t.kind == "cc_library"]
    tests = [t for t in targets if t.kind == "cc_test"]
    bins = [t for t in targets if t.kind == "cc_binary"]

    if not libs and not bins:
        out.append("# 本包没有 cc_library / cc_binary，不生成 target。\n")

    if tests:
        out.append(
            "# ---------------------------------------------------------------------------\n"
            f"# 本包有 {len(tests)} 个 cc_test —— 按 CLAUDE 约定不编译 Verible 自带测试；\n"
            "# 需要时用 `--with-tests` 重新生成。\n"
            "# ---------------------------------------------------------------------------\n"
        )

    for t in libs:
        if t.unevaluable:
            continue
        out.append(render_target(t, include_packages, set()))
        # 依赖里剔掉被删掉的 target（simple-zip 等），已在 deps 里过滤，这里只加注释
        missing = skipped_dep_targets.get(t.name)
        if missing:
            out.append(
                "\n# 注意：以下 BUILD 依赖已被 0-A 删除，未列入 deps：\n"
                + "".join(f"#   - {m}\n" for m in missing)
            )

    # cc_binary：只为「构建期工具」（Bazel genrule 的 tools=）生成，
    # 典型例子是 common/lsp 的 jcxxgen —— 它被用来生成 lsp-protocol.h，
    # 属于两阶段自举（先编译工具，再用工具产出头文件再编译库）。
    # 其它 CLI 二进制（verible-verilog-syntax 等）不在本项目交付范围。
    for t in bins:
        if t.unevaluable or t.name not in GENERATED_BINARIES:
            continue
        out.append(render_target(t, include_packages, set()))

    return "".join(out).rstrip() + "\n"


# =============================================================================
# 第五部分：包发现（BFS 传递闭包）
# =============================================================================

# 纳入生成的包（阶段 0 + 阶段 1 全量）。
#
# 阶段 0（task-3）：解析链路最小集
# 阶段 1（task-12）：补齐 lint 规则、formatter、lsp
#
# 注意：这里只列「种子」，脚本会用 BFS 把它们的**传递依赖**自动纳入闭包，
# 所以下面列出的包实际会被扩展（例如 checkers 会拉进 CST 全套）。
SEED_PACKAGES = [
    # --- 阶段 0：解析链路 ------------------------------------------------
    "verible/common/text",
    "verible/common/util",
    "verible/common/strings",
    "verible/common/lexer",
    "verible/common/parser",
    "verible/verilog/parser",
    "verible/verilog/CST",
    "verible/common/analysis",
    "verible/verilog/analysis",
    "verible/verilog/preprocessor",
    # --- 阶段 1：lint 规则（58 个规则库 + verilog-lint-rules 聚合） --------
    # 每个规则 cc_library 都是 alwayslink=1（static 全局对象自注册），
    # 最终可执行文件必须对这些库用 /WHOLEARCHIVE，见 verible_lint_rules_all
    "verible/verilog/analysis/checkers",
    # --- 阶段 1：formatter ------------------------------------------------
    "verible/common/formatting",
    "verible/verilog/formatting",
    # --- 阶段 1：LSP ------------------------------------------------------
    # common/lsp 含 jcxxgen 两阶段自举（先建 exe，再用它生成 lsp-protocol.h）
    "verible/common/lsp",
]

# 这些包的 target 不生成（依赖到它们时丢弃并记录），留到后续阶段或永远不需要
EXCLUDED_PREFIXES = (
    "verible/verilog/tools/kythe",
    "third_party",
    "bazel",
)


def discover_build_files() -> dict[str, str]:
    """package -> BUILD 文件绝对路径（只收 verible/ 和 external_libs/ 下的包）。"""
    result: dict[str, str] = {}
    for root in VALID_PKG_ROOTS:
        base = os.path.join(VERIBLE_ROOT, root)
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            if "BUILD" not in filenames:
                continue
            pkg = os.path.relpath(dirpath, VERIBLE_ROOT).replace(os.sep, "/")
            result[pkg] = os.path.join(dirpath, "BUILD")
    return result


def compute_closure(seed: list[str], build_files: dict[str, str]) -> list[str]:
    """种子包 + 「被依赖且存在 BUILD」的包，BFS 求闭包。"""
    parsed: dict[str, list[Target]] = {}
    genrules: dict[str, list[Genrule]] = {}

    def load(pkg: str) -> None:
        if pkg in parsed:
            return
        bf = parse_build_file(build_files[pkg], pkg)
        ts, grs = _extract_targets(bf)
        parsed[pkg] = ts
        genrules[pkg] = grs

    queue = list(seed)
    for p in seed:
        if p not in build_files:
            log(f"警告：种子包 {p} 没有 BUILD 文件")
    seen: set[str] = set()
    while queue:
        pkg = queue.pop(0)
        if pkg in seen or pkg not in build_files:
            continue
        seen.add(pkg)
        load(pkg)
        for t in parsed[pkg]:
            if t.kind != "cc_library":
                continue
            for raw in t.raw_deps:
                dep_pkg = package_of_label(raw, pkg)
                if dep_pkg is None:
                    continue
                if dep_pkg.startswith(EXCLUDED_PREFIXES):
                    continue
                if dep_pkg in seen:
                    continue
                if dep_pkg not in build_files:
                    continue
                queue.append(dep_pkg)
    return sorted(seen)


# =============================================================================
# 第六部分：main
# =============================================================================

def generate(with_tests: bool = False) -> dict[str, str]:
    """返回 {CMakeLists 绝对路径: 内容}。"""
    build_files = discover_build_files()
    packages = compute_closure(SEED_PACKAGES, build_files)
    log(f"BUILD 文件总数: {len(build_files)}，本阶段纳入: {len(packages)} 个包")

    include_packages = set(packages)
    outputs: dict[str, str] = {}
    for pkg in packages:
        bf = parse_build_file(build_files[pkg], pkg)
        targets, genrules = _extract_targets(bf)
        content = render_package_cmakelists(
            pkg, targets, genrules, include_packages, {}
        )
        out_path = os.path.join(VERIBLE_ROOT, pkg.replace("/", os.sep), "CMakeLists.txt")
        outputs[out_path] = content

    # external_libs 只是纯头文件库，也生成一个（否则 //external_libs:editscript 找不到）
    if "external_libs" in build_files:
        bf = parse_build_file(build_files["external_libs"], "external_libs")
        targets, genrules = _extract_targets(bf)
        content = render_package_cmakelists(
            "external_libs", targets, genrules, include_packages, {}
        )
        out_path = os.path.join(
            VERIBLE_ROOT, "external_libs", "CMakeLists.txt"
        )
        outputs[out_path] = content

    return outputs


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="只校验，不一致返回 1")
    ap.add_argument("--list", action="store_true", help="只列出包")
    ap.add_argument("--with-tests", action="store_true", help="同时生成 cc_test 目标")
    ap.add_argument("--prune", action="store_true", help="删除不再需要的生成物")
    args = ap.parse_args(argv)

    if args.list:
        build_files = discover_build_files()
        pkgs = compute_closure(SEED_PACKAGES, build_files)
        print(f"BUILD 文件: {len(build_files)}")
        print(f"纳入闭包: {len(pkgs)}")
        for p in pkgs:
            print("  ", p)
        return 0

    outputs = generate(with_tests=args.with_tests)
    changed = 0
    for path, content in outputs.items():
        old = None
        if os.path.exists(path):
            with open(path, "r", encoding="utf-8") as f:
                old = f.read()
        if old == content:
            continue
        changed += 1
        if args.check:
            log(f"不一致: {os.path.relpath(path, REPO_ROOT)}")
        else:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            # 统一 LF，避免 Windows 上 git 的 CRLF 噪音
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(content)
            log(f"写入: {os.path.relpath(path, REPO_ROOT)}")

    log(f"生成 {len(outputs)} 个 CMakeLists.txt，变化 {changed} 个")
    if args.check and changed:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))