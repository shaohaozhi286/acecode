#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <unordered_set>

namespace acecode {

class ActiveSessionPowerGuard {
public:
    using AcquireFn = std::function<bool(std::string*)>;
    using ReleaseFn = std::function<void()>;

    ActiveSessionPowerGuard(AcquireFn acquire, ReleaseFn release);

    void set_busy(const std::string& session_id, bool busy);
    void release_session(const std::string& session_id);
    void release_all();

    std::size_t busy_count() const;
    bool inhibitor_active() const;
    std::string last_error() const;

private:
    void acquire_locked();
    void release_locked();

    mutable std::mutex mu_;
    std::unordered_set<std::string> busy_sessions_;
    AcquireFn acquire_;
    ReleaseFn release_;
    bool inhibitor_active_ = false;
    bool acquire_attempted_for_epoch_ = false;
    std::string last_error_;
};

class SystemPowerInhibitor {
public:
    SystemPowerInhibitor() = default;
    ~SystemPowerInhibitor();

    SystemPowerInhibitor(const SystemPowerInhibitor&) = delete;
    SystemPowerInhibitor& operator=(const SystemPowerInhibitor&) = delete;

    bool acquire(std::string* error = nullptr);
    void release();
    bool active() const;

private:
    bool active_ = false;
#if defined(__APPLE__) || (defined(__unix__) && !defined(_WIN32))
    int helper_pid_ = -1;
#endif
};

ActiveSessionPowerGuard& process_power_guard();
void note_process_session_busy(const std::string& session_id, bool busy);
void release_process_session_power(const std::string& session_id);
void release_all_process_session_power();

} // namespace acecode
