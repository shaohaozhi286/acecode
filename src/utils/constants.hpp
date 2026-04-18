#pragma once

namespace acecode {
namespace constants {

// Token estimation
constexpr int CHARS_PER_TOKEN = 4;

// Token costs (per 1M tokens)
constexpr double TOKEN_COST_INPUT_1M = 15.0;
constexpr double TOKEN_COST_OUTPUT_1M = 60.0;

// File size limits
constexpr size_t MAX_FILE_SIZE = 10 * 1024 * 1024; // 10MB

// Output limits
constexpr size_t MAX_BASH_OUTPUT_SIZE = 100 * 1024; // 100KB
constexpr size_t MAX_GLOB_RESULTS = 500;
constexpr size_t MAX_GREP_RESULTS = 200;

// Timeouts
constexpr int DEFAULT_BASH_TIMEOUT_MS = 120000; // 2 minutes

// Compact thresholds
constexpr int MINIMUM_TOKENS_TO_COMPACT = 200;

} // namespace constants
} // namespace acecode
