// routes_experts.cpp — local expert component discovery and managed CRUD.
#include "../server_impl.hpp"

namespace acecode::web {

using nlohmann::json;

void WebServer::Impl::register_experts() {
    CROW_ROUTE(app, "/api/experts").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) { return cors_preflight(req); });
    CROW_ROUTE(app, "/api/experts/<string>").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req, const std::string&) {
        return cors_preflight(req);
    });

    auto workspace_for_request = [this](const crow::request& req)
        -> std::optional<acecode::desktop::WorkspaceMeta> {
        const char* raw = req.url_params.get("workspace");
        return resolve_workspace(raw && *raw ? std::string(raw) : "__local__");
    };

    CROW_ROUTE(app, "/api/experts").methods(crow::HTTPMethod::GET)
    ([this, workspace_for_request](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.expert_registry) return crow::response(503);
        const auto workspace = workspace_for_request(req);
        if (!workspace) {
            crow::response response(404);
            response.body = json{{"error", "UNKNOWN_WORKSPACE"},
                                 {"message", "unknown workspace"}}.dump();
            response.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(response));
        }

        std::vector<ExpertDiagnostic> diagnostics;
        const auto experts = deps.expert_registry->list(workspace->cwd, &diagnostics);
        json items = json::array();
        for (const auto& expert : experts) {
            items.push_back(expert_definition_to_json(expert, false));
        }
        json problems = json::array();
        for (const auto& diagnostic : diagnostics) {
            problems.push_back({{"path", diagnostic.path},
                                {"message", diagnostic.message}});
        }
        crow::response response(200);
        response.body = json{
            {"experts", std::move(items)},
            {"diagnostics", std::move(problems)},
            {"workspace_hash", workspace->hash},
            {"cwd", workspace->cwd},
            {"global_root", path_to_utf8(deps.expert_registry->global_root())},
        }.dump();
        response.add_header("Content-Type", "application/json");
        return with_cors(req, std::move(response));
    });

    CROW_ROUTE(app, "/api/experts/<string>").methods(crow::HTTPMethod::GET)
    ([this, workspace_for_request](const crow::request& req, const std::string& id) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.expert_registry) return crow::response(503);
        const auto workspace = workspace_for_request(req);
        if (!workspace) {
            crow::response response(404);
            response.body = json{{"error", "UNKNOWN_WORKSPACE"}}.dump();
            response.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(response));
        }
        auto expert = deps.expert_registry->find(workspace->cwd, id);
        if (!expert) {
            crow::response response(404);
            response.body = json{{"error", "EXPERT_NOT_FOUND"},
                                 {"message", "expert component not found"}}.dump();
            response.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(response));
        }
        crow::response response(expert_definition_to_json(*expert, true).dump());
        response.add_header("Content-Type", "application/json");
        return with_cors(req, std::move(response));
    });

    CROW_ROUTE(app, "/api/experts").methods(crow::HTTPMethod::POST)
    ([this, workspace_for_request](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.expert_registry) return crow::response(503);
        auto error_response = [&](int status, const char* code, const std::string& message) {
            crow::response response(status);
            response.body = json{{"error", code}, {"message", message}}.dump();
            response.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(response));
        };
        const auto workspace = workspace_for_request(req);
        if (!workspace) return error_response(404, "UNKNOWN_WORKSPACE", "unknown workspace");
        json body;
        try {
            body = json::parse(req.body);
        } catch (const std::exception& ex) {
            return error_response(400, "BAD_JSON", ex.what());
        }
        std::string error;
        auto draft = ExpertRegistry::draft_from_json(body, &error);
        if (!draft) return error_response(400, "INVALID_EXPERT", error);
        if (!deps.expert_registry->create_global(*draft, &error, workspace->cwd)) {
            return error_response(error.find("already exists") != std::string::npos ? 409 : 400,
                                  "CREATE_FAILED", error);
        }
        auto created = deps.expert_registry->find(workspace->cwd, draft->id);
        crow::response response(201);
        response.body = created
            ? expert_definition_to_json(*created, true).dump()
            : json{{"id", draft->id}, {"ok", true}}.dump();
        response.add_header("Content-Type", "application/json");
        return with_cors(req, std::move(response));
    });

    CROW_ROUTE(app, "/api/experts/<string>").methods(crow::HTTPMethod::PUT)
    ([this, workspace_for_request](const crow::request& req, const std::string& id) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.expert_registry) return crow::response(503);
        auto error_response = [&](int status, const char* code, const std::string& message) {
            crow::response response(status);
            response.body = json{{"error", code}, {"message", message}}.dump();
            response.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(response));
        };
        const auto workspace = workspace_for_request(req);
        if (!workspace) return error_response(404, "UNKNOWN_WORKSPACE", "unknown workspace");
        const auto effective = deps.expert_registry->find(workspace->cwd, id);
        if (effective && !effective->managed_global) {
            return error_response(409, "WORKSPACE_EXPERT_READ_ONLY",
                                  "workspace expert packages are read-only through this API");
        }
        json body;
        try {
            body = json::parse(req.body);
        } catch (const std::exception& ex) {
            return error_response(400, "BAD_JSON", ex.what());
        }
        if (!body.is_object()) return error_response(400, "BAD_JSON", "body must be an object");
        if (!body.contains("id") && !body.contains("name")) body["id"] = id;
        std::string error;
        auto draft = ExpertRegistry::draft_from_json(body, &error);
        if (!draft) return error_response(400, "INVALID_EXPERT", error);
        if (!deps.expert_registry->update_global(id, *draft, &error, workspace->cwd)) {
            return error_response(error.find("does not exist") != std::string::npos ? 404 : 400,
                                  "UPDATE_FAILED", error);
        }
        auto updated = deps.expert_registry->find(workspace->cwd, id);
        crow::response response(updated
            ? expert_definition_to_json(*updated, true).dump()
            : json{{"id", id}, {"ok", true}}.dump());
        response.add_header("Content-Type", "application/json");
        return with_cors(req, std::move(response));
    });

    CROW_ROUTE(app, "/api/experts/<string>").methods(crow::HTTPMethod::Delete)
    ([this, workspace_for_request](const crow::request& req, const std::string& id) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.expert_registry) return crow::response(503);
        auto error_response = [&](int status, const char* code, const std::string& message) {
            crow::response response(status);
            response.body = json{{"error", code}, {"message", message}}.dump();
            response.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(response));
        };
        const auto workspace = workspace_for_request(req);
        if (!workspace) return error_response(404, "UNKNOWN_WORKSPACE", "unknown workspace");
        const auto effective = deps.expert_registry->find(workspace->cwd, id);
        if (effective && !effective->managed_global) {
            return error_response(409, "WORKSPACE_EXPERT_READ_ONLY",
                                  "workspace expert packages are read-only through this API");
        }
        std::string error;
        if (!deps.expert_registry->delete_global(id, &error)) {
            return error_response(404, "DELETE_FAILED", error);
        }
        crow::response response(json{{"ok", true}, {"id", id}}.dump());
        response.add_header("Content-Type", "application/json");
        return with_cors(req, std::move(response));
    });
}

} // namespace acecode::web
