// =============================================================================
//  src/platform/log.cpp
// =============================================================================

#include "platform/log.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>  // 提供 stderr_color_sink_mt（无独立的 stderr 头）
#include <spdlog/spdlog.h>

#include <mutex>
#include <utility>
#include <vector>

#include "platform/encoding.h"
#include "platform/path.h"

namespace fpgalsp::platform {
namespace {

spdlog::level::level_enum to_spdlog_level(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return spdlog::level::trace;
        case LogLevel::Debug: return spdlog::level::debug;
        case LogLevel::Info: return spdlog::level::info;
        case LogLevel::Warn: return spdlog::level::warn;
        case LogLevel::Error: return spdlog::level::err;
        case LogLevel::Off: return spdlog::level::off;
    }
    return spdlog::level::info;
}

}  // namespace

struct Logger::Impl {
    std::shared_ptr<spdlog::logger> logger;  // nullptr == disabled
};

Logger::Logger() : impl_(std::make_unique<Impl>()) {}
Logger::~Logger() { flush(); }

Logger::Logger(Logger&&) noexcept = default;
Logger& Logger::operator=(Logger&&) noexcept = default;

bool Logger::enabled() const noexcept {
    return impl_ && impl_->logger != nullptr;
}

void Logger::log(LogLevel level, std::string_view message) const {
    if (!enabled() || level == LogLevel::Off) return;
    try {
        // spdlog 的 fmt 要求的格式串不能是运行时字符串，所以这里用 "{}" + 参数
        impl_->logger->log(to_spdlog_level(level), "{}", message);
    } catch (...) {
        // never-raise：日志失败绝不影响主流程
    }
}

void Logger::trace(std::string_view message) const { log(LogLevel::Trace, message); }
void Logger::debug(std::string_view message) const { log(LogLevel::Debug, message); }
void Logger::info(std::string_view message) const { log(LogLevel::Info, message); }
void Logger::warn(std::string_view message) const { log(LogLevel::Warn, message); }
void Logger::error(std::string_view message) const { log(LogLevel::Error, message); }

void Logger::flush() const {
    if (!enabled()) return;
    try {
        impl_->logger->flush();
    } catch (...) {
    }
}

Logger create_logger(const LogOptions& options) {
    Logger result;

    // 对应 Python：path 为空 -> 静默禁用
    if (options.file_path.empty()) {
        return result;
    }

    try {
        const std::string normalized = normalize_path(options.file_path);

        // Python 版会 makedirs(dirname(path), exist_ok=True)
        const std::string parent = path_dirname(normalized);
        if (!parent.empty()) {
            ensure_directory(parent);
        }

        std::vector<spdlog::sink_ptr> sinks;

        if (options.max_file_size > 0) {
            sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                normalized, options.max_file_size, options.max_files));
        } else {
            // 追加模式（对应 Python 的 open(path, "a")）
            sinks.push_back(
                std::make_shared<spdlog::sinks::basic_file_sink_mt>(normalized, /*truncate=*/false));
        }

        // 注意：这里**绝不**添加 stdout sink —— stdout 归协议独占。
        if (options.also_stderr) {
            sinks.push_back(std::make_shared<spdlog::sinks::stderr_color_sink_mt>());
        }

        auto logger = std::make_shared<spdlog::logger>("fpgalsp", sinks.begin(), sinks.end());
        logger->set_level(to_spdlog_level(options.level));
        logger->flush_on(spdlog::level::trace);  // 对应 Python 的行缓冲 + 逐行 flush
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");

        result.impl_->logger = std::move(logger);
        spdlog::register_logger(result.impl_->logger);
    } catch (...) {
        // 打开失败不抛，退化为 disabled（对应 Python 的 except OSError: stream = None）
        result.impl_ = std::make_unique<Logger::Impl>();
    }

    return result;
}

// -----------------------------------------------------------------------------
// 全局默认 logger
// -----------------------------------------------------------------------------

namespace {
std::mutex g_logger_mutex;
Logger g_default_logger;
}  // namespace

Logger& default_logger() { return g_default_logger; }

void set_default_logger(Logger logger) {
    std::lock_guard<std::mutex> lock(g_logger_mutex);
    g_default_logger = std::move(logger);
}

void log_info(std::string_view message) { g_default_logger.info(message); }
void log_warn(std::string_view message) { g_default_logger.warn(message); }
void log_error(std::string_view message) { g_default_logger.error(message); }

}  // namespace fpgalsp::platform