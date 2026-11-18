// =============================================================================
//  src/platform/log.h —— 基于 spdlog 的日志（绝不写 stdout）
// =============================================================================
//  对应参考实现：Y:\fpga-lsp\fpgalsp\lsp.py :: setup_stdio_log
//      * 路径为空时静默禁用（返回 no-op 的 log 可调用对象）
//      * 打开失败也不抛，退化为 no-op
//      * 追加模式写入，逐行 flush
//
//  ⚠ LSP/MCP 场景下 stdout 归协议独占。因此本模块**只写文件**，
//    绝不添加任何 stdout sink。stderr sink 仅在显式开启时才加。
// =============================================================================

#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace spdlog {
class logger;
}  // namespace spdlog

namespace fpgalsp::platform {

enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Off,
};

struct LogOptions {
    // 日志文件路径。为空 -> 静默禁用（对应 Python 的 path 为 None/""）
    std::string file_path;

    LogLevel level = LogLevel::Info;

    // 滚动策略：单文件最大字节数（0 表示不滚动）
    std::size_t max_file_size = 8 * 1024 * 1024;

    // 最多保留的历史文件数
    std::size_t max_files = 3;

    // 是否同时写 stderr（默认关闭；调试时用 --verbose 打开）
    // 注意：**从不**写 stdout。
    bool also_stderr = false;
};

class Logger {
public:
    // 创建一个空操作的 Logger（丢弃所有日志）。
    Logger();
    ~Logger();

    Logger(Logger&&) noexcept;
    Logger& operator=(Logger&&) noexcept;

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // 是否真的会写日志（对应 Python 里 stream is None 的判定）
    bool enabled() const noexcept;

    // 日志写入。never-raise：任何 IO 失败都静默吞掉（对应 Python 的 except OSError: pass）
    void log(LogLevel level, std::string_view message) const;

    void trace(std::string_view message) const;
    void debug(std::string_view message) const;
    void info(std::string_view message) const;
    void warn(std::string_view message) const;
    void error(std::string_view message) const;

    // 在关闭/替换前把缓冲刷到磁盘。
    void flush() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // create_logger 需要构造带实际 sink 的 Logger
    friend Logger create_logger(const LogOptions& options);
};

// 按选项创建 Logger。路径为空或打开失败时返回一个 disabled 的 Logger（不抛）。
Logger create_logger(const LogOptions& options);

// 全局默认 logger 的访问（供没有显式传递 Logger 的模块使用）。
// 初始为 disabled；由 app 启动时调用 set_default_logger 替换。
Logger& default_logger();
void set_default_logger(Logger logger);

// 便捷函数：走 default_logger()
void log_info(std::string_view message);
void log_warn(std::string_view message);
void log_error(std::string_view message);

}  // namespace fpgalsp::platform