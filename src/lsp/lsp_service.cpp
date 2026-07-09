#include "lsp_service.hpp"

#include "lsp_uri.hpp"
#include "../utils/logger.hpp"
#include "../utils/utf8_path.hpp"

#include <algorithm>
#include <filesystem>

namespace acecode::lsp {
namespace {

namespace fs = std::filesystem;

bool real_file_exists(const std::string& utf8_path) {
    std::error_code ec;
    return fs::exists(path_from_utf8(utf8_path), ec) && !ec;
}

// file 是否位于 workspace 之内(分隔符统一,Windows 大小写不敏感)。
bool within_workspace(const std::string& utf8_file, const std::string& workspace) {
    auto canon = [](std::string p) {
        std::replace(p.begin(), p.end(), '\\', '/');
        while (!p.empty() && p.back() == '/') p.pop_back();
#ifdef _WIN32
        std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
#endif
        return p;
    };
    const std::string f = canon(utf8_file);
    const std::string w = canon(workspace);
    if (w.empty()) return false;
    if (f == w) return true;
    return f.size() > w.size() && f.compare(0, w.size(), w) == 0 && f[w.size()] == '/';
}

std::string absolutize(const std::string& utf8_path, const std::string& workspace) {
    fs::path p = path_from_utf8(utf8_path);
    if (p.is_absolute()) return utf8_path;
    return path_to_utf8(path_from_utf8(workspace) / p);
}

// junction/symlink 归一:C:\Users\x 是 N:\Users\x 的 junction 时,调用方
// 可能任意一种形态传入(lsp 工具 weakly_canonical 过、文件工具没过),
// 不归一则 within_workspace 前缀比较假性失败、slot key 分裂。失败时原样返回。
std::string canonicalize_path(const std::string& utf8_path) {
    if (utf8_path.empty()) return utf8_path;
    std::error_code ec;
    const fs::path canon = fs::weakly_canonical(path_from_utf8(utf8_path), ec);
    if (ec || canon.empty()) return utf8_path;
    return path_to_utf8(canon);
}

std::string display_root(const std::string& root, const std::string& workspace) {
    // 跨 workspace 的 root(多 workspace 会话)不做相对化,直接显示绝对路径,
    // 避免出现一长串 "../.." 的展示噪声。
    if (!within_workspace(root, workspace)) return root;
    std::error_code ec;
    const fs::path rel = fs::relative(path_from_utf8(root), path_from_utf8(workspace), ec);
    if (ec || rel.empty()) return root;
    const std::string text = path_to_utf8_generic(rel);
    return text.empty() ? "." : text;
}

} // namespace

LspService::LspService(const LspConfig& cfg, std::string workspace_cwd)
    : enabled_(cfg.enabled), workspace_cwd_(canonicalize_path(workspace_cwd)) {
    if (enabled_) {
        defs_ = merge_server_defs(cfg);
    }
}

LspService::~LspService() {
    shutdown_all();
}

std::shared_ptr<LspService::Slot> LspService::slot_for(const std::string& key,
                                                       const std::string& server_id,
                                                       const std::string& root) {
    std::lock_guard<std::mutex> lk(slots_mu_);
    auto it = slots_.find(key);
    if (it != slots_.end()) return it->second;
    auto slot = std::make_shared<Slot>();
    slot->server_id = server_id;
    slot->root = root;
    slots_[key] = slot;
    return slot;
}

LspService::ResolvedPath LspService::resolve_path(const std::string& utf8_path,
                                                  const std::string& session_cwd) const {
    ResolvedPath rp;
    rp.workspace = session_cwd.empty() ? workspace_cwd_ : canonicalize_path(session_cwd);
    rp.abs = canonicalize_path(absolutize(utf8_path, rp.workspace));
    return rp;
}

std::vector<std::shared_ptr<LspClient>> LspService::clients_for(const ResolvedPath& rp) {
    std::vector<std::shared_ptr<LspClient>> out;
    if (!enabled_ || shutting_down_.load()) return out;

    const std::string& abs = rp.abs;
    if (!within_workspace(abs, rp.workspace)) return out;

    for (const auto& def : defs_) {
        if (!extensions_match(def, abs)) continue;
        auto root = detect_root(def, abs, rp.workspace, real_file_exists);
        if (!root.has_value()) continue;

        const std::string key = def.id + "\n" + normalize_path_key(*root);
        auto slot = slot_for(key, def.id, *root);

        // slot 锁把同 key 的并发首次触发串行化:第一个线程 spawn,其余
        // 等锁后直接复用/看到 broken。spawn + initialize 可能到 45s,
        // abort 探针不打断该阶段(仅首个触达者付出这一代价)。
        std::lock_guard<std::mutex> slot_lk(slot->mu);
        if (slot->broken) continue;
        if (slot->client && slot->client->alive()) {
            out.push_back(slot->client);
            continue;
        }
        if (slot->client && !slot->client->alive()) {
            // server 中途死亡:同 broken 处理,不做重启风暴。
            slot->broken = true;
            slot->client.reset();
            continue;
        }
        if (shutting_down_.load()) continue;

        auto resolved = resolve_spawn(def, *root, rp.workspace);
        if (!resolved.has_value()) {
            slot->broken = true; // 可执行探测失败,本进程内不再尝试
            continue;
        }
        LspClient::CreateOptions options;
        options.server_id = def.id;
        options.root = *root;
        options.spawn = std::move(resolved->spawn);
        options.initialization = resolved->initialization;
        std::string error;
        auto client = LspClient::create(std::move(options), &error);
        if (!client) {
            slot->broken = true;
            LOG_WARN("[lsp] failed to start " + def.id + " for root " + *root +
                     ": " + error);
            continue;
        }
        LOG_INFO("[lsp] connected " + def.id + " (root: " + *root + ")");
        slot->client = std::shared_ptr<LspClient>(client.release());
        out.push_back(slot->client);
    }
    return out;
}

bool LspService::has_server_for(const std::string& utf8_path,
                                const std::string& session_cwd) {
    if (!enabled_) return false;
    const ResolvedPath rp = resolve_path(utf8_path, session_cwd);
    const std::string& abs = rp.abs;
    if (!within_workspace(abs, rp.workspace)) return false;

    for (const auto& def : defs_) {
        if (!extensions_match(def, abs)) continue;
        auto root = detect_root(def, abs, rp.workspace, real_file_exists);
        if (!root.has_value()) continue;
        const std::string key = def.id + "\n" + normalize_path_key(*root);
        {
            std::lock_guard<std::mutex> lk(slots_mu_);
            auto it = slots_.find(key);
            if (it != slots_.end()) {
                // 已有槽位:broken → 不可用;有活 client → 可用。
                // (不抢 slot->mu,这里只做乐观判断。)
                if (it->second->broken) continue;
                if (it->second->client) return true;
            }
        }
        if (which(def.command.empty() ? std::string{} : def.command[0]).has_value()) {
            return true;
        }
    }
    return false;
}

std::vector<nlohmann::json> LspService::collect_diagnostics_after_write(
    const std::string& utf8_path,
    std::chrono::milliseconds wait_timeout,
    const AbortProbe& should_abort,
    const std::string& session_cwd) {
    std::vector<nlohmann::json> merged;
    if (!enabled_) return merged;

    const ResolvedPath rp = resolve_path(utf8_path, session_cwd);
    const std::string& abs = rp.abs;
    auto clients = clients_for(rp);
    if (clients.empty()) return merged;

    // 所有 client 共享一个总 deadline:多 server 命中同一文件时编辑
    // 延迟仍有硬上限。
    const auto deadline = std::chrono::steady_clock::now() + wait_timeout;
    for (const auto& client : clients) {
        if (should_abort && should_abort()) break;
        auto version = client->touch_file(abs);
        if (!version.has_value()) continue;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::milliseconds(0)) continue;
        client->wait_for_diagnostics(abs, *version, remaining, should_abort);
    }
    for (const auto& client : clients) {
        auto items = client->diagnostics_for(abs);
        merged.insert(merged.end(), items.begin(), items.end());
    }
    return merged;
}

std::vector<nlohmann::json> LspService::request_for_file(const std::string& utf8_path,
                                                         const std::string& method,
                                                         const nlohmann::json& params,
                                                         std::chrono::milliseconds timeout,
                                                         const AbortProbe& should_abort,
                                                         const std::string& session_cwd) {
    std::vector<nlohmann::json> out;
    if (!enabled_) return out;

    const ResolvedPath rp = resolve_path(utf8_path, session_cwd);
    const std::string& abs = rp.abs;
    auto clients = clients_for(rp);
    for (const auto& client : clients) {
        if (should_abort && should_abort()) break;
        // 查询前确保 server 已加载该文件;短等诊断给 server 索引留时间。
        auto version = client->touch_file(abs);
        if (version.has_value()) {
            client->wait_for_diagnostics(abs, *version, std::chrono::milliseconds(2000),
                                         should_abort);
        }
        auto result = client->request(method, params, timeout);
        if (result.has_value() && !result->is_null()) {
            out.push_back(std::move(*result));
        }
    }
    return out;
}

std::vector<nlohmann::json> LspService::call_hierarchy_for_file(
    const std::string& utf8_path,
    const nlohmann::json& prepare_params,
    const std::string& direction_method,
    std::chrono::milliseconds timeout,
    const AbortProbe& should_abort,
    const std::string& session_cwd) {
    std::vector<nlohmann::json> out;
    if (!enabled_) return out;

    const ResolvedPath rp = resolve_path(utf8_path, session_cwd);
    const std::string& abs = rp.abs;
    auto clients = clients_for(rp);
    for (const auto& client : clients) {
        if (should_abort && should_abort()) break;
        auto version = client->touch_file(abs);
        if (version.has_value()) {
            client->wait_for_diagnostics(abs, *version, std::chrono::milliseconds(2000),
                                         should_abort);
        }
        auto items = client->request("textDocument/prepareCallHierarchy",
                                     prepare_params, timeout);
        if (!items.has_value() || !items->is_array() || items->empty()) continue;
        auto result = client->request(direction_method,
                                      {{"item", (*items)[0]}}, timeout);
        if (result.has_value() && !result->is_null()) out.push_back(std::move(*result));
    }
    return out;
}

std::vector<nlohmann::json> LspService::request_all_connected(
    const std::string& method,
    const nlohmann::json& params,
    std::chrono::milliseconds timeout) {
    std::vector<std::shared_ptr<LspClient>> clients;
    {
        std::lock_guard<std::mutex> lk(slots_mu_);
        for (const auto& [key, slot] : slots_) {
            if (slot->client && slot->client->alive()) clients.push_back(slot->client);
        }
    }
    std::vector<nlohmann::json> out;
    for (const auto& client : clients) {
        auto result = client->request(method, params, timeout);
        if (result.has_value() && !result->is_null()) out.push_back(std::move(*result));
    }
    return out;
}

std::vector<LspService::StatusEntry> LspService::connected_snapshot() {
    std::vector<StatusEntry> connected;
    if (!enabled_) return connected;
    std::lock_guard<std::mutex> lk(slots_mu_);
    for (const auto& [key, slot] : slots_) {
        if (slot->client && slot->client->alive()) {
            StatusEntry entry;
            entry.server_id = slot->server_id;
            entry.root = display_root(slot->root, workspace_cwd_);
            entry.open_files = slot->client->open_file_count();
            connected.push_back(std::move(entry));
        }
    }
    return connected;
}

std::vector<LspService::StatusEntry> LspService::connected_for_cwd(
    const std::string& session_cwd) {
    std::vector<StatusEntry> connected;
    if (!enabled_) return connected;
    // 会话 workspace 边界:空 → 进程级 init cwd。两侧都 canonical 化,
    // 与 slot->root(由 canonical 路径推导)落在同一形态才能正确前缀匹配。
    const std::string boundary =
        session_cwd.empty() ? workspace_cwd_ : canonicalize_path(session_cwd);
    if (boundary.empty()) return connected;
    std::lock_guard<std::mutex> lk(slots_mu_);
    for (const auto& [key, slot] : slots_) {
        if (slot->client && slot->client->alive() &&
            within_workspace(slot->root, boundary)) {
            StatusEntry entry;
            entry.server_id = slot->server_id;
            entry.root = display_root(slot->root, boundary);
            entry.open_files = slot->client->open_file_count();
            connected.push_back(std::move(entry));
        }
    }
    return connected;
}

LspService::Status LspService::status_snapshot() {
    Status status;
    status.enabled = enabled_;
    if (!enabled_) return status;

    status.connected = connected_snapshot();
    {
        std::lock_guard<std::mutex> lk(slots_mu_);
        for (const auto& [key, slot] : slots_) {
            if (slot->broken) {
                StatusEntry entry;
                entry.server_id = slot->server_id;
                entry.root = display_root(slot->root, workspace_cwd_);
                status.broken.push_back(std::move(entry));
            }
        }
    }
    for (const auto& def : defs_) {
        if (def.command.empty()) continue;
        if (!which(def.command[0]).has_value()) {
            status.not_installed.push_back(def.id);
        }
    }
    return status;
}

void LspService::shutdown_all() {
    shutting_down_.store(true);
    std::vector<std::shared_ptr<LspClient>> clients;
    {
        std::lock_guard<std::mutex> lk(slots_mu_);
        for (auto& [key, slot] : slots_) {
            if (slot->client) clients.push_back(slot->client);
        }
        slots_.clear();
    }
    // 教训沿用 pty 关闭路径:kill/join 在容器锁外做,避免回抢。
    for (const auto& client : clients) {
        client->shutdown();
    }
}

// ---- 进程级单例 ----

namespace {
std::mutex g_runtime_mu;
std::unique_ptr<LspService> g_service;
} // namespace

void init(const LspConfig& cfg, const std::string& workspace_cwd) {
    std::lock_guard<std::mutex> lk(g_runtime_mu);
    if (g_service) {
        LOG_WARN("[lsp] init called twice; keeping the first runtime");
        return;
    }
    g_service = std::make_unique<LspService>(cfg, workspace_cwd);
}

void shutdown() {
    std::unique_ptr<LspService> stale;
    {
        std::lock_guard<std::mutex> lk(g_runtime_mu);
        stale = std::move(g_service);
    }
    if (stale) stale->shutdown_all();
}

bool is_initialized() {
    std::lock_guard<std::mutex> lk(g_runtime_mu);
    return static_cast<bool>(g_service);
}

LspService& service() {
    std::lock_guard<std::mutex> lk(g_runtime_mu);
    return *g_service;
}

void set_service_for_test(std::unique_ptr<LspService> service_) {
    std::lock_guard<std::mutex> lk(g_runtime_mu);
    g_service = std::move(service_);
}

} // namespace acecode::lsp
