#pragma once

namespace acecode {
namespace constants {

// Token estimation
constexpr int CHARS_PER_TOKEN = 4;

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

// Daemon / web defaults
// 28080 是高位端口,大概率不与常用开发服务(3000/5173/8080/8000/9000)冲撞;
// 用户可在 config.json 的 web.port 覆盖。Service 模式下端口被占直接拒启,不 retry。
constexpr int DEFAULT_WEB_PORT = 28080;
constexpr int DEFAULT_HEARTBEAT_INTERVAL_MS = 2000;
constexpr int DEFAULT_HEARTBEAT_TIMEOUT_MS = 15000;
constexpr int TOKEN_BYTES = 32; // raw bytes; url-safe base64 encodes to ~43 chars

// Subdirectories under ~/.acecode/
inline constexpr const char* SUBDIR_RUN  = "run";
inline constexpr const char* SUBDIR_LOGS = "logs";

// Runtime files inside ~/.acecode/run/
inline constexpr const char* RUN_FILE_PID       = "daemon.pid";
inline constexpr const char* RUN_FILE_PORT      = "daemon.port";
inline constexpr const char* RUN_FILE_GUID      = "daemon.guid";
inline constexpr const char* RUN_FILE_HEARTBEAT = "heartbeat";
inline constexpr const char* RUN_FILE_TOKEN     = "token";

} // namespace constants
} // namespace acecode
