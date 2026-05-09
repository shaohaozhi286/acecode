// src/tui/model_picker.hpp
//
// /model 无参时弹出的选择器状态构造器。和现有 /resume picker 走同一种
// 状态-标志 + main.cpp 渲染分支 + 事件处理分支模式 —— 不使用 FTXUI Modal
// (main.cpp 全程没用过),而是把 picker rows 推进 TuiState,主循环
// 看到 model_picker_open=true 时画一层 inline overlay,Up/Down/Enter/Esc
// 由 main.cpp 的 CatchEvent 同 resume_picker_active 一样拦截。
//
// build_model_picker_options 是纯函数,无 IO / 无 FTXUI 依赖,可单测。
#pragma once

#include "../config/config.hpp"
#include "../config/saved_models.hpp"

#include <string>
#include <vector>

namespace acecode {

// picker 中的一行。is_current 命中当前 effective entry 时为 true,
// main.cpp 渲染时该行前缀 "*"。
struct ModelPickerOption {
    std::string name;        // saved_models name 或合成的 "(legacy)"
    std::string provider;
    std::string model;
    bool        is_current = false;
};

// 构造选项列表:cfg.saved_models 顺序保留,如果其中没有 "(legacy)" 则在
// 末尾追加一条合成 legacy entry(`synth_legacy_entry(cfg)`)。current_name
// 命中的行 is_current=true。空 saved_models 也合法 —— 只返回单条 legacy。
std::vector<ModelPickerOption> build_model_picker_options(
    const AppConfig& cfg, const std::string& current_name);

} // namespace acecode
