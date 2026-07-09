#include "power_inhibitor.hpp"

#include "logger.hpp"

#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#if defined(__APPLE__) || (defined(__unix__) && !defined(_WIN32))
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace acecode {

namespace {

std::string normalize_session_id(const std::string& session_id) {
    return session_id.empty() ? std::string{"default"} : session_id;
}

#if defined(__unix__) && !defined(__APPLE__) && !defined(_WIN32)
bool executable_on_path(const std::string& name) {
    const char* raw_path = std::getenv("PATH");
    if (!raw_path || name.empty()) return false;
    std::string path(raw_path);
    std::size_t pos = 0;
    while (pos <= path.size()) {
        const std::size_t next = path.find(':', pos);
        std::string dir = path.substr(pos, next == std::string::npos
            ? std::string::npos
            : next - pos);
        if (dir.empty()) dir = ".";
        std::filesystem::path candidate = std::filesystem::path(dir) / name;
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) && !ec &&
            ::access(candidate.c_str(), X_OK) == 0) {
            return true;
        }
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    return false;
}
#endif

} // namespace

ActiveSessionPowerGuard::ActiveSessionPowerGuard(AcquireFn acquire, ReleaseFn release)
    : acquire_(std::move(acquire)), release_(std::move(release)) {}

void ActiveSessionPowerGuard::set_busy(const std::string& session_id, bool busy) {
    std::lock_guard<std::mutex> lk(mu_);
    const std::string key = normalize_session_id(session_id);
    if (busy) {
        const bool was_empty = busy_sessions_.empty();
        busy_sessions_.insert(key);
        if (was_empty && !inhibitor_active_ && !acquire_attempted_for_epoch_) {
            acquire_locked();
        }
        return;
    }

    busy_sessions_.erase(key);
    if (busy_sessions_.empty()) {
        release_locked();
    }
}

void ActiveSessionPowerGuard::release_session(const std::string& session_id) {
    set_busy(session_id, false);
}

void ActiveSessionPowerGuard::release_all() {
    std::lock_guard<std::mutex> lk(mu_);
    busy_sessions_.clear();
    release_locked();
}

std::size_t ActiveSessionPowerGuard::busy_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return busy_sessions_.size();
}

bool ActiveSessionPowerGuard::inhibitor_active() const {
    std::lock_guard<std::mutex> lk(mu_);
    return inhibitor_active_;
}

std::string ActiveSessionPowerGuard::last_error() const {
    std::lock_guard<std::mutex> lk(mu_);
    return last_error_;
}

void ActiveSessionPowerGuard::acquire_locked() {
    acquire_attempted_for_epoch_ = true;
    last_error_.clear();
    std::string error;
    const bool ok = acquire_ ? acquire_(&error) : false;
    inhibitor_active_ = ok;
    if (!ok) {
        last_error_ = error.empty()
            ? std::string{"power inhibitor unavailable"}
            : error;
        LOG_WARN("[power] failed to acquire sleep inhibitor: " + last_error_);
    } else {
        LOG_INFO("[power] acquired sleep inhibitor");
    }
}

void ActiveSessionPowerGuard::release_locked() {
    if (inhibitor_active_ && release_) {
        release_();
        LOG_INFO("[power] released sleep inhibitor");
    }
    inhibitor_active_ = false;
    acquire_attempted_for_epoch_ = false;
    last_error_.clear();
}

SystemPowerInhibitor::~SystemPowerInhibitor() {
    release();
}

bool SystemPowerInhibitor::acquire(std::string* error) {
    if (active_) return true;
#ifdef _WIN32
    EXECUTION_STATE previous = ::SetThreadExecutionState(
        ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
    if (previous == 0) {
        if (error) *error = "SetThreadExecutionState failed";
        return false;
    }
    active_ = true;
    return true;
#elif defined(__APPLE__)
    const std::string parent_pid = std::to_string(static_cast<long long>(::getpid()));
    const pid_t pid = ::fork();
    if (pid < 0) {
        if (error) *error = "fork failed while starting caffeinate";
        return false;
    }
    if (pid == 0) {
        ::execl("/usr/bin/caffeinate",
                "caffeinate",
                "-dims",
                "-w",
                parent_pid.c_str(),
                static_cast<char*>(nullptr));
        _exit(127);
    }
    helper_pid_ = static_cast<int>(pid);
    active_ = true;
    return true;
#elif defined(__unix__)
    if (!executable_on_path("systemd-inhibit") || !executable_on_path("sleep")) {
        if (error) *error = "systemd-inhibit or sleep not found on PATH";
        return false;
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        if (error) *error = "fork failed while starting systemd-inhibit";
        return false;
    }
    if (pid == 0) {
        ::execlp("systemd-inhibit",
                 "systemd-inhibit",
                 "--what=idle:sleep",
                 "--mode=block",
                 "--who=ACECode",
                 "--why=ACECode session is running",
                 "sleep",
                 "infinity",
                 static_cast<char*>(nullptr));
        _exit(127);
    }
    helper_pid_ = static_cast<int>(pid);
    active_ = true;
    return true;
#else
    if (error) *error = "unsupported platform";
    return false;
#endif
}

void SystemPowerInhibitor::release() {
    if (!active_) return;
#ifdef _WIN32
    ::SetThreadExecutionState(ES_CONTINUOUS);
#elif defined(__APPLE__) || (defined(__unix__) && !defined(_WIN32))
    if (helper_pid_ > 0) {
        ::kill(static_cast<pid_t>(helper_pid_), SIGTERM);
        int status = 0;
        (void)::waitpid(static_cast<pid_t>(helper_pid_), &status, 0);
        helper_pid_ = -1;
    }
#endif
    active_ = false;
}

bool SystemPowerInhibitor::active() const {
    return active_;
}

ActiveSessionPowerGuard& process_power_guard() {
    static SystemPowerInhibitor inhibitor;
    static ActiveSessionPowerGuard guard(
        [](std::string* error) { return inhibitor.acquire(error); },
        [] { inhibitor.release(); });
    return guard;
}

void note_process_session_busy(const std::string& session_id, bool busy) {
    process_power_guard().set_busy(session_id, busy);
}

void release_process_session_power(const std::string& session_id) {
    process_power_guard().release_session(session_id);
}

void release_all_process_session_power() {
    process_power_guard().release_all();
}

} // namespace acecode
