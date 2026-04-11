#pragma once

#include <string>
#include <chrono>
#include <map>
#include <mutex>
#include <filesystem>

namespace acecode {

// Tracks file modification times to detect concurrent external edits.
// Thread-safe: guarded by internal mutex.
class MtimeTracker {
public:
    using clock = std::filesystem::file_time_type;

    // Record the mtime of a file at the time it was read by the agent.
    void record_read(const std::string& path);

    // Check if a file has been externally modified since the last recorded read.
    // Returns true if the file was modified externally (mtime changed).
    // Returns false if no record exists or mtime is unchanged.
    bool was_externally_modified(const std::string& path) const;

    // Update the record after a successful write (so subsequent edits don't false-alarm).
    void record_write(const std::string& path);

    static MtimeTracker& instance();

private:
    mutable std::mutex mu_;
    std::map<std::string, clock> records_;
};

} // namespace acecode
