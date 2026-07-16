#include "connectors/connector_auth_recovery.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <stdexcept>

using namespace acecode;

namespace {

// 可变的「假磁盘」:load_disk_config 每次返回其当前快照,
// 假钩子 runner 在「登录成功」时改写它,模拟 apikeyset 回写 config.json。
struct FakeDisk {
    AppConfig config;
};

AppConfig make_config_with_connector(const std::string& prefix,
                                     const std::string& api_key,
                                     bool enabled = true) {
    AppConfig cfg;
    ConnectorConfig c;
    c.id = "helper";
    c.name = "Helper";
    c.description = "";
    c.enabled = enabled;
    ConnectorHookConfig hook;
    hook.command = "helper.exe";
    hook.timeout_ms = 5000;
    c.on_auth_error = hook;
    c.auth_error_base_url_prefix = prefix;
    cfg.connectors.push_back(std::move(c));

    ModelProfile m;
    m.name = "model-a";
    m.provider = "openai";
    m.base_url = prefix + "/v1";
    m.api_key = api_key;
    m.model = "model-a";
    cfg.saved_models.push_back(std::move(m));
    return cfg;
}

} // namespace

TEST(ConnectorAuthRecovery, HookSuccessReturnsFreshKey) {
    auto disk = std::make_shared<FakeDisk>();
    disk->config = make_config_with_connector("https://models.example.com", "stale-key");
    std::atomic<int> runner_calls{0};
    std::atomic<int> refreshed_calls{0};

    ConnectorAuthRecovery::Options opts;
    opts.load_disk_config = [disk]() { return disk->config; };
    opts.hook_runner = [disk, &runner_calls](const HookCommandSpec&, int) {
        ++runner_calls;
        disk->config.saved_models[0].api_key = "fresh-key";
        HookProcessResult r;
        r.started = true;
        r.exit_code = 0;
        return r;
    };
    opts.on_config_refreshed = [&refreshed_calls]() { ++refreshed_calls; };
    ConnectorAuthRecovery recovery(std::move(opts));

    auto key = recovery.recover("model-a", "https://models.example.com/v1", "stale-key");
    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(*key, "fresh-key");
    EXPECT_EQ(runner_calls.load(), 1);
    EXPECT_EQ(refreshed_calls.load(), 1);
}

TEST(ConnectorAuthRecovery, NoMatchingConnectorReturnsNullopt) {
    auto disk = std::make_shared<FakeDisk>();
    disk->config = make_config_with_connector("https://models.example.com", "stale-key");
    std::atomic<int> runner_calls{0};

    ConnectorAuthRecovery::Options opts;
    opts.load_disk_config = [disk]() { return disk->config; };
    opts.hook_runner = [&runner_calls](const HookCommandSpec&, int) {
        ++runner_calls;
        return HookProcessResult{};
    };
    ConnectorAuthRecovery recovery(std::move(opts));

    EXPECT_FALSE(recovery.recover("model-a", "https://other.example.org/v1", "stale-key").has_value());
    EXPECT_EQ(runner_calls.load(), 0);
}

TEST(ConnectorAuthRecovery, DisabledConnectorIsIgnored) {
    auto disk = std::make_shared<FakeDisk>();
    disk->config = make_config_with_connector("https://models.example.com", "stale-key",
                                              /*enabled=*/false);
    std::atomic<int> runner_calls{0};

    ConnectorAuthRecovery::Options opts;
    opts.load_disk_config = [disk]() { return disk->config; };
    opts.hook_runner = [&runner_calls](const HookCommandSpec&, int) {
        ++runner_calls;
        return HookProcessResult{};
    };
    ConnectorAuthRecovery recovery(std::move(opts));

    EXPECT_FALSE(recovery.recover("model-a", "https://models.example.com/v1", "stale-key").has_value());
    EXPECT_EQ(runner_calls.load(), 0);
}

TEST(ConnectorAuthRecovery, FreshKeyOnDiskShortCircuitsWithoutHook) {
    auto disk = std::make_shared<FakeDisk>();
    disk->config = make_config_with_connector("https://models.example.com", "already-new-key");
    std::atomic<int> runner_calls{0};

    ConnectorAuthRecovery::Options opts;
    opts.load_disk_config = [disk]() { return disk->config; };
    opts.hook_runner = [&runner_calls](const HookCommandSpec&, int) {
        ++runner_calls;
        return HookProcessResult{};
    };
    ConnectorAuthRecovery recovery(std::move(opts));

    auto key = recovery.recover("model-a", "https://models.example.com/v1", "stale-key");
    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(*key, "already-new-key");
    EXPECT_EQ(runner_calls.load(), 0);
}

TEST(ConnectorAuthRecovery, SiblingFreshKeyDoesNotShortCircuitExactModel) {
    auto disk = std::make_shared<FakeDisk>();
    disk->config = make_config_with_connector("https://models.example.com", "stale-key");
    ModelProfile sibling = disk->config.saved_models[0];
    sibling.name = "model-b";
    sibling.model = "model-b";
    sibling.api_key = "sibling-fresh-key";
    disk->config.saved_models.push_back(std::move(sibling));
    std::atomic<int> runner_calls{0};

    ConnectorAuthRecovery::Options opts;
    opts.load_disk_config = [disk]() { return disk->config; };
    opts.hook_runner = [&runner_calls](const HookCommandSpec&, int) {
        ++runner_calls;
        HookProcessResult r;
        r.started = true;
        r.exit_code = 1;
        return r;
    };
    ConnectorAuthRecovery recovery(std::move(opts));

    EXPECT_FALSE(recovery.recover("model-a", "https://models.example.com/v1",
                                  "stale-key").has_value());
    EXPECT_EQ(runner_calls.load(), 1);
}

TEST(ConnectorAuthRecovery, HookUpdatingOnlySiblingDoesNotReturnItsKey) {
    auto disk = std::make_shared<FakeDisk>();
    disk->config = make_config_with_connector("https://models.example.com", "stale-key");
    ModelProfile sibling = disk->config.saved_models[0];
    sibling.name = "model-b";
    sibling.model = "model-b";
    disk->config.saved_models.push_back(std::move(sibling));

    ConnectorAuthRecovery::Options opts;
    opts.load_disk_config = [disk]() { return disk->config; };
    opts.hook_runner = [disk](const HookCommandSpec&, int) {
        disk->config.saved_models[1].api_key = "sibling-fresh-key";
        HookProcessResult r;
        r.started = true;
        r.exit_code = 0;
        return r;
    };
    ConnectorAuthRecovery recovery(std::move(opts));

    EXPECT_FALSE(recovery.recover("model-a", "https://models.example.com/v1",
                                  "stale-key").has_value());
}

TEST(ConnectorAuthRecovery, HookFailureReturnsNullopt) {
    auto disk = std::make_shared<FakeDisk>();
    disk->config = make_config_with_connector("https://models.example.com", "stale-key");

    ConnectorAuthRecovery::Options opts;
    opts.load_disk_config = [disk]() { return disk->config; };
    opts.hook_runner = [](const HookCommandSpec&, int) {
        HookProcessResult r;
        r.started = true;
        r.exit_code = 1;
        return r;
    };
    ConnectorAuthRecovery recovery(std::move(opts));

    EXPECT_FALSE(recovery.recover("model-a", "https://models.example.com/v1", "stale-key").has_value());
}

TEST(ConnectorAuthRecovery, UnchangedKeyAfterHookReturnsNullopt) {
    auto disk = std::make_shared<FakeDisk>();
    disk->config = make_config_with_connector("https://models.example.com", "stale-key");

    ConnectorAuthRecovery::Options opts;
    opts.load_disk_config = [disk]() { return disk->config; };
    opts.hook_runner = [](const HookCommandSpec&, int) {
        HookProcessResult r;
        r.started = true;
        r.exit_code = 0;  // 钩子退出 0 但没写出新 key
        return r;
    };
    ConnectorAuthRecovery recovery(std::move(opts));

    EXPECT_FALSE(recovery.recover("model-a", "https://models.example.com/v1", "stale-key").has_value());
}

TEST(ConnectorAuthRecovery, LoadConfigThrowingReturnsNullopt) {
    std::atomic<int> runner_calls{0};

    ConnectorAuthRecovery::Options opts;
    opts.load_disk_config = []() -> AppConfig {
        throw std::runtime_error("disk config read failed");
    };
    opts.hook_runner = [&runner_calls](const HookCommandSpec&, int) {
        ++runner_calls;
        return HookProcessResult{};
    };
    ConnectorAuthRecovery recovery(std::move(opts));

    EXPECT_FALSE(recovery.recover("model-a", "https://models.example.com/v1", "stale-key").has_value());
    EXPECT_EQ(runner_calls.load(), 0);
}

TEST(ConnectorAuthRecovery, CooldownBlocksSecondLaunch) {
    auto disk = std::make_shared<FakeDisk>();
    disk->config = make_config_with_connector("https://models.example.com", "stale-key");
    std::atomic<int> runner_calls{0};

    ConnectorAuthRecovery::Options opts;
    opts.load_disk_config = [disk]() { return disk->config; };
    opts.hook_runner = [&runner_calls](const HookCommandSpec&, int) {
        ++runner_calls;
        HookProcessResult r;
        r.started = true;
        r.exit_code = 1;  // 第一次失败,进入冷却
        return r;
    };
    opts.cooldown_ms = 60000;
    ConnectorAuthRecovery recovery(std::move(opts));

    EXPECT_FALSE(recovery.recover("model-a", "https://models.example.com/v1", "stale-key").has_value());
    EXPECT_FALSE(recovery.recover("model-a", "https://models.example.com/v1", "stale-key").has_value());
    EXPECT_EQ(runner_calls.load(), 1);  // 冷却期内不再拉起
}
