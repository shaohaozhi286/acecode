#include "model_command.hpp"

#include "../config/config.hpp"
#include "../config/saved_models.hpp"
#include "../provider/cwd_model_override.hpp"
#include "../provider/model_context_resolver.hpp"
#include "../provider/model_resolver.hpp"
#include "../provider/provider_swap.hpp"

#include <cctype>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>

namespace acecode {

namespace {

// 在 saved_models 中按 name 找;失败时如果 name == "(legacy)" 返回 legacy
// entry。其它情况返回 nullopt。调用方负责报"未知 name"错误。
std::optional<ModelProfile> lookup_entry_by_name(const AppConfig& cfg,
                                                 const std::string& name) {
    if (name == "(legacy)") return synth_legacy_entry(cfg);
    for (const auto& e : cfg.saved_models) {
        if (e.name == name) return e;
    }
    return std::nullopt;
}

// 把切换结果反馈到 TUI。调用前已做完 swap;这里只更新状态行 / 系统消息。
// 注意:调用方 MUST NOT 持 ctx.state.mu —— 本函数自己加锁。
void announce_switch(CommandContext& ctx, const ModelProfile& entry,
                     const std::string& scope_note) {
    std::lock_guard<std::mutex> lk(ctx.state.mu);
    ctx.agent_loop.set_context_window(ctx.config.context_window);
    ctx.state.token_status = ctx.token_tracker.format_status(ctx.config.context_window);
    std::ostringstream oss;
    oss << "Switched to " << entry.name
        << " (" << entry.provider << "/" << entry.model << ")";
    if (!scope_note.empty()) oss << " " << scope_note;
    std::string info = oss.str();
    ctx.state.status_line = info;
    ctx.state.conversation.push_back({"system", info, false});
    ctx.state.chat_follow_tail = true;
}

// 报"未知 name"错误。同 announce 一样自己加锁。
void report_unknown_name(CommandContext& ctx, const std::string& name) {
    std::lock_guard<std::mutex> lk(ctx.state.mu);
    std::ostringstream oss;
    oss << "Unknown model name: " << name
        << ". Run /model to pick from available.";
    ctx.state.conversation.push_back({"system", oss.str(), false});
    ctx.state.chat_follow_tail = true;
}

// 当前 effective entry —— 用 name 与 saved_models 匹配;命中 saved_models 则
// 复用,否则合成 legacy entry。仅用于 picker 高亮当前选中行。
std::string current_effective_name(CommandContext& ctx) {
    std::string pname = ctx.provider.name();
    std::string pmodel = ctx.provider.model();
    for (const auto& e : ctx.config.saved_models) {
        if (e.provider == pname && e.model == pmodel) return e.name;
    }
    return "(legacy)";
}

// 简单 trim helper —— args 里可能有多余空格。
std::string trim(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    size_t j = s.size();
    while (j > i && std::isspace(static_cast<unsigned char>(s[j - 1]))) --j;
    return s.substr(i, j - i);
}

// 解析 args 为 (flag, name) 对。flag ∈ {"", "--cwd", "--default"}。
// 返回 false 表示参数格式无效。
bool parse_model_args(const std::string& raw, std::string& flag,
                      std::string& name) {
    flag.clear();
    name.clear();
    std::string s = trim(raw);
    if (s.empty()) return true;  // 无参 → picker

    if (s.rfind("--", 0) == 0) {
        // --cwd <name> 或 --default <name>
        auto sp = s.find_first_of(" \t");
        if (sp == std::string::npos) {
            // 只给了 flag 没给 name
            flag = s;
            return false;
        }
        flag = s.substr(0, sp);
        name = trim(s.substr(sp + 1));
        if (flag != "--cwd" && flag != "--default") return false;
        if (name.empty()) return false;
        return true;
    }

    // 直接是 name —— in-memory 切换
    name = s;
    return true;
}

// 渲染 picker 列表(纯文本回退,无 FTXUI)。覆盖 saved_models + 兜底 (legacy)。
// 当前 effective 行加 "*"。这是最小骨架 —— 真正的 FTXUI picker(任务 5.2)
// 留待后续(picker 与 TuiState/screen 耦合,本 phase 用文本展示先解锁验证)。
void render_model_picker(CommandContext& ctx) {
    std::lock_guard<std::mutex> lk(ctx.state.mu);
    std::ostringstream oss;
    oss << "Available models:\n";
    std::string current = current_effective_name(ctx);
    bool legacy_listed_in_saved = false;
    for (const auto& e : ctx.config.saved_models) {
        if (e.name == "(legacy)") legacy_listed_in_saved = true;
        oss << "  " << (e.name == current ? "*" : " ")
            << " " << e.name << "  (" << e.provider << "/" << e.model << ")\n";
    }
    if (!legacy_listed_in_saved) {
        ModelProfile legacy = synth_legacy_entry(ctx.config);
        oss << "  " << (legacy.name == current ? "*" : " ")
            << " " << legacy.name << "  (" << legacy.provider << "/"
            << legacy.model << ")\n";
    }
    oss << "\nUse: /model <name>           - switch this session (in-memory)\n"
        << "     /model --cwd <name>     - switch + persist to this directory\n"
        << "     /model --default <name> - switch + persist as global default";
    ctx.state.conversation.push_back({"system", oss.str(), false});
    ctx.state.chat_follow_tail = true;
}

void cmd_model(CommandContext& ctx, const std::string& args) {
    std::string flag, name;
    if (!parse_model_args(args, flag, name)) {
        std::lock_guard<std::mutex> lk(ctx.state.mu);
        ctx.state.conversation.push_back({"system",
            "Usage: /model | /model <name> | /model --cwd <name> | /model --default <name>",
            false});
        ctx.state.chat_follow_tail = true;
        return;
    }

    // 无参 → 列表(picker 占位)
    if (flag.empty() && name.empty()) {
        render_model_picker(ctx);
        return;
    }

    auto entry = lookup_entry_by_name(ctx.config, name);
    if (!entry.has_value()) {
        report_unknown_name(ctx, name);
        return;
    }

    // 切换前保护:provider_slot 缺失则退化为旧行为。理论上启动期 main.cpp
    // 总会传入,但 init_command 等测试桩不会。
    if (ctx.provider_slot) {
        swap_provider_if_needed(ctx.provider_slot->provider,
                                ctx.provider_slot->mu, *entry, ctx.config);
    } else {
        ctx.provider.set_model(entry->model);
        ctx.config.context_window = resolve_model_context_window(
            ctx.config, ctx.provider.name(), ctx.provider.model(),
            ctx.config.context_window);
    }

    std::string scope_note;
    if (flag == "--cwd") {
        save_cwd_model_override(ctx.cwd, name);
        scope_note = "[persisted to cwd]";
    } else if (flag == "--default") {
        ctx.config.default_model_name = name;
        save_config(ctx.config);
        scope_note = "[persisted as default]";
    }
    announce_switch(ctx, *entry, scope_note);
}

} // namespace

void register_model_command(CommandRegistry& registry) {
    registry.register_command({"model", "Show or switch current model", cmd_model});
}

} // namespace acecode
