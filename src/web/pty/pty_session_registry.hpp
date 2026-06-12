#pragma once

// PTY 会话注册表(openspec/changes/add-console-dock,specs/console-pty-backend)。
//
// 持有 daemon 的全部控制台会话:每会话一个 PtyProcess + 2MB 回滚缓冲 +
// 单调输出游标 + WS 订阅者集合。协议语义(对齐 opencode pty 设计):
//   - 游标 = 累计输出字节数。客户端带 cursor 重连时补发缓冲中 cursor 之后
//     的部分(64KB 分帧),再发 {"cursor": N} 控制帧;之后是实时流。
//   - 控制帧 = 0x00 + UTF-8 JSON(encode_pty_control_frame);其余帧为原始
//     PTY 字节。进程退出广播 {"exit_code": N}。
//   - 会话生存期独立于订阅者:WS 全断,shell 照跑,缓冲继续滚。
//
// 线程模型:所有公开方法加锁(mu_);PtyProcess 的 on_data/on_exit 从读线程
// 进来,同样走锁。sender 回调(Crow conn->send_binary)在锁内调用 — 项目已有
// EventDispatcher 从 worker 线程推 WS 的先例,Crow 跨线程 send 安全。
// 不依赖 Crow 头:订阅者以 void* key + sender 函数注入,单测可 mock。

#include "pty_backend.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace acecode {

// 缓冲与帧常量(对齐 opencode:BUFFER_LIMIT 2MB / BUFFER_CHUNK 64KB)。
inline constexpr std::size_t kPtyBufferLimit = 2 * 1024 * 1024;
inline constexpr std::size_t kPtyBufferChunk = 64 * 1024;
inline constexpr int kPtyMaxSessions = 16;

// 0x00 前缀控制帧编码(纯函数,单测覆盖)。
std::string encode_pty_control_frame(const std::string& json_payload);

struct PtySessionInfo {
    std::string id;
    std::string title;
    std::string shell;
    std::string cwd;
    std::string status;  // "running" | "exited"
    int pid = 0;
    int exit_code = 0;   // status=="exited" 时有效
    PtyBackendKind backend = PtyBackendKind::Pipe;
};

class PtySessionRegistry {
public:
    // backend: 启动期 detect_pty_backend() 的结果。default_cwd: daemon cwd。
    // configured_shell: config console.shell(可空,经 resolve_console_shell)。
    PtySessionRegistry(PtyBackendKind backend, std::string default_cwd,
                       std::string configured_shell);
    ~PtySessionRegistry();

    PtySessionRegistry(const PtySessionRegistry&) = delete;
    PtySessionRegistry& operator=(const PtySessionRegistry&) = delete;

    // 创建会话并 spawn shell。失败返回 nullopt 并写 error;会话数达上限
    // (kPtyMaxSessions)时同样失败(error 含 "limit")。
    // shell_override 非空时用它(已解析的完整命令行,见 resolve_shell_command_by_id),
    // 空则用注册表默认 shell_。
    std::optional<PtySessionInfo> create(const std::string& cwd_override,
                                         const std::string& title,
                                         const std::string& shell_override,
                                         std::string& error);

    std::vector<PtySessionInfo> list() const;
    std::optional<PtySessionInfo> get(const std::string& id) const;

    // kill 进程 + 断开订阅者 + 移除会话。不存在返回 false。
    bool remove(const std::string& id);

    bool resize(const std::string& id, int cols, int rows);

    // 改会话标题(终端内程序经 OSC 0/2 设置标题,前端 xterm onTitleChange
    // 同步回来,刷新恢复会话时标题不丢)。UTF-8 安全截断到 200 字节;
    // 空白标题忽略(返回 true)。会话不存在返回 false。
    bool set_title(const std::string& id, const std::string& title);

    // WS 入站字节直写 PTY(运行中才写)。
    void write_input(const std::string& id, const std::string& data);

    // 订阅:cursor >= 0 从该游标补发缓冲;cursor < 0 只要新输出。补发后
    // 发 {"cursor": 当前} 控制帧。已退出的会话补发后立刻再发 exit 控制帧。
    // 会话不存在返回 false(调用方应关闭连接)。
    bool connect(const std::string& id, const void* subscriber,
                 std::int64_t cursor,
                 std::function<void(const std::string&)> sender);

    void disconnect(const std::string& id, const void* subscriber);

    // daemon 退出:杀掉全部会话(spec: stop_all)。
    void stop_all();

    PtyBackendKind backend() const { return backend_; }
    std::string shell() const { return shell_; }

private:
    struct Session {
        PtySessionInfo info;
        std::unique_ptr<PtyProcess> process;
        std::string buffer;                 // 滚动缓冲(最旧字节被丢弃)
        std::uint64_t buffer_start = 0;     // buffer[0] 对应的全局游标
        std::uint64_t cursor = 0;           // 累计输出字节
        std::map<const void*, std::function<void(const std::string&)>> subscribers;
    };

    void on_pty_data(const std::string& id, const std::string& data);
    void on_pty_exit(const std::string& id, int exit_code);

    PtyBackendKind backend_;
    std::string default_cwd_;
    std::string shell_;
    mutable std::mutex mu_;
    std::map<std::string, std::unique_ptr<Session>> sessions_;
    std::uint64_t next_id_ = 1;
};

} // namespace acecode
