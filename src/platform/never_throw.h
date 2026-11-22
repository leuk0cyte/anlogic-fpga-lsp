// =============================================================================
//  src/platform/never_throw.h —— "我确实决定忽略"的两个有名字的边界（A-3/A-4）
// =============================================================================
//  为什么需要：0.26.0 的 `write_service` 把全部文件写入收成一个返回 `Result`
//  的入口，但调用点普遍写成 `(void)platform::writes::Write(...)` —— 语义是
//  "编译器让我消警告"，而不是"我知道失败会怎样"。磁盘满/权限拒绝时用户看到
//  的是"报告缺了一段"，日志里一条都没有。
//
//  `IgnoreResult(x)`：把"忽略失败"变成显式决策，且失败至少留一条 debug 日志
//    （默认日志级别下零成本：Logger::debug 在 level=Info 时直接返回）。
//  `NeverThrow(fn)`：把空 `catch` 变成"有名字的吞异常边界"，保留原函数的
//    `noexcept` 契约（异常到边界为止，绝不外抛）。
//
//  ⚠ 两者都不得抛：IgnoreResult/NeverThrow 自身是 noexcept。日志写入失败在
//    Logger 内部已被吞掉（log.h 的 never-raise 契约）。
// =============================================================================

#pragma once

#include <exception>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "platform/log.h"

namespace fpgalsp::platform {

namespace detail {

// 一条 "suppressed" debug 日志。what/why 为空时相应片段省略。
inline void LogSuppressed(std::string_view what, std::string_view why) noexcept {
    // 边界内不许抛：连"构造日志字符串"（bad_alloc）和 Logger 写入都包住。
    try {
        std::string line = "suppressed exception";
        if (!what.empty()) {
            line += " at ";
            line += what;
        }
        if (!why.empty()) {
            line += ": ";
            line += why;
        }
        default_logger().debug(line);
    } catch (...) {
        // 日志失败也不能让异常越过 noexcept 边界。
        return;
    }
}

}  // namespace detail

// 显式忽略一个"可能失败"的返回值（当前用于 platform::writes::Result）。
// 失败时记一条 debug 日志：路径 + 原因，便于排查"为什么少了一段输出"。
template <class T>
void IgnoreResult(T&& value) noexcept {
    using Value = std::remove_cvref_t<T>;
    if constexpr (requires(const Value& v) {
                      v.ok;
                      v.error;
                      v.path;
                  }) {
        if (value.ok) return;
        // 同上：构造日志字符串/写日志都可能抛，必须留在 noexcept 边界内。
        try {
            std::string line = "ignored write failure";
            if (!value.path.empty()) {
                line += " (";
                line += value.path;
                line += ')';
            }
            line += ": ";
            line += value.error;
            default_logger().debug(line);
        } catch (...) {
            // 同 LogSuppressed：吞掉，绝不越过 noexcept 边界。
            return;
        }
    } else {
        // 不是 write_service 的形状：只需要把值求值掉（保留副作用）。
        static_cast<void>(value);
    }
}

// 有名字的"吞异常边界"。用于原本写空 catch 的地方（A-4：commands.cpp 读分析
// 缓存、td_diagnostics.cpp 写设备缓存）。**保持 noexcept**：原函数不抛，
// 边界内也不许抛。
template <class F>
void NeverThrow(F&& fn, std::string_view what = {}) noexcept {
    try {
        std::forward<F>(fn)();
    } catch (const std::exception& error) {
        detail::LogSuppressed(what, error.what());
    } catch (...) {
        detail::LogSuppressed(what, "unknown exception");
    }
}

}  // namespace fpgalsp::platform
