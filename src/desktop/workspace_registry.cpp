#include "workspace_registry.hpp"

#include "../utils/atomic_file.hpp"
#include "../utils/cwd_hash.hpp"
#include "../utils/logger.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace acecode::desktop {

namespace {

constexpr const char* kWorkspaceJson = "workspace.json";

// 给 hash 目录 + 根 projects_dir 拼出 workspace.json 完整路径。
std::string workspace_json_path(const std::string& projects_dir, const std::string& hash) {
    return (fs::path(projects_dir) / hash / kWorkspaceJson).string();
}

// 尝试读取 workspace.json。返回 nullopt 表示文件不存在 / 损坏 / 缺关键字段。
std::optional<WorkspaceMeta> read_workspace_json(const std::string& projects_dir,
                                                 const std::string& hash) {
    std::string p = workspace_json_path(projects_dir, hash);
    std::error_code ec;
    if (!fs::exists(p, ec) || ec) return std::nullopt;

    std::ifstream ifs(p);
    if (!ifs.is_open()) return std::nullopt;

    std::stringstream buf;
    buf << ifs.rdbuf();
    std::string contents = buf.str();
    if (contents.empty()) return std::nullopt;

    try {
        auto j = nlohmann::json::parse(contents);
        if (!j.is_object()) {
            LOG_WARN("[workspace_registry] workspace.json is not an object at " + p);
            return std::nullopt;
        }
        if (!j.contains("cwd") || !j["cwd"].is_string()) {
            LOG_WARN("[workspace_registry] workspace.json missing cwd at " + p);
            return std::nullopt;
        }

        WorkspaceMeta m;
        m.hash = hash;
        m.cwd = j["cwd"].get<std::string>();
        if (j.contains("name") && j["name"].is_string()) {
            m.name = j["name"].get<std::string>();
        }
        if (m.name.empty()) m.name = default_workspace_name(m.cwd);
        if (j.contains("desktop_visible") && j["desktop_visible"].is_boolean()) {
            m.desktop_visible = j["desktop_visible"].get<bool>();
        }
        return m;
    } catch (const std::exception& e) {
        LOG_WARN(std::string("[workspace_registry] workspace.json parse failed at ")
                 + p + ": " + e.what());
        return std::nullopt;
    }
}

bool write_workspace_json(const std::string& projects_dir, const WorkspaceMeta& m) {
    fs::path dir = fs::path(projects_dir) / m.hash;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        LOG_WARN("[workspace_registry] create_directories failed: " + ec.message());
        return false;
    }
    std::string p = (dir / kWorkspaceJson).string();

    nlohmann::json j;
    j["cwd"] = m.cwd;
    j["name"] = m.name;
    j["desktop_visible"] = m.desktop_visible;
    return atomic_write_file(p, j.dump(2));
}

} // namespace

std::string default_workspace_name(const std::string& cwd) {
    // path("foo/").filename() 在 C++17 是空；Windows 上
    // fs::path(std::string) 还会按本地窄编码解释 UTF-8。这里用纯字符串切
    // basename,既避开尾斜杠,也保留中文等 UTF-8 名称不被转码。
    std::string clean = cwd;
    while (clean.size() > 1 && (clean.back() == '/' || clean.back() == '\\')) {
        clean.pop_back();
    }

    size_t slash = std::string::npos;
    for (size_t i = clean.size(); i > 0; --i) {
        if (clean[i - 1] == '/' || clean[i - 1] == '\\') {
            slash = i - 1;
            break;
        }
    }
    std::string base = slash == std::string::npos ? clean : clean.substr(slash + 1);
    if (!base.empty()) return base;

    if (clean.size() >= 2 && clean[1] == ':') {
        return clean.substr(0, 2);
    }

    return "workspace";
}

void WorkspaceRegistry::scan(const std::string& projects_dir) {
    std::lock_guard<std::mutex> lk(mu_);
    entries_.clear();

    std::error_code ec;
    if (!fs::exists(projects_dir, ec) || !fs::is_directory(projects_dir, ec)) {
        return; // 空目录或不存在 — 注册表保持空
    }

    for (auto it = fs::directory_iterator(projects_dir, ec);
         !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        if (ec) break;
        if (!it->is_directory(ec)) continue;
        std::string hash = it->path().filename().string();
        // hash 目录名约定: 16 位 hex。不强制(防止过滤掉合法案例),只防过短目录。
        if (hash.size() < 4) continue;

        auto m = read_workspace_json(projects_dir, hash);
        if (!m) continue;
        if (!m->desktop_visible) continue;
        entries_[hash] = std::move(*m);
    }
}

std::vector<WorkspaceMeta> WorkspaceRegistry::list() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<WorkspaceMeta> out;
    out.reserve(entries_.size());
    for (const auto& [_, m] : entries_) out.push_back(m);
    return out;
}

std::optional<WorkspaceMeta> WorkspaceRegistry::get(const std::string& hash) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(hash);
    if (it == entries_.end()) return std::nullopt;
    return it->second;
}

WorkspaceMeta WorkspaceRegistry::register_new(const std::string& projects_dir,
                                              const std::string& cwd) {
    std::string hash = compute_cwd_hash(cwd);
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = entries_.find(hash);
        if (it != entries_.end()) return it->second; // 已注册:返回已有 meta
    }

    WorkspaceMeta m;
    m.hash = hash;
    m.cwd = cwd;
    m.name = default_workspace_name(cwd);
    m.desktop_visible = true;

    if (!write_workspace_json(projects_dir, m)) {
        // 写盘失败 — 仍把 meta 入册以便用户当前会话能用,但下次启动 scan 会缺失。
        // 不抛异常,让 UI 端用 daemon_state=failed 之类显示。
        LOG_WARN("[workspace_registry] register_new write failed for cwd=" + cwd);
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        entries_[hash] = m;
    }
    return m;
}

bool ensure_workspace_metadata(const std::string& projects_dir, const std::string& cwd) {
    if (cwd.empty()) return false;

    WorkspaceMeta m;
    m.hash = compute_cwd_hash(cwd);
    m.cwd = cwd;
    m.name = default_workspace_name(cwd);
    m.desktop_visible = false;

    std::string p = workspace_json_path(projects_dir, m.hash);
    std::error_code ec;
    if (fs::exists(p, ec) && !ec) {
        return true;
    }
    return write_workspace_json(projects_dir, m);
}

bool WorkspaceRegistry::set_name(const std::string& projects_dir,
                                 const std::string& hash,
                                 const std::string& name) {
    if (name.empty()) return false;

    WorkspaceMeta updated;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = entries_.find(hash);
        if (it == entries_.end()) return false;
        updated = it->second;
    }
    updated.name = name;

    if (!write_workspace_json(projects_dir, updated)) return false;

    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(hash);
    if (it == entries_.end()) return false; // 极端竞态:写盘期间被 scan 清空
    it->second = updated;
    return true;
}

bool workspace_hash_matches_cwd(const std::string& hash, const std::string& cwd) {
    if (hash.empty() || cwd.empty()) return false;
    return hash == compute_cwd_hash(cwd);
}

} // namespace acecode::desktop
