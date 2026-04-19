#include "session_storage.hpp"
#include "session_serializer.hpp"
#include "../config/config.hpp"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <random>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <functional>

namespace fs = std::filesystem;

namespace acecode {

// Simple FNV-1a 64-bit hash (no external dependency needed)
static uint64_t fnv1a_64(const std::string& data) {
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : data) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string SessionStorage::compute_project_hash(const std::string& cwd) {
    // Normalize: forward slashes, lowercase
    std::string normalized = cwd;
    for (auto& c : normalized) {
        if (c == '\\') c = '/';
    }
    // Lowercase for case-insensitive filesystems (Windows)
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Remove trailing slash
    while (normalized.size() > 1 && normalized.back() == '/') {
        normalized.pop_back();
    }

    uint64_t hash = fnv1a_64(normalized);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << hash;
    return oss.str();
}

std::string SessionStorage::generate_session_id() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &time_t_now);
#else
    gmtime_r(&time_t_now, &tm_buf);
#endif

    std::ostringstream oss;
    oss << std::setfill('0')
        << std::setw(4) << (tm_buf.tm_year + 1900)
        << std::setw(2) << (tm_buf.tm_mon + 1)
        << std::setw(2) << tm_buf.tm_mday
        << '-'
        << std::setw(2) << tm_buf.tm_hour
        << std::setw(2) << tm_buf.tm_min
        << std::setw(2) << tm_buf.tm_sec
        << '-';

    // 4 hex random chars
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 0xFFFF);
    oss << std::hex << std::setw(4) << dist(gen);

    return oss.str();
}

std::string SessionStorage::get_project_dir(const std::string& cwd) {
    std::string acecode_dir = get_acecode_dir();
    std::string hash = compute_project_hash(cwd);
    return (fs::path(acecode_dir) / "projects" / hash).string();
}

void SessionStorage::append_message(const std::string& session_path, const ChatMessage& msg) {
    std::string line = serialize_message(msg);
    std::ofstream ofs(session_path, std::ios::app);
    if (ofs.is_open()) {
        ofs << line << '\n';
        ofs.flush();
    }
}

std::vector<ChatMessage> SessionStorage::load_messages(const std::string& session_path) {
    std::vector<ChatMessage> messages;
    std::ifstream ifs(session_path);
    if (!ifs.is_open()) return messages;

    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        try {
            messages.push_back(deserialize_message(line));
        } catch (...) {
            // Skip unparseable lines (crash protection for truncated last line)
        }
    }
    return messages;
}

void SessionStorage::write_meta(const std::string& meta_path, const SessionMeta& meta) {
    nlohmann::json j;
    j["id"] = meta.id;
    j["cwd"] = meta.cwd;
    j["created_at"] = meta.created_at;
    j["updated_at"] = meta.updated_at;
    j["message_count"] = meta.message_count;
    j["summary"] = meta.summary;
    j["provider"] = meta.provider;
    j["model"] = meta.model;
    if (!meta.title.empty()) {
        j["title"] = meta.title;
    }

    std::ofstream ofs(meta_path);
    if (ofs.is_open()) {
        ofs << j.dump(2) << '\n';
    }
}

SessionMeta SessionStorage::read_meta(const std::string& meta_path) {
    SessionMeta meta;
    std::ifstream ifs(meta_path);
    if (!ifs.is_open()) return meta;

    try {
        nlohmann::json j = nlohmann::json::parse(ifs);
        if (j.contains("id"))            meta.id            = j["id"].get<std::string>();
        if (j.contains("cwd"))           meta.cwd           = j["cwd"].get<std::string>();
        if (j.contains("created_at"))    meta.created_at    = j["created_at"].get<std::string>();
        if (j.contains("updated_at"))    meta.updated_at    = j["updated_at"].get<std::string>();
        if (j.contains("message_count")) meta.message_count = j["message_count"].get<int>();
        if (j.contains("summary"))       meta.summary       = j["summary"].get<std::string>();
        if (j.contains("provider"))      meta.provider      = j["provider"].get<std::string>();
        if (j.contains("model"))         meta.model         = j["model"].get<std::string>();
        meta.title = j.value("title", std::string{});
    } catch (...) {
        // Return empty meta on parse failure
    }
    return meta;
}

std::vector<SessionMeta> SessionStorage::list_sessions(const std::string& project_dir) {
    std::vector<SessionMeta> sessions;
    if (!fs::exists(project_dir) || !fs::is_directory(project_dir)) {
        return sessions;
    }

    for (const auto& entry : fs::directory_iterator(project_dir)) {
        if (!entry.is_regular_file()) continue;
        std::string filename = entry.path().filename().string();
        // Look for *.meta.json files
        if (filename.size() > 10 && filename.substr(filename.size() - 10) == ".meta.json") {
            SessionMeta meta = read_meta(entry.path().string());
            if (!meta.id.empty()) {
                sessions.push_back(meta);
            }
        }
    }

    // Sort by updated_at descending (most recent first)
    std::sort(sessions.begin(), sessions.end(),
        [](const SessionMeta& a, const SessionMeta& b) {
            return a.updated_at > b.updated_at;
        });

    return sessions;
}

std::string SessionStorage::session_path(const std::string& project_dir, const std::string& session_id) {
    return (fs::path(project_dir) / (session_id + ".jsonl")).string();
}

std::string SessionStorage::meta_path(const std::string& project_dir, const std::string& session_id) {
    return (fs::path(project_dir) / (session_id + ".meta.json")).string();
}

std::string SessionStorage::now_iso8601() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &time_t_now);
#else
    gmtime_r(&time_t_now, &tm_buf);
#endif

    std::ostringstream oss;
    oss << std::setfill('0')
        << std::setw(4) << (tm_buf.tm_year + 1900)
        << '-' << std::setw(2) << (tm_buf.tm_mon + 1)
        << '-' << std::setw(2) << tm_buf.tm_mday
        << 'T' << std::setw(2) << tm_buf.tm_hour
        << ':' << std::setw(2) << tm_buf.tm_min
        << ':' << std::setw(2) << tm_buf.tm_sec
        << 'Z';
    return oss.str();
}

} // namespace acecode
