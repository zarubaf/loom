// SPDX-License-Identifier: Apache-2.0
// Loom Logging System
//
// A lightweight, header-only logging system with log levels and component prefixes.
// Thread-safe output with optional color support.

#pragma once

#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <string>
#include <mutex>

namespace loom {

// Log levels
enum class LogLevel : uint8_t {
    Debug = 0,
    Info = 1,
    Warning = 2,
    Error = 3,
    None = 4
};

// ANSI color codes
namespace color {
    constexpr const char* Reset   = "\033[0m";
    constexpr const char* Red     = "\033[31m";
    constexpr const char* Green   = "\033[32m";
    constexpr const char* Yellow  = "\033[33m";
    constexpr const char* Blue    = "\033[34m";
    constexpr const char* Magenta = "\033[35m";
    constexpr const char* Cyan    = "\033[36m";
    constexpr const char* Gray    = "\033[90m";
}

// Global log configuration
class LogConfig {
public:
    static LogConfig& instance() {
        static LogConfig config;
        return config;
    }

    void set_level(LogLevel level) { level_ = level; }
    LogLevel level() const { return level_; }

    void set_color_enabled(bool enabled) { color_enabled_ = enabled; }
    bool color_enabled() const { return color_enabled_; }

    std::mutex& mutex() { return mutex_; }

private:
    LogConfig() : level_(LogLevel::Info), color_enabled_(true) {}
    LogLevel level_;
    bool color_enabled_;
    std::mutex mutex_;
};

// Logger class for a specific component
class Logger {
public:
    explicit Logger(std::string component)
        : component_(std::move(component)) {}

    template<typename... Args>
    void debug(const char* fmt, Args... args) const {
        log(LogLevel::Debug, fmt, args...);
    }

    template<typename... Args>
    void info(const char* fmt, Args... args) const {
        log(LogLevel::Info, fmt, args...);
    }

    template<typename... Args>
    void warning(const char* fmt, Args... args) const {
        log(LogLevel::Warning, fmt, args...);
    }

    template<typename... Args>
    void error(const char* fmt, Args... args) const {
        log(LogLevel::Error, fmt, args...);
    }

    // Varargs version for C compatibility
    void debug_v(const char* fmt, ...) const {
        va_list args;
        va_start(args, fmt);
        log_v(LogLevel::Debug, fmt, args);
        va_end(args);
    }

    void info_v(const char* fmt, ...) const {
        va_list args;
        va_start(args, fmt);
        log_v(LogLevel::Info, fmt, args);
        va_end(args);
    }

    void warning_v(const char* fmt, ...) const {
        va_list args;
        va_start(args, fmt);
        log_v(LogLevel::Warning, fmt, args);
        va_end(args);
    }

    void error_v(const char* fmt, ...) const {
        va_list args;
        va_start(args, fmt);
        log_v(LogLevel::Error, fmt, args);
        va_end(args);
    }

private:
    template<typename... Args>
    void log(LogLevel level, const char* fmt, Args... args) const {
        auto& config = LogConfig::instance();
        if (level < config.level()) return;

        std::lock_guard<std::mutex> lock(config.mutex());

        FILE* out = (level >= LogLevel::Warning) ? stderr : stdout;

        print_prefix(out, level, config.color_enabled());
        if constexpr (sizeof...(args) == 0) {
            // No args - print format string directly (safely)
            std::fputs(fmt, out);
        } else {
            std::fprintf(out, fmt, args...);
        }
        std::fprintf(out, "\n");
        std::fflush(out);
    }

    void log_v(LogLevel level, const char* fmt, va_list args) const {
        auto& config = LogConfig::instance();
        if (level < config.level()) return;

        std::lock_guard<std::mutex> lock(config.mutex());

        FILE* out = (level >= LogLevel::Warning) ? stderr : stdout;

        print_prefix(out, level, config.color_enabled());
        std::vfprintf(out, fmt, args);
        std::fprintf(out, "\n");
        std::fflush(out);
    }

    void print_prefix(FILE* out, LogLevel level, bool use_color) const {
        const char* level_str = "";
        const char* level_color = "";
        const char* component_color = color::Cyan;

        switch (level) {
        case LogLevel::Debug:
            level_str = "DEBUG";
            level_color = color::Gray;
            break;
        case LogLevel::Info:
            level_str = "INFO";
            level_color = color::Green;
            break;
        case LogLevel::Warning:
            level_str = "WARN";
            level_color = color::Yellow;
            break;
        case LogLevel::Error:
            level_str = "ERROR";
            level_color = color::Red;
            break;
        default:
            break;
        }

        if (use_color) {
            std::fprintf(out, "%s[%s]%s %s%-5s%s ",
                         component_color, component_.c_str(), color::Reset,
                         level_color, level_str, color::Reset);
        } else {
            std::fprintf(out, "[%s] %-5s ", component_.c_str(), level_str);
        }
    }

    std::string component_;
};

// Convenience function to create a logger
inline Logger make_logger(const std::string& component) {
    return Logger(component);
}

// Global log level setter
inline void set_log_level(LogLevel level) {
    LogConfig::instance().set_level(level);
}

// Global color enable/disable
inline void set_log_color(bool enabled) {
    LogConfig::instance().set_color_enabled(enabled);
}

} // namespace loom
