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

namespace acecode {

enum class LogLevel { Dbg = 0, Info = 1, Warn = 2, Err = 3 };

class Logger {
public:
    static Logger& instance() {
        static Logger inst;
        return inst;
    }

    void init(const std::string& log_file) {
        std::lock_guard<std::mutex> lk(mu_);
        if (ofs_.is_open()) ofs_.close();
        ofs_.open(log_file, std::ios::out | std::ios::app);
        enabled_ = ofs_.is_open();
    }

    void set_level(LogLevel level) { level_ = level; }

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

        // Extract just the filename from path
        std::string fname(file);
        auto sep = fname.find_last_of("/\\");
        if (sep != std::string::npos) fname = fname.substr(sep + 1);

        ofs_ << std::put_time(&tm_buf, "%H:%M:%S") << "." 
             << std::setfill('0') << std::setw(3) << ms.count()
             << " " << level_str(level)
             << " [" << fname << ":" << line << "] "
             << msg << "\n";
        ofs_.flush();
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

    std::ofstream ofs_;
    std::mutex mu_;
    LogLevel level_ = LogLevel::Dbg;
    bool enabled_ = false;
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
