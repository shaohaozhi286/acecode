#pragma once

#include "../provider/llm_provider.hpp"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace acecode {

struct FileCheckpointBackup {
    std::string backup_file_name;
    int version = 0;
    std::string backup_time;
    bool absent = false;
};

struct FileCheckpointSnapshot {
    std::string uuid;
    std::string message_uuid;
    std::map<std::string, FileCheckpointBackup> tracked_file_backups;
    std::string timestamp;
};

struct FileCheckpointDiffStats {
    std::vector<std::string> files_changed;
    int insertions = 0;
    int deletions = 0;
    std::vector<std::string> errors;

    bool has_changes() const { return !files_changed.empty(); }
};

struct FileCheckpointRestoreResult {
    std::vector<std::string> files_changed;
    std::vector<std::string> errors;

    bool ok() const { return errors.empty(); }
};

class FileCheckpointStore {
public:
    explicit FileCheckpointStore(size_t max_snapshots = 100);

    void set_session(std::string project_dir, std::string session_id);
    void reset();

    FileCheckpointSnapshot make_snapshot(const std::string& user_message_uuid);
    std::optional<FileCheckpointSnapshot> track_before_write(const std::string& file_path);

    bool can_restore(const std::string& user_message_uuid) const;
    FileCheckpointDiffStats diff_stats(const std::string& user_message_uuid) const;
    FileCheckpointRestoreResult rewind_to(const std::string& user_message_uuid) const;

    void load_from_messages(const std::string& project_dir,
                            const std::string& session_id,
                            const std::vector<ChatMessage>& messages);

    std::vector<ChatMessage> fork_to_session(
        const std::string& new_session_id,
        const std::set<std::string>& retained_user_message_uuids);

    const std::vector<FileCheckpointSnapshot>& snapshots() const { return snapshots_; }
    std::string checkpoint_dir() const;

    static ChatMessage encode_snapshot_message(const FileCheckpointSnapshot& snapshot);
    static std::optional<FileCheckpointSnapshot> decode_snapshot_message(const ChatMessage& msg);
    static void remove_session_backups(const std::string& project_dir, const std::string& session_id);

private:
    std::string normalize_tracking_path(const std::string& file_path) const;
    std::string backup_path(const std::string& backup_file_name,
                            const std::string& session_id_override = {}) const;
    std::string make_backup_file_name(const std::string& tracking_path, int version) const;
    int next_version_for(const std::string& tracking_path) const;
    void enforce_snapshot_cap();
    void cleanup_unreferenced_backups() const;
    const FileCheckpointSnapshot* find_snapshot(const std::string& user_message_uuid) const;
    FileCheckpointSnapshot* find_active_snapshot();

    std::string project_dir_;
    std::string session_id_;
    size_t max_snapshots_ = 100;
    std::vector<FileCheckpointSnapshot> snapshots_;
    std::set<std::string> tracked_files_;
    std::string active_message_uuid_;
    std::set<std::string> active_captured_paths_;
};

} // namespace acecode
