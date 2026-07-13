// routes_loops.cpp — daemon-owned LOOP CRUD and run-history routes.
#include "../server_impl.hpp"

#include "../../loop/loop_schedule.hpp"

#include <algorithm>
#include <chrono>

namespace acecode::web {

using nlohmann::json;

namespace {

std::int64_t loop_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

json store_error_json(const acecode::loop::StoreError& error) {
    json out{{"error", error.code.empty() ? "LOOP_STORE_ERROR" : error.code},
             {"message", error.message.empty() ? "LOOP operation failed" : error.message}};
    if (error.conflict) {
        out["conflict"] = {
            {"loop_id", error.conflict->loop_id},
            {"loop_name", error.conflict->loop_name},
            {"first_conflict_at_ms", error.conflict->first_conflict_at_ms},
        };
    }
    return out;
}

} // namespace

void WebServer::Impl::register_loops() {
    CROW_ROUTE(app, "/api/loops").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) { return cors_preflight(req); });
    CROW_ROUTE(app, "/api/loops/<string>").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req, const std::string&) { return cors_preflight(req); });
    CROW_ROUTE(app, "/api/loops/<string>/enabled").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req, const std::string&) { return cors_preflight(req); });
    CROW_ROUTE(app, "/api/loops/<string>/runs").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req, const std::string&) { return cors_preflight(req); });

    auto unsupported = [this](const crow::request& req) {
        crow::response r(501);
        r.add_header("Content-Type", "application/json");
        r.body = json{{"error", "LOOP_UNAVAILABLE"},
                      {"message", "This daemon does not support LOOP"}}.dump();
        return with_cors(req, std::move(r));
    };
    auto json_response = [this](const crow::request& req, int status, const json& body) {
        crow::response r(status);
        r.add_header("Content-Type", "application/json");
        r.body = body.dump();
        return with_cors(req, std::move(r));
    };
    auto parse_definition = [this, json_response](const crow::request& req,
                                                   acecode::loop::LoopDefinition& value)
        -> std::optional<crow::response> {
        json body = json::parse(req.body, nullptr, false);
        if (body.is_discarded()) {
            return json_response(req, 400,
                {{"error", "BAD_JSON"}, {"message", "invalid JSON body"}});
        }
        acecode::loop::ValidationError validation;
        if (!acecode::loop::loop_from_json(body, value, &validation)) {
            return json_response(req, 400,
                {{"error", validation.code}, {"field", validation.field},
                 {"message", validation.message}});
        }
        if (body.contains("schedule") && body["schedule"].is_object() &&
            !body["schedule"].contains("timezone_offset_minutes")) {
            value.schedule.timezone_offset_minutes =
                acecode::loop::current_timezone_offset_minutes(loop_now_ms());
        }
        if (!value.workspace_hash.empty()) {
            auto workspace = resolve_workspace(value.workspace_hash);
            if (!workspace || workspace->cwd != value.workspace_cwd) {
                return json_response(req, 400,
                    {{"error", "INVALID_WORKSPACE"},
                     {"message", "workspace is not registered or its path changed"}});
            }
        }
        {
            std::lock_guard<std::mutex> lock(app_config_mu);
            const bool exists = deps.app_config && std::any_of(
                deps.app_config->saved_models.begin(), deps.app_config->saved_models.end(),
                [&](const ModelProfile& model) { return model.name == value.model_name; });
            if (!exists) {
                return json_response(req, 400,
                    {{"error", "INVALID_MODEL"}, {"message", "selected model is unavailable"}});
            }
        }
        return std::nullopt;
    };

    CROW_ROUTE(app, "/api/loops").methods(crow::HTTPMethod::GET)
    ([this, unsupported, json_response](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.loop_store) return unsupported(req);
        acecode::loop::StoreError error;
        auto loops = deps.loop_store->list_loops(&error);
        if (!error.code.empty()) return json_response(req, 503, store_error_json(error));
        json items = json::array();
        for (const auto& loop : loops) {
            auto item = acecode::loop::loop_to_json(loop);
            auto latest_runs = deps.loop_store->list_runs(loop.id, 1, &error);
            if (!error.code.empty()) {
                return json_response(req, 503, store_error_json(error));
            }
            item["latest_run"] = latest_runs.empty()
                ? json(nullptr)
                : acecode::loop::run_to_json(latest_runs.front());
            items.push_back(std::move(item));
        }
        return json_response(req, 200, {{"loops", std::move(items)}});
    });

    CROW_ROUTE(app, "/api/loops/<string>").methods(crow::HTTPMethod::GET)
    ([this, unsupported, json_response](const crow::request& req, const std::string& id) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.loop_store) return unsupported(req);
        acecode::loop::StoreError error;
        auto loop = deps.loop_store->get_loop(id, &error);
        if (!loop) {
            if (error.code.empty()) {
                error.code = "NOT_FOUND";
                error.message = "LOOP not found";
            }
            return json_response(req, error.code == "NOT_FOUND" ? 404 : 503,
                                 store_error_json(error));
        }
        return json_response(req, 200, acecode::loop::loop_to_json(*loop));
    });

    CROW_ROUTE(app, "/api/loops").methods(crow::HTTPMethod::POST)
    ([this, unsupported, json_response, parse_definition](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.loop_store) return unsupported(req);
        acecode::loop::LoopDefinition value;
        if (auto invalid = parse_definition(req, value)) return std::move(*invalid);
        acecode::loop::StoreError error;
        auto created = deps.loop_store->create_loop(std::move(value), loop_now_ms(), &error);
        if (!created) {
            return json_response(req, error.code == "SCHEDULE_CONFLICT" ? 409 : 400,
                                 store_error_json(error));
        }
        if (deps.on_loops_changed) deps.on_loops_changed();
        return json_response(req, 201, acecode::loop::loop_to_json(*created));
    });

    CROW_ROUTE(app, "/api/loops/<string>").methods(crow::HTTPMethod::PUT)
    ([this, unsupported, json_response, parse_definition](const crow::request& req,
                                                           const std::string& id) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.loop_store) return unsupported(req);
        acecode::loop::LoopDefinition value;
        if (auto invalid = parse_definition(req, value)) return std::move(*invalid);
        acecode::loop::StoreError error;
        auto updated = deps.loop_store->update_loop(id, std::move(value), loop_now_ms(), &error);
        if (!updated) {
            int status = error.code == "NOT_FOUND" ? 404 :
                         error.code == "SCHEDULE_CONFLICT" ? 409 : 400;
            return json_response(req, status, store_error_json(error));
        }
        if (deps.on_loops_changed) deps.on_loops_changed();
        return json_response(req, 200, acecode::loop::loop_to_json(*updated));
    });

    CROW_ROUTE(app, "/api/loops/<string>/enabled").methods(crow::HTTPMethod::PUT)
    ([this, unsupported, json_response](const crow::request& req, const std::string& id) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.loop_store) return unsupported(req);
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object() || !body.contains("enabled") ||
            !body["enabled"].is_boolean()) {
            return json_response(req, 400,
                {{"error", "BAD_REQUEST"}, {"message", "enabled must be boolean"}});
        }
        const bool enabled = body["enabled"].get<bool>();
        acecode::loop::StoreError error;
        auto updated = deps.loop_store->set_loop_enabled(id, enabled, loop_now_ms(), &error);
        if (!updated) {
            int status = error.code == "NOT_FOUND" ? 404 :
                         error.code == "SCHEDULE_CONFLICT" ? 409 : 400;
            return json_response(req, status, store_error_json(error));
        }
        if (deps.on_loops_changed) deps.on_loops_changed();
        return json_response(req, 200, acecode::loop::loop_to_json(*updated));
    });

    CROW_ROUTE(app, "/api/loops/<string>").methods(crow::HTTPMethod::Delete)
    ([this, unsupported, json_response](const crow::request& req, const std::string& id) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.loop_store) return unsupported(req);
        acecode::loop::StoreError error;
        if (!deps.loop_store->delete_loop(id, &error)) {
            return json_response(req, error.code == "NOT_FOUND" ? 404 : 503,
                                 store_error_json(error));
        }
        if (deps.on_loops_changed) deps.on_loops_changed();
        return json_response(req, 200, {{"ok", true}});
    });

    CROW_ROUTE(app, "/api/loops/<string>/runs").methods(crow::HTTPMethod::GET)
    ([this, unsupported, json_response](const crow::request& req, const std::string& id) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.loop_store) return unsupported(req);
        int limit = 100;
        if (const char* raw = req.url_params.get("limit")) {
            try { limit = std::clamp(std::stoi(raw), 1, 500); } catch (...) {}
        }
        acecode::loop::StoreError error;
        auto runs = deps.loop_store->list_runs(id, limit, &error);
        if (!error.code.empty()) return json_response(req, 503, store_error_json(error));
        json items = json::array();
        for (const auto& run : runs) items.push_back(acecode::loop::run_to_json(run));
        return json_response(req, 200, {{"runs", std::move(items)}});
    });
}

} // namespace acecode::web
