#include <gtest/gtest.h>

#include "hooks/hook_manager.hpp"
#include "hooks/hook_runner.hpp"

#include <filesystem>
#include <string>
#include <vector>

TEST(HookRunner, SendsPayloadOnStdinWithZeroTimeoutAsInfinite) {
    acecode::HookCommandSpec cmd;
#ifdef _WIN32
    cmd.command = "cmd.exe";
    cmd.args = {"/c", "more"};
#else
    cmd.command = "/bin/cat";
#endif

    auto result = acecode::run_hook_process(cmd, "hello hook\n", 0, "");
    EXPECT_TRUE(result.started) << result.error;
    EXPECT_FALSE(result.timed_out);
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_NE(result.output.find("hello hook"), std::string::npos);
}

TEST(HookRunner, ShellCommandRunsThroughPlatformShellWithStdin) {
#ifdef _WIN32
    const std::string command = "more";
#else
    const std::string command = "cat";
#endif
    auto result = acecode::run_hook_shell_command(command, "hello shell hook\n", 3000, "");
    EXPECT_TRUE(result.started) << result.error;
    EXPECT_FALSE(result.timed_out);
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_NE(result.stdout_text.find("hello shell hook"), std::string::npos);
}

TEST(HookRunner, PositiveTimeoutTerminatesLongRunningHook) {
    acecode::HookCommandSpec cmd;
#ifdef _WIN32
    cmd.command = "cmd.exe";
    cmd.args = {"/c", "ping -n 3 127.0.0.1 > nul"};
#else
    cmd.command = "/bin/sh";
    cmd.args = {"-c", "sleep 2"};
#endif

    auto result = acecode::run_hook_process(cmd, "", 50, "");
    EXPECT_TRUE(result.started) << result.error;
    EXPECT_TRUE(result.timed_out);
}

TEST(HookRunner, ResolvesCommandBesideCurrentExecutableBeforePathSearch) {
#ifdef _WIN32
    const std::string binary_name = "acecode_unit_tests.exe";
#else
    const std::string binary_name = "acecode_unit_tests";
#endif

    auto resolved = acecode::resolve_hook_command_path(binary_name);
    EXPECT_NE(resolved, binary_name);
    EXPECT_TRUE(std::filesystem::path(resolved).is_absolute());
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(resolved)));
}

TEST(HookRunner, MissingBareCommandStaysAvailableForPathSearch) {
    const std::string command = "__acecode_missing_hook_command_for_path_search__";
    EXPECT_EQ(acecode::resolve_hook_command_path(command), command);
}

TEST(HookRunner, LegacyDispatchPreservesDirectRunnerAndDiagnosticOnlyOutput) {
    acecode::HookConfig cfg;
    cfg.enabled = true;

    for (const std::string event : {
             acecode::kHookEventStartupBeforeModelLoad,
             acecode::kHookEventStartupModelsLoaded,
             acecode::kHookEventAssistantMessageCompleted,
         }) {
        acecode::HookDefinition hook;
        hook.id = event + ".test";
        hook.event = event;
        hook.mode = acecode::HookMode::Sync;
        hook.timeout_ms = 123;
        hook.command.command = "legacy-hook";
        hook.command.args = {event};
        cfg.events[event].push_back(std::move(hook));
    }

    std::vector<std::string> events_seen;
    std::vector<std::string> cwd_seen;
    acecode::HookManager manager(std::move(cfg),
        [&](const acecode::HookCommandSpec& command,
            const std::string& stdin_text,
            int timeout_ms,
            const std::string& cwd) -> acecode::HookProcessResult {
            EXPECT_EQ(command.command, "legacy-hook");
            EXPECT_EQ(command.args.size(), 1u);
            if (!command.args.empty()) {
                EXPECT_EQ(timeout_ms, 123);
            }
            auto payload = nlohmann::json::parse(stdin_text);
            events_seen.push_back(payload.value("event", ""));
            cwd_seen.push_back(cwd);

            acecode::HookProcessResult result;
            result.started = true;
            result.exit_code = 0;
            result.stdout_text = R"({"decision":"block","reason":"ignored for legacy"})";
            result.output = result.stdout_text;
            return result;
        });

    const std::string cwd = "/tmp/acecode-legacy-hooks";
    for (const std::string event : {
             acecode::kHookEventStartupBeforeModelLoad,
             acecode::kHookEventStartupModelsLoaded,
             acecode::kHookEventAssistantMessageCompleted,
         }) {
        EXPECT_EQ(manager.dispatch(event, nlohmann::json::object(), cwd), 1u);
    }

    ASSERT_EQ(events_seen.size(), 3u);
    EXPECT_EQ(events_seen[0], acecode::kHookEventStartupBeforeModelLoad);
    EXPECT_EQ(events_seen[1], acecode::kHookEventStartupModelsLoaded);
    EXPECT_EQ(events_seen[2], acecode::kHookEventAssistantMessageCompleted);
    ASSERT_EQ(cwd_seen.size(), 3u);
    EXPECT_EQ(cwd_seen[0], cwd);
    EXPECT_EQ(cwd_seen[1], cwd);
    EXPECT_EQ(cwd_seen[2], cwd);
}
