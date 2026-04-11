#include "mtime_tracker.hpp"

namespace acecode {

MtimeTracker& MtimeTracker::instance() {
    static MtimeTracker tracker;
    return tracker;
}

void MtimeTracker::record_read(const std::string& path) {
    try {
        auto mtime = std::filesystem::last_write_time(path);
        std::lock_guard<std::mutex> lk(mu_);
        records_[path] = mtime;
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
        auto current_mtime = std::filesystem::last_write_time(path);
        return current_mtime != it->second;
    } catch (...) {
        return false; // file deleted or inaccessible
    }
}

void MtimeTracker::record_write(const std::string& path) {
    try {
        auto mtime = std::filesystem::last_write_time(path);
        std::lock_guard<std::mutex> lk(mu_);
        records_[path] = mtime;
    } catch (...) {}
}

} // namespace acecode
