#pragma once

// 封装 ~/.acecode/run/ 下的运行时产物文件读写: pid/port/guid/heartbeat/token。
// 写入一律走 atomic_write_file(.tmp + rename),token 额外限制权限(0600 / 仅当
// 前用户 ACL)。心跳文件用 JSON 格式带时间戳,supervisor 直接读内容判超时,
// 避免 mtime 在 FAT32 / 网络盘上精度不足的坑。

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace acecode::daemon {

struct Heartbeat {
    std::int64_t pid = 0;
    std::string guid;
    std::int64_t timestamp_ms = 0; // unix epoch ms,UTC
};

struct DesktopManagedRuntime {
    std::int64_t pid = 0;
    std::string guid;
    std::string kind;
    int protocol_version = 0;
    std::string acecode_version;
};

struct DesktopOwnerRecord {
    std::int64_t pid = 0;
    std::string instance_id;
    std::int64_t timestamp_ms = 0;
};

struct RuntimeSnapshot {
    std::optional<std::int64_t> pid;
    std::optional<int> port;
    std::optional<std::string> guid;
    std::optional<Heartbeat> heartbeat;
    std::optional<std::string> token;
    std::optional<DesktopManagedRuntime> desktop_managed;
};

struct RuntimeValidationOptions {
    int heartbeat_timeout_ms = 15000;
    bool require_token = true;
    bool require_port_probe = true;
    bool require_process_identity = true;
    std::int64_t now_ms = 0; // 0 = use current time
};

struct RuntimeReuseCheck {
    bool reusable = false;
    std::string reason;
};

enum class DaemonProcessIdentity {
    Match,
    Mismatch,
    Unknown,
};

// 创建 ~/.acecode/run/ 目录(若已存在则忽略)。返回目录绝对路径。
std::string ensure_run_dir();

// daemon.pid / daemon.port / daemon.guid: 一行纯文本。
bool write_pid_file(std::int64_t pid);
bool write_port_file(int port);
bool write_guid_file(const std::string& guid);

std::optional<std::int64_t> read_pid_file();
std::optional<int>          read_port_file();
std::optional<std::string>  read_guid_file();

// heartbeat: JSON 内容,supervisor 用 timestamp_ms 判超时,不依赖 mtime。
bool write_heartbeat(const Heartbeat& hb);
std::optional<Heartbeat> read_heartbeat();

// token: 限权写。单独的接口避免误用 atomic_write_file 时忘了 restrict_permissions。
bool write_token(const std::string& token);
std::optional<std::string> read_token();

// Desktop-only runtime identity. The manifest is written last after the normal
// pid/port/token bundle so its presence means the generation is fully
// initialized. Explicit run_dir overloads are used by the native Desktop
// process before it has applied a daemon-side run-dir override.
bool write_desktop_managed_runtime(
    const DesktopManagedRuntime& runtime,
    const std::string& run_dir = std::string());
std::optional<DesktopManagedRuntime> read_desktop_managed_runtime(
    const std::string& run_dir = std::string());

bool write_desktop_owner_record(const std::string& run_dir,
                                const DesktopOwnerRecord& owner);
std::optional<DesktopOwnerRecord> read_desktop_owner_record(
    const std::string& run_dir = std::string());

RuntimeSnapshot read_runtime_snapshot(const std::string& run_dir = std::string());
RuntimeReuseCheck validate_runtime_snapshot_for_reuse(
    const RuntimeSnapshot& snapshot,
    const RuntimeValidationOptions& options = RuntimeValidationOptions{});
RuntimeReuseCheck validate_runtime_files_for_reuse(
    const std::string& run_dir = std::string(),
    const RuntimeValidationOptions& options = RuntimeValidationOptions{});

// Single loopback TCP probe used to decide whether recorded daemon.port is
// still served by a live daemon.
bool probe_loopback_port(int port);

// Best-effort process-generation checks used before Desktop attaches to or
// signals a recorded PID. Executable identity is tri-state so a proven
// mismatch (safe stale-state cleanup) stays distinct from an unreadable
// identity (fail closed). Process start time is Unix epoch milliseconds when
// the platform can expose it.
DaemonProcessIdentity inspect_daemon_process_identity(std::int64_t pid);
std::optional<std::int64_t> process_start_time_ms(std::int64_t pid);

// A mismatch proves that the recorded daemon PID now names another
// executable. A process start later than the matching recorded heartbeat also
// proves PID reuse, including reuse by another executable with the same name.
// The timestamp tolerance avoids false positives from coarse platform clocks.
bool runtime_pid_reuse_is_proven(
    const RuntimeSnapshot& snapshot,
    DaemonProcessIdentity process_identity,
    const std::optional<std::int64_t>& live_process_start_time_ms,
    std::int64_t timestamp_tolerance_ms = 2000);

// Compatibility helper for existing destructive callers. Unknown/unreadable
// identities return false; callers that need to distinguish mismatch from
// unknown must use inspect_daemon_process_identity().
bool process_is_acecode_daemon(std::int64_t pid);

// daemon stop / 优雅退出时调用: 删 pid/port/heartbeat/token,保留 guid
// (guid 可用于事后追溯)。删除失败不视为错误。
void cleanup_runtime_files();

// Remove one daemon runtime generation only when the current on-disk pid and
// guid still match. The Desktop owner record belongs to the Desktop instance,
// not the daemon generation, and is intentionally preserved. Managed callers
// set remove_guid=true so a replacement can use a fresh generation id. Returns
// false on an ownership mismatch.
bool cleanup_runtime_files_if_owned(std::int64_t expected_pid,
                                    const std::string& expected_guid,
                                    const std::string& run_dir = std::string(),
                                    bool remove_guid = false);

// helper: 拿当前时间的 unix epoch 毫秒
inline std::int64_t now_unix_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
}

} // namespace acecode::daemon
