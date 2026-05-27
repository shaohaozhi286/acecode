// src/tui/model_picker.cpp
//
// /model picker 的纯逻辑实现。把 saved_models 拆成结构化选项给
// main.cpp 的 inline overlay 渲染层 + 事件层用。
#include "model_picker.hpp"

#include "../config/model_provider_registry.hpp"

#include <utility>

namespace acecode {

std::vector<ModelPickerOption> build_model_picker_options(
    const AppConfig& cfg, const std::string& current_name) {
    std::vector<ModelPickerOption> out;

    out.reserve(cfg.saved_models.size());
    for (const auto& e : cfg.saved_models) {
        if (!is_runtime_model_provider_enabled(e.provider)) continue;
        ModelPickerOption o;
        o.name = e.name;
        o.provider = e.provider;
        o.model = e.model;
        o.is_current = (e.name == current_name);
        out.push_back(std::move(o));
    }
    return out;
}

} // namespace acecode
