#pragma once

// /api/models 与 /api/sessions/:id/model 端点的纯逻辑(可单测)。Crow 路由
// 注册在 src/web/server.cpp,本头只暴露 list / find,server.cpp 自己拼
// HTTP response。
//
// list_models:返回 saved_models 数组。前端 model-picker 拉这个填充 <select>。
//
// find_model_by_name:线性查找(saved_models 通常 < 20 条),命中返回
// ModelProfile;未命中返回 nullopt。Caller 决定 404 / 400 文案。

#include "../../config/config.hpp"
#include "../../config/saved_models.hpp"
#include "../../config/saved_models_editor.hpp"
#include "../../session/session_client.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace acecode::web {

// 列出所有 saved_models。
// 输出形如:
//   [
//     {"name":"copilot-fast","provider":"copilot","model":"gpt-4o"},
//     {"name":"local-lm","provider":"openai","model":"llama-3",
//      "base_url":"http://localhost:1234/v1"}
//   ]
nlohmann::json list_models(const AppConfig& cfg);

// 通过 name 找 ModelProfile。未命中返回 nullopt。
std::optional<ModelProfile>
find_model_by_name(const AppConfig& cfg, const std::string& name);

// Serialize current per-session model state for GET/POST
// /api/sessions/:id/model.
nlohmann::json model_state_to_json(const SessionModelState& state);

// 把 SavedModelEditError 映射到 HTTP 状态码。
// 200 不触发(成功路径不调这个);其它分支:
//   - NOT_FOUND          → 404
//   - NAME_TAKEN / IN_USE_AS_DEFAULT → 409
//   - 其它(校验失败)   → 400
int http_status_for_edit_error(SavedModelEditError e);

// 把 ModelProfile 序列化到模型管理 JSON 格式。
// 给 POST/PUT 成功响应用。
nlohmann::json profile_to_safe_json(const ModelProfile& entry);

// 解析 POST/PUT body 到 SavedModelDraft。失败返 nullopt + err 写错误说明。
std::optional<SavedModelDraft> parse_model_draft(const nlohmann::json& body,
                                                  std::string& err);

struct ModelProbeRequest {
    std::string provider;
    std::string base_url;
    std::string api_key;
    std::map<std::string, std::string> request_headers;
};

std::optional<ModelProbeRequest> parse_model_probe_request(const nlohmann::json& body,
                                                           std::string& err_code,
                                                           std::string& err);

std::vector<std::string> parse_openai_model_ids(const nlohmann::json& body);

} // namespace acecode::web
