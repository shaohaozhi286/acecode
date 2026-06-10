#pragma once

#include "../provider/llm_provider.hpp"

#include <optional>
#include <string>

namespace acecode {

std::string sanitize_generated_session_title(std::string raw);

std::optional<std::string> generate_session_title(
    LlmProvider& provider,
    const std::string& first_user_text,
    int max_input_bytes);

} // namespace acecode
