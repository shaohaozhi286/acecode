#pragma once

// SessionRegistry: daemon 进程内多 session 的注册表(openspec add-web-daemon
// 任务 7.2)。TUI 模式不用这个 — TUI 直接持有一个 AgentLoop。
//
// 每个 session 是一个 SessionEntry,内部持有:
//   - AgentLoop(独立 worker thread)
//   - SessionManager(独立 jsonl + meta,带本进程 pid 后缀)
//   - PermissionManager(独立 session_allowed_,mode/rules 从全局复制)
//   - AsyncPrompter(注入到 AgentLoop)
//
// 共享(整个 daemon 一份):
//   - ProviderAccessor (provider 实例 + 锁,daemon 启动时建立)
//   - ToolExecutor (工具注册表 + MCP)
//   - 全局 cwd
//   - SkillRegistry / MemoryRegistry / 各种 *Config

#include "../agent_loop.hpp"
#include "../config/config.hpp"
#include "../permissions.hpp"
#include "../provider/llm_provider.hpp"
#include "../tool/tool_executor.hpp"
#include "permission_prompter.hpp"
#include "session_client.hpp"
#include "session_manager.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace acecode {

class SkillRegistry;
class MemoryRegistry;

// SessionEntry: 一个 session 的所有 per-session 状态。所有指针成员都是
// unique_ptr,SessionEntry 析构时按相反顺序销毁(AgentLoop 先 shutdown
// worker thread,再销毁 prompter,再销毁 SessionManager)。
struct SessionEntry {
    std::string id;
    std::unique_ptr<SessionManager>     sm;
    std::unique_ptr<PermissionManager>  perm;
    std::unique_ptr<AgentLoop>           loop;
    AsyncPrompter*                       prompter = nullptr; // owned by loop after move
};

// SessionRegistryDeps: 构造 SessionRegistry 时一次性传入的"全局共享物"。
// 通过引用持有,生命周期由调用方保证(典型: daemon worker.cpp)。
struct SessionRegistryDeps {
    AgentLoop::ProviderAccessor      provider_accessor;
    ToolExecutor*                    tools = nullptr;
    std::string                      cwd;
    const AppConfig*                 config = nullptr;
    const SkillRegistry*             skill_registry = nullptr;
    const MemoryRegistry*            memory_registry = nullptr;
    const MemoryConfig*              memory_cfg = nullptr;
    const ProjectInstructionsConfig* project_instructions_cfg = nullptr;
    // 全局 PermissionManager(用于派生 per-session perm 的 mode + rules
    // 起始值)。每个 session 自己的 PermissionManager 是独立实例,session_allowed_
    // 不串。
    const PermissionManager*         template_permissions = nullptr;
};

class SessionRegistry {
public:
    explicit SessionRegistry(SessionRegistryDeps deps);
    ~SessionRegistry();

    SessionRegistry(const SessionRegistry&) = delete;
    SessionRegistry& operator=(const SessionRegistry&) = delete;

    // 创建新 session。返回 session_id(SessionManager 实际生成时机是 lazy,
    // 第一条消息时落盘;为了 HTTP 立刻能 subscribe,这里**主动**触发 ensure
    // session_id 的生成 —— 通过给 SessionManager 注入一条空 user 消息?不,
    // 改用 SessionManager::start_session 之后立刻调一个新增的 ensure_id 接口)。
    // v1 简化: create_session 调 start_session;真正的 ensure 留 lazy,客户端
    // 拿到 session_id 是 SessionRegistry 自己生成的 UUID(暂作为 entry key),
    // 与 jsonl 里的 SessionStorage::generate_session_id 是两个东西。第二轮
    // 重构再统一。当前 v1 直接复用 SessionStorage::generate_session_id。
    std::string create(const SessionOptions& opts);

    // 找一个 entry。返回 nullptr 表示不存在。返回 raw 指针由调用者立刻使用,
    // **不要存储**(随时可能被 destroy)。线程安全:加锁拷贝 raw 指针,但
    // entry 自身 lifetime 不在锁内保证 — 设计上 destroy 是显式的,客户代码
    // 不应在调 destroy 时同时持指针。
    SessionEntry* lookup(const std::string& id);

    // 销毁 session。abort 当前 LLM/tool + join worker + 移出 map。
    void destroy(const std::string& id);

    // 列出当前 daemon 内活跃 session 的元数据(只看内存,不读磁盘历史)。
    std::vector<SessionInfo> list_active() const;

    // 当前活跃 session 数(测试 / 监控用)。
    std::size_t size() const;

private:
    SessionRegistryDeps                                          deps_;
    mutable std::mutex                                            mu_;
    std::unordered_map<std::string, std::unique_ptr<SessionEntry>> entries_;
};

} // namespace acecode
