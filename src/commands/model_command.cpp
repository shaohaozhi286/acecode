#include "model_command.hpp"

#include "../config/config.hpp"
#include "../config/model_provider_registry.hpp"
#include "../config/saved_models.hpp"
#include "../config/saved_models_editor.hpp"
#include "../provider/apply_model_to_session.hpp"
#include "../provider/cwd_model_override.hpp"
#include "../provider/model_context_resolver.hpp"
#include "../tui/model_picker.hpp"

#include <cctype>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace acecode {

namespace {

// 在 saved_models 中按 name 找;失败时返回 nullopt。调用方负责报"未知 name"错误。
std::optional<ModelProfile> lookup_entry_by_name(const AppConfig& cfg,
                                                 const std::string& name) {
    for (const auto& e : cfg.saved_models) {
        if (e.name == name && is_runtime_model_provider_enabled(e.provider)) return e;
    }
    return std::nullopt;
}

int parse_nonnegative_int_or_invalid(const std::string& value) {
    if (value.empty()) return -1;
    long long parsed = 0;
    for (char ch : value) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) return -1;
        parsed = parsed * 10 + (ch - '0');
        if (parsed > std::numeric_limits<int>::max()) return -1;
    }
    return static_cast<int>(parsed);
}

// 把切换结果反馈到 TUI。调用前已做完 swap;这里只更新状态行 / 系统消息。
// 注意:调用方 MUST NOT 持 ctx.state.mu —— 本函数自己加锁。
void announce_switch(CommandContext& ctx, const ModelProfile& entry,
                     const std::string& scope_note) {
    std::lock_guard<std::mutex> lk(ctx.state.mu);
    ctx.agent_loop.set_context_window(ctx.config.context_window);
    ctx.state.token_status = ctx.token_tracker.format_status(ctx.config.context_window);
    ctx.state.token_percent = ctx.token_tracker.context_percent(ctx.config.context_window);
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

// 当前 effective entry —— 用 provider/model 与 saved_models 匹配。
// 仅用于 picker 高亮当前选中行。
std::string current_effective_name(CommandContext& ctx) {
    auto provider_snap = ctx.provider_slot ? ctx.provider_slot->provider : nullptr;
    if (!provider_snap) return "";
    std::string pname = provider_snap->name();
    std::string pmodel = provider_snap->model();
    for (const auto& e : ctx.config.saved_models) {
        if (e.provider == pname && e.model == pmodel) return e.name;
    }
    return "";
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
    if (options.empty()) {
        {
            std::lock_guard<std::mutex> lk(ctx.state.mu);
            ctx.state.conversation.push_back({
                "system",
                u8"没有已配置的模型。请先运行 acecode configure，或使用 /model add 添加模型。",
                false
            });
            ctx.state.chat_follow_tail = true;
            ctx.state.model_picker_open = false;
            ctx.state.model_picker_options.clear();
        }
        if (ctx.post_event) ctx.post_event();
        return;
    }

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
        // 找 entry。
        std::optional<ModelProfile> entry;
        for (const auto& e : config_ptr->saved_models) {
            if (e.name == name && is_runtime_model_provider_enabled(e.provider)) {
                entry = e;
                break;
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
            config_ptr->context_window = resolve_model_profile_context_window(
                *config_ptr, *entry, config_ptr->context_window);
        }

        // 等价于 announce_switch(它会再加锁,这里把内联出来避免重入)。
        al->set_context_window(config_ptr->context_window);
        state_ptr->token_status = token_tracker.format_status(config_ptr->context_window);
        state_ptr->token_percent = token_tracker.context_percent(config_ptr->context_window);
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

void render_default_model_picker(CommandContext& ctx) {
    auto options = build_model_picker_options(ctx.config, ctx.config.default_model_name);
    if (options.empty()) {
        {
            std::lock_guard<std::mutex> lk(ctx.state.mu);
            ctx.state.conversation.push_back({
                "system",
                u8"没有已配置的模型。请先运行 acecode configure，或使用 /model add 添加模型。",
                false
            });
            ctx.state.chat_follow_tail = true;
            ctx.state.model_picker_open = false;
            ctx.state.model_picker_options.clear();
        }
        if (ctx.post_event) ctx.post_event();
        return;
    }

    auto* state_ptr = &ctx.state;
    auto* config_ptr = &ctx.config;

    auto callback = [state_ptr, config_ptr](const std::string& name) {
        bool found = false;
        for (const auto& e : config_ptr->saved_models) {
            if (e.name == name && is_runtime_model_provider_enabled(e.provider)) {
                found = true;
                break;
            }
        }
        if (!found) {
            state_ptr->conversation.push_back(
                {"system", "Unknown model name: " + name, false});
            state_ptr->chat_follow_tail = true;
            return;
        }

        const std::string before = config_ptr->default_model_name;
        config_ptr->default_model_name = name;
        try {
            save_config(*config_ptr);
        } catch (const std::exception& e) {
            config_ptr->default_model_name = before;
            state_ptr->conversation.push_back(
                {"system", std::string("/model set-default: write failed: ") + e.what(), false});
            state_ptr->chat_follow_tail = true;
            return;
        }
        state_ptr->conversation.push_back({"system", "Default: " + name, false});
        state_ptr->chat_follow_tail = true;
    };

    {
        std::lock_guard<std::mutex> lk(ctx.state.mu);
        ctx.state.model_picker_options = std::move(options);
        ctx.state.model_picker_selected = 0;
        ctx.state.model_picker_view_offset = 0;
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

// 把 SavedModelDraft 从 kvs 抽出来。default_name 只在 edit 路径用 ——
// 用户没显式给 name= 时回落到 sub 后面的 bare name。
SavedModelDraft draft_from_kvs(const std::map<std::string, std::string>& kvs,
                                const std::string& default_name = "") {
    SavedModelDraft d;
    auto get = [&](const char* k, std::string& out) {
        auto it = kvs.find(k);
        if (it != kvs.end()) out = it->second;
    };
    get("name", d.name);
    if (d.name.empty()) d.name = default_name;
    get("provider", d.provider);
    get("model", d.model);
    get("base_url", d.base_url);
    get("api_key", d.api_key);
    auto it_pid = kvs.find("models_dev_provider_id");
    if (it_pid != kvs.end()) d.models_dev_provider_id = it_pid->second;
    auto it_context = kvs.find("context_window");
    if (it_context != kvs.end()) {
        d.context_window = parse_nonnegative_int_or_invalid(it_context->second);
    }
    auto it_stream_timeout = kvs.find("stream_timeout_ms");
    if (it_stream_timeout != kvs.end()) {
        d.stream_timeout_ms = parse_nonnegative_int_or_invalid(it_stream_timeout->second);
    }
    return d;
}

// 把编辑结果写到 conversation 行。OK 用 ok_msg,非 OK 用 to_string(rc)。
void announce_editor_result(CommandContext& ctx, SavedModelEditError rc,
                              const std::string& ok_msg) {
    std::lock_guard<std::mutex> lk(ctx.state.mu);
    if (rc == SavedModelEditError::OK) {
        ctx.state.conversation.push_back({"system", ok_msg, false});
    } else {
        ctx.state.conversation.push_back({"system",
            std::string("/model failed: ") + to_string(rc), false});
    }
    ctx.state.chat_follow_tail = true;
}

void cmd_model_add(CommandContext& ctx, const ParsedModelSub& p) {
    auto d = draft_from_kvs(p.kvs);
    auto rc = add_saved_model(ctx.config, d);
    if (rc == SavedModelEditError::OK) {
        try {
            save_config(ctx.config);
        } catch (const std::exception& e) {
            // 回滚内存:add 把 entry push_back 到末尾,所以 pop 即可。
            if (!ctx.config.saved_models.empty()) ctx.config.saved_models.pop_back();
            std::lock_guard<std::mutex> lk(ctx.state.mu);
            ctx.state.conversation.push_back({"system",
                std::string("/model add: write failed: ") + e.what(), false});
            ctx.state.chat_follow_tail = true;
            return;
        }
    }
    announce_editor_result(ctx, rc, "Added: " + d.name);
}

void cmd_model_edit(CommandContext& ctx, const ParsedModelSub& p) {
    if (p.name.empty()) {
        announce_editor_result(ctx, SavedModelEditError::INVALID_NAME, "");
        return;
    }
    auto d = draft_from_kvs(p.kvs, p.name);
    // patch 语义:缺省字段 fallback 到现有条目,避免误清空 base_url/api_key。
    for (const auto& e : ctx.config.saved_models) {
        if (e.name == p.name) {
            if (d.provider.empty()) d.provider = e.provider;
            if (d.model.empty()) d.model = e.model;
            if (d.base_url.empty()) d.base_url = e.base_url;
            if (d.api_key.empty()) d.api_key = e.api_key;
            if (!d.models_dev_provider_id.has_value())
                d.models_dev_provider_id = e.models_dev_provider_id;
            if (!d.context_window.has_value())
                d.context_window = e.context_window;
            if (!d.stream_timeout_ms.has_value())
                d.stream_timeout_ms = e.stream_timeout_ms;
            break;
        }
    }
    auto snapshot = ctx.config.saved_models;
    auto rc = update_saved_model(ctx.config, p.name, d);
    if (rc == SavedModelEditError::OK) {
        try {
            save_config(ctx.config);
        } catch (const std::exception& e) {
            ctx.config.saved_models = std::move(snapshot);
            std::lock_guard<std::mutex> lk(ctx.state.mu);
            ctx.state.conversation.push_back({"system",
                std::string("/model edit: write failed: ") + e.what(), false});
            ctx.state.chat_follow_tail = true;
            return;
        }
    }
    announce_editor_result(ctx, rc, "Updated: " + d.name);
}

void cmd_model_rm(CommandContext& ctx, const ParsedModelSub& p) {
    if (p.name.empty()) {
        announce_editor_result(ctx, SavedModelEditError::INVALID_NAME, "");
        return;
    }
    auto snapshot = ctx.config.saved_models;
    auto rc = remove_saved_model(ctx.config, p.name);
    if (rc == SavedModelEditError::OK) {
        try {
            save_config(ctx.config);
        } catch (const std::exception& e) {
            ctx.config.saved_models = std::move(snapshot);
            std::lock_guard<std::mutex> lk(ctx.state.mu);
            ctx.state.conversation.push_back({"system",
                std::string("/model rm: write failed: ") + e.what(), false});
            ctx.state.chat_follow_tail = true;
            return;
        }
    }
    announce_editor_result(ctx, rc, "Removed: " + p.name);
}

void cmd_model_set_default(CommandContext& ctx, const ParsedModelSub& p) {
    if (p.name.empty()) {
        render_default_model_picker(ctx);
        return;
    }
    bool found = false;
    for (const auto& e : ctx.config.saved_models) {
        if (e.name == p.name && is_runtime_model_provider_enabled(e.provider)) {
            found = true;
            break;
        }
    }
    if (!found) {
        announce_editor_result(ctx, SavedModelEditError::NOT_FOUND, "");
        return;
    }
    std::string before = ctx.config.default_model_name;
    ctx.config.default_model_name = p.name;
    try {
        save_config(ctx.config);
    } catch (const std::exception& e) {
        ctx.config.default_model_name = before;
        std::lock_guard<std::mutex> lk(ctx.state.mu);
        ctx.state.conversation.push_back({"system",
            std::string("/model set-default: write failed: ") + e.what(), false});
        ctx.state.chat_follow_tail = true;
        return;
    }
    announce_editor_result(ctx, SavedModelEditError::OK, "Default: " + p.name);
}

void cmd_model(CommandContext& ctx, const std::string& args) {
    ParsedModelSub p;
    if (!parse_model_subcommand(args, p)) {
        std::lock_guard<std::mutex> lk(ctx.state.mu);
        ctx.state.conversation.push_back({"system",
            "Usage: /model | /model <name> | /model --cwd <name> | /model --default <name>\n"
            "       /model add name=X provider=openai|anthropic model=Y base_url=Z api_key=K [context_window=N] [stream_timeout_ms=N]\n"
            "       /model edit <name> [field=value ...]\n"
            "       /model rm <name>\n"
            "       /model set-default [name]",
            false});
        ctx.state.chat_follow_tail = true;
        return;
    }

    if (p.sub == "add")          { cmd_model_add(ctx, p); return; }
    if (p.sub == "edit")         { cmd_model_edit(ctx, p); return; }
    if (p.sub == "rm")           { cmd_model_rm(ctx, p); return; }
    if (p.sub == "set-default")  { cmd_model_set_default(ctx, p); return; }

    // 无参 → 列表(picker 占位)
    if (p.flag.empty() && p.name.empty()) {
        render_model_picker(ctx);
        return;
    }

    auto entry = lookup_entry_by_name(ctx.config, p.name);
    if (!entry.has_value()) {
        report_unknown_name(ctx, p.name);
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
        ctx.config.context_window = resolve_model_profile_context_window(
            ctx.config, *entry, ctx.config.context_window);
    }

    std::string scope_note;
    if (p.flag == "--cwd") {
        save_cwd_model_override(ctx.cwd, p.name);
        scope_note = "[persisted to cwd]";
    } else if (p.flag == "--default") {
        ctx.config.default_model_name = p.name;
        save_config(ctx.config);
        scope_note = "[persisted as default]";
    }
    announce_switch(ctx, *entry, scope_note);
}

} // namespace

// parse_model_subcommand 暴露给单测和 cmd_model。raw 是 /model 后面的
// 字符串。命中 add/edit/rm/set-default 时 out.sub 非空,否则回退到旧的
// flag/name 解析(无 sub 路径)。返回 false 表示参数格式无效。
bool parse_model_subcommand(const std::string& raw, ParsedModelSub& out) {
    out = {};
    std::string s = trim(raw);
    if (s.empty()) return true;  // 无参 → picker

    static const std::vector<std::string> SUBS = {
        "add", "edit", "rm", "set-default"};
    for (const auto& k : SUBS) {
        // 必须是完整 token —— "add" 或 "add <rest>"。"adder ..." 不命中。
        if (s == k || (s.size() > k.size() && s.compare(0, k.size(), k) == 0
                       && std::isspace(static_cast<unsigned char>(s[k.size()])))) {
            out.sub = k;
            std::string rest = s.size() > k.size()
                ? trim(s.substr(k.size())) : std::string{};

            if (k == "add") {
                // add 全 kv,无 bare name(name 通过 name=... 提供)。
                std::istringstream iss(rest);
                std::string tok;
                while (iss >> tok) {
                    auto eq = tok.find('=');
                    if (eq == std::string::npos) return false;
                    out.kvs[tok.substr(0, eq)] = tok.substr(eq + 1);
                }
            } else {
                // edit/rm 要求第一个 token 是 name;set-default 无参打开
                // 默认模型 picker,有参时第一个 token 是 name。
                std::istringstream iss(rest);
                std::string tok;
                if (!(iss >> tok)) {
                    if (k == "set-default") return true;
                    return false;
                }
                out.name = tok;
                while (iss >> tok) {
                    auto eq = tok.find('=');
                    if (eq == std::string::npos) return false;
                    out.kvs[tok.substr(0, eq)] = tok.substr(eq + 1);
                }
            }
            return true;
        }
    }

    // 不是已知 sub → fallback 到旧的 flag/name 解析。
    std::string flag, name;
    if (!parse_model_args(raw, flag, name)) return false;
    out.flag = flag;
    out.name = name;
    return true;
}

void register_model_command(CommandRegistry& registry) {
    registry.register_command({"model", "Show or switch current model", cmd_model});
}

} // namespace acecode
