#pragma once

#include <cstddef>
#include <string>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <filesystem>
#include <optional>
#include <vector>

namespace acecode {

struct FileReadEditMetadata {
    std::string encoding;
    std::string line_ending;
    int start_line = 0;
    int end_line = 0;
    bool lossy = false;
    size_t lossy_replacement_count = 0;
};

// Tracks file modification times to detect concurrent external edits.
// Thread-safe: guarded by internal mutex.
class MtimeTracker {
public:
    using clock = std::filesystem::file_time_type;

    class FileWriteGuard {
    public:
        explicit FileWriteGuard(std::shared_ptr<std::mutex> mutex);
        FileWriteGuard(FileWriteGuard&&) noexcept = default;
        FileWriteGuard& operator=(FileWriteGuard&&) noexcept = delete;
        FileWriteGuard(const FileWriteGuard&) = delete;
        FileWriteGuard& operator=(const FileWriteGuard&) = delete;

    private:
        std::shared_ptr<std::mutex> mutex_;
        std::unique_lock<std::mutex> lock_;
    };

    enum class ReadBaselineStatus {
        Ok,
        NotRead,
        UnsafeRead,
        ExternallyModified
    };

    struct ReadBaselineCheck {
        ReadBaselineStatus status = ReadBaselineStatus::Ok;
        bool content_unchanged_after_mtime_change = false;
    };

    // Record the mtime of a file at the time it was read or observed by the agent.
    void record_read(const std::string& path);

    // Record a full or partial read. Full reads keep content so later edit checks can
    // distinguish real external changes from timestamp-only churn.
    void record_read(const std::string& path, const std::string& content, bool partial);
    void record_read(const std::string& path,
                     const std::string& normalized_content,
                     bool partial,
                     const FileReadEditMetadata& metadata);

    // Check if a file has been externally modified since the last recorded read.
    // Returns true if the file was modified externally (mtime changed).
    // Returns false if no record exists or mtime is unchanged.
    bool was_externally_modified(const std::string& path) const;

    // Require a prior non-lossy read baseline before editing. Full reads keep content
    // so timestamp-only churn can still be accepted; ranged reads are accepted only
    // while the recorded mtime is unchanged.
    ReadBaselineCheck validate_read_baseline_for_edit(
        const std::string& path,
        const std::string& current_content
    ) const;

    // Update the record after a successful write (so subsequent edits don't false-alarm).
    void record_write(const std::string& path);
    void record_write(const std::string& path, const std::string& content);

    std::optional<FileReadEditMetadata> read_metadata(const std::string& path) const;

    // Serialize validate-and-write sections per file so concurrent tool calls cannot
    // interleave after the stale-file check.
    FileWriteGuard acquire_write_guard(const std::string& path);

    static MtimeTracker& instance();

private:
    struct Record {
        clock mtime;
        bool partial = false;
        std::optional<std::string> content;
        std::optional<FileReadEditMetadata> metadata;
    };

    mutable std::mutex mu_;
    std::map<std::string, Record> records_;
    std::map<std::string, std::weak_ptr<std::mutex>> file_locks_;
};

} // namespace acecode
