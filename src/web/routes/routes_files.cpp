// routes_files.cpp — Route registrations extracted from server.cpp
#include "../server_impl.hpp"
#include "../../skills/skill_init.hpp"

namespace acecode::web {

using nlohmann::json;

void WebServer::Impl::register_files() {
        CROW_ROUTE(app, "/api/files").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/files/content").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });
        CROW_ROUTE(app, "/api/files/blob").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });

        // 把 FileError 序列化为 (status_code, json) — 共用给两个端点。
        // err.message 透出到 body.detail 便于浏览器 network tab 排查;不会泄露
        // 系统路径以外的敏感信息(全是 caller 自己传的 cwd/path)。
        auto error_response = [this](const crow::request& req,
                                      const FileError& err) -> crow::response {
            crow::response r;
            r.add_header("Content-Type", "application/json");
            json body;
            switch (err.kind) {
                case FileErrorKind::UnknownWorkspace:
                    r.code = 400;
                    body["error"] = "unknown workspace";
                    break;
                case FileErrorKind::PathOutsideWorkspace:
                    r.code = 400;
                    body["error"] = "path outside workspace";
                    break;
                case FileErrorKind::NotFound:
                    r.code = 404;
                    body["error"] = "not found";
                    break;
                case FileErrorKind::TooLarge:
                    r.code = 415;
                    body["error"] = "file too large";
                    body["size"]  = err.size;
                    break;
                case FileErrorKind::Binary:
                    r.code = 415;
                    body["error"] = "binary";
                    break;
                case FileErrorKind::IoError:
                default:
                    r.code = 500;
                    body["error"] = "io error";
                    break;
            }
            if (!err.message.empty()) body["detail"] = err.message;
            r.body = body.dump();
            return with_cors(req, std::move(r));
        };

        // GET /api/files?cwd=<abs>&path=<rel>&show_hidden=<0|1>
        CROW_ROUTE(app, "/api/files").methods(crow::HTTPMethod::GET)
        ([this, error_response](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);

            std::string cwd_q;
            std::string path_q;
            bool show_hidden = false;
            if (auto c = req.url_params.get("cwd"))         cwd_q = c;
            if (auto p = req.url_params.get("path"))        path_q = p;
            if (auto s = req.url_params.get("show_hidden")) {
                std::string v = s;
                show_hidden = (v == "1" || v == "true");
            }
            if (cwd_q.empty()) {
                crow::response r(400);
                r.body = R"({"error":"cwd parameter required"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            auto allowed_cwds = allowed_file_cwds();
            auto validated = validate_path_within(cwd_q, path_q, allowed_cwds);
            if (std::holds_alternative<FileError>(validated)) {
                return error_response(req, std::get<FileError>(validated));
            }
            auto abs_target = std::get<std::filesystem::path>(validated);
            auto abs_cwd_v = validate_path_within(cwd_q, "", allowed_cwds);
            // abs_cwd_v 不可能失败(同样的 cwd 才到这一步);保险起见 fallback
            std::filesystem::path abs_cwd =
                std::holds_alternative<std::filesystem::path>(abs_cwd_v)
                    ? std::get<std::filesystem::path>(abs_cwd_v)
                    : path_from_utf8(cwd_q);

            auto listed = list_directory(abs_target, abs_cwd, show_hidden);
            if (std::holds_alternative<FileError>(listed)) {
                return error_response(req, std::get<FileError>(listed));
            }
            auto& entries = std::get<std::vector<FileEntry>>(listed);
            json arr = json::array();
            for (const auto& e : entries) {
                json item;
                item["name"] = e.name;
                item["path"] = e.path;
                item["kind"] = e.kind;
                if (e.size.has_value())        item["size"]        = *e.size;
                if (e.modified_ms.has_value()) item["modified_ms"] = *e.modified_ms;
                arr.push_back(std::move(item));
            }
            crow::response r(arr.dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        // GET /api/files/content?cwd=<abs>&path=<rel>
        CROW_ROUTE(app, "/api/files/content").methods(crow::HTTPMethod::GET)
        ([this, error_response](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);

            std::string cwd_q;
            std::string path_q;
            if (auto c = req.url_params.get("cwd"))  cwd_q  = c;
            if (auto p = req.url_params.get("path")) path_q = p;
            if (cwd_q.empty() || path_q.empty()) {
                crow::response r(400);
                r.body = R"({"error":"cwd and path parameters required"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            auto allowed_cwds = allowed_file_cwds();
            auto validated = validate_path_within(cwd_q, path_q, allowed_cwds);
            if (std::holds_alternative<FileError>(validated)) {
                return error_response(req, std::get<FileError>(validated));
            }
            auto abs_file = std::get<std::filesystem::path>(validated);

            auto content = read_file_content(abs_file);
            if (std::holds_alternative<FileError>(content)) {
                return error_response(req, std::get<FileError>(content));
            }
            crow::response r(std::get<std::string>(content));
            r.add_header("Content-Type", "text/plain; charset=utf-8");
            r.add_header("Cache-Control", "no-cache");
            return with_cors(req, std::move(r));
        });

        // GET /api/files/blob?cwd=<abs>&path=<rel>
        CROW_ROUTE(app, "/api/files/blob").methods(crow::HTTPMethod::GET)
        ([this, error_response](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);

            std::string cwd_q;
            std::string path_q;
            if (auto c = req.url_params.get("cwd"))  cwd_q  = c;
            if (auto p = req.url_params.get("path")) path_q = p;
            if (cwd_q.empty() || path_q.empty()) {
                crow::response r(400);
                r.body = R"({"error":"cwd and path parameters required"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            auto mime = preview_blob_mime(path_q);
            if (!mime.has_value()) {
                crow::response r(415);
                r.body = R"({"error":"unsupported file type"})";
                r.add_header("Content-Type", "application/json");
                return with_cors(req, std::move(r));
            }

            auto allowed_cwds = allowed_file_cwds();
            auto validated = validate_path_within(cwd_q, path_q, allowed_cwds);
            if (std::holds_alternative<FileError>(validated)) {
                return error_response(req, std::get<FileError>(validated));
            }
            auto abs_file = std::get<std::filesystem::path>(validated);

            std::error_code ec;
            if (!std::filesystem::exists(abs_file, ec) || ec) {
                return error_response(req, FileError{FileErrorKind::NotFound, 0, "file not found"});
            }
            if (std::filesystem::is_directory(abs_file, ec) || ec) {
                return error_response(req, FileError{FileErrorKind::NotFound, 0, "is a directory"});
            }
            auto sz = std::filesystem::file_size(abs_file, ec);
            if (ec) {
                return error_response(req, FileError{FileErrorKind::IoError, 0, ec.message()});
            }
            constexpr std::uint64_t kMaxBlobPreviewBytes = 20ull * 1024 * 1024;
            if (sz > kMaxBlobPreviewBytes) {
                return error_response(req, FileError{
                    FileErrorKind::TooLarge,
                    static_cast<std::uint64_t>(sz),
                    "file exceeds blob preview cap",
                });
            }

            std::ifstream in(abs_file, std::ios::binary);
            if (!in) {
                return error_response(req, FileError{FileErrorKind::IoError, 0, "failed to open file"});
            }
            std::string body((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
            if (!in.good() && !in.eof()) {
                return error_response(req, FileError{FileErrorKind::IoError, 0, "failed to read file"});
            }

            crow::response r(std::move(body));
            r.add_header("Content-Type", *mime);
            r.add_header("Cache-Control", "no-cache");
            r.add_header("X-Content-Type-Options", "nosniff");
            return with_cors(req, std::move(r));
        });
    }

void WebServer::Impl::register_skills() {
        CROW_ROUTE(app, "/api/skills/root").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });

        // GET /api/skills/root?workspace=<hash>: resolve effective skill directory
        CROW_ROUTE(app, "/api/skills/root").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);

            std::optional<acecode::desktop::WorkspaceMeta> ws;
            const char* workspace_q = req.url_params.get("workspace");
            if (workspace_q && *workspace_q) {
                ws = resolve_workspace(workspace_q);
                if (!ws.has_value()) {
                    crow::response r(404);
                    r.body = R"({"error":"workspace not found"})";
                    r.add_header("Content-Type", "application/json");
                    return with_cors(req, std::move(r));
                }
            } else {
                ws = compatibility_workspace();
            }

            auto selected = resolve_skill_root_for_cwd(ws->cwd);
            json body{
                {"path", path_to_utf8(selected.path)},
                {"source", selected.source},
                {"global_path", path_to_utf8(selected.global_path)},
                {"workspace_hash", ws->hash},
                {"cwd", ws->cwd},
            };
            crow::response r(body.dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });

        // GET /api/skills?workspace=<hash>: spec 9.7 + 设置页技能 tab。
        // 全量扫描(含禁用中的 skill,带完整元数据),每条带 enabled 与
        // source("project"/"global")。workspace 缺省 = daemon 兼容 workspace。
        CROW_ROUTE(app, "/api/skills").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            json arr = json::array();
            if (deps.app_config) {
                std::optional<acecode::desktop::WorkspaceMeta> ws;
                const char* workspace_q = req.url_params.get("workspace");
                if (workspace_q && *workspace_q) {
                    ws = resolve_workspace(workspace_q);
                    if (!ws.has_value()) {
                        crow::response r(404);
                        r.body = R"({"error":"workspace not found"})";
                        r.add_header("Content-Type", "application/json");
                        return r;
                    }
                } else {
                    ws = compatibility_workspace();
                }
                std::lock_guard<std::mutex> config_lock(app_config_mu);
                arr = build_skills_payload(*deps.app_config, ws->cwd);
            }
            crow::response r(arr.dump());
            r.add_header("Content-Type", "application/json");
            return r;
        });

        // PUT /api/skills/<name>?workspace=<hash> body {enabled: bool}: 切启停。
        // workspace 参数用于"已知性"校验:daemon 全局 registry 只扫 daemon cwd
        // 的项目链,其它 workspace 的项目 skill 需要按该 workspace cwd 临时扫描
        // 才能 find 到,否则误报 404。
        CROW_ROUTE(app, "/api/skills/<string>").methods(crow::HTTPMethod::PUT)
        ([this](const crow::request& req, const std::string& name) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.app_config || !deps.skill_registry) return crow::response(503);

            bool enabled;
            try {
                auto j = json::parse(req.body);
                if (!j.contains("enabled") || !j["enabled"].is_boolean()) {
                    crow::response r(400);
                    r.body = R"({"error":"enabled (boolean) required"})";
                    r.add_header("Content-Type", "application/json");
                    return r;
                }
                enabled = j["enabled"].get<bool>();
            } catch (const std::exception& e) {
                crow::response r(400);
                r.body = json{{"error", std::string("bad json: ") + e.what()}}.dump();
                r.add_header("Content-Type", "application/json");
                return r;
            }

            std::optional<acecode::desktop::WorkspaceMeta> ws;
            const char* workspace_q = req.url_params.get("workspace");
            if (workspace_q && *workspace_q) {
                ws = resolve_workspace(workspace_q);
                if (!ws.has_value()) {
                    crow::response r(404);
                    r.body = R"({"error":"workspace not found"})";
                    r.add_header("Content-Type", "application/json");
                    return r;
                }
            }

            std::lock_guard<std::mutex> config_lock(app_config_mu);
            std::optional<acecode::SkillRegistry> workspace_lookup;
            if (ws.has_value() && !ws->cwd.empty()) {
                workspace_lookup.emplace();
                acecode::initialize_skill_registry(*workspace_lookup,
                                                   *deps.app_config, ws->cwd);
            }
            auto result = set_skill_enabled(name, enabled,
                                               *deps.app_config,
                                               *deps.skill_registry,
                                              deps.config_path,
                                              workspace_lookup ? &*workspace_lookup
                                                               : nullptr);
            crow::response r(result.http_status);
            r.body = result.body.dump();
            r.add_header("Content-Type", "application/json");
            return r;
        });

        // GET /api/skills/<name>/body: 返回 SKILL.md 全文(含 frontmatter)
        CROW_ROUTE(app, "/api/skills/<string>/body").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req, const std::string& name) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            if (!deps.skill_registry) return crow::response(503);
            auto body = get_skill_body(name, *deps.skill_registry);
            if (!body.has_value()) {
                crow::response r(404);
                r.body = R"({"error":"skill not found"})";
                r.add_header("Content-Type", "application/json");
                return r;
            }
            crow::response r(*body);
            r.add_header("Content-Type", "text/markdown; charset=utf-8");
            return r;
        });
    }

void WebServer::Impl::register_commands() {
        // GET /api/commands?workspace=<hash>: webui-slash-commands 用,只读返回
        // builtin + skill 列表。`workspace` 参数让 handler 按目标 workspace cwd
        // 实时扫描该项目下的 .agent/skills、.acecode/skills,与 daemon 全局 skills
        // 合并 — desktop 多 workspace 共享一个 daemon,daemon 启动 cwd 固定,
        // 不带 workspace 时用户切到的 workspace 项目里的 skill 会看不到。
        CROW_ROUTE(app, "/api/commands").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);
            std::optional<std::string> workspace_cwd;
            const char* workspace_q = req.url_params.get("workspace");
            if (workspace_q && *workspace_q) {
                if (auto m = resolve_workspace(workspace_q)) {
                    if (!m->cwd.empty()) workspace_cwd = m->cwd;
                }
            }
            SkillRegistry empty_registry;
            const auto& registry = deps.skill_registry ? *deps.skill_registry : empty_registry;
            json payload;
            {
                std::lock_guard<std::mutex> config_lock(app_config_mu);
                payload = build_commands_payload(registry, workspace_cwd, deps.app_config);
            }
            crow::response r(payload.dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });
        CROW_ROUTE(app, "/api/commands").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });
    }
} // namespace acecode::web
