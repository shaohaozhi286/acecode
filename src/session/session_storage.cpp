#include "session_storage.hpp"
#include "session_serializer.hpp"
#include "../config/config.hpp"
#include "../utils/atomic_file.hpp"
#include "../utils/cwd_hash.hpp"
#include "../utils/utf8_path.hpp"

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
#include <regex>
#include <mutex>
#include <iterator>

namespace fs = std::filesystem;

namespace acecode {

std::string SessionStorage::compute_project_hash(const std::string& cwd) {
    // 委托到 utils/cwd_hash.cpp 的共享实现 — desktop 的 WorkspaceRegistry 与
    // daemon 的 SessionStorage 必须用同一份算法,否则同一目录两边算出不同 hash。
    return acecode::compute_cwd_hash(cwd);
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
    return path_to_utf8(path_from_utf8(acecode_dir) / "projects" / hash);
}

void SessionStorage::append_message(const std::string& session_path, const ChatMessage& msg) {
    std::string record = serialize_message(msg);
    record.push_back('\n');

    static std::mutex append_mu;
    std::lock_guard<std::mutex> lk(append_mu);

    std::error_code ec;
    fs::create_directories(path_from_utf8(session_path).parent_path(), ec);

    std::ofstream ofs(path_from_utf8(session_path), std::ios::binary | std::ios::app);
    if (!ofs.is_open()) return;
    ofs.write(record.data(), static_cast<std::streamsize>(record.size()));
    ofs.flush();
}

void SessionStorage::write_messages(const std::string& session_path,
                                    const std::vector<ChatMessage>& messages) {
    std::error_code ec;
    fs::create_directories(path_from_utf8(session_path).parent_path(), ec);

    std::string content;
    for (const auto& msg : messages) {
        content += serialize_message(msg);
        content.push_back('\n');
    }
    atomic_write_file(session_path, content);
}

std::vector<ChatMessage> SessionStorage::load_messages(const std::string& session_path) {
    std::vector<ChatMessage> messages;
    std::ifstream ifs(path_from_utf8(session_path), std::ios::binary);
    if (!ifs.is_open()) return messages;

    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    size_t start = 0;
    while (start < content.size()) {
        const size_t nl = content.find('\n', start);
        if (nl == std::string::npos) {
            break; // trailing partial record
        }
        std::string line = content.substr(start, nl - start);
        start = nl + 1;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) continue;
        try {
            messages.push_back(deserialize_message(line));
        } catch (...) {
            // Skip malformed complete lines.
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
    if (!meta.model_preset.empty()) {
        j["model_preset"] = meta.model_preset;
    }
    if (!meta.title.empty()) {
        j["title"] = meta.title;
    }
    // fork 元数据:空字符串时省略,保持老 meta 文件 byte-byte 不变。
    if (!meta.forked_from.empty()) {
        j["forked_from"] = meta.forked_from;
    }
    if (!meta.fork_message_id.empty()) {
        j["fork_message_id"] = meta.fork_message_id;
    }
    if (meta.archived) {
        j["archived"] = true;
    }

    std::error_code ec;
    fs::create_directories(path_from_utf8(meta_path).parent_path(), ec);
    atomic_write_file(meta_path, j.dump(2) + '\n');
}

SessionMeta SessionStorage::read_meta(const std::string& meta_path) {
    SessionMeta meta;
    std::ifstream ifs(path_from_utf8(meta_path));
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
        meta.model_preset    = j.value("model_preset",    std::string{});
        meta.title           = j.value("title",           std::string{});
        meta.forked_from     = j.value("forked_from",     std::string{});
        meta.fork_message_id = j.value("fork_message_id", std::string{});
        meta.archived        = j.value("archived",        false);
    } catch (...) {
        // Return empty meta on parse failure
    }
    return meta;
}

// Canonical filename match:
//   group 1 = session_id (YYYYMMDD-HHMMSS-XXXX)
static const std::regex& session_filename_regex() {
    static const std::regex re(
        R"(^(\d{8}-\d{6}-[0-9a-f]{4})\.jsonl$)");
    return re;
}

static const std::regex& meta_filename_regex() {
    static const std::regex re(
        R"(^(\d{8}-\d{6}-[0-9a-f]{4})\.meta\.json$)");
    return re;
}

static const std::regex& pid_session_filename_regex() {
    static const std::regex re(
        R"(^(\d{8}-\d{6}-[0-9a-f]{4})-(\d+)\.jsonl$)");
    return re;
}

static const std::regex& pid_meta_filename_regex() {
    static const std::regex re(
        R"(^(\d{8}-\d{6}-[0-9a-f]{4})-(\d+)\.meta\.json$)");
    return re;
}

// 返回一个单调比较意义上的 mtime(单位是 file_clock tick;不是 unix epoch
// 也不需要是)。我们只用它做候选文件之间的排序,绝对时间没意义。失败返回 0。
static std::int64_t file_mtime_epoch(const fs::path& p) {
    std::error_code ec;
    auto ftime = fs::last_write_time(p, ec);
    if (ec) return 0;
    return static_cast<std::int64_t>(ftime.time_since_epoch().count());
}

size_t utf8_safe_prefix_length_storage(const std::string& text, size_t max_bytes) {
    if (text.size() <= max_bytes) return text.size();
    size_t i = max_bytes;
    while (i > 0 && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) {
        --i;
    }
    return i;
}

std::string extract_storage_summary(const std::string& content) {
    constexpr size_t max_summary_bytes = 80;
    constexpr size_t min_word_break_bytes = 60;
    if (content.size() <= max_summary_bytes) return content;

    const size_t safe_limit = utf8_safe_prefix_length_storage(content, max_summary_bytes);
    if (safe_limit == 0) return "...";
    size_t cut = safe_limit;
    while (cut > min_word_break_bytes && content[cut - 1] != ' ') {
        --cut;
    }
    if (cut <= min_word_break_bytes) cut = safe_limit;
    return content.substr(0, cut) + "...";
}

bool is_visible_history_message(const ChatMessage& msg) {
    if (msg.is_meta || msg.is_compact_summary) return false;
    if (!msg.content.empty()) return true;
    if (!msg.tool_calls.is_null() && !msg.tool_calls.empty()) return true;
    return false;
}

void enrich_meta_from_messages(const std::string& project_dir,
                               const std::string& session_id,
                               SessionMeta& meta) {
    if (meta.message_count > 0 && !meta.summary.empty()) return;

    const auto path = SessionStorage::session_path(project_dir, session_id);
    auto messages = SessionStorage::load_messages(path);
    if (messages.empty()) return;

    int visible_count = 0;
    std::string latest_user;
    for (const auto& msg : messages) {
        if (!is_visible_history_message(msg)) continue;
        ++visible_count;
        if (msg.role == "user" && !msg.content.empty()) {
            latest_user = msg.content;
        }
    }
    if (meta.message_count <= 0) meta.message_count = visible_count;
    if (meta.summary.empty() && !latest_user.empty()) {
        meta.summary = extract_storage_summary(latest_user);
    }
}

std::vector<SessionStorage::SessionFileCandidate>
SessionStorage::find_session_files(const std::string& project_dir,
                                    const std::string& session_id) {
    std::vector<SessionFileCandidate> result;
    fs::path project_path = path_from_utf8(project_dir);
    if (session_id.empty() || !fs::exists(project_path) || !fs::is_directory(project_path)) {
        return result;
    }

    const fs::path jsonl = path_from_utf8(SessionStorage::session_path(project_dir, session_id));
    std::error_code ec;
    if (!fs::is_regular_file(jsonl, ec)) {
        return result;
    }

    SessionFileCandidate c;
    c.jsonl_path = path_to_utf8(jsonl);
    c.meta_path = SessionStorage::meta_path(project_dir, session_id);
    c.pid = 0;
    c.mtime = file_mtime_epoch(jsonl);
    result.push_back(std::move(c));
    return result;
}

bool SessionStorage::has_incompatible_pid_session_files(
    const std::string& project_dir, const std::string& session_id) {
    fs::path project_path = path_from_utf8(project_dir);
    if (!fs::exists(project_path) || !fs::is_directory(project_path)) {
        return false;
    }

    const auto& jsonl_re = pid_session_filename_regex();
    const auto& meta_re = pid_meta_filename_regex();
    for (const auto& entry : fs::directory_iterator(project_path)) {
        if (!entry.is_regular_file()) continue;
        std::string fname = path_to_utf8(entry.path().filename());
        std::smatch m;
        if (std::regex_match(fname, m, jsonl_re) ||
            std::regex_match(fname, m, meta_re)) {
            if (session_id.empty() || m[1].str() == session_id) {
                return true;
            }
        }
    }
    return false;
}

std::vector<SessionMeta> SessionStorage::list_sessions(const std::string& project_dir) {
    std::vector<SessionMeta> sessions;
    fs::path project_path = path_from_utf8(project_dir);
    if (!fs::exists(project_path) || !fs::is_directory(project_path)) {
        return sessions;
    }

    const auto& re = meta_filename_regex();
    for (const auto& entry : fs::directory_iterator(project_path)) {
        if (!entry.is_regular_file()) continue;
        std::string fname = path_to_utf8(entry.path().filename());
        std::smatch m;
        if (!std::regex_match(fname, m, re)) continue;
        std::string id = m[1].str();

        SessionMeta meta = read_meta(path_to_utf8(entry.path()));
        if (meta.id.empty()) continue;
        enrich_meta_from_messages(project_dir, id, meta);
        sessions.push_back(std::move(meta));
    }

    std::sort(sessions.begin(), sessions.end(),
        [](const SessionMeta& a, const SessionMeta& b) {
            return a.updated_at > b.updated_at;
        });
    return sessions;
}

static std::string make_session_path(const std::string& project_dir,
                                     const std::string& session_id,
                                     const std::string& suffix,
                                     int pid) {
    std::string fname = session_id;
    if (pid > 0) {
        fname += '-';
        fname += std::to_string(pid);
    }
    fname += suffix;
    return path_to_utf8(path_from_utf8(project_dir) / fname);
}

std::string SessionStorage::session_path(const std::string& project_dir,
                                          const std::string& session_id,
                                          int pid) {
    return make_session_path(project_dir, session_id, ".jsonl", pid);
}

std::string SessionStorage::meta_path(const std::string& project_dir,
                                       const std::string& session_id,
                                       int pid) {
    return make_session_path(project_dir, session_id, ".meta.json", pid);
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
