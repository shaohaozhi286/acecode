// src/provider/apply_model_to_session.hpp
// per-session 模型切换的所有副作用集中在这里。daemon 的 SessionRegistry 与
// TUI 的 /model 命令都调用这个函数,确保两端语义一致。
#pragma once

#include "../config/config.hpp"
#include "../config/saved_models.hpp"
#include "../session/session_client.hpp"  // for SessionModelState
#include "../session/session_registry.hpp"  // for SessionEntry::ProviderSlot

#include <stdexcept>
#include <string>

namespace acecode {

class SessionManager;
class AgentLoop;

struct ApplyModelResult {
    SessionModelState state;
    std::string warning;  // 非致命警告(如 Copilot silent_auth 失败、写 meta 失败)
};

struct ApplyModelDeps {
    SessionEntry::ProviderSlot* provider_slot = nullptr;  // 必填
    SessionManager*             sm = nullptr;             // 选填:TUI 早期可能没有
    AgentLoop*                  loop = nullptr;           // 选填:用于 set_context_window
    AppConfig*                  cfg = nullptr;            // 必填
};

// 失败时抛 std::runtime_error,内容形如:
//   - "config unavailable"        (cfg == nullptr)
//   - "provider slot unavailable" (provider_slot == nullptr)
//   - "provider create failed: <原因>"  (create_provider_from_entry 返回 null)
ApplyModelResult apply_model_to_session(const ModelProfile& profile,
                                         const ApplyModelDeps& deps);

} // namespace acecode
