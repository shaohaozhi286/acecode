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

TEST(DaemonRuntimeFiles, ValidationRejectsNonDaemonProcessIdentity) {
    auto options = test_options();
    options.require_process_identity = true;

    auto check = acecode::daemon::validate_runtime_snapshot_for_reuse(
        fresh_snapshot(),
        options);

    EXPECT_FALSE(check.reusable);
    EXPECT_NE(check.reason.find("acecode daemon process"), std::string::npos);
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

TEST(DaemonRuntimeFiles, ReadsDesktopManagedManifestAndOwnerRecord) {
    fs::path dir = unique_temp_dir("acecode_desktop_runtime");
    acecode::daemon::DesktopManagedRuntime managed;
    managed.pid = acecode::daemon::current_pid();
    managed.guid = "managed-guid";
    managed.kind = "acecode-desktop";
    managed.protocol_version = 1;
    managed.acecode_version = "0.7.4-test";
    ASSERT_TRUE(acecode::daemon::write_desktop_managed_runtime(
        managed, dir.string()));

    acecode::daemon::DesktopOwnerRecord owner;
    owner.pid = acecode::daemon::current_pid();
    owner.instance_id = "desktop-instance";
    owner.timestamp_ms = 123456;
    ASSERT_TRUE(acecode::daemon::write_desktop_owner_record(
        dir.string(), owner));

    auto read_managed =
        acecode::daemon::read_desktop_managed_runtime(dir.string());
    auto read_owner =
        acecode::daemon::read_desktop_owner_record(dir.string());
    ASSERT_TRUE(read_managed.has_value());
    ASSERT_TRUE(read_owner.has_value());
    EXPECT_EQ(read_managed->pid, managed.pid);
    EXPECT_EQ(read_managed->guid, managed.guid);
    EXPECT_EQ(read_managed->kind, managed.kind);
    EXPECT_EQ(read_managed->protocol_version, managed.protocol_version);
    EXPECT_EQ(read_managed->acecode_version, managed.acecode_version);
    EXPECT_EQ(read_owner->pid, owner.pid);
    EXPECT_EQ(read_owner->instance_id, owner.instance_id);
    EXPECT_EQ(read_owner->timestamp_ms, owner.timestamp_ms);

    fs::remove_all(dir);
}

TEST(DaemonRuntimeFiles, GenerationCleanupRejectsStaleOwner) {
    fs::path dir = unique_temp_dir("acecode_generation_cleanup_stale");
    write_text(dir / "daemon.pid", "222");
    write_text(dir / "daemon.guid", "new-guid");
    write_text(dir / "daemon.port", "43210");
    write_text(dir / "token", "new-token");
    write_text(dir / "heartbeat",
               R"({"guid":"new-guid","pid":222,"timestamp_ms":100000})");
    write_text(dir / "desktop-managed.json",
               R"({"pid":222,"guid":"new-guid","kind":"acecode-desktop","protocol_version":1,"acecode_version":"test"})");
    write_text(dir / "desktop-owner.json",
               R"({"pid":333,"instance_id":"new-owner","timestamp_ms":100000})");

    EXPECT_FALSE(acecode::daemon::cleanup_runtime_files_if_owned(
        111, "old-guid", dir.string(), true));
    EXPECT_TRUE(fs::exists(dir / "daemon.pid"));
    EXPECT_TRUE(fs::exists(dir / "daemon.guid"));
    EXPECT_TRUE(fs::exists(dir / "daemon.port"));
    EXPECT_TRUE(fs::exists(dir / "token"));
    EXPECT_TRUE(fs::exists(dir / "heartbeat"));
    EXPECT_TRUE(fs::exists(dir / "desktop-managed.json"));
    EXPECT_TRUE(fs::exists(dir / "desktop-owner.json"));

    fs::remove_all(dir);
}

TEST(DaemonRuntimeFiles, GenerationCleanupRemovesMatchingManagedBundle) {
    fs::path dir = unique_temp_dir("acecode_generation_cleanup_match");
    write_text(dir / "daemon.pid", "222");
    write_text(dir / "daemon.guid", "matching-guid");
    write_text(dir / "daemon.port", "43210");
    write_text(dir / "token", "token");
    write_text(dir / "heartbeat",
               R"({"guid":"matching-guid","pid":222,"timestamp_ms":100000})");
    write_text(dir / "desktop-managed.json",
               R"({"pid":222,"guid":"matching-guid","kind":"acecode-desktop","protocol_version":1,"acecode_version":"test"})");
    write_text(dir / "desktop-owner.json",
               R"({"pid":333,"instance_id":"owner","timestamp_ms":100000})");

    EXPECT_TRUE(acecode::daemon::cleanup_runtime_files_if_owned(
        222, "matching-guid", dir.string(), true));
    EXPECT_FALSE(fs::exists(dir / "daemon.pid"));
    EXPECT_FALSE(fs::exists(dir / "daemon.guid"));
    EXPECT_FALSE(fs::exists(dir / "daemon.port"));
    EXPECT_FALSE(fs::exists(dir / "token"));
    EXPECT_FALSE(fs::exists(dir / "heartbeat"));
    EXPECT_FALSE(fs::exists(dir / "desktop-managed.json"));
    EXPECT_TRUE(fs::exists(dir / "desktop-owner.json"));

    fs::remove_all(dir);
}

TEST(DaemonRuntimeFiles, GenerationCleanupCanRemoveStoppedGuidOnlyBundle) {
    fs::path dir = unique_temp_dir("acecode_generation_cleanup_stopped");
    write_text(dir / "daemon.guid", "stopped-guid");
    write_text(dir / "daemon.port", "43210");
    write_text(dir / "token", "stale-token");
    write_text(dir / "desktop-owner.json",
               R"({"pid":333,"instance_id":"new-owner","timestamp_ms":100000})");

    EXPECT_TRUE(acecode::daemon::cleanup_runtime_files_if_owned(
        0, "stopped-guid", dir.string(), true));
    EXPECT_FALSE(fs::exists(dir / "daemon.guid"));
    EXPECT_FALSE(fs::exists(dir / "daemon.port"));
    EXPECT_FALSE(fs::exists(dir / "token"));
    EXPECT_TRUE(fs::exists(dir / "desktop-owner.json"));

    fs::remove_all(dir);
}
