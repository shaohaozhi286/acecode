#include <gtest/gtest.h>

#include "hooks/hook_runner.hpp"

#include <filesystem>
#include <string>

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
