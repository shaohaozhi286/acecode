#include "pty_session_registry.hpp"

#include "../../utils/encoding.hpp"
#include "../../utils/logger.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

namespace acecode {

std::string encode_pty_control_frame(const std::string& json_payload) {
    std::string out;
    out.reserve(json_payload.size() + 1);
    out.push_back('\0');
    out += json_payload;
    return out;
}

namespace {

std::string exit_frame(int exit_code) {
    return encode_pty_control_frame(
        nlohmann::json{{"exit_code", exit_code}}.dump());
}

std::string cursor_frame(std::uint64_t cursor) {
    return encode_pty_control_frame(
        nlohmann::json{{"cursor", cursor}}.dump());
}

} // namespace

PtySessionRegistry::PtySessionRegistry(PtyBackendKind backend,
                                       std::string default_cwd,
                                       std::string configured_shell)
    : backend_(backend)
    , default_cwd_(std::move(default_cwd))
    , shell_(resolve_console_shell(configured_shell)) {}

PtySessionRegistry::~PtySessionRegistry() { stop_all(); }

std::optional<PtySessionInfo> PtySessionRegistry::create(
    const std::string& cwd_override, const std::string& title,
    const std::string& shell_override, std::string& error) {
    std::unique_lock<std::mutex> lock(mu_);

    if (sessions_.size() >= static_cast<std::size_t>(kPtyMaxSessions)) {
        error = "session limit reached (" + std::to_string(kPtyMaxSessions) + ")";
        return std::nullopt;
    }

    std::string id = "pty-" + std::to_string(next_id_++);
    const std::string shell = shell_override.empty() ? shell_ : shell_override;

    PtySpawnSpec spec;
    spec.shell = shell;
    spec.cwd = cwd_override.empty() ? default_cwd_ : cwd_override;

    PtyCallbacks callbacks;
    callbacks.on_data = [this, id](const std::string& data) {
        on_pty_data(id, data);
    };
    callbacks.on_exit = [this, id](int exit_code) {
        on_pty_exit(id, exit_code);
    };

    // spawn 在锁内:on_data/on_exit 回调会回抢 mu_,但 spawn 期间读线程刚
    // 起步,第一段输出到达前 spawn 已返回(管道缓冲兜底,无死锁窗口 — 回调
    // 拿锁等待本函数退出即可)。
    std::string spawn_error;
    auto process = spawn_pty(backend_, spec, std::move(callbacks), spawn_error);
    if (!process) {
        error = spawn_error;
        return std::nullopt;
    }

    auto session = std::make_unique<Session>();
    session->info.id = id;
    session->info.title = title.empty()
        ? ("Terminal " + std::to_string(next_id_ - 1)) : title;
    session->info.shell = shell;
    session->info.cwd = spec.cwd;
    session->info.status = "running";
    session->info.pid = process->pid();
    session->info.backend = process->kind();
    session->process = std::move(process);

    PtySessionInfo info = session->info;
    sessions_[id] = std::move(session);
    LOG_INFO("[pty] created session " + id + " shell=" + shell +
             " backend=" + pty_backend_kind_name(info.backend));
    return info;
}

std::vector<PtySessionInfo> PtySessionRegistry::list() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<PtySessionInfo> out;
    out.reserve(sessions_.size());
    for (const auto& [id, session] : sessions_) out.push_back(session->info);
    return out;
}

std::optional<PtySessionInfo> PtySessionRegistry::get(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return std::nullopt;
    return it->second->info;
}

bool PtySessionRegistry::remove(const std::string& id) {
    std::unique_ptr<Session> victim;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = sessions_.find(id);
        if (it == sessions_.end()) return false;
        victim = std::move(it->second);
        sessions_.erase(it);
    }
    // kill 在锁外:它要 join 读线程,而读线程的 on_data/on_exit 会抢 mu_,
    // 锁内 kill 会死锁。会话已摘出 map,回调进来查不到 id,安全 no-op。
    if (victim->process) victim->process->kill();
    LOG_INFO("[pty] removed session " + id);
    return true;
}

bool PtySessionRegistry::resize(const std::string& id, int cols, int rows) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return false;
    if (it->second->info.status != "running") return true;  // 幂等吞掉
    it->second->process->resize(cols, rows);
    return true;
}

bool PtySessionRegistry::set_title(const std::string& id,
                                   const std::string& title) {
    // 去首尾空白;全空白的标题(程序发空 OSC)不覆盖现有标题。
    std::string trimmed = title;
    auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    trimmed.erase(trimmed.begin(),
                  std::find_if(trimmed.begin(), trimmed.end(), not_space));
    trimmed.erase(std::find_if(trimmed.rbegin(), trimmed.rend(), not_space).base(),
                  trimmed.end());

    std::lock_guard<std::mutex> lock(mu_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return false;
    if (trimmed.empty()) return true;
    it->second->info.title = truncate_utf8_prefix(trimmed, 200);
    return true;
}

void PtySessionRegistry::write_input(const std::string& id,
                                     const std::string& data) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return;
    if (it->second->info.status != "running") return;
    it->second->process->write(data);
}

bool PtySessionRegistry::connect(const std::string& id, const void* subscriber,
                                 std::int64_t cursor,
                                 std::function<void(const std::string&)> sender) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return false;
    Session& s = *it->second;

    // 补发缓冲:from = clamp(请求游标, buffer_start, cursor)。负数 = 只要新流。
    if (cursor >= 0) {
        std::uint64_t from = std::max<std::uint64_t>(
            static_cast<std::uint64_t>(cursor), s.buffer_start);
        if (from < s.cursor) {
            std::size_t offset = static_cast<std::size_t>(from - s.buffer_start);
            for (std::size_t i = offset; i < s.buffer.size(); i += kPtyBufferChunk) {
                sender(s.buffer.substr(i, kPtyBufferChunk));
            }
        }
    }
    sender(cursor_frame(s.cursor));
    if (s.info.status == "exited") {
        sender(exit_frame(s.info.exit_code));
    }

    s.subscribers[subscriber] = std::move(sender);
    return true;
}

void PtySessionRegistry::disconnect(const std::string& id,
                                    const void* subscriber) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return;
    it->second->subscribers.erase(subscriber);
}

void PtySessionRegistry::stop_all() {
    std::vector<std::unique_ptr<Session>> victims;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& [id, session] : sessions_) victims.push_back(std::move(session));
        sessions_.clear();
    }
    for (auto& v : victims) {
        if (v->process) v->process->kill();  // 锁外,理由同 remove()
    }
}

void PtySessionRegistry::on_pty_data(const std::string& id,
                                     const std::string& data) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return;  // remove() 已摘除,丢弃尾流
    Session& s = *it->second;

    s.cursor += data.size();
    for (auto& [key, sender] : s.subscribers) sender(data);

    s.buffer += data;
    if (s.buffer.size() > kPtyBufferLimit) {
        std::size_t excess = s.buffer.size() - kPtyBufferLimit;
        s.buffer.erase(0, excess);
        s.buffer_start += excess;
    }
}

void PtySessionRegistry::on_pty_exit(const std::string& id, int exit_code) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) return;
    Session& s = *it->second;
    s.info.status = "exited";
    s.info.exit_code = exit_code;
    LOG_INFO("[pty] session " + id + " exited code=" + std::to_string(exit_code));
    const std::string frame = exit_frame(exit_code);
    for (auto& [key, sender] : s.subscribers) sender(frame);
}

} // namespace acecode
