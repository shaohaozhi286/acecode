#include "runtime_files.hpp"

#include "../config/config.hpp"
#include "../utils/atomic_file.hpp"
#include "../utils/constants.hpp"
#include "../utils/logger.hpp"
#include "../utils/utf8_path.hpp"
#include "platform.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#endif
#endif

namespace fs = std::filesystem;

namespace acecode::daemon {
namespace {

fs::path effective_run_dir_path(const std::string& run_dir) {
    return path_from_utf8(run_dir.empty() ? acecode::get_run_dir() : run_dir);
}

std::string ensure_run_dir_for(const std::string& run_dir) {
    std::string dir = run_dir.empty() ? acecode::get_run_dir() : run_dir;
    std::error_code ec;
    fs::create_directories(path_from_utf8(dir), ec);
    return dir;
}

std::string run_path_for(const std::string& run_dir, const char* fname) {
    return path_to_utf8(effective_run_dir_path(run_dir) / fname);
}

std::string run_path(const char* fname) {
    return run_path_for(std::string(), fname);
}

static std::optional<std::string> read_text(const std::string& path) {
    std::ifstream ifs(path_from_utf8(path), std::ios::binary);
    if (!ifs.is_open()) return std::nullopt;
    std::string s((std::istreambuf_iterator<char>(ifs)),
                   std::istreambuf_iterator<char>());
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

std::optional<std::int64_t> read_i64_path(const std::string& path) {
    auto s = read_text(path);
    if (!s) return std::nullopt;
    try { return static_cast<std::int64_t>(std::stoll(*s)); }
    catch (...) { return std::nullopt; }
}

std::optional<int> read_int_path(const std::string& path) {
    auto s = read_text(path);
    if (!s) return std::nullopt;
    try { return std::stoi(*s); }
    catch (...) { return std::nullopt; }
}

std::optional<Heartbeat> read_heartbeat_path(const std::string& path) {
    auto s = read_text(path);
    if (!s || s->empty()) return std::nullopt;
    try {
        auto j = nlohmann::json::parse(*s);
        Heartbeat hb;
        hb.pid          = j.value("pid", static_cast<std::int64_t>(0));
        hb.guid         = j.value("guid", std::string{});
        hb.timestamp_ms = j.value("timestamp_ms", static_cast<std::int64_t>(0));
        return hb;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<DesktopManagedRuntime> read_desktop_managed_runtime_path(
    const std::string& path) {
    auto text = read_text(path);
    if (!text || text->empty()) return std::nullopt;
    try {
        const auto j = nlohmann::json::parse(*text);
        if (!j.is_object()) return std::nullopt;
        DesktopManagedRuntime runtime;
        runtime.pid = j.value("pid", static_cast<std::int64_t>(0));
        runtime.guid = j.value("guid", std::string{});
        runtime.kind = j.value("kind", std::string{});
        runtime.protocol_version = j.value("protocol_version", 0);
        runtime.acecode_version = j.value("acecode_version", std::string{});
        if (runtime.pid <= 0 || runtime.guid.empty() || runtime.kind.empty() ||
            runtime.protocol_version <= 0) {
            return std::nullopt;
        }
        return runtime;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<DesktopOwnerRecord> read_desktop_owner_record_path(
    const std::string& path) {
    auto text = read_text(path);
    if (!text || text->empty()) return std::nullopt;
    try {
        const auto j = nlohmann::json::parse(*text);
        if (!j.is_object()) return std::nullopt;
        DesktopOwnerRecord owner;
        owner.pid = j.value("pid", static_cast<std::int64_t>(0));
        owner.instance_id = j.value("instance_id", std::string{});
        owner.timestamp_ms = j.value("timestamp_ms", static_cast<std::int64_t>(0));
        if (owner.pid <= 0 || owner.instance_id.empty() ||
            owner.timestamp_ms <= 0) {
            return std::nullopt;
        }
        return owner;
    } catch (...) {
        return std::nullopt;
    }
}

#ifdef _WIN32
DaemonProcessIdentity inspect_daemon_process_identity_impl(std::int64_t pid) {
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                             FALSE,
                             static_cast<DWORD>(pid));
    if (!h) return DaemonProcessIdentity::Unknown;

    std::vector<wchar_t> buffer(32768);
    DWORD size = static_cast<DWORD>(buffer.size());
    BOOL ok = ::QueryFullProcessImageNameW(h, 0, buffer.data(), &size);
    ::CloseHandle(h);
    if (!ok || size == 0) return DaemonProcessIdentity::Unknown;

    fs::path image(std::wstring(buffer.data(), size));
    std::wstring name = image.filename().wstring();
    std::transform(name.begin(), name.end(), name.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    if (name == L"acecode.exe" || name == L"acecode-daemon.exe") {
        return DaemonProcessIdentity::Match;
    }
    return DaemonProcessIdentity::Mismatch;
}

std::optional<std::int64_t> process_start_time_ms_impl(std::int64_t pid) {
    if (pid <= 0) return std::nullopt;
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                             FALSE,
                             static_cast<DWORD>(pid));
    if (!h) return std::nullopt;

    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    const BOOL ok = ::GetProcessTimes(h, &creation, &exit, &kernel, &user);
    ::CloseHandle(h);
    if (!ok) return std::nullopt;

    ULARGE_INTEGER ticks{};
    ticks.LowPart = creation.dwLowDateTime;
    ticks.HighPart = creation.dwHighDateTime;
    constexpr unsigned long long kWindowsToUnixEpoch100ns =
        116444736000000000ULL;
    if (ticks.QuadPart < kWindowsToUnixEpoch100ns) return std::nullopt;
    return static_cast<std::int64_t>(
        (ticks.QuadPart - kWindowsToUnixEpoch100ns) / 10000ULL);
}

struct WsaInit {
    WsaInit() {
        WSADATA d{};
        ::WSAStartup(MAKEWORD(2, 2), &d);
    }
};

WsaInit g_wsa_init;
#else
DaemonProcessIdentity inspect_daemon_process_identity_impl(std::int64_t pid) {
    if (pid <= 0) return DaemonProcessIdentity::Unknown;
    fs::path image;
#ifdef __APPLE__
    std::vector<char> buffer(PROC_PIDPATHINFO_MAXSIZE);
    const int length = ::proc_pidpath(
        static_cast<int>(pid), buffer.data(), static_cast<uint32_t>(buffer.size()));
    if (length <= 0) return DaemonProcessIdentity::Unknown;
    image = fs::path(std::string(buffer.data(), static_cast<std::size_t>(length)));
#elif defined(__linux__)
    std::vector<char> buffer(4096);
    const std::string proc_path = "/proc/" + std::to_string(pid) + "/exe";
    const ssize_t length = ::readlink(proc_path.c_str(), buffer.data(), buffer.size() - 1);
    if (length <= 0) return DaemonProcessIdentity::Unknown;
    buffer[static_cast<std::size_t>(length)] = '\0';
    image = fs::path(std::string(buffer.data(), static_cast<std::size_t>(length)));
#else
    return DaemonProcessIdentity::Unknown;
#endif
    std::string name = image.filename().string();
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
#ifdef __linux__
    const std::string deleted_suffix = " (deleted)";
    if (name.size() >= deleted_suffix.size() &&
        name.compare(
            name.size() - deleted_suffix.size(),
            deleted_suffix.size(),
            deleted_suffix) == 0) {
        name.erase(name.size() - deleted_suffix.size());
    }
#endif
    if (name == "acecode" || name == "acecode.exe" ||
        name == "acecode-daemon" || name == "acecode-daemon.exe") {
        return DaemonProcessIdentity::Match;
    }
    return DaemonProcessIdentity::Mismatch;
}

std::optional<std::int64_t> process_start_time_ms_impl(std::int64_t pid) {
    if (pid <= 0) return std::nullopt;
#ifdef __APPLE__
    proc_bsdinfo info{};
    const int size = ::proc_pidinfo(
        static_cast<int>(pid), PROC_PIDTBSDINFO, 0, &info, sizeof(info));
    if (size != static_cast<int>(sizeof(info)) || info.pbi_start_tvsec == 0) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(info.pbi_start_tvsec) * 1000 +
           static_cast<std::int64_t>(info.pbi_start_tvusec) / 1000;
#elif defined(__linux__)
    std::ifstream stat_file("/proc/" + std::to_string(pid) + "/stat");
    std::string stat_line;
    if (!std::getline(stat_file, stat_line)) return std::nullopt;
    const std::size_t command_end = stat_line.rfind(')');
    if (command_end == std::string::npos || command_end + 2 >= stat_line.size()) {
        return std::nullopt;
    }

    std::istringstream fields(stat_line.substr(command_end + 2));
    std::string value;
    unsigned long long start_ticks = 0;
    for (int field_number = 3; field_number <= 22; ++field_number) {
        if (!(fields >> value)) return std::nullopt;
        if (field_number == 22) {
            try {
                start_ticks = std::stoull(value);
            } catch (...) {
                return std::nullopt;
            }
        }
    }

    std::ifstream proc_stat("/proc/stat");
    std::string key;
    std::int64_t boot_time_seconds = 0;
    while (proc_stat >> key) {
        if (key == "btime") {
            if (!(proc_stat >> boot_time_seconds)) return std::nullopt;
            break;
        }
        std::string rest;
        std::getline(proc_stat, rest);
    }
    const long ticks_per_second = ::sysconf(_SC_CLK_TCK);
    if (boot_time_seconds <= 0 || ticks_per_second <= 0) return std::nullopt;
    return boot_time_seconds * 1000 +
           static_cast<std::int64_t>(
               (start_ticks * 1000ULL) /
               static_cast<unsigned long long>(ticks_per_second));
#else
    return std::nullopt;
#endif
}
#endif

std::string port_desc(int port) {
    return "port=" + std::to_string(port);
}

} // namespace

DaemonProcessIdentity inspect_daemon_process_identity(std::int64_t pid) {
    return inspect_daemon_process_identity_impl(pid);
}

std::optional<std::int64_t> process_start_time_ms(std::int64_t pid) {
    return process_start_time_ms_impl(pid);
}

bool runtime_pid_reuse_is_proven(
    const RuntimeSnapshot& snapshot,
    DaemonProcessIdentity process_identity,
    const std::optional<std::int64_t>& live_process_start_time_ms,
    std::int64_t timestamp_tolerance_ms) {
    if (process_identity == DaemonProcessIdentity::Mismatch) return true;
    if (!snapshot.pid.has_value() || !snapshot.heartbeat.has_value() ||
        snapshot.heartbeat->pid != *snapshot.pid ||
        snapshot.heartbeat->timestamp_ms <= 0 ||
        !live_process_start_time_ms.has_value() ||
        *live_process_start_time_ms <= 0) {
        return false;
    }
    const std::int64_t tolerance =
        std::max<std::int64_t>(0, timestamp_tolerance_ms);
    return *live_process_start_time_ms >
           snapshot.heartbeat->timestamp_ms + tolerance;
}

std::string ensure_run_dir() {
    return ensure_run_dir_for(std::string());
}

bool write_pid_file(std::int64_t pid) {
    return atomic_write_file(run_path(constants::RUN_FILE_PID), std::to_string(pid));
}
bool write_port_file(int port) {
    return atomic_write_file(run_path(constants::RUN_FILE_PORT), std::to_string(port));
}
bool write_guid_file(const std::string& guid) {
    return atomic_write_file(run_path(constants::RUN_FILE_GUID), guid);
}

std::optional<std::int64_t> read_pid_file() {
    return read_i64_path(run_path(constants::RUN_FILE_PID));
}
std::optional<int> read_port_file() {
    return read_int_path(run_path(constants::RUN_FILE_PORT));
}
std::optional<std::string> read_guid_file() {
    return read_text(run_path(constants::RUN_FILE_GUID));
}

bool write_heartbeat(const Heartbeat& hb) {
    nlohmann::json j;
    j["pid"]          = hb.pid;
    j["guid"]         = hb.guid;
    j["timestamp_ms"] = hb.timestamp_ms;
    return atomic_write_file(run_path(constants::RUN_FILE_HEARTBEAT), j.dump());
}

std::optional<Heartbeat> read_heartbeat() {
    return read_heartbeat_path(run_path(constants::RUN_FILE_HEARTBEAT));
}

bool write_token(const std::string& token) {
    return atomic_write_file(run_path(constants::RUN_FILE_TOKEN),
                              token,
                              /*restrict_permissions=*/true);
}

std::optional<std::string> read_token() {
    return read_text(run_path(constants::RUN_FILE_TOKEN));
}

bool write_desktop_managed_runtime(const DesktopManagedRuntime& runtime,
                                   const std::string& run_dir) {
    ensure_run_dir_for(run_dir);
    nlohmann::json j{
        {"pid", runtime.pid},
        {"guid", runtime.guid},
        {"kind", runtime.kind},
        {"protocol_version", runtime.protocol_version},
        {"acecode_version", runtime.acecode_version},
    };
    return atomic_write_file(
        run_path_for(run_dir, constants::RUN_FILE_DESKTOP_MANAGED),
        j.dump(),
        /*restrict_permissions=*/true);
}

std::optional<DesktopManagedRuntime> read_desktop_managed_runtime(
    const std::string& run_dir) {
    return read_desktop_managed_runtime_path(
        run_path_for(run_dir, constants::RUN_FILE_DESKTOP_MANAGED));
}

bool write_desktop_owner_record(const std::string& run_dir,
                                const DesktopOwnerRecord& owner) {
    ensure_run_dir_for(run_dir);
    nlohmann::json j{
        {"pid", owner.pid},
        {"instance_id", owner.instance_id},
        {"timestamp_ms", owner.timestamp_ms},
    };
    return atomic_write_file(
        run_path_for(run_dir, constants::RUN_FILE_DESKTOP_OWNER),
        j.dump(),
        /*restrict_permissions=*/true);
}

std::optional<DesktopOwnerRecord> read_desktop_owner_record(
    const std::string& run_dir) {
    return read_desktop_owner_record_path(
        run_path_for(run_dir, constants::RUN_FILE_DESKTOP_OWNER));
}

RuntimeSnapshot read_runtime_snapshot(const std::string& run_dir) {
    RuntimeSnapshot snapshot;
    snapshot.pid = read_i64_path(run_path_for(run_dir, constants::RUN_FILE_PID));
    snapshot.port = read_int_path(run_path_for(run_dir, constants::RUN_FILE_PORT));
    snapshot.guid = read_text(run_path_for(run_dir, constants::RUN_FILE_GUID));
    snapshot.heartbeat = read_heartbeat_path(run_path_for(run_dir, constants::RUN_FILE_HEARTBEAT));
    snapshot.token = read_text(run_path_for(run_dir, constants::RUN_FILE_TOKEN));
    snapshot.desktop_managed = read_desktop_managed_runtime_path(
        run_path_for(run_dir, constants::RUN_FILE_DESKTOP_MANAGED));
    return snapshot;
}

RuntimeReuseCheck validate_runtime_snapshot_for_reuse(
    const RuntimeSnapshot& snapshot,
    const RuntimeValidationOptions& options) {
    RuntimeReuseCheck check;
    if (!snapshot.pid.has_value() || *snapshot.pid <= 0) {
        check.reason = "missing or invalid daemon.pid";
        return check;
    }
    if (!snapshot.port.has_value() || *snapshot.port <= 0 || *snapshot.port > 65535) {
        check.reason = "missing or invalid daemon.port";
        return check;
    }
    if (options.require_token && (!snapshot.token.has_value() || snapshot.token->empty())) {
        check.reason = "missing token";
        return check;
    }
    if (!is_pid_alive(*snapshot.pid)) {
        check.reason = "recorded pid is not alive";
        return check;
    }
    if (!snapshot.heartbeat.has_value()) {
        check.reason = "missing heartbeat";
        return check;
    }
    if (snapshot.heartbeat->pid != *snapshot.pid) {
        std::ostringstream oss;
        oss << "heartbeat pid mismatch heartbeat=" << snapshot.heartbeat->pid
            << " pid=" << *snapshot.pid;
        check.reason = oss.str();
        return check;
    }
    if (snapshot.heartbeat->timestamp_ms <= 0) {
        check.reason = "invalid heartbeat timestamp";
        return check;
    }
    const std::int64_t now = options.now_ms > 0 ? options.now_ms : now_unix_ms();
    const std::int64_t age_ms = std::max<std::int64_t>(0, now - snapshot.heartbeat->timestamp_ms);
    if (options.heartbeat_timeout_ms > 0 && age_ms > options.heartbeat_timeout_ms) {
        std::ostringstream oss;
        oss << "stale heartbeat age_ms=" << age_ms
            << " timeout_ms=" << options.heartbeat_timeout_ms;
        check.reason = oss.str();
        return check;
    }
    if (options.require_port_probe && !probe_loopback_port(*snapshot.port)) {
        check.reason = "recorded " + port_desc(*snapshot.port) + " is not reachable";
        return check;
    }
    if (options.require_process_identity) {
        DaemonProcessIdentity identity =
            inspect_daemon_process_identity(*snapshot.pid);
        if (identity == DaemonProcessIdentity::Mismatch) {
            check.reason = "recorded pid is not an acecode daemon process";
            return check;
        }
        if (identity == DaemonProcessIdentity::Unknown) {
            check.reason = "recorded process executable identity is unavailable";
            return check;
        }
    }

    check.reusable = true;
    check.reason = "runtime files describe a reusable daemon";
    return check;
}

RuntimeReuseCheck validate_runtime_files_for_reuse(
    const std::string& run_dir,
    const RuntimeValidationOptions& options) {
    return validate_runtime_snapshot_for_reuse(read_runtime_snapshot(run_dir), options);
}

bool probe_loopback_port(int port) {
    if (port <= 0 || port > 65535) return false;
#ifdef _WIN32
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    DWORD tv = 500;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int rc = ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::closesocket(s);
    return rc == 0;
#else
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return false;

    int flags = ::fcntl(s, F_GETFL, 0);
    if (flags >= 0) ::fcntl(s, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int rc = ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc == 0) {
        ::close(s);
        return true;
    }
    if (errno != EINPROGRESS) {
        ::close(s);
        return false;
    }

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(s, &wfds);
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 500 * 1000;
    rc = ::select(s + 1, nullptr, &wfds, nullptr, &tv);
    bool ok = false;
    if (rc > 0 && FD_ISSET(s, &wfds)) {
        int err = 0;
        socklen_t len = sizeof(err);
        ok = (::getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0);
    }
    ::close(s);
    return ok;
#endif
}

bool process_is_acecode_daemon(std::int64_t pid) {
    return inspect_daemon_process_identity(pid) ==
           DaemonProcessIdentity::Match;
}

void cleanup_runtime_files() {
    std::error_code ec;
    fs::remove(path_from_utf8(run_path(constants::RUN_FILE_PID)), ec);
    fs::remove(path_from_utf8(run_path(constants::RUN_FILE_PORT)), ec);
    fs::remove(path_from_utf8(run_path(constants::RUN_FILE_HEARTBEAT)), ec);
    fs::remove(path_from_utf8(run_path(constants::RUN_FILE_TOKEN)), ec);
    fs::remove(path_from_utf8(run_path(constants::RUN_FILE_DESKTOP_MANAGED)), ec);
    // guid 故意保留: 事后追溯用
}

bool cleanup_runtime_files_if_owned(std::int64_t expected_pid,
                                    const std::string& expected_guid,
                                    const std::string& run_dir,
                                    bool remove_guid) {
    const auto snapshot = read_runtime_snapshot(run_dir);
    const bool pid_matches =
        snapshot.pid.has_value() && *snapshot.pid == expected_pid;
    const bool stopped_generation_matches =
        remove_guid && !snapshot.pid.has_value();
    if (!snapshot.guid.has_value() || *snapshot.guid != expected_guid ||
        (!pid_matches && !stopped_generation_matches)) {
        LOG_WARN("[daemon] skipped runtime cleanup for stale generation pid=" +
                 std::to_string(expected_pid) + " guid=" + expected_guid);
        return false;
    }

    std::error_code ec;
    const auto remove_file = [&](const char* name) {
        ec.clear();
        fs::remove(path_from_utf8(run_path_for(run_dir, name)), ec);
    };
    remove_file(constants::RUN_FILE_PID);
    remove_file(constants::RUN_FILE_PORT);
    remove_file(constants::RUN_FILE_HEARTBEAT);
    remove_file(constants::RUN_FILE_TOKEN);
    remove_file(constants::RUN_FILE_DESKTOP_MANAGED);
    if (remove_guid) remove_file(constants::RUN_FILE_GUID);
    return true;
}

} // namespace acecode::daemon
