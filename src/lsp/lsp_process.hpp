#pragma once

// LSP server 子进程:spawn(stdio 管道接管 stdin/stdout,stderr 丢弃)、
// 阻塞读 stdout、带锁写 stdin、优雅/强制停止。架构与
// provider/codex/codex_app_server_client 的进程段同源,差异:
// - argv 向量 + cwd + 追加环境变量(server 定义与 config 都可注入 env)
// - Windows 上 argv[0] 为 .cmd/.bat 时自动经 cmd.exe /d /c 执行
//   (CreateProcess 不能直接执行批处理;npm 全局命令都是 .cmd shim)

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace acecode::lsp {

struct LspSpawnOptions {
    // argv[0] = 可执行文件(建议为 which() 解析出的完整路径)。
    std::vector<std::string> argv;
    std::string cwd; // UTF-8;空 = 继承父进程 cwd
    std::vector<std::pair<std::string, std::string>> extra_env;
};

// Windows 命令行参数引用规则(CommandLineToArgvW 逆操作)。
// 暴露为自由函数以便单测。POSIX 上不使用。
std::string quote_windows_arg(const std::string& arg);

class LspProcess {
public:
    LspProcess() = default;
    ~LspProcess();
    LspProcess(const LspProcess&) = delete;
    LspProcess& operator=(const LspProcess&) = delete;

    bool start(const LspSpawnOptions& opts, std::string* error);

    // 阻塞读 stdout。>0 = 读到的字节数;0 = EOF(子进程退出/管道关闭);
    // <0 = 读错误。只允许单一 reader 线程调用。
    long read_stdout(char* buf, std::size_t len);

    bool write_stdin(const char* data, std::size_t len, std::string* error);
    // 关闭 stdin 写端(LSP exit 之后让 server 观察到 EOF)。
    void close_stdin();

    bool started() const { return started_; }
    // 等待子进程退出,超时返回 false。
    bool wait_exit(int timeout_ms);
    // 强杀 + 关闭全部句柄。可重复调用。
    void terminate();

private:
#ifdef _WIN32
    void* process_handle_ = nullptr;
    void* stdin_write_ = nullptr;
    void* stdout_read_ = nullptr;
#else
    int process_id_ = -1;
    int stdin_write_ = -1;
    int stdout_read_ = -1;
#endif
    bool started_ = false;
};

} // namespace acecode::lsp
