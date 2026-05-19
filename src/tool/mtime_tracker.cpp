#include "mtime_tracker.hpp"
#include "utils/utf8_path.hpp"

namespace acecode {

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
        auto mtime = std::filesystem::last_write_time(path_from_utf8(path));
        std::lock_guard<std::mutex> lk(mu_);
        records_[path] = Record{
            mtime,
            partial,
            partial ? std::optional<std::string>{} : std::optional<std::string>{normalized_content},
            metadata.read_id.empty() ? std::optional<FileReadEditMetadata>{}
                                     : std::optional<FileReadEditMetadata>{metadata}
        };
    } catch (...) {
        // File may not exist yet; that's OK
    }
}

bool MtimeTracker::was_externally_modified(const std::string& path) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = records_.find(path);
    if (it == records_.end()) {
        return false; // no record, assume safe
    }
    try {
        auto current_mtime = std::filesystem::last_write_time(path_from_utf8(path));
        return current_mtime != it->second.mtime;
    } catch (...) {
        return false; // file deleted or inaccessible
    }
}

MtimeTracker::FullReadCheck MtimeTracker::validate_full_read_for_edit(
    const std::string& path,
    const std::string& current_content
) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = records_.find(path);
    if (it == records_.end()) {
        return {FullReadStatus::NotRead, false};
    }
    if (it->second.partial) {
        return {FullReadStatus::PartialRead, false};
    }

    try {
        auto current_mtime = std::filesystem::last_write_time(path_from_utf8(path));
        if (current_mtime == it->second.mtime) {
            return {FullReadStatus::Ok, false};
        }

        // Windows/同步盘/杀毒软件可能只刷新 mtime。全量读过的文件再比一次内容,
        // 内容没变就允许继续写,避免 agent 陷入反复重读/重试。
        if (it->second.content.has_value() && *it->second.content == current_content) {
            return {FullReadStatus::Ok, true};
        }
    } catch (...) {
        // Fall through to the conservative conflict path.
    }

    return {FullReadStatus::ExternallyModified, false};
}

void MtimeTracker::record_write(const std::string& path) {
    try {
        auto mtime = std::filesystem::last_write_time(path_from_utf8(path));
        std::lock_guard<std::mutex> lk(mu_);
        records_[path] = Record{mtime, false, std::optional<std::string>{}, std::optional<FileReadEditMetadata>{}};
    } catch (...) {}
}

void MtimeTracker::record_write(const std::string& path, const std::string& content) {
    try {
        auto mtime = std::filesystem::last_write_time(path_from_utf8(path));
        std::lock_guard<std::mutex> lk(mu_);
        records_[path] = Record{mtime, false, std::optional<std::string>{content}, std::optional<FileReadEditMetadata>{}};
    } catch (...) {}
}

std::optional<FileReadEditMetadata> MtimeTracker::read_metadata(const std::string& path) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = records_.find(path);
    if (it == records_.end()) return std::nullopt;
    return it->second.metadata;
}

} // namespace acecode
