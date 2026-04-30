#pragma once

// /api/models 与 /api/sessions/:id/model 端点的纯逻辑(可单测)。Crow 路由
// 注册在 src/web/server.cpp,本头只暴露 list / find,server.cpp 自己拼
// HTTP response。
//
// list_models:返回 saved_models 数组 + 末尾追加合成 (legacy) 行(对应
// `synth_legacy_entry(cfg)`)。前端 model-picker 拉这个填充 <select>。
//
// find_model_by_name:线性查找(saved_models 通常 < 20 条),命中返回
// ModelProfile;未命中返回 nullopt。Caller 决定 404 / 400 文案。

#include "../../config/config.hpp"
#include "../../config/saved_models.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace acecode::web {

// 列出所有 saved_models + 一个合成 (legacy) 行。
// 输出形如:
//   [
//     {"name":"copilot-fast","provider":"copilot","model":"gpt-4o","is_legacy":false},
//     {"name":"local-lm","provider":"openai","model":"llama-3",
//      "base_url":"http://localhost:1234/v1","is_legacy":false},
//     {"name":"(legacy)","provider":"copilot","model":"gpt-4o","is_legacy":true}
//   ]
nlohmann::json list_models(const AppConfig& cfg);

// 通过 name 找 ModelProfile。"(legacy)" 走 synth_legacy_entry(cfg)。
// 未命中返回 nullopt。
std::optional<ModelProfile>
find_model_by_name(const AppConfig& cfg, const std::string& name);

} // namespace acecode::web
