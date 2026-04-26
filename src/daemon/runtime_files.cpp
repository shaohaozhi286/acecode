#include "runtime_files.hpp"

#include "../config/config.hpp"
#include "../utils/atomic_file.hpp"
#include "../utils/constants.hpp"
#include "../utils/logger.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace acecode::daemon {

std::string ensure_run_dir() {
    std::string dir = acecode::get_run_dir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

static std::string run_path(const char* fname) {
    return (fs::path(acecode::get_run_dir()) / fname).string();
}

static std::optional<std::string> read_text(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return std::nullopt;
    std::string s((std::istreambuf_iterator<char>(ifs)),
                   std::istreambuf_iterator<char>());
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
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
    auto s = read_text(run_path(constants::RUN_FILE_PID));
    if (!s) return std::nullopt;
    try { return static_cast<std::int64_t>(std::stoll(*s)); }
    catch (...) { return std::nullopt; }
}
std::optional<int> read_port_file() {
    auto s = read_text(run_path(constants::RUN_FILE_PORT));
    if (!s) return std::nullopt;
    try { return std::stoi(*s); }
    catch (...) { return std::nullopt; }
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
    auto s = read_text(run_path(constants::RUN_FILE_HEARTBEAT));
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

bool write_token(const std::string& token) {
    return atomic_write_file(run_path(constants::RUN_FILE_TOKEN),
                              token,
                              /*restrict_permissions=*/true);
}

std::optional<std::string> read_token() {
    return read_text(run_path(constants::RUN_FILE_TOKEN));
}

void cleanup_runtime_files() {
    std::error_code ec;
    fs::remove(run_path(constants::RUN_FILE_PID), ec);
    fs::remove(run_path(constants::RUN_FILE_PORT), ec);
    fs::remove(run_path(constants::RUN_FILE_HEARTBEAT), ec);
    fs::remove(run_path(constants::RUN_FILE_TOKEN), ec);
    // guid 故意保留: 事后追溯用
}

} // namespace acecode::daemon
