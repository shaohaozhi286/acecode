#pragma once

// Worker 主流程: daemon 真实跑 HTTP/WebSocket server 的进程入口。
// 现阶段(Section 4+5)只做最小骨架: GUID 校验 + pid/port/guid/token 文件 +
// HeartbeatWriter 起线程 + 阻塞等终止信号。Section 9 把 Crow server 接进来
// 后,run_worker() 会替换成"启动 Crow + 阻塞 app.run()"。
//
// CLI 三种入口都最终走 run_worker():
//   acecode daemon --foreground            → standalone 模式
//   acecode daemon start (再起子进程)      → standalone 模式(detached)
//   acecode service install + SCM 启动     → supervised 模式(launcher 派生)
//
// supervised 模式: launcher 通过 --supervised --guid=<G> 派 GUID 进来。
// standalone 模式: 自己生成 GUID。两种模式都会校验"是否已有别的 daemon 在跑"。

#include "../config/config.hpp"

#include <string>

namespace acecode::daemon {

struct WorkerOptions {
    bool        supervised = false; // true = launcher 派生; false = 独立运行
    std::string guid;               // supervised 时必填,standalone 时留空(自动生成)
    bool        foreground = false; // 仅影响日志(stderr 同时输出)。spec 12.3
    bool        dangerous  = false; // 透传 -dangerous,worker 启动期校验
};

// 主入口。返回进程退出码: 0 成功正常退出,非 0 启动失败 / 异常。
// 阻塞直到收到 SIGTERM/SIGINT(POSIX)或 CTRL_BREAK_EVENT(Windows)。
int run_worker(const WorkerOptions& opts, const AppConfig& cfg);

// 校验"是否可以以新 worker 身份启动":
//   - supervised: opts.guid 必须与 ~/.acecode/run/daemon.guid 一致(若文件存在)
//   - standalone: 若 daemon.guid + daemon.pid 都存在且对应进程仍存活 → 拒启
//
// 返回空字符串 = 通过;否则返回人类可读的拒启理由(调用方应打印 + 非零退出)。
std::string validate_can_start(const WorkerOptions& opts);

} // namespace acecode::daemon
