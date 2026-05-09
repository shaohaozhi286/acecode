// src/tui/model_picker.cpp
//
// /model picker 的纯逻辑实现。行为和 model_command.cpp::render_model_picker
// 旧版本(纯文本 fallback)的列表构造完全一致 —— 只是把列表拆成结构化
// 选项给 main.cpp 的 inline overlay 渲染层 + 事件层用。
#include "model_picker.hpp"

#include "../provider/model_resolver.hpp"

#include <utility>

namespace acecode {

std::vector<ModelPickerOption> build_model_picker_options(
    const AppConfig& cfg, const std::string& current_name) {
    std::vector<ModelPickerOption> out;
    bool legacy_in_saved = false;

    out.reserve(cfg.saved_models.size() + 1);
    for (const auto& e : cfg.saved_models) {
        ModelPickerOption o;
        o.name = e.name;
        o.provider = e.provider;
        o.model = e.model;
        o.is_current = (e.name == current_name);
        if (e.name == "(legacy)") legacy_in_saved = true;
        out.push_back(std::move(o));
    }

    if (!legacy_in_saved) {
        ModelProfile legacy = synth_legacy_entry(cfg);
        ModelPickerOption o;
        o.name = legacy.name;
        o.provider = legacy.provider;
        o.model = legacy.model;
        o.is_current = (legacy.name == current_name);
        out.push_back(std::move(o));
    }
    return out;
}

} // namespace acecode
