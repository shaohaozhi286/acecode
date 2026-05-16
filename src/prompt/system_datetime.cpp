#include "system_datetime.hpp"

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <sstream>

namespace acecode {
namespace {

std::tm local_tm_from_time(std::time_t t) {
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    return tm_buf;
}

std::tm utc_tm_from_time(std::time_t t) {
    std::tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    return tm_buf;
}

int utc_offset_minutes_for(std::time_t t) {
    std::tm local_tm = local_tm_from_time(t);
    std::tm utc_tm = utc_tm_from_time(t);
    const std::time_t local_as_time = std::mktime(&local_tm);
    const std::time_t utc_as_local_time = std::mktime(&utc_tm);
    return static_cast<int>(
        std::difftime(local_as_time, utc_as_local_time) / 60);
}

const char* weekday_name(int wday) {
    static constexpr const char* kWeekdays[] = {
        "Sunday", "Monday", "Tuesday", "Wednesday",
        "Thursday", "Friday", "Saturday",
    };
    if (wday < 0 || wday >= 7) return "Unknown";
    return kWeekdays[wday];
}

} // namespace

std::string format_utc_offset(int offset_minutes) {
    const char sign = offset_minutes < 0 ? '-' : '+';
    const int abs_minutes = std::abs(offset_minutes);
    const int hours = abs_minutes / 60;
    const int minutes = abs_minutes % 60;

    std::ostringstream oss;
    oss << "UTC" << sign
        << std::setw(2) << std::setfill('0') << hours
        << ":"
        << std::setw(2) << std::setfill('0') << minutes;
    return oss.str();
}

std::string format_prompt_datetime(const std::tm& local_time,
                                   int utc_offset_minutes) {
    std::ostringstream oss;
    oss << std::setw(4) << std::setfill('0') << (local_time.tm_year + 1900)
        << "-"
        << std::setw(2) << std::setfill('0') << (local_time.tm_mon + 1)
        << "-"
        << std::setw(2) << std::setfill('0') << local_time.tm_mday
        << " "
        << std::setw(2) << std::setfill('0') << local_time.tm_hour
        << ":"
        << std::setw(2) << std::setfill('0') << local_time.tm_min
        << ":"
        << std::setw(2) << std::setfill('0') << local_time.tm_sec
        << " "
        << format_utc_offset(utc_offset_minutes)
        << " ("
        << weekday_name(local_time.tm_wday)
        << ")";
    return oss.str();
}

std::string current_prompt_datetime() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    return format_prompt_datetime(local_tm_from_time(t),
                                  utc_offset_minutes_for(t));
}

} // namespace acecode
