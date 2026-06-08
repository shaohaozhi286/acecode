#include <gtest/gtest.h>

#include "daemon/platform.hpp"
#include "daemon/runtime_files.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

fs::path unique_temp_dir(const std::string& name) {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = fs::temp_directory_path() /
        (name + "_" + std::to_string(acecode::daemon::current_pid()) +
         "_" + std::to_string(now));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

acecode::daemon::RuntimeValidationOptions test_options() {
    acecode::daemon::RuntimeValidationOptions options;
    options.heartbeat_timeout_ms = 1000;
    options.require_port_probe = false;
    options.require_process_identity = false;
    options.now_ms = 100000;
    return options;
}

acecode::daemon::RuntimeSnapshot fresh_snapshot() {
    acecode::daemon::RuntimeSnapshot snapshot;
    snapshot.pid = acecode::daemon::current_pid();
    snapshot.port = 43210;
    snapshot.token = "token";
    snapshot.guid = "guid";
    snapshot.heartbeat = acecode::daemon::Heartbeat{
        acecode::daemon::current_pid(),
        "guid",
        100000,
    };
    return snapshot;
}

} // namespace

TEST(DaemonRuntimeFiles, ValidationAcceptsFreshRuntimeSnapshot) {
    auto check = acecode::daemon::validate_runtime_snapshot_for_reuse(
        fresh_snapshot(),
        test_options());

    EXPECT_TRUE(check.reusable) << check.reason;
}

TEST(DaemonRuntimeFiles, ValidationRejectsStaleHeartbeat) {
    auto snapshot = fresh_snapshot();
    snapshot.heartbeat->timestamp_ms = 98000;

    auto check = acecode::daemon::validate_runtime_snapshot_for_reuse(
        snapshot,
        test_options());

    EXPECT_FALSE(check.reusable);
    EXPECT_NE(check.reason.find("stale heartbeat"), std::string::npos);
}

TEST(DaemonRuntimeFiles, ValidationRejectsHeartbeatPidMismatch) {
    auto snapshot = fresh_snapshot();
    snapshot.heartbeat->pid = snapshot.heartbeat->pid + 1000;

    auto check = acecode::daemon::validate_runtime_snapshot_for_reuse(
        snapshot,
        test_options());

    EXPECT_FALSE(check.reusable);
    EXPECT_NE(check.reason.find("heartbeat pid mismatch"), std::string::npos);
}

TEST(DaemonRuntimeFiles, ValidationRejectsMissingTokenWhenRequired) {
    auto snapshot = fresh_snapshot();
    snapshot.token.reset();

    auto check = acecode::daemon::validate_runtime_snapshot_for_reuse(
        snapshot,
        test_options());

    EXPECT_FALSE(check.reusable);
    EXPECT_NE(check.reason.find("missing token"), std::string::npos);
}

TEST(DaemonRuntimeFiles, ReadsSnapshotFromExplicitRunDir) {
    fs::path dir = unique_temp_dir("acecode_runtime_snapshot");
    write_text(dir / "daemon.pid", std::to_string(acecode::daemon::current_pid()));
    write_text(dir / "daemon.port", "43210");
    write_text(dir / "daemon.guid", "guid-from-file");
    write_text(dir / "token", "token-from-file");
    write_text(dir / "heartbeat",
               "{\"guid\":\"guid-from-file\",\"pid\":" +
               std::to_string(acecode::daemon::current_pid()) +
               ",\"timestamp_ms\":100000}");

    auto snapshot = acecode::daemon::read_runtime_snapshot(dir.string());
    auto check = acecode::daemon::validate_runtime_snapshot_for_reuse(
        snapshot,
        test_options());

    EXPECT_TRUE(check.reusable) << check.reason;
    ASSERT_TRUE(snapshot.guid.has_value());
    ASSERT_TRUE(snapshot.token.has_value());
    EXPECT_EQ(*snapshot.guid, "guid-from-file");
    EXPECT_EQ(*snapshot.token, "token-from-file");

    fs::remove_all(dir);
}
