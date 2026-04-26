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

// daemon stop / 优雅退出时调用: 删 pid/port/heartbeat/token,保留 guid
// (guid 可用于事后追溯)。删除失败不视为错误。
void cleanup_runtime_files();

// helper: 拿当前时间的 unix epoch 毫秒
inline std::int64_t now_unix_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
}

} // namespace acecode::daemon
