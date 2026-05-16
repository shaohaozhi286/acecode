#pragma once

#include <ctime>
#include <string>

namespace acecode {

std::string format_utc_offset(int offset_minutes);
std::string format_prompt_datetime(const std::tm& local_time,
                                   int utc_offset_minutes);
std::string current_prompt_datetime();

} // namespace acecode
