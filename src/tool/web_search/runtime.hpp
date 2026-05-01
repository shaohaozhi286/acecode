#pragma once

// 联网搜索的进程级单例 — 持有 BackendRouter + RegionDetector,供 /websearch
// 命令、工具注册、daemon worker 共用。与 network::proxy_resolver() 同套路。
//
// 启动序列(main.cpp / daemon worker):
//   1. acecode::web_search::init(cfg.web_search)   // 一次,构造 router + detector
//   2. 异步线程里 runtime().detector().get_or_detect() + runtime().router().resolve_active(...)
//   3. 把 runtime().router() 传给 create_web_search_tool() 注册到 ToolExecutor
//
// 测试通过 set_runtime_for_test 注入隔离的 runtime;不调用 init 也安全(全部
// 接口都允许 nullptr)。

#include "config/config.hpp"

namespace acecode::web_search {

class BackendRouter;
class RegionDetector;

class Runtime {
public:
    Runtime(const WebSearchConfig& cfg);
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    BackendRouter& router();
    RegionDetector& detector();
    const WebSearchConfig& cfg() const;

private:
    struct Impl;
    Impl* impl_;
};

// 进程级初始化。重复调 → LOG_WARN 并保留第一次的。enabled=false 时仍然会
// 创建 runtime(让 /websearch status 能报告状态),但 runtime().router() 拿到的
// router 不会被 register_default_backends 注入任何 backend。
void init(const WebSearchConfig& cfg);

// 关闭 runtime 单例;主要给测试 / 干净 shutdown 用。生产代码进程退出时不必调。
void shutdown();

// 是否已 init。/websearch 命令在未初始化时显式提示而不是 crash。
bool is_initialized();

// 取单例。未初始化时 abort(配合 is_initialized() 检查)。
Runtime& runtime();

} // namespace acecode::web_search
