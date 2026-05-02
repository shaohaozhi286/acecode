#include "pick_active.hpp"

#include "../utils/cwd_hash.hpp"

namespace acecode::desktop {

std::string pick_active(const std::string& last_active_hash,
                        const std::string& process_cwd,
                        const WorkspaceRegistry& registry) {
    // 1. 上次活跃且仍在 registry 里
    if (!last_active_hash.empty() && registry.get(last_active_hash).has_value()) {
        return last_active_hash;
    }
    // 2. process cwd 对应的 hash 在 registry 里
    if (!process_cwd.empty()) {
        std::string h = compute_cwd_hash(process_cwd);
        if (registry.get(h).has_value()) return h;
    }
    // 3. registry 第一项(顺序由 unordered_map 决定,稳定性弱;MVP 接受)
    auto all = registry.list();
    if (!all.empty()) return all.front().hash;
    // 4. 没有任何已知 workspace
    return "";
}

} // namespace acecode::desktop
