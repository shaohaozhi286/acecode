// routes_lsp.cpp — /api/lsp/* 路由(每会话 LSP 指示器,聊天头部显示)。
// 只读状态查询,直接从进程级 LSP 单例取廉价快照,不触发 spawn。
#include "../server_impl.hpp"
#include "../../lsp/lsp_service.hpp"

namespace acecode::web {

using nlohmann::json;

void WebServer::Impl::register_lsp() {
        CROW_ROUTE(app, "/api/lsp/status").methods(crow::HTTPMethod::Options)
        ([this](const crow::request& req) {
            return cors_preflight(req);
        });

        // GET /api/lsp/status?cwd=<abs>
        //   → {enabled, servers:[{server_id, root, open_files}]}
        // servers 只含 root 落在该会话 cwd(workspace)之内的已连接 server。
        // LSP 未初始化 / 禁用 / cwd 为空 → servers 为空数组。
        CROW_ROUTE(app, "/api/lsp/status").methods(crow::HTTPMethod::GET)
        ([this](const crow::request& req) {
            if (auto rej = require_auth(req)) return std::move(*rej);

            std::string cwd_q;
            if (auto c = req.url_params.get("cwd")) cwd_q = c;

            json servers = json::array();
            bool enabled = false;
            if (acecode::lsp::is_initialized()) {
                auto& svc = acecode::lsp::service();
                enabled = svc.enabled();
                if (enabled && !cwd_q.empty()) {
                    for (const auto& e : svc.connected_for_cwd(cwd_q)) {
                        servers.push_back({
                            {"server_id", e.server_id},
                            {"root", e.root},
                            {"open_files", e.open_files},
                        });
                    }
                }
            }

            json out{{"enabled", enabled}, {"servers", std::move(servers)}};
            crow::response r(200, out.dump());
            r.add_header("Content-Type", "application/json");
            return with_cors(req, std::move(r));
        });
}

} // namespace acecode::web
