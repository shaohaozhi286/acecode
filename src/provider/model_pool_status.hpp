#pragma once

// 模型池负载监控(model-pool load monitor)。
//
// 背景:wizard-ai code_pilot 的模型池负载查询接口
//   GET https://wizard-ai.paic.com.cn/code_pilot/api/monitor/getModelPoolStatus
// 返回每个池的 modelPoolName、实时 usageRate(0..100 负载百分比)和
// maxWindowTokens(池窗口)。配置的 model id 与 modelPoolName 精确相等即视为池成员。
// 本服务每 30s 轮询一次,缓存结果供 TUI / daemon / web 展示负载,并把
// 0.8 * maxWindowTokens 作为该模型的有效上下文窗口(驱动占用% 与自动压缩)。
//
// 接口内网直连、无需认证;URL 写死(见 kModelPoolStatusUrl)。抓取走
// proxy_resolver 以正确处理 NO_PROXY。FetchFn 可注入,单测用 JSON mock,不打真网络。

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

namespace acecode {

// 写死的模型池负载接口(内网直连,无需认证)。
inline constexpr const char* kModelPoolStatusUrl =
    "https://wizard-ai.paic.com.cn/code_pilot/api/monitor/getModelPoolStatus";

// 单个模型池的状态快照。
struct ModelPoolStatus {
    int usage_rate = -1;              // 负载百分比 0..100;-1 = 未知
    long long max_window_tokens = 0;  // 池上报的最大窗口 token;0 = 未知
};

// 负载色阶,与 UI 染色一一对应。
enum class ModelLoadTier { Unknown, Green, Yellow, Red };

// --- 纯函数(无副作用,单测直接覆盖)---------------------------------------

// 解析接口返回的 JSON body,返回 modelPoolName -> ModelPoolStatus。
// 容错:body 非法 JSON / 不是对象 / 缺 data / data 非数组 / 单项字段类型不符
// 都安全跳过,绝不抛异常。取的是每项顶层的 maxWindowTokens(150000 那个),
// 不是 generateParam 里的嵌套值。
std::unordered_map<std::string, ModelPoolStatus>
parse_model_pool_status(const std::string& body);

// 负载 -> 色阶。规则(用户定义):
//   usage < 70        → 绿
//   70 <= usage <= 90 → 黄
//   usage > 90        → 红
//   usage < 0(未知)  → Unknown
ModelLoadTier model_load_tier(int usage_rate);

// 有效上下文窗口 = round(0.8 * max_window_tokens)。max<=0 → 0。
// 例:150000 → 120000。
int effective_context_window(long long max_window_tokens);

// --- 注入式抓取(单测 mock 用)-------------------------------------------

struct ModelPoolFetchResult {
    long status_code = 0;
    std::string body;
    std::string error;  // status_code==0 时的传输层错误
};
using ModelPoolFetchFn = std::function<ModelPoolFetchResult(const std::string& url)>;

// 默认抓取:cpr GET,走 proxy_resolver,8s 超时。
ModelPoolFetchResult default_model_pool_fetch(const std::string& url);

// --- 服务 ---------------------------------------------------------------

class ModelPoolStatusService {
public:
    static constexpr std::chrono::seconds kPollInterval{30};

    // fetch 为空 → 用 default_model_pool_fetch;url 为空 → kModelPoolStatusUrl。
    explicit ModelPoolStatusService(ModelPoolFetchFn fetch = nullptr,
                                    std::string url = {});
    ~ModelPoolStatusService();

    ModelPoolStatusService(const ModelPoolStatusService&) = delete;
    ModelPoolStatusService& operator=(const ModelPoolStatusService&) = delete;

    // 启动 30s 轮询后台线程(幂等;重复调用 no-op)。on_update 在每次成功刷新后
    // 回调一次(用于回灌上下文窗口 / 触发 UI 重绘),可空。回调在后台线程执行,
    // 调用方需自行保证线程安全(TUI 应 post 到事件队列)。
    void start(std::function<void()> on_update = nullptr);
    void stop();

    // 同步抓一次并更新缓存。返回 true = 解析到至少一个池。供 start 循环与单测用。
    bool refresh_once();

    // 线程安全读。
    std::unordered_map<std::string, ModelPoolStatus> snapshot() const;
    std::optional<ModelPoolStatus> get(const std::string& model_pool_name) const;

    // 某模型的有效上下文窗口:缓存命中且 max>0 → 0.8x;否则 0(调用方回退默认)。
    int effective_context_window_for(const std::string& model) const;

private:
    void run_loop(std::function<void()> on_update);

    ModelPoolFetchFn fetch_;
    std::string url_;

    mutable std::mutex mu_;  // 保护 cache_
    std::unordered_map<std::string, ModelPoolStatus> cache_;

    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex cv_mu_;
    std::condition_variable cv_;
};

// 进程级单例(Meyers singleton,仿 proxy_resolver())。TUI / daemon 启动时按需
// start(),退出前 stop();session_registry / web server / TUI render / context
// 窗口解析都通过它读负载与有效窗口,免去逐层穿构造函数。
ModelPoolStatusService& model_pool_status_service();

} // namespace acecode
