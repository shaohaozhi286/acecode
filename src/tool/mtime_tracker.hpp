#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <filesystem>
#include <optional>
#include <tuple>
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

    struct ReadObservationKey {
        std::string path;
        int start_line = 0;
        int end_line = 0;
        bool byte_mode = false;
        uint64_t byte_offset = 0;
        size_t max_bytes = 0;

        bool operator<(const ReadObservationKey& other) const {
            return std::tie(path, start_line, end_line, byte_mode,
                            byte_offset, max_bytes) <
                   std::tie(other.path, other.start_line, other.end_line,
                            other.byte_mode, other.byte_offset, other.max_bytes);
        }
        bool operator==(const ReadObservationKey& other) const {
            return path == other.path &&
                   start_line == other.start_line &&
                   end_line == other.end_line &&
                   byte_mode == other.byte_mode &&
                   byte_offset == other.byte_offset &&
                   max_bytes == other.max_bytes;
        }
    };

    struct ReadObservation {
        clock mtime;
        std::string tool_call_id;
        std::string persisted_output_path;
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

    // Seed a full-file baseline from resumed transcript history. The stored
    // mtime is intentionally stale so validation re-checks current content
    // before allowing a write.
    void seed_transcript_read_baseline(const std::string& path,
                                       const std::string& normalized_content,
                                       const FileReadEditMetadata& metadata);

    // Claude Code style read-observation cache: repeated file_read calls for
    // the same requested range can return a compact unchanged stub when the
    // file mtime has not changed since the previous actual read.
    bool has_unchanged_read_observation(
        const std::string& path,
        int start_line,
        int end_line,
        bool byte_mode = false,
        uint64_t byte_offset = 0,
        size_t max_bytes = 0
    ) const;
    std::optional<ReadObservation> unchanged_read_observation(
        const std::string& path,
        int start_line,
        int end_line,
        bool byte_mode = false,
        uint64_t byte_offset = 0,
        size_t max_bytes = 0
    ) const;
    void record_read_observation(const std::string& path,
                                 int start_line,
                                 int end_line,
                                 bool byte_mode = false,
                                 uint64_t byte_offset = 0,
                                 size_t max_bytes = 0);
    void record_read_observation_result(const std::string& path,
                                        int start_line,
                                        int end_line,
                                        const std::string& tool_call_id,
                                        const std::string& persisted_output_path,
                                        bool byte_mode = false,
                                        uint64_t byte_offset = 0,
                                        size_t max_bytes = 0);
    void invalidate_read_observations(const std::string& path);
    void clear_read_observations();

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
    std::map<ReadObservationKey, ReadObservation> read_observations_;
    std::vector<ReadObservationKey> read_observation_lru_;
    std::map<std::string, std::weak_ptr<std::mutex>> file_locks_;
};

} // namespace acecode
