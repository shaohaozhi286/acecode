// routes_git.cpp — /api/git/* 路由注册(openspec add-git-context)。
// 业务逻辑在 handlers/git_handler.cpp(纯函数),这里只做 HTTP 壳。
#include "../server_impl.hpp"
#include "../handlers/git_handler.hpp"

namespace acecode::web {

using nlohmann::json;

void WebServer::Impl::register_git() {
        CROW_ROUTE(app, "/api/git/info").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/git/checkout").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/git/changes").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/git/diff").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });

        // config 读取小助手:git_context 的 enabled/timeout(加锁快照)。
        auto git_cfg = [this]() {
            std::pair<bool, int> out{true, 3000};
            std::lock_guard<std::mutex> config_lock(app_config_mu);
            if (deps.app_config) {
                out.first = deps.app_config->git_context.enabled;
                out.second = deps.app_config->git_context.timeout_ms;
            }
            return out;
        };

        // GET /api/git/info?cwd=<abs>
        CROW_ROUTE(app, "/api/git/info").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);

            std::string cwd_q;
            if (auto c = req.url_params.get("cwd")) cwd_q = c;
            if (cwd_q.empty()) {
                crow::response r(400);
                r.body = R"({"error":"cwd parameter required"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            bool enabled = true;
            int timeout_ms = 3000;
            {
                std::lock_guard<std::mutex> config_lock(app_config_mu);
                if (deps.app_config) {
                    enabled = deps.app_config->git_context.enabled;
                    timeout_ms = deps.app_config->git_context.timeout_ms;
                }
            }

            GitApiResponse payload = build_git_info_payload(
                cwd_q, allowed_file_cwds(), enabled, timeout_ms);
            crow::response r(payload.status, payload.body.dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        // POST /api/git/checkout body {cwd, branch, stash}
        // (openspec add-webui-git-session-pill:新会话 pill 的分支切换)
        CROW_ROUTE(app, "/api/git/checkout").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);

            std::string cwd_q;
            std::string branch;
            bool stash = false;
            try {
                auto j = json::parse(req.body);
                if (j.contains("cwd") && j["cwd"].is_string())
                    cwd_q = j["cwd"].get<std::string>();
                if (j.contains("branch") && j["branch"].is_string())
                    branch = j["branch"].get<std::string>();
                if (j.contains("stash") && j["stash"].is_boolean())
                    stash = j["stash"].get<bool>();
            } catch (const std::exception& e) {
                crow::response r(400);
                r.body = json{{"error", std::string("bad json: ") + e.what()}}.dump();
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }
            if (cwd_q.empty() || branch.empty()) {
                crow::response r(400);
                r.body = R"({"error":"cwd and branch required"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            bool enabled = true;
            int timeout_ms = 3000;
            {
                std::lock_guard<std::mutex> config_lock(app_config_mu);
                if (deps.app_config) {
                    enabled = deps.app_config->git_context.enabled;
                    timeout_ms = deps.app_config->git_context.timeout_ms;
                }
            }

            auto busy_probe = [this, &cwd_q]() {
                return deps.session_registry &&
                       deps.session_registry->any_busy_in_cwd(cwd_q);
            };
            GitApiResponse payload = build_git_checkout_payload(
                cwd_q, branch, stash, allowed_file_cwds(), enabled, timeout_ms,
                busy_probe);

            // checkout 成功后失效该 workspace 全部会话的 gitStatus 快照 ——
            // 下一次模型请求按新分支重采(openspec add-git-context 的联动)。
            if (payload.status == 200 && deps.session_registry) {
                deps.session_registry->invalidate_git_snapshots_in_cwd(cwd_q);
            }

            crow::response r(payload.status, payload.body.dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        // GET /api/git/changes?cwd=<abs>&base=<ref>
        // (openspec redesign-sidepanel-git-changes:SidePanel git 级变更列表)
        CROW_ROUTE(app, "/api/git/changes").methods(crow::HTTPMethod::GET)
        ([this, git_cfg](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);

            std::string cwd_q;
            std::string base_q;
            if (auto c = req.url_params.get("cwd"))  cwd_q = c;
            if (auto b = req.url_params.get("base")) base_q = b;
            if (cwd_q.empty()) {
                crow::response r(400);
                r.body = R"({"error":"cwd parameter required"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            auto [enabled, timeout_ms] = git_cfg();
            // numstat 对大 diff 比 status 重,超时放宽 2×(design 决策)。
            GitApiResponse payload = build_git_changes_payload(
                cwd_q, base_q, allowed_file_cwds(), enabled, timeout_ms * 2);
            crow::response r(payload.status, payload.body.dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        // GET /api/git/diff?cwd=<abs>&path=<rel>&base=<ref>(单文件懒加载)
        CROW_ROUTE(app, "/api/git/diff").methods(crow::HTTPMethod::GET)
        ([this, git_cfg](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);

            std::string cwd_q;
            std::string base_q;
            std::string path_q;
            if (auto c = req.url_params.get("cwd"))  cwd_q = c;
            if (auto b = req.url_params.get("base")) base_q = b;
            if (auto p = req.url_params.get("path")) path_q = p;
            if (cwd_q.empty() || path_q.empty()) {
                crow::response r(400);
                r.body = R"({"error":"cwd and path parameters required"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            auto [enabled, timeout_ms] = git_cfg();
            GitApiResponse payload = build_git_file_diff_payload(
                cwd_q, base_q, path_q, allowed_file_cwds(), enabled, timeout_ms);
            crow::response r(payload.status, payload.body.dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });
}

} // namespace acecode::web
