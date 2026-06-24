#pragma once

// 解析 WebView bridge 传来的 aceDesktop_setTrayMenu 参数。
//
// 不同 webview 绑定会把 JS 调用参数包装成 JSON array;前端当前为了和既有
// bridge 调用保持一致,传的是 JSON.stringify(payload),所以 native 需要兼容:
//   - [{"workspace_name": "...", "pinned": [], "recent": []}]
//   - ["{\"workspace_name\":\"...\",\"pinned\":[],\"recent\":[]}"]
//   - {"workspace_name": "...", "pinned": [], "recent": []}

#include "tray_icon_win.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace acecode::desktop {

inline nlohmann::json tray_menu_payload_json_from_bridge_request(const std::string& req) {
    nlohmann::json root = nlohmann::json::parse(req);
    nlohmann::json payload = root;

    if (root.is_array()) {
        if (root.empty()) {
            throw std::invalid_argument("expect [{workspace_name,pinned,recent}] or [JSON string]");
        }
        payload = root[0];
    }

    if (payload.is_string()) {
        payload = nlohmann::json::parse(payload.get<std::string>());
    }

    if (!payload.is_object()) {
        throw std::invalid_argument("expect {workspace_name,pinned,recent}");
    }
    return payload;
}

inline TrayMenuPayload tray_menu_payload_from_bridge_request(const std::string& req) {
    const nlohmann::json p = tray_menu_payload_json_from_bridge_request(req);

    TrayMenuPayload payload;
    if (p.contains("workspace_name") && p["workspace_name"].is_string()) {
        payload.workspace_name = p["workspace_name"].get<std::string>();
    }

    auto fill = [](const nlohmann::json& src, std::vector<TrayMenuItem>& dst) {
        if (!src.is_array()) return;
        for (const auto& it : src) {
            if (!it.is_object()) continue;
            TrayMenuItem item;
            if (it.contains("session_id") && it["session_id"].is_string()) {
                item.session_id = it["session_id"].get<std::string>();
            }
            if (it.contains("workspace_hash") && it["workspace_hash"].is_string()) {
                item.workspace_hash = it["workspace_hash"].get<std::string>();
            }
            if (it.contains("title") && it["title"].is_string()) {
                item.title = it["title"].get<std::string>();
            }
            if (it.contains("subtitle") && it["subtitle"].is_string()) {
                item.subtitle = it["subtitle"].get<std::string>();
            }
            if (item.session_id.empty()) continue;
            dst.push_back(std::move(item));
        }
    };

    if (p.contains("pinned")) fill(p["pinned"], payload.pinned);
    if (p.contains("recent")) fill(p["recent"], payload.recent);
    return payload;
}

} // namespace acecode::desktop
