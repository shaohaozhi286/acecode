#pragma once

// 单个 LSP server 连接:进程 + Content-Length 帧 JSON-RPC + 文件同步 +
// 诊断缓存(push/pull 双通道)+ document 级诊断等待。
// 线程模型:一条 reader 线程解析 server 输出;请求方线程经 pending map +
// condvar 等应答;server→client 请求在 reader 线程内同步应答(全部是
// 轻量纯计算)。所有可变状态由 state_mu_ 保护;stdin 写由 write_mu_ 串行。

#include "lsp_process.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace acecode::lsp {

// 外部中断探针:返回 true = 立即放弃当前等待(用户 abort / 进程退出)。
using AbortProbe = std::function<bool()>;

class LspClient {
public:
    struct CreateOptions {
        std::string server_id;
        std::string root;                // UTF-8 绝对路径
        LspSpawnOptions spawn;
        nlohmann::json initialization;   // initializationOptions;可为 null
        // initialize 握手超时。默认对齐 opencode(45s,兼容慢启动 server)。
        std::chrono::milliseconds initialize_timeout{45000};
    };

    // 启动进程并完成 initialize 握手。任一步失败返回 nullptr(进程已清理)。
    static std::unique_ptr<LspClient> create(CreateOptions options, std::string* error);

    ~LspClient();
    LspClient(const LspClient&) = delete;
    LspClient& operator=(const LspClient&) = delete;

    const std::string& server_id() const { return server_id_; }
    const std::string& root() const { return root_; }
    bool alive() const { return running_.load(); }
    int open_file_count() const;

    // 文件同步:未打开 → didOpen(读磁盘全文);已打开 → didChange(全量)。
    // 返回该文件的新 version;读文件失败/进程已死返回 nullopt。
    // 同一 client 上的 touch 全序串行(内部锁),对应多 session 并发编辑。
    std::optional<std::int64_t> touch_file(const std::string& utf8_path);

    // 该文件当前诊断(push + pull 合并去重)。key 规范化在内部完成。
    std::vector<nlohmann::json> diagnostics_for(const std::string& utf8_path) const;
    // 所有文件的诊断快照(key = 规范化路径)。
    std::map<std::string, std::vector<nlohmann::json>> all_diagnostics() const;

    // document 模式诊断等待:直到 (a) 收到匹配 version/时间戳的 push 且
    // 150ms 内无更新(debounce),或 (b) pull 通道返回了该文件的报告,
    // 或 (c) 超时/abort。不抛错,尽力而为。
    void wait_for_diagnostics(const std::string& utf8_path,
                              std::int64_t version,
                              std::chrono::milliseconds timeout,
                              const AbortProbe& should_abort);

    // 通用请求(lsp 工具的九类查询走这里)。错误/超时返回 nullopt。
    std::optional<nlohmann::json> request(const std::string& method,
                                          const nlohmann::json& params,
                                          std::chrono::milliseconds timeout);
    bool notify(const std::string& method, const nlohmann::json& params);

    // 优雅退出:shutdown 请求(短超时)→ exit 通知 → 关 stdin → 等进程,
    // 超时强杀;随后 join reader。可重复调用。
    void shutdown();

private:
    LspClient() = default;

    struct PendingResponse {
        bool done = false;
        nlohmann::json result;
        nlohmann::json error;
    };

    struct OpenFile {
        std::int64_t version = 0;
        std::string text; // didChange 的 incremental 全量替换需要旧文本算 end 位置
    };

    struct PublishStamp {
        std::chrono::steady_clock::time_point at;
        std::optional<std::int64_t> version;
    };

    bool initialize_handshake(std::string* error);
    void read_loop();
    void handle_message(nlohmann::json&& message);
    void handle_server_request(const nlohmann::json& message);
    bool write_frame(const nlohmann::json& message, std::string* error);
    void fail_all_pending(const std::string& reason);
    // pull 一次诊断(textDocument/diagnostic,3s 超时)。
    // 返回 true = server 处理了请求且报告覆盖了该文件。
    bool pull_diagnostics_once(const std::string& utf8_path);
    bool pull_supported() const;

    std::string server_id_;
    std::string root_;
    nlohmann::json initialization_;

    LspProcess process_;
    std::thread reader_;
    std::atomic<bool> running_{false};
    std::atomic<std::int64_t> next_id_{1};

    std::mutex write_mu_;
    std::mutex touch_mu_;

    mutable std::mutex state_mu_;
    std::condition_variable responses_cv_;
    std::condition_variable diag_cv_;
    std::map<std::int64_t, PendingResponse> responses_;
    std::map<std::string, OpenFile> files_;                       // key = 规范化路径
    std::map<std::string, std::vector<nlohmann::json>> push_diags_; // key = 规范化路径
    std::map<std::string, std::vector<nlohmann::json>> pull_diags_;
    std::map<std::string, PublishStamp> published_;
    std::set<std::string> pull_identifiers_;   // 动态注册的 diagnostic identifier
    bool static_pull_provider_ = false;        // initialize 应答声明 diagnosticProvider
    bool dynamic_pull_registered_ = false;
    int sync_kind_ = 1;                        // 1=Full 2=Incremental
    bool shutdown_done_ = false;
};

} // namespace acecode::lsp
