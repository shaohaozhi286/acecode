#include "file_checkpoint_store.hpp"

#include "../tool/diff_utils.hpp"
#include "../utils/logger.hpp"
#include "../utils/uuid.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>

namespace fs = std::filesystem;

namespace acecode {

namespace {

uint64_t fnv1a_64(const std::string& data) {
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : data) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string hash_hex16(const std::string& data) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << fnv1a_64(data);
    return oss.str();
}

std::optional<std::string> read_binary_file(const fs::path& path, std::string* error = nullptr) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        if (error) *error = "cannot open " + path.string();
        return std::nullopt;
    }
    std::string s((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return s;
}

bool write_copy_with_dirs(const fs::path& from, const fs::path& to, std::string& error) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    ec.clear();
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    ec.clear();
    auto perms = fs::status(from, ec).permissions();
    if (!ec) {
        fs::permissions(to, perms, ec);
    }
    return true;
}

nlohmann::json encode_backup(const FileCheckpointBackup& backup) {
    nlohmann::json j;
    j["version"] = backup.version;
    j["backup_time"] = backup.backup_time;
    j["absent"] = backup.absent;
    if (!backup.backup_file_name.empty()) {
        j["backup_file_name"] = backup.backup_file_name;
    }
    return j;
}

std::optional<FileCheckpointBackup> decode_backup(const nlohmann::json& j) {
    if (!j.is_object()) return std::nullopt;
    FileCheckpointBackup backup;
    if (j.contains("version") && j["version"].is_number_integer()) {
        backup.version = j["version"].get<int>();
    }
    if (j.contains("backup_time") && j["backup_time"].is_string()) {
        backup.backup_time = j["backup_time"].get<std::string>();
    }
    if (j.contains("absent") && j["absent"].is_boolean()) {
        backup.absent = j["absent"].get<bool>();
    }
    if (j.contains("backup_file_name") && j["backup_file_name"].is_string()) {
        backup.backup_file_name = j["backup_file_name"].get<std::string>();
    }
    if (!backup.absent && backup.backup_file_name.empty()) return std::nullopt;
    if (backup.version <= 0) return std::nullopt;
    return backup;
}

} // namespace

FileCheckpointStore::FileCheckpointStore(size_t max_snapshots)
    : max_snapshots_(max_snapshots == 0 ? 1 : max_snapshots) {}

void FileCheckpointStore::set_session(std::string project_dir, std::string session_id) {
    project_dir_ = std::move(project_dir);
    session_id_ = std::move(session_id);
}

void FileCheckpointStore::reset() {
    snapshots_.clear();
    tracked_files_.clear();
    active_message_uuid_.clear();
    active_captured_paths_.clear();
}

std::string FileCheckpointStore::checkpoint_dir() const {
    if (project_dir_.empty() || session_id_.empty()) return {};
    return (fs::path(project_dir_) / "file-checkpoints" / session_id_).string();
}

std::string FileCheckpointStore::normalize_tracking_path(const std::string& file_path) const {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(file_path, ec);
    if (ec) {
        p = fs::absolute(file_path, ec);
    }
    if (ec) p = fs::path(file_path);
    std::string s = p.lexically_normal().string();
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

std::string FileCheckpointStore::backup_path(const std::string& backup_file_name,
                                             const std::string& session_id_override) const {
    const std::string& sid = session_id_override.empty() ? session_id_ : session_id_override;
    return (fs::path(project_dir_) / "file-checkpoints" / sid / backup_file_name).string();
}

std::string FileCheckpointStore::make_backup_file_name(
    const std::string& tracking_path,
    int version) const {
    return hash_hex16(tracking_path) + "@v" + std::to_string(version);
}

int FileCheckpointStore::next_version_for(const std::string& tracking_path) const {
    int latest = 0;
    for (const auto& snapshot : snapshots_) {
        auto it = snapshot.tracked_file_backups.find(tracking_path);
        if (it != snapshot.tracked_file_backups.end()) {
            latest = std::max(latest, it->second.version);
        }
    }
    return latest + 1;
}

FileCheckpointSnapshot FileCheckpointStore::make_snapshot(const std::string& user_message_uuid) {
    FileCheckpointSnapshot snapshot;
    snapshot.uuid = generate_uuid();
    snapshot.message_uuid = user_message_uuid;
    snapshot.timestamp = iso_timestamp();
    if (!snapshots_.empty()) {
        snapshot.tracked_file_backups = snapshots_.back().tracked_file_backups;
    }

    snapshots_.push_back(snapshot);
    active_message_uuid_ = user_message_uuid;
    active_captured_paths_.clear();
    enforce_snapshot_cap();
    return snapshots_.back();
}

std::optional<FileCheckpointSnapshot> FileCheckpointStore::track_before_write(
    const std::string& file_path) {
    auto* snapshot = find_active_snapshot();
    if (!snapshot || project_dir_.empty() || session_id_.empty()) return std::nullopt;

    const std::string tracking_path = normalize_tracking_path(file_path);
    if (active_captured_paths_.count(tracking_path) > 0) {
        return std::nullopt;
    }

    FileCheckpointBackup backup;
    backup.version = next_version_for(tracking_path);
    backup.backup_time = iso_timestamp();

    std::error_code ec;
    if (!fs::exists(tracking_path, ec)) {
        backup.absent = true;
    } else {
        backup.backup_file_name = make_backup_file_name(tracking_path, backup.version);
        const fs::path dest = backup_path(backup.backup_file_name);
        fs::create_directories(dest.parent_path(), ec);
        ec.clear();
        fs::copy_file(tracking_path, dest, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            LOG_WARN("File checkpoint backup failed for " + tracking_path + ": " + ec.message());
            return std::nullopt;
        }
        ec.clear();
        auto perms = fs::status(tracking_path, ec).permissions();
        if (!ec) {
            fs::permissions(dest, perms, ec);
        }
    }

    snapshot->tracked_file_backups[tracking_path] = backup;
    snapshot->timestamp = iso_timestamp();
    tracked_files_.insert(tracking_path);
    active_captured_paths_.insert(tracking_path);
    return *snapshot;
}

bool FileCheckpointStore::can_restore(const std::string& user_message_uuid) const {
    return find_snapshot(user_message_uuid) != nullptr;
}

FileCheckpointDiffStats FileCheckpointStore::diff_stats(const std::string& user_message_uuid) const {
    FileCheckpointDiffStats stats;
    const auto* snapshot = find_snapshot(user_message_uuid);
    if (!snapshot) return stats;

    for (const auto& [tracking_path, backup] : snapshot->tracked_file_backups) {
        const fs::path original = tracking_path;
        std::optional<std::string> target_content;
        if (!backup.absent) {
            std::string error;
            target_content = read_binary_file(backup_path(backup.backup_file_name), &error);
            if (!target_content.has_value()) {
                stats.errors.push_back("Missing backup for " + tracking_path + ": " + error);
                continue;
            }
        } else {
            target_content = std::string{};
        }

        std::optional<std::string> current_content;
        if (fs::exists(original)) {
            current_content = read_binary_file(original);
            if (!current_content.has_value()) {
                stats.errors.push_back("Cannot read current file " + tracking_path);
                continue;
            }
        } else {
            current_content = std::string{};
        }

        const bool current_exists = fs::exists(original);
        const bool target_exists = !backup.absent;
        if (current_exists == target_exists && current_content == target_content) {
            continue;
        }

        DiffStats ds;
        generate_unified_diff(current_content.value_or(std::string{}),
                              target_content.value_or(std::string{}),
                              tracking_path,
                              ds);
        stats.files_changed.push_back(tracking_path);
        stats.insertions += ds.additions;
        stats.deletions += ds.deletions;
    }
    return stats;
}

FileCheckpointRestoreResult FileCheckpointStore::rewind_to(
    const std::string& user_message_uuid) const {
    FileCheckpointRestoreResult result;
    const auto* snapshot = find_snapshot(user_message_uuid);
    if (!snapshot) {
        result.errors.push_back("No file checkpoint found for this message.");
        return result;
    }

    for (const auto& [tracking_path, backup] : snapshot->tracked_file_backups) {
        std::error_code ec;
        if (backup.absent) {
            if (fs::exists(tracking_path, ec)) {
                fs::remove(tracking_path, ec);
                if (ec) {
                    result.errors.push_back("Failed to delete " + tracking_path + ": " + ec.message());
                } else {
                    result.files_changed.push_back(tracking_path);
                }
            }
            continue;
        }

        const fs::path src = backup_path(backup.backup_file_name);
        if (!fs::exists(src, ec)) {
            result.errors.push_back("Backup missing for " + tracking_path);
            continue;
        }

        bool changed = true;
        auto current = fs::exists(tracking_path, ec) ? read_binary_file(tracking_path) : std::optional<std::string>{};
        auto target = read_binary_file(src);
        if (current.has_value() && target.has_value() && current == target) changed = false;
        if (!changed) continue;

        std::string error;
        if (!write_copy_with_dirs(src, tracking_path, error)) {
            result.errors.push_back("Failed to restore " + tracking_path + ": " + error);
        } else {
            result.files_changed.push_back(tracking_path);
        }
    }
    return result;
}

void FileCheckpointStore::load_from_messages(
    const std::string& project_dir,
    const std::string& session_id,
    const std::vector<ChatMessage>& messages) {
    set_session(project_dir, session_id);
    reset();
    set_session(project_dir, session_id);

    for (const auto& msg : messages) {
        auto decoded = decode_snapshot_message(msg);
        if (!decoded.has_value()) continue;

        auto existing = std::find_if(snapshots_.begin(), snapshots_.end(),
            [&](const FileCheckpointSnapshot& s) {
                return s.message_uuid == decoded->message_uuid;
            });
        if (existing != snapshots_.end()) {
            *existing = *decoded;
        } else {
            snapshots_.push_back(*decoded);
        }
    }

    if (!snapshots_.empty()) {
        active_message_uuid_ = snapshots_.back().message_uuid;
        for (const auto& [path, _] : snapshots_.back().tracked_file_backups) {
            tracked_files_.insert(path);
        }
    }
    enforce_snapshot_cap();
    active_captured_paths_.clear();
}

std::vector<ChatMessage> FileCheckpointStore::fork_to_session(
    const std::string& new_session_id,
    const std::set<std::string>& retained_user_message_uuids) {
    const std::string old_session_id = session_id_;
    std::vector<FileCheckpointSnapshot> retained;
    std::set<std::string> copied_backups;

    for (const auto& snapshot : snapshots_) {
        if (retained_user_message_uuids.count(snapshot.message_uuid) == 0) continue;
        retained.push_back(snapshot);
        for (const auto& [_, backup] : snapshot.tracked_file_backups) {
            if (backup.absent || backup.backup_file_name.empty()) continue;
            if (!copied_backups.insert(backup.backup_file_name).second) continue;

            const fs::path src = backup_path(backup.backup_file_name, old_session_id);
            const fs::path dst = backup_path(backup.backup_file_name, new_session_id);
            std::error_code ec;
            fs::create_directories(dst.parent_path(), ec);
            ec.clear();
            fs::create_hard_link(src, dst, ec);
            if (ec) {
                ec.clear();
                fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
                if (ec) {
                    LOG_WARN("File checkpoint fork copy failed for " + src.string() + ": " + ec.message());
                }
            }
        }
    }

    session_id_ = new_session_id;
    snapshots_ = retained;
    tracked_files_.clear();
    active_message_uuid_.clear();
    if (!snapshots_.empty()) {
        active_message_uuid_ = snapshots_.back().message_uuid;
        for (const auto& [path, _] : snapshots_.back().tracked_file_backups) {
            tracked_files_.insert(path);
        }
    }
    active_captured_paths_.clear();

    std::vector<ChatMessage> meta_messages;
    meta_messages.reserve(snapshots_.size());
    for (const auto& snapshot : snapshots_) {
        meta_messages.push_back(encode_snapshot_message(snapshot));
    }
    return meta_messages;
}

ChatMessage FileCheckpointStore::encode_snapshot_message(const FileCheckpointSnapshot& snapshot) {
    ChatMessage msg;
    msg.role = "system";
    msg.content = "[File checkpoint]";
    msg.uuid = snapshot.uuid.empty() ? generate_uuid() : snapshot.uuid;
    msg.timestamp = snapshot.timestamp.empty() ? iso_timestamp() : snapshot.timestamp;
    msg.subtype = "file_checkpoint";
    msg.is_meta = true;

    nlohmann::json tracked = nlohmann::json::object();
    for (const auto& [path, backup] : snapshot.tracked_file_backups) {
        tracked[path] = encode_backup(backup);
    }
    msg.metadata = {
        {"message_uuid", snapshot.message_uuid},
        {"tracked_files", tracked},
        {"timestamp", msg.timestamp},
    };
    return msg;
}

std::optional<FileCheckpointSnapshot> FileCheckpointStore::decode_snapshot_message(
    const ChatMessage& msg) {
    if (!msg.is_meta || msg.subtype != "file_checkpoint" || !msg.metadata.is_object()) {
        return std::nullopt;
    }
    if (!msg.metadata.contains("message_uuid") ||
        !msg.metadata["message_uuid"].is_string()) {
        return std::nullopt;
    }
    FileCheckpointSnapshot snapshot;
    snapshot.uuid = msg.uuid;
    snapshot.message_uuid = msg.metadata["message_uuid"].get<std::string>();
    snapshot.timestamp = msg.metadata.value("timestamp", msg.timestamp);

    if (!msg.metadata.contains("tracked_files") ||
        !msg.metadata["tracked_files"].is_object()) {
        return snapshot;
    }
    for (const auto& item : msg.metadata["tracked_files"].items()) {
        auto backup = decode_backup(item.value());
        if (backup.has_value()) {
            snapshot.tracked_file_backups[item.key()] = *backup;
        }
    }
    return snapshot;
}

void FileCheckpointStore::remove_session_backups(
    const std::string& project_dir,
    const std::string& session_id) {
    std::error_code ec;
    fs::remove_all(fs::path(project_dir) / "file-checkpoints" / session_id, ec);
}

void FileCheckpointStore::enforce_snapshot_cap() {
    while (snapshots_.size() > max_snapshots_) {
        snapshots_.erase(snapshots_.begin());
    }
    cleanup_unreferenced_backups();
}

void FileCheckpointStore::cleanup_unreferenced_backups() const {
    const std::string dir = checkpoint_dir();
    if (dir.empty() || !fs::exists(dir)) return;

    std::set<std::string> referenced;
    for (const auto& snapshot : snapshots_) {
        for (const auto& [_, backup] : snapshot.tracked_file_backups) {
            if (!backup.absent && !backup.backup_file_name.empty()) {
                referenced.insert(backup.backup_file_name);
            }
        }
    }

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec || !entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        if (referenced.count(name) == 0) {
            fs::remove(entry.path(), ec);
        }
    }
}

const FileCheckpointSnapshot* FileCheckpointStore::find_snapshot(
    const std::string& user_message_uuid) const {
    for (auto it = snapshots_.rbegin(); it != snapshots_.rend(); ++it) {
        if (it->message_uuid == user_message_uuid) return &*it;
    }
    return nullptr;
}

FileCheckpointSnapshot* FileCheckpointStore::find_active_snapshot() {
    if (active_message_uuid_.empty()) return nullptr;
    for (auto it = snapshots_.rbegin(); it != snapshots_.rend(); ++it) {
        if (it->message_uuid == active_message_uuid_) return &*it;
    }
    return nullptr;
}

} // namespace acecode
