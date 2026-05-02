#pragma once

// daemon CLI 子命令分发: acecode daemon {start|stop|status|--foreground}
// 负责 argv 解析、与 worker.cpp 交互、维护 ~/.acecode/run/ 文件状态。
//
// 接口设计上,run() 接收一个 argv 切片(去掉 "daemon" 这个前缀字)和当前
// 进程的可执行路径(用于 spawn 自身),返回 ExitCode。main.cpp 只负责派发。

#include <string>
#include <vector>

namespace acecode::daemon::cli {

struct Args {
    // 解析后的子命令: "start" / "stop" / "status" / "foreground" / ""(无参 = 显示帮助)
    std::string sub;
    bool dangerous = false;     // 透传给 worker
    bool supervised = false;    // --supervised 标记(launcher 派 worker 用)
    std::string guid;           // --guid=<G>(配合 --supervised)
    int port_override = 0;      // --port=<N> 覆盖 cfg.web.port,0 = 不覆盖。desktop 子进程用。
    std::string token_override; // --token=<T> 覆盖自动生成的 token。desktop 子进程用,让父进程预知 token。
    std::string static_dir_override; // --static-dir=<path> 覆盖 cfg.web.static_dir。desktop dev 模式注入仓库 web/ 目录。
    std::string cwd_override;   // --cwd=<path> 覆盖 daemon 工作目录,用于从任意目录调试指定 workspace。
    std::string run_dir_override; // --run-dir=<path> 把 daemon runtime files (heartbeat/pid/port/token/GUID 锁) 切到非默认目录。desktop 多 workspace 用 ~/.acecode/projects/<hash>/run/。
    std::string error;          // 解析错误,非空时打印 + 非零退出
};

Args parse(const std::vector<std::string>& tokens);

// 主入口。tokens 是 daemon 子命令之后的参数(不含 argv[0] 也不含 "daemon")。
// exe_path 用于 spawn_detached 时找到自身;留空则使用 current_executable_path。
int run(const std::vector<std::string>& tokens,
        const std::string& exe_path = {});

} // namespace acecode::daemon::cli
