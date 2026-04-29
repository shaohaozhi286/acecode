#pragma once

// Undefine Windows macros that conflict with our enum names
#ifdef ERROR
#undef ERROR
#endif
#ifdef DEBUG
#undef DEBUG
#endif

#include <string>
#include <fstream>
#include <mutex>
#include <chrono>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <filesystem>
#include <functional>
#include <cstdio>

namespace acecode {

enum class LogLevel { Dbg = 0, Info = 1, Warn = 2, Err = 3 };

class Logger {
public:
    static Logger& instance() {
        static Logger inst;
        return inst;
    }

    // TUI / 单文件模式: 写入指定文件,不滚动,不镜像 stderr。
    // 与原 v1 行为兼容,保持已有调用点(main.cpp 写 acecode.log)零改动。
    void init(const std::string& log_file) {
        std::lock_guard<std::mutex> lk(mu_);
        if (ofs_.is_open()) ofs_.close();
        rotation_enabled_ = false;
        mirror_stderr_ = false;
        rotation_dir_.clear();
        rotation_base_.clear();
        last_open_date_.clear();
        ofs_.open(log_file, std::ios::out | std::ios::app);
        enabled_ = ofs_.is_open();
    }

    // daemon 模式: 写入 dir/<base_name>-<YYYY-MM-DD>.log,跨本地午夜
    // 自动滚动到新日期文件。mirror_stderr=true 时每条日志同时写 stderr
    // (foreground 模式)。dir 不存在会被创建。
    void init_with_rotation(const std::string& dir,
                            const std::string& base_name,
                            bool mirror_stderr) {
        std::lock_guard<std::mutex> lk(mu_);
        if (ofs_.is_open()) ofs_.close();
        rotation_enabled_ = true;
        mirror_stderr_ = mirror_stderr;
        rotation_dir_ = dir;
        rotation_base_ = base_name;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        open_rotated_locked_(current_date_string_());
    }

    void set_level(LogLevel level) { level_ = level; }

    // 测试专用: 注入一个返回 "YYYY-MM-DD" 字符串的 callable,用于强制
    // 触发跨日滚动而无需等真实午夜。传空 std::function 还原为真实时钟。
    void set_clock_for_test(std::function<std::string()> fn) {
        std::lock_guard<std::mutex> lk(mu_);
        clock_for_test_ = std::move(fn);
    }

    void log(LogLevel level, const char* file, int line, const std::string& msg) {
        if (!enabled_ || level < level_) return;
        std::lock_guard<std::mutex> lk(mu_);

        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        std::tm tm_buf{};
#ifdef _WIN32
        localtime_s(&tm_buf, &time);
#else
        localtime_r(&time, &tm_buf);
#endif

        if (rotation_enabled_) {
            std::string today = clock_for_test_
                ? clock_for_test_()
                : format_date_(tm_buf);
            if (today != last_open_date_) {
                open_rotated_locked_(today);
            }
        }

        // Extract just the filename from path
        std::string fname(file);
        auto sep = fname.find_last_of("/\\");
        if (sep != std::string::npos) fname = fname.substr(sep + 1);

        std::ostringstream line_oss;
        line_oss << std::put_time(&tm_buf, "%H:%M:%S") << "."
                 << std::setfill('0') << std::setw(3) << ms.count()
                 << " " << level_str(level)
                 << " [" << fname << ":" << line << "] "
                 << msg << "\n";
        const std::string s = line_oss.str();

        if (ofs_.is_open()) {
            ofs_ << s;
            ofs_.flush();
        }
        if (mirror_stderr_) {
            std::cerr << s;
            std::cerr.flush();
        }
    }

private:
    Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    static const char* level_str(LogLevel l) {
        switch (l) {
            case LogLevel::Dbg:  return "DBG";
            case LogLevel::Info: return "INF";
            case LogLevel::Warn: return "WRN";
            case LogLevel::Err:  return "ERR";
        }
        return "???";
    }

    static std::string format_date_(const std::tm& tm_buf) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                      tm_buf.tm_year + 1900,
                      tm_buf.tm_mon + 1,
                      tm_buf.tm_mday);
        return std::string(buf);
    }

    static std::string current_date_string_() {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf{};
#ifdef _WIN32
        localtime_s(&tm_buf, &time);
#else
        localtime_r(&time, &tm_buf);
#endif
        return format_date_(tm_buf);
    }

    void open_rotated_locked_(const std::string& date) {
        if (ofs_.is_open()) ofs_.close();
        auto path = std::filesystem::path(rotation_dir_) /
                    (rotation_base_ + "-" + date + ".log");
        ofs_.open(path.string(), std::ios::out | std::ios::app);
        last_open_date_ = date;
        enabled_ = ofs_.is_open();
    }

    std::ofstream ofs_;
    std::mutex mu_;
    LogLevel level_ = LogLevel::Dbg;
    bool enabled_ = false;

    bool rotation_enabled_ = false;
    bool mirror_stderr_ = false;
    std::string rotation_dir_;
    std::string rotation_base_;
    std::string last_open_date_;
    std::function<std::string()> clock_for_test_;
};

// Truncate long strings for logging
inline std::string log_truncate(const std::string& s, size_t max_len = 500) {
    if (s.size() <= max_len) return s;
    return s.substr(0, max_len) + "...(" + std::to_string(s.size()) + " bytes)";
}

} // namespace acecode

#define LOG_DEBUG(msg) ::acecode::Logger::instance().log(::acecode::LogLevel::Dbg,  __FILE__, __LINE__, msg)
#define LOG_INFO(msg)  ::acecode::Logger::instance().log(::acecode::LogLevel::Info, __FILE__, __LINE__, msg)
#define LOG_WARN(msg)  ::acecode::Logger::instance().log(::acecode::LogLevel::Warn, __FILE__, __LINE__, msg)
#define LOG_ERROR(msg) ::acecode::Logger::instance().log(::acecode::LogLevel::Err,  __FILE__, __LINE__, msg)
