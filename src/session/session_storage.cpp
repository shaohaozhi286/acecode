#include "session_storage.hpp"
#include "session_serializer.hpp"
#include "../config/config.hpp"
#include "../daemon/platform.hpp"
#include "../utils/cwd_hash.hpp"

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
#include <unordered_map>

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

// 文件名匹配:
//   group 1 = session_id (YYYYMMDD-HHMMSS-XXXX)
//   group 3 = pid (纯数字,可选;不存在表示旧格式)
// XXXX 是 4 个 hex 字符,可能含 a-f,所以 id 内部不会被误识别为 pid 段。
static const std::regex& session_filename_regex() {
    static const std::regex re(
        R"(^(\d{8}-\d{6}-[0-9a-f]{4})(-(\d+))?\.jsonl$)");
    return re;
}

static const std::regex& meta_filename_regex() {
    static const std::regex re(
        R"(^(\d{8}-\d{6}-[0-9a-f]{4})(-(\d+))?\.meta\.json$)");
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
                               int pid,
                               SessionMeta& meta) {
    if (meta.message_count > 0 && !meta.summary.empty()) return;

    const auto path = SessionStorage::session_path(project_dir, session_id, pid);
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
    if (!fs::exists(project_dir) || !fs::is_directory(project_dir)) {
        return result;
    }

    const auto& re = session_filename_regex();
    for (const auto& entry : fs::directory_iterator(project_dir)) {
        if (!entry.is_regular_file()) continue;
        std::string fname = entry.path().filename().string();
        std::smatch m;
        if (!std::regex_match(fname, m, re)) continue;
        if (m[1].str() != session_id) continue;

        SessionFileCandidate c;
        c.jsonl_path = entry.path().string();
        c.pid = m[3].matched ? std::stoi(m[3].str()) : 0;
        c.meta_path = SessionStorage::meta_path(project_dir, session_id, c.pid);
        c.mtime = file_mtime_epoch(entry.path());
        result.push_back(std::move(c));
    }

    std::sort(result.begin(), result.end(),
        [](const SessionFileCandidate& a, const SessionFileCandidate& b) {
            return a.mtime > b.mtime;
        });
    return result;
}

std::vector<SessionMeta> SessionStorage::list_sessions(const std::string& project_dir) {
    std::vector<SessionMeta> sessions;
    if (!fs::exists(project_dir) || !fs::is_directory(project_dir)) {
        return sessions;
    }

    // 同一 session_id 可能有多份 pid 后缀的 meta(daemon + TUI 各跑一份)。
    // 按 id 分组,每个 id 只保留 mtime 最新那份。
    struct Entry {
        SessionMeta meta;
        std::int64_t mtime = 0;
    };
    std::unordered_map<std::string, Entry> by_id;

    const auto& re = meta_filename_regex();
    for (const auto& entry : fs::directory_iterator(project_dir)) {
        if (!entry.is_regular_file()) continue;
        std::string fname = entry.path().filename().string();
        std::smatch m;
        if (!std::regex_match(fname, m, re)) continue;
        std::string id = m[1].str();
        int pid = m[3].matched ? std::stoi(m[3].str()) : 0;

        SessionMeta meta = read_meta(entry.path().string());
        if (meta.id.empty()) continue;
        enrich_meta_from_messages(project_dir, id, pid, meta);
        std::int64_t mtime = file_mtime_epoch(entry.path());

        auto it = by_id.find(id);
        if (it == by_id.end() || mtime > it->second.mtime) {
            by_id[id] = Entry{std::move(meta), mtime};
        }
    }

    sessions.reserve(by_id.size());
    for (auto& [_, e] : by_id) sessions.push_back(std::move(e.meta));

    std::sort(sessions.begin(), sessions.end(),
        [](const SessionMeta& a, const SessionMeta& b) {
            return a.updated_at > b.updated_at;
        });
    return sessions;
}

static std::string make_path_with_pid(const std::string& project_dir,
                                       const std::string& session_id,
                                       const std::string& suffix,
                                       int pid) {
    if (pid < 0) {
        // 默认: 用本进程 pid
        pid = static_cast<int>(acecode::daemon::current_pid());
    }
    std::string fname = session_id;
    if (pid > 0) {
        fname += '-';
        fname += std::to_string(pid);
    }
    fname += suffix;
    return (fs::path(project_dir) / fname).string();
}

std::string SessionStorage::session_path(const std::string& project_dir,
                                          const std::string& session_id,
                                          int pid) {
    return make_path_with_pid(project_dir, session_id, ".jsonl", pid);
}

std::string SessionStorage::meta_path(const std::string& project_dir,
                                       const std::string& session_id,
                                       int pid) {
    return make_path_with_pid(project_dir, session_id, ".meta.json", pid);
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
