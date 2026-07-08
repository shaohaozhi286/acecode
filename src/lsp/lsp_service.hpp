#pragma once

// LspService:server 定义 → (server_id, root) 客户端池的运行时。
// - 惰性 spawn:首次有文件命中某 server+root 才启动进程;
//   同 key 并发触发经 per-key slot 锁天然单飞。
// - broken 集合:spawn/握手失败的 key 本进程内不再重试。
// - 查询/诊断 API 对匹配的多个 client 聚合,单 client 失败静默吞。
// - workspace 边界按调用传入的会话 cwd(session_cwd)判定,空则回退
//   进程级 init cwd —— daemon 单进程服务多 workspace 会话(routes_workspaces)
//   后,进程 cwd 不再等于会话 cwd。路径与边界统一 weakly_canonical 归一,
//   消掉 junction/symlink 形态差异(C:\Users\x 是 N:\Users\x 的 junction 时,
//   canonical 前后前缀比较会假性不匹配)。
// 生命周期:daemon worker / TUI main 退出路径调 shutdown_all()。

#include "lsp_client.hpp"
#include "lsp_server_registry.hpp"
#include "../config/config.hpp"

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace acecode::lsp {

class LspService {
public:
    LspService(const LspConfig& cfg, std::string workspace_cwd);
    ~LspService();
    LspService(const LspService&) = delete;
    LspService& operator=(const LspService&) = delete;

    bool enabled() const { return enabled_; }
    const std::string& workspace_cwd() const { return workspace_cwd_; }

    // 该文件是否有可用(已连接或可启动)的 server。不触发 spawn。
    // session_cwd = 会话工作目录(workspace 边界);空串回退进程级 init cwd。
    bool has_server_for(const std::string& utf8_path,
                        const std::string& session_cwd);

    // 编辑落盘后的诊断收集:确保 client 就绪 → touch → 等 document 诊断
    // (全部 client 共享一个总超时)→ 返回该文件合并诊断。任何一步不可用
    // 都静默降级为空结果。abort 探针以 ≤50ms 粒度生效于等待阶段。
    std::vector<nlohmann::json> collect_diagnostics_after_write(
        const std::string& utf8_path,
        std::chrono::milliseconds wait_timeout,
        const AbortProbe& should_abort,
        const std::string& session_cwd);

    // lsp 工具的位置类查询:对每个匹配 client 先 touch + 短等诊断(让
    // server 加载文件),再发请求;所有非空结果按 client 依次聚合返回。
    std::vector<nlohmann::json> request_for_file(const std::string& utf8_path,
                                                 const std::string& method,
                                                 const nlohmann::json& params,
                                                 std::chrono::milliseconds timeout,
                                                 const AbortProbe& should_abort,
                                                 const std::string& session_cwd);

    // workspace/symbol 类:对所有已连接 client 广播(不触发新 spawn)。
    std::vector<nlohmann::json> request_all_connected(const std::string& method,
                                                      const nlohmann::json& params,
                                                      std::chrono::milliseconds timeout);

    // 调用层级两段式:同一 client 上先 prepareCallHierarchy 取第一个
    // item,再发 incoming/outgoing 请求(direction = LSP 方法名)。
    std::vector<nlohmann::json> call_hierarchy_for_file(const std::string& utf8_path,
                                                        const nlohmann::json& prepare_params,
                                                        const std::string& direction_method,
                                                        std::chrono::milliseconds timeout,
                                                        const AbortProbe& should_abort,
                                                        const std::string& session_cwd);

    struct StatusEntry {
        std::string server_id;
        std::string root;   // workspace 相对(展示用);root == workspace 时为 "."
        int open_files = 0;
    };
    struct Status {
        bool enabled = false;
        std::vector<StatusEntry> connected;
        std::vector<StatusEntry> broken;
        // 内置定义中可执行文件探测不到的(展示「未安装」提示)。
        std::vector<std::string> not_installed;
    };
    // 廉价快照:只读 slots(锁 + 小拷贝),TUI 侧边栏每帧调用安全。
    std::vector<StatusEntry> connected_snapshot();
    // 完整快照:额外做 which() 可执行探测(文件系统访问),仅 /lsp 命令用。
    Status status_snapshot();

    // 逐 client 协议级退出;kill 兜底。幂等。
    void shutdown_all();

private:
    struct Slot {
        std::mutex mu; // 串行化该 key 的 spawn / 复用判定(单飞)
        std::shared_ptr<LspClient> client;
        bool broken = false;
        std::string server_id;
        std::string root;
    };

    // 解析后的调用上下文:workspace 边界 + 文件绝对路径,均已 canonical 化。
    struct ResolvedPath {
        std::string workspace;
        std::string abs;
    };
    // session_cwd 空 → 进程级 workspace_cwd_;两侧统一 weakly_canonical,
    // 保证 within_workspace / root 探测 / slot key / URI 落在同一路径形态。
    ResolvedPath resolve_path(const std::string& utf8_path,
                              const std::string& session_cwd) const;

    // 匹配 + root 探测 + 惰性 spawn。返回可用 client 集合。
    std::vector<std::shared_ptr<LspClient>> clients_for(const ResolvedPath& rp);
    std::shared_ptr<Slot> slot_for(const std::string& key,
                                   const std::string& server_id,
                                   const std::string& root);

    bool enabled_ = false;
    std::string workspace_cwd_;
    std::vector<LspServerDef> defs_;

    std::mutex slots_mu_;
    std::map<std::string, std::shared_ptr<Slot>> slots_;
    std::atomic<bool> shutting_down_{false};
};

// ---- 进程级单例(与 web_search::runtime 同套路) ----

// 重复 init → LOG_WARN 并保留第一次。enabled=false 时也创建 service
// (让 /lsp 能报告禁用状态),但一切入口都零行为。
void init(const LspConfig& cfg, const std::string& workspace_cwd);
void shutdown();
bool is_initialized();
LspService& service();

// 测试注入(替换/清空单例)。
void set_service_for_test(std::unique_ptr<LspService> service);

} // namespace acecode::lsp
