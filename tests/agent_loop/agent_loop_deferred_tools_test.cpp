#include <gtest/gtest.h>

#include "agent_loop.hpp"
#include "permissions.hpp"
#include "tool/tool_executor.hpp"

#include <memory>
#include <unordered_set>

namespace {

acecode::ToolImpl deferred_control_tool() {
    acecode::ToolImpl tool;
    tool.definition.name = "session_control";
    tool.definition.description = "test deferred session control";
    tool.definition.parameters = nlohmann::json::object();
    tool.defer_loading = true;
    tool.execute = [](const std::string&, const acecode::ToolContext&) {
        return acecode::ToolResult{"ok", true};
    };
    return tool;
}

bool contains(const std::vector<acecode::ToolDef>& definitions,
              const std::string& name) {
    for (const auto& definition : definitions) {
        if (definition.name == name) return true;
    }
    return false;
}

} // namespace

TEST(AgentLoopDeferredTools, UnlockIsIsolatedToCurrentSession) {
    acecode::ToolExecutor tools;
    ASSERT_TRUE(tools.register_tool(deferred_control_tool()));
    acecode::PermissionManager permissions_a;
    acecode::PermissionManager permissions_b;
    acecode::AgentLoop loop_a(
        []() -> std::shared_ptr<acecode::LlmProvider> { return nullptr; },
        tools, {}, "/tmp/deferred-a", permissions_a);
    acecode::AgentLoop loop_b(
        []() -> std::shared_ptr<acecode::LlmProvider> { return nullptr; },
        tools, {}, "/tmp/deferred-b", permissions_b);

    EXPECT_TRUE(loop_a.enable_deferred_tool("session_control"));
    const auto policy_a = loop_a.tool_capability_policy_snapshot();
    const auto policy_b = loop_b.tool_capability_policy_snapshot();
    EXPECT_TRUE(contains(tools.get_model_tool_definitions(&policy_a),
                         "session_control"));
    EXPECT_FALSE(contains(tools.get_model_tool_definitions(&policy_b),
                          "session_control"));
}

TEST(AgentLoopDeferredTools, UnlockCannotBypassExpertBuiltinScope) {
    acecode::ToolExecutor tools;
    ASSERT_TRUE(tools.register_tool(deferred_control_tool()));
    acecode::PermissionManager permissions;
    acecode::AgentLoop loop(
        []() -> std::shared_ptr<acecode::LlmProvider> { return nullptr; },
        tools, {}, "/tmp/deferred-scope", permissions);
    acecode::ToolCapabilityPolicy denied;
    denied.builtin_tools = std::unordered_set<std::string>{};
    loop.set_tool_capability_policy(std::move(denied));

    EXPECT_FALSE(loop.enable_deferred_tool("session_control"));
    EXPECT_FALSE(loop.enable_deferred_tool("missing"));
    const auto policy = loop.tool_capability_policy_snapshot();
    EXPECT_TRUE(policy.enabled_deferred_tools.empty());
}
