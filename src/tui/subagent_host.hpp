#pragma once

// SubagentHost — TUI 进程内的子代理宿主。
//
// daemon 的 spawn_subagent 依赖 SessionRegistry / LocalSessionClient,而这
// 两者本身不依赖任何 web 层代码,TUI 进程可以直接实例化。本类把它们与
// TUI 需要的桥接逻辑收拢到一处:
//   - on_spawned(工具的 on_spawn 回调):登记「运行中任务」并订阅子会话
//     事件流(busy / title / permission_request)。
//   - 运行中任务快照经 Deps::publish_tasks 交付(main.cpp 把它写进
//     TuiState.subagent_tasks 供右侧 sidebar 渲染)。按用户决策,右侧列
//     **只显示运行中**任务:busy=false 即从快照移除。
//   - 子会话的 permission_request 经 Deps::on_permission_request 冒泡到
//     TUI 主确认 UI(排队占用 confirm overlay);回答走 respond_permission。
//     AskUserQuestion 不走事件桥接 —— TUI 与子会话共享 ToolExecutor,子会
//     话执行的本来就是 TUI 版工具(直连 ask overlay,内部排队)。
//   - /tasks 命令的 list / abort / clear 后端。
//
// 线程模型:事件回调在子会话的 AgentLoop worker 线程触发;内部状态由
// mu_ 保护;所有对外交付都经 Deps 回调(由 main.cpp 负责 lock TuiState +
// PostEvent)。本文件不依赖 FTXUI,编译进 acecode_testable 供单测。

#include "../session/local_session_client.hpp"
#include "../session/session_registry.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace acecode::tui {

struct SubagentTaskSnapshot {
    std::string id;
    std::string title;   // auto-title;为空时 UI 退到 prompt 摘要
    std::string prompt;  // spawn 时的原始 prompt(截断展示用)
    std::chrono::steady_clock::time_point started{};
};

class SubagentHost {
public:
    struct Deps {
        SessionRegistryDeps registry_deps;
        // 当前主会话 id(lazy 创建,所以是 provider 而不是快照)。
        std::function<std::string()> parent_session_id;
        // 运行中任务快照变化(增/删/标题更新)。回调方负责线程安全与重绘。
        std::function<void(std::vector<SubagentTaskSnapshot>)> publish_tasks;
        // 子会话权限请求冒泡:payload 含 request_id/tool/args。
        std::function<void(const std::string& session_id,
                           const std::string& task_title,
                           nlohmann::json payload)> on_permission_request;
    };

    explicit SubagentHost(Deps deps);

    SessionRegistry& registry() { return registry_; }
    LocalSessionClient& client() { return client_; }

    // spawn_subagent 工具的 on_spawn 回调实现。
    void on_spawned(const std::string& child_id, const std::string& prompt);

    std::vector<SubagentTaskSnapshot> running_tasks() const;

    // /tasks 支持 ------------------------------------------------------

    struct TaskListEntry {
        std::string id;
        std::string title;
        bool running = false;
        int message_count = 0;
    };
    // 运行中(registry)+ 已结束(磁盘 meta parent 匹配)合并清单。
    std::vector<TaskListEntry> list_tasks(const std::string& project_dir) const;

    // 中止运行中的子会话(loop abort;不销毁)。返回 false = 未找到。
    bool abort_task(const std::string& id);

    // 永久删除全部已结束的子任务(destroy + 删磁盘)。返回删除数。
    int clear_settled(const std::string& project_dir);

    // 把 confirm overlay 的用户选择路由回子会话。choice 取
    // "allow" / "allow_session" / "deny"。
    void respond_permission(const std::string& session_id,
                            const std::string& request_id,
                            const std::string& choice);

private:
    void publish_locked();
    void remove_task(const std::string& id);
    void update_title(const std::string& id, const std::string& title);
    std::string title_for(const std::string& id) const;

    SessionRegistry registry_;
    LocalSessionClient client_;
    Deps deps_;

    mutable std::mutex mu_;
    std::vector<SubagentTaskSnapshot> running_;
};

} // namespace acecode::tui
