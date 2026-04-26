#include <gtest/gtest.h>

#include "session/file_checkpoint_store.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <set>
#include <string>

namespace fs = std::filesystem;

using acecode::FileCheckpointStore;

namespace {

fs::path make_temp_dir(const std::string& hint) {
    auto dir = fs::temp_directory_path() /
               ("acecode_checkpoint_" + hint + "_" +
                std::to_string(std::random_device{}()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream ofs(path, std::ios::binary);
    ofs << content;
}

std::string read_file(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(ifs)),
                       std::istreambuf_iterator<char>());
}

} // namespace

TEST(FileCheckpointStore, ExistingFileBackupDiffAndRestore) {
    auto project = make_temp_dir("restore_existing");
    auto file = project / "work.txt";
    write_file(file, "a\nb\n");

    FileCheckpointStore store;
    store.set_session(project.string(), "s1");
    store.make_snapshot("u1");
    ASSERT_TRUE(store.track_before_write(file.string()).has_value());

    write_file(file, "a\nx\n");

    auto stats = store.diff_stats("u1");
    EXPECT_TRUE(stats.has_changes());
    ASSERT_EQ(stats.files_changed.size(), 1u);
    EXPECT_TRUE(stats.errors.empty());

    auto restored = store.rewind_to("u1");
    EXPECT_TRUE(restored.ok());
    ASSERT_EQ(restored.files_changed.size(), 1u);
    EXPECT_EQ(read_file(file), "a\nb\n");

    fs::remove_all(project);
}

TEST(FileCheckpointStore, AbsentMarkerDeletesCreatedFile) {
    auto project = make_temp_dir("delete_created");
    auto file = project / "new.txt";

    FileCheckpointStore store;
    store.set_session(project.string(), "s1");
    store.make_snapshot("u1");
    ASSERT_TRUE(store.track_before_write(file.string()).has_value());

    write_file(file, "created\n");
    ASSERT_TRUE(fs::exists(file));

    auto restored = store.rewind_to("u1");
    EXPECT_TRUE(restored.ok());
    EXPECT_FALSE(fs::exists(file));

    fs::remove_all(project);
}

TEST(FileCheckpointStore, TrackBeforeWriteCapturesOncePerSnapshot) {
    auto project = make_temp_dir("once");
    auto file = project / "once.txt";
    write_file(file, "original\n");

    FileCheckpointStore store;
    store.set_session(project.string(), "s1");
    store.make_snapshot("u1");

    auto first = store.track_before_write(file.string());
    ASSERT_TRUE(first.has_value());
    write_file(file, "mid\n");

    auto second = store.track_before_write(file.string());
    EXPECT_FALSE(second.has_value());
    write_file(file, "final\n");

    auto restored = store.rewind_to("u1");
    EXPECT_TRUE(restored.ok());
    EXPECT_EQ(read_file(file), "original\n");

    fs::remove_all(project);
}

TEST(FileCheckpointStore, EncodeDecodeSnapshotMetaRoundtrip) {
    acecode::FileCheckpointSnapshot snapshot;
    snapshot.uuid = "snapshot-1";
    snapshot.message_uuid = "user-1";
    snapshot.timestamp = "2026-04-26T00:00:00Z";
    snapshot.tracked_file_backups["/tmp/a.txt"] =
        acecode::FileCheckpointBackup{"hash@v1", 1, "2026-04-26T00:00:00Z", false};

    auto msg = FileCheckpointStore::encode_snapshot_message(snapshot);
    EXPECT_EQ(msg.role, "system");
    EXPECT_TRUE(msg.is_meta);
    EXPECT_EQ(msg.subtype, "file_checkpoint");

    auto decoded = FileCheckpointStore::decode_snapshot_message(msg);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->uuid, "snapshot-1");
    EXPECT_EQ(decoded->message_uuid, "user-1");
    ASSERT_EQ(decoded->tracked_file_backups.size(), 1u);
    EXPECT_EQ(decoded->tracked_file_backups.begin()->second.backup_file_name, "hash@v1");
}

TEST(FileCheckpointStore, LoadFromMessagesReconstructsCheckpointState) {
    auto project = make_temp_dir("load");
    auto file = project / "load.txt";
    write_file(file, "old\n");

    FileCheckpointStore store;
    store.set_session(project.string(), "s1");
    auto snapshot = store.make_snapshot("u1");
    snapshot = *store.track_before_write(file.string());
    auto meta = FileCheckpointStore::encode_snapshot_message(snapshot);

    FileCheckpointStore loaded;
    loaded.load_from_messages(project.string(), "s1", {meta});
    EXPECT_TRUE(loaded.can_restore("u1"));

    write_file(file, "new\n");
    auto restored = loaded.rewind_to("u1");
    EXPECT_TRUE(restored.ok());
    EXPECT_EQ(read_file(file), "old\n");

    fs::remove_all(project);
}

TEST(FileCheckpointStore, ForkCopiesRetainedBackupsAndDropsDiscardedSnapshots) {
    auto project = make_temp_dir("fork");
    auto file = project / "fork.txt";
    write_file(file, "old\n");

    FileCheckpointStore store;
    store.set_session(project.string(), "s1");
    store.make_snapshot("u1");
    ASSERT_TRUE(store.track_before_write(file.string()).has_value());
    write_file(file, "mid\n");

    store.make_snapshot("u2");
    ASSERT_TRUE(store.track_before_write(file.string()).has_value());
    write_file(file, "new\n");

    auto meta = store.fork_to_session("s2", std::set<std::string>{"u1"});

    ASSERT_EQ(meta.size(), 1u);
    EXPECT_TRUE(store.can_restore("u1"));
    EXPECT_FALSE(store.can_restore("u2"));

    auto restored = store.rewind_to("u1");
    EXPECT_TRUE(restored.ok());
    EXPECT_EQ(read_file(file), "old\n");

    fs::remove_all(project);
}

TEST(FileCheckpointStore, MissingBackupReportsError) {
    auto project = make_temp_dir("missing");
    auto file = project / "missing.txt";
    write_file(file, "old\n");

    FileCheckpointStore store;
    store.set_session(project.string(), "s1");
    store.make_snapshot("u1");
    auto snapshot = store.track_before_write(file.string());
    ASSERT_TRUE(snapshot.has_value());

    const auto backup = snapshot->tracked_file_backups.begin()->second.backup_file_name;
    fs::remove(fs::path(store.checkpoint_dir()) / backup);
    write_file(file, "new\n");

    auto stats = store.diff_stats("u1");
    EXPECT_FALSE(stats.errors.empty());

    auto restored = store.rewind_to("u1");
    EXPECT_FALSE(restored.ok());
    EXPECT_FALSE(restored.errors.empty());

    fs::remove_all(project);
}

TEST(FileCheckpointStore, SnapshotCapEvictsOldestMetadata) {
    auto project = make_temp_dir("cap");

    FileCheckpointStore store(1);
    store.set_session(project.string(), "s1");
    store.make_snapshot("u1");
    store.make_snapshot("u2");

    ASSERT_EQ(store.snapshots().size(), 1u);
    EXPECT_FALSE(store.can_restore("u1"));
    EXPECT_TRUE(store.can_restore("u2"));

    fs::remove_all(project);
}
