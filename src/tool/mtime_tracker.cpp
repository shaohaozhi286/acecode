#include "mtime_tracker.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <utility>

namespace acecode {

namespace {

constexpr std::size_t kMaxReadObservationEntries = 100;

std::string normalize_tracker_path_key(const std::string& path) {
    std::error_code ec;
    auto fs_path = path_from_utf8(path);
    auto canonical = std::filesystem::weakly_canonical(fs_path, ec);
    if (!ec && !canonical.empty()) {
        return path_to_utf8(canonical.lexically_normal());
    }

    ec.clear();
    auto absolute = std::filesystem::absolute(fs_path, ec);
    if (!ec && !absolute.empty()) {
        return path_to_utf8(absolute.lexically_normal());
    }

    return path_to_utf8(fs_path.lexically_normal());
}

MtimeTracker::ReadObservationKey make_read_observation_key(
    const std::string& path,
    int start_line,
    int end_line,
    bool byte_mode,
    uint64_t byte_offset,
    size_t max_bytes
) {
    return MtimeTracker::ReadObservationKey{
        normalize_tracker_path_key(path),
        start_line,
        end_line,
        byte_mode,
        byte_offset,
        max_bytes
    };
}

void remove_read_observations_for_path_locked(
    std::map<MtimeTracker::ReadObservationKey, MtimeTracker::ReadObservation>& observations,
    std::vector<MtimeTracker::ReadObservationKey>& lru,
    const std::string& normalized_path
) {
    for (auto it = observations.begin(); it != observations.end();) {
        if (it->first.path == normalized_path) {
            it = observations.erase(it);
        } else {
            ++it;
        }
    }
    lru.erase(
        std::remove_if(lru.begin(), lru.end(), [&](const MtimeTracker::ReadObservationKey& key) {
            return key.path == normalized_path;
        }),
        lru.end());
}

void touch_read_observation_lru_locked(
    std::map<MtimeTracker::ReadObservationKey, MtimeTracker::ReadObservation>& observations,
    std::vector<MtimeTracker::ReadObservationKey>& lru,
    const MtimeTracker::ReadObservationKey& key
) {
    lru.erase(std::remove(lru.begin(), lru.end(), key), lru.end());
    lru.push_back(key);
    while (lru.size() > kMaxReadObservationEntries) {
        auto evict = lru.front();
        lru.erase(lru.begin());
        observations.erase(evict);
    }
}

} // namespace

MtimeTracker::FileWriteGuard::FileWriteGuard(std::shared_ptr<std::mutex> mutex)
    : mutex_(std::move(mutex)), lock_(*mutex_) {}

MtimeTracker& MtimeTracker::instance() {
    static MtimeTracker tracker;
    return tracker;
}

void MtimeTracker::record_read(const std::string& path) {
    record_read(path, "", false);
}

void MtimeTracker::record_read(const std::string& path, const std::string& content, bool partial) {
    record_read(path, content, partial, FileReadEditMetadata{});
}

void MtimeTracker::record_read(const std::string& path,
                               const std::string& normalized_content,
                               bool partial,
                               const FileReadEditMetadata& metadata) {
    try {
        const std::string key = normalize_tracker_path_key(path);
        auto mtime = std::filesystem::last_write_time(path_from_utf8(key));
        std::lock_guard<std::mutex> lk(mu_);
        records_[key] = Record{
            mtime,
            partial,
            partial ? std::optional<std::string>{} : std::optional<std::string>{normalized_content},
            std::optional<FileReadEditMetadata>{metadata}
        };
    } catch (...) {
        // File may not exist yet; that's OK
    }
}

void MtimeTracker::seed_transcript_read_baseline(
    const std::string& path,
    const std::string& normalized_content,
    const FileReadEditMetadata& metadata
) {
    const std::string key = normalize_tracker_path_key(path);
    std::lock_guard<std::mutex> lk(mu_);
    records_[key] = Record{
        clock::time_point::min(),
        false,
        std::optional<std::string>{normalized_content},
        std::optional<FileReadEditMetadata>{metadata}
    };
    remove_read_observations_for_path_locked(read_observations_, read_observation_lru_, key);
}

bool MtimeTracker::has_unchanged_read_observation(
    const std::string& path,
    int start_line,
    int end_line,
    bool byte_mode,
    uint64_t byte_offset,
    size_t max_bytes
) const {
    return unchanged_read_observation(
        path, start_line, end_line, byte_mode, byte_offset, max_bytes).has_value();
}

std::optional<MtimeTracker::ReadObservation> MtimeTracker::unchanged_read_observation(
    const std::string& path,
    int start_line,
    int end_line,
    bool byte_mode,
    uint64_t byte_offset,
    size_t max_bytes
) const {
    const auto key = make_read_observation_key(
        path, start_line, end_line, byte_mode, byte_offset, max_bytes);
    std::lock_guard<std::mutex> lk(mu_);
    auto it = read_observations_.find(key);
    if (it == read_observations_.end()) return std::nullopt;
    try {
        auto current_mtime = std::filesystem::last_write_time(path_from_utf8(key.path));
        if (current_mtime == it->second.mtime) {
            return it->second;
        }
    } catch (...) {
    }
    return std::nullopt;
}

void MtimeTracker::record_read_observation(const std::string& path,
                                           int start_line,
                                           int end_line,
                                           bool byte_mode,
                                           uint64_t byte_offset,
                                           size_t max_bytes) {
    try {
        auto key = make_read_observation_key(
            path, start_line, end_line, byte_mode, byte_offset, max_bytes);
        auto mtime = std::filesystem::last_write_time(path_from_utf8(key.path));
        std::lock_guard<std::mutex> lk(mu_);
        read_observations_[key] = ReadObservation{mtime, {}, {}};
        touch_read_observation_lru_locked(read_observations_, read_observation_lru_, key);
    } catch (...) {
        // File may not exist or may be inaccessible; dedup is optional.
    }
}

void MtimeTracker::record_read_observation_result(
    const std::string& path,
    int start_line,
    int end_line,
    const std::string& tool_call_id,
    const std::string& persisted_output_path,
    bool byte_mode,
    uint64_t byte_offset,
    size_t max_bytes
) {
    if (tool_call_id.empty() && persisted_output_path.empty()) return;
    const auto key = make_read_observation_key(
        path, start_line, end_line, byte_mode, byte_offset, max_bytes);
    std::lock_guard<std::mutex> lk(mu_);
    auto it = read_observations_.find(key);
    if (it == read_observations_.end()) return;
    if (!tool_call_id.empty()) it->second.tool_call_id = tool_call_id;
    if (!persisted_output_path.empty()) {
        it->second.persisted_output_path = persisted_output_path;
    }
}

void MtimeTracker::invalidate_read_observations(const std::string& path) {
    const std::string key = normalize_tracker_path_key(path);
    std::lock_guard<std::mutex> lk(mu_);
    remove_read_observations_for_path_locked(read_observations_, read_observation_lru_, key);
}

void MtimeTracker::clear_read_observations() {
    std::lock_guard<std::mutex> lk(mu_);
    read_observations_.clear();
    read_observation_lru_.clear();
}

bool MtimeTracker::was_externally_modified(const std::string& path) const {
    const std::string key = normalize_tracker_path_key(path);
    std::lock_guard<std::mutex> lk(mu_);
    auto it = records_.find(key);
    if (it == records_.end()) {
        return false; // no record, assume safe
    }
    try {
        auto current_mtime = std::filesystem::last_write_time(path_from_utf8(key));
        return current_mtime != it->second.mtime;
    } catch (...) {
        return false; // file deleted or inaccessible
    }
}

MtimeTracker::ReadBaselineCheck MtimeTracker::validate_read_baseline_for_edit(
    const std::string& path,
    const std::string& current_content
) const {
    const std::string key = normalize_tracker_path_key(path);
    std::lock_guard<std::mutex> lk(mu_);
    auto it = records_.find(key);
    if (it == records_.end()) {
        return {ReadBaselineStatus::NotRead, false};
    }

    try {
        auto current_mtime = std::filesystem::last_write_time(path_from_utf8(key));
        if (current_mtime == it->second.mtime) {
            if (it->second.metadata && it->second.metadata->lossy) {
                return {ReadBaselineStatus::UnsafeRead, false};
            }
            return {ReadBaselineStatus::Ok, false};
        }

        if (it->second.partial) {
            return it->second.metadata && it->second.metadata->lossy
                ? ReadBaselineCheck{ReadBaselineStatus::UnsafeRead, false}
                : ReadBaselineCheck{ReadBaselineStatus::ExternallyModified, false};
        }

        // Windows/同步盘/杀毒软件可能只刷新 mtime。全量读过的文件再比一次内容,
        // 内容没变就允许继续写,避免 agent 陷入反复重读/重试。
        if (it->second.content.has_value() && *it->second.content == current_content) {
            return {ReadBaselineStatus::Ok, true};
        }
    } catch (...) {
        // Fall through to the conservative conflict path.
    }

    return {ReadBaselineStatus::ExternallyModified, false};
}

void MtimeTracker::record_write(const std::string& path) {
    try {
        const std::string key = normalize_tracker_path_key(path);
        auto mtime = std::filesystem::last_write_time(path_from_utf8(key));
        std::lock_guard<std::mutex> lk(mu_);
        records_[key] = Record{mtime, false, std::optional<std::string>{}, std::optional<FileReadEditMetadata>{}};
        remove_read_observations_for_path_locked(read_observations_, read_observation_lru_, key);
    } catch (...) {}
}

void MtimeTracker::record_write(const std::string& path, const std::string& content) {
    try {
        const std::string key = normalize_tracker_path_key(path);
        auto mtime = std::filesystem::last_write_time(path_from_utf8(key));
        std::lock_guard<std::mutex> lk(mu_);
        records_[key] = Record{mtime, false, std::optional<std::string>{content}, std::optional<FileReadEditMetadata>{}};
        remove_read_observations_for_path_locked(read_observations_, read_observation_lru_, key);
    } catch (...) {}
}

std::optional<FileReadEditMetadata> MtimeTracker::read_metadata(const std::string& path) const {
    const std::string key = normalize_tracker_path_key(path);
    std::lock_guard<std::mutex> lk(mu_);
    auto it = records_.find(key);
    if (it == records_.end()) return std::nullopt;
    return it->second.metadata;
}

MtimeTracker::FileWriteGuard MtimeTracker::acquire_write_guard(const std::string& path) {
    const std::string key = normalize_tracker_path_key(path);
    std::shared_ptr<std::mutex> file_mutex;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto& weak = file_locks_[key];
        file_mutex = weak.lock();
        if (!file_mutex) {
            file_mutex = std::make_shared<std::mutex>();
            weak = file_mutex;
        }
    }
    return FileWriteGuard{std::move(file_mutex)};
}

} // namespace acecode
