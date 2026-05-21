// tests/provider/apply_model_to_session_test.cpp
//
// 覆盖 src/provider/apply_model_to_session.cpp。两条调用方(daemon 的
// SessionRegistry::switch_model 与 TUI 的 /model 命令)都靠这一份逻辑,
// 任一分支退化都会让 per-session 切换语义破裂。
//
// 触发场景 / 期望:
//   - cfg 缺失 → 抛 runtime_error,slot.provider 不动(错误前置防止半改)
//   - slot 缺失 → 抛 runtime_error
//   - 正常切换:slot.provider 替换 + state.context_window > 0 + sm 调用
//   - sm/loop 任一为 null:不崩(覆盖 TUI 启动早期场景)

#include <gtest/gtest.h>

#include "provider/apply_model_to_session.hpp"

#include "config/config.hpp"
#include "config/saved_models.hpp"
#include "session/session_registry.hpp"

using acecode::AppConfig;
using acecode::ApplyModelDeps;
using acecode::apply_model_to_session;
using acecode::ModelProfile;
using acecode::SessionEntry;

namespace {

// 构造一个 copilot cfg。create_provider_from_entry 对 copilot 不需要
// 网络访问,适合做单测的 happy path。
AppConfig make_copilot_cfg() {
    AppConfig cfg;
    cfg.provider = "copilot";
    cfg.copilot.model = "gpt-4o";
    cfg.context_window = 128000;
    return cfg;
}

ModelProfile make_copilot_profile(const std::string& model = "gpt-4o-mini") {
    ModelProfile p;
    p.name = "copilot-mini";
    p.provider = "copilot";
    p.model = model;
    return p;
}

} // namespace

// 场景:cfg 为 nullptr → 立刻抛,slot 不被触碰。
// 这是错误前置原则 — caller 给的 deps 残缺时不能让 slot 进入半成功状态。
TEST(ApplyModelToSession, ThrowsWhenCfgMissing) {
    SessionEntry::ProviderSlot slot;
    auto profile = make_copilot_profile();
    ApplyModelDeps deps;
    deps.provider_slot = &slot;
    deps.cfg = nullptr;
    EXPECT_THROW(apply_model_to_session(profile, deps), std::runtime_error);
    EXPECT_FALSE(slot.provider);  // 仍未设置
}

// 场景:slot 为 nullptr → 抛。caller 必须先把 slot 准备好。
TEST(ApplyModelToSession, ThrowsWhenSlotMissing) {
    auto cfg = make_copilot_cfg();
    auto profile = make_copilot_profile();
    ApplyModelDeps deps;
    deps.cfg = &cfg;
    deps.provider_slot = nullptr;
    EXPECT_THROW(apply_model_to_session(profile, deps), std::runtime_error);
}

// 场景:正常切换 copilot 模型 → slot.provider 被设;state 字段填好;无 warning。
// 触发:WebUI / TUI 用户切到一个 copilot 预设。
TEST(ApplyModelToSession, SwapsProviderAndPopulatesState) {
    auto cfg = make_copilot_cfg();
    SessionEntry::ProviderSlot slot;
    auto profile = make_copilot_profile();
    ApplyModelDeps deps;
    deps.cfg = &cfg;
    deps.provider_slot = &slot;
    deps.sm = nullptr;
    deps.loop = nullptr;

    auto result = apply_model_to_session(profile, deps);

    EXPECT_EQ(result.state.name, "copilot-mini");
    EXPECT_EQ(result.state.provider, "copilot");
    EXPECT_EQ(result.state.model, "gpt-4o-mini");
    EXPECT_GT(result.state.context_window, 0);
    {
        std::lock_guard<std::mutex> lk(slot.mu);
        ASSERT_TRUE(slot.provider);
        EXPECT_EQ(slot.provider->name(), "copilot");
        EXPECT_EQ(slot.provider->model(), "gpt-4o-mini");
    }
}

// 场景:切换到 codex saved model → 只构造 CodexProvider,不触碰 Codex token。
TEST(ApplyModelToSession, SwapsToCodexProvider) {
    auto cfg = make_copilot_cfg();
    SessionEntry::ProviderSlot slot;
    ModelProfile profile;
    profile.name = "codex";
    profile.provider = "codex";
    profile.model = "gpt-5.5";
    ApplyModelDeps deps;
    deps.cfg = &cfg;
    deps.provider_slot = &slot;

    auto result = apply_model_to_session(profile, deps);

    EXPECT_EQ(result.state.name, "codex");
    EXPECT_EQ(result.state.provider, "codex");
    EXPECT_EQ(result.state.model, "gpt-5.5");
    EXPECT_EQ(result.state.context_window, 272000);
    {
        std::lock_guard<std::mutex> lk(slot.mu);
        ASSERT_TRUE(slot.provider);
        EXPECT_EQ(slot.provider->name(), "codex");
        EXPECT_EQ(slot.provider->model(), "gpt-5.5");
    }
}
