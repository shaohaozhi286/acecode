#include "model_command.hpp"

#include "../config/config.hpp"
#include "../config/saved_models.hpp"
#include "../provider/apply_model_to_session.hpp"
#include "../provider/cwd_model_override.hpp"
#include "../provider/model_context_resolver.hpp"
#include "../provider/model_resolver.hpp"
#include "../tui/model_picker.hpp"

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
    auto provider_snap = ctx.provider_slot ? ctx.provider_slot->provider : nullptr;
    if (!provider_snap) return "(legacy)";
    std::string pname = provider_snap->name();
    std::string pmodel = provider_snap->model();
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

// 打开 /model picker —— 把选项列表写进 TuiState、注册 on_pick 回调,然后
// post 一帧让 main.cpp 渲染层看到 model_picker_open=true 并画 inline overlay。
// 选中行用 build_model_picker_options 算出来的 is_current=true 那条;用户
// 直接 Esc 取消时初始 highlight 不会误导。
//
// on_pick 回调 by-value 捕获 ctx 里需要的指针,不能持 ctx 引用 —— picker
// 关闭时 cmd_model 已经返回了。语义和 /resume callback 一致:apply +
// announce 都在主线程跑(事件分支调它),因此可以放心读 cfg / state。
void render_model_picker(CommandContext& ctx) {
    auto options = build_model_picker_options(ctx.config, current_effective_name(ctx));

    // 捕获 picker on_pick 需要的所有原始引用 / 指针。
    auto* state_ptr = &ctx.state;
    auto* config_ptr = &ctx.config;
    auto* provider_slot = ctx.provider_slot;
    auto* sm = ctx.session_manager;
    auto* al = &ctx.agent_loop;
    auto& token_tracker = ctx.token_tracker;

    // 约定:调用方 MUST 持 state_ptr->mu(和 /resume callback 一致 ——
    // main.cpp 的 Enter 分支已经持 unique_lock<state.mu>;callback 内部
    // 不再加锁,直接读写 state)。
    auto callback = [state_ptr, config_ptr, provider_slot, sm, al, &token_tracker](
                        const std::string& name) {
        // 找 entry —— "(legacy)" 走 synth,否则在 saved_models 里查。
        std::optional<ModelProfile> entry;
        if (name == "(legacy)") {
            entry = synth_legacy_entry(*config_ptr);
        } else {
            for (const auto& e : config_ptr->saved_models) {
                if (e.name == name) { entry = e; break; }
            }
        }
        if (!entry.has_value()) {
            state_ptr->conversation.push_back(
                {"system", "Unknown model name: " + name, false});
            state_ptr->chat_follow_tail = true;
            return;
        }

        // 切换。语义和 cmd_model 主路径里的 if (ctx.provider_slot) {...}
        // else {...} 完全一致 —— 这里没法构造完整 CommandContext,所以
        // 内联出来直接用捕获的指针走 apply_model_to_session。
        if (provider_slot) {
            ApplyModelDeps deps;
            deps.provider_slot = provider_slot;
            deps.sm = sm;
            deps.loop = al;
            deps.cfg = config_ptr;
            try {
                auto result = apply_model_to_session(*entry, deps);
                config_ptr->context_window = result.state.context_window;
                if (!result.warning.empty()) {
                    state_ptr->conversation.push_back(
                        {"system", "Warning: " + result.warning, false});
                    state_ptr->chat_follow_tail = true;
                }
            } catch (const std::exception& e) {
                state_ptr->conversation.push_back(
                    {"system", std::string("Switch failed: ") + e.what(), false});
                state_ptr->chat_follow_tail = true;
                return;
            }
        } else {
            config_ptr->context_window = resolve_model_context_window(
                *config_ptr, entry->provider, entry->model, config_ptr->context_window);
        }

        // 等价于 announce_switch(它会再加锁,这里把内联出来避免重入)。
        al->set_context_window(config_ptr->context_window);
        state_ptr->token_status = token_tracker.format_status(config_ptr->context_window);
        std::ostringstream oss;
        oss << "Switched to " << entry->name
            << " (" << entry->provider << "/" << entry->model << ")";
        std::string info = oss.str();
        state_ptr->status_line = info;
        state_ptr->conversation.push_back({"system", info, false});
        state_ptr->chat_follow_tail = true;
    };

    {
        std::lock_guard<std::mutex> lk(ctx.state.mu);
        ctx.state.model_picker_options = std::move(options);
        ctx.state.model_picker_selected = 0;
        ctx.state.model_picker_view_offset = 0;
        // 初始 highlight 落在当前 effective 行(build_model_picker_options 已标 is_current)。
        for (std::size_t i = 0; i < ctx.state.model_picker_options.size(); ++i) {
            if (ctx.state.model_picker_options[i].is_current) {
                ctx.state.model_picker_selected = static_cast<int>(i);
                break;
            }
        }
        ctx.state.model_picker_callback = std::move(callback);
        ctx.state.model_picker_open = true;
    }
    if (ctx.post_event) ctx.post_event();
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

    // 切换前保护:provider_slot 缺失则忽略(测试桩没接 slot 时只更新配置)。
    // 启动期 main.cpp 总会传入。
    if (ctx.provider_slot) {
        ApplyModelDeps deps;
        deps.provider_slot = ctx.provider_slot;
        deps.sm = ctx.session_manager;
        deps.loop = &ctx.agent_loop;
        deps.cfg = &ctx.config;
        try {
            auto result = apply_model_to_session(*entry, deps);
            ctx.config.context_window = result.state.context_window;
            if (!result.warning.empty()) {
                std::lock_guard<std::mutex> lk(ctx.state.mu);
                ctx.state.conversation.push_back({"system",
                    "Warning: " + result.warning, false});
                ctx.state.chat_follow_tail = true;
            }
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(ctx.state.mu);
            ctx.state.conversation.push_back({"system",
                std::string("Switch failed: ") + e.what(), false});
            ctx.state.chat_follow_tail = true;
            return;
        }
    } else {
        // 无 slot —— 至少把 context_window 按目标 entry 重算,避免 picker 显示
        // 与实际不一致。set_model / 真切换交给上层注入 slot 后再做。
        ctx.config.context_window = resolve_model_context_window(
            ctx.config, entry->provider, entry->model,
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
