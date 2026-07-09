#include "headless_runner.hpp"

#include "headless_mode.hpp"

#include "../config/config.hpp"
#include "../daemon/mcp_runtime.hpp"
#include "../desktop/workspace_registry.hpp"
#include "../hooks/hook_config.hpp"
#include "../hooks/hook_manager.hpp"
#include "../lsp/lsp_service.hpp"
#include "../network/proxy_resolver.hpp"
#include "../permissions.hpp"
#include "../session/local_session_client.hpp"
#include "../session/session_registry.hpp"
#include "../skills/skill_init.hpp"
#include "../skills/skill_registry.hpp"
#include "../tool/ask_user_question_tool.hpp"
#include "../tool/builtin_tool_registry.hpp"
#include "../tool/spawn_subagent_tool.hpp"
#include "../tool/skill_view_tool.hpp"
#include "../tool/skills_tool.hpp"
#include "../tool/tool_executor.hpp"
#include "../tool/web_search/backend_router.hpp"
#include "../tool/web_search/region_detector.hpp"
#include "../tool/web_search/runtime.hpp"
#include "../utils/logger.hpp"
#include "../utils/paths.hpp"
#include "../utils/utf8_path.hpp"
#include "../web/handlers/skill_command_expander.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <io.h>
#else
#  include <unistd.h>
#endif

namespace acecode::headless {

namespace {

// ---- Ctrl+C ----
// signal handler 里只允许碰 atomic;等待循环轮询它并把 abort 转发给会话。
std::atomic<bool> g_interrupt_requested{false};
std::atomic<int>  g_interrupt_count{0};

void sigint_handler(int) {
    g_interrupt_requested.store(true);
    g_interrupt_count.fetch_add(1);
}

bool stdin_is_tty() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(STDIN_FILENO) != 0;
#endif
}

// 管道输入:读完 stdin 全部字节(按 UTF-8 处理)。
std::string read_all_stdin() {
    std::ostringstream oss;
    oss << std::cin.rdbuf();
    return oss.str();
}

std::string trim_copy(std::string s) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

// prompt 组装:stdin(管道)与位置参数可同时给,拼成
// "<stdin>\n\n<arg>" —— 支持 `git diff | acecode -p "review this diff"`
// 的经典管道用法(与 claude -p 语义一致)。
std::string build_effective_prompt(const std::string& arg_prompt) {
    std::string piped;
    if (!stdin_is_tty()) {
        piped = trim_copy(read_all_stdin());
    }
    std::string arg = trim_copy(arg_prompt);
    if (piped.empty()) return arg;
    if (arg.empty()) return piped;
    return piped + "\n\n" + arg;
}

// 从最终消息列表里取 baseline 之后最后一条非空 assistant 回复。
std::string last_assistant_text_after(SessionManager& sm, std::size_t baseline) {
    auto messages = sm.load_active_messages();
    for (std::size_t i = messages.size(); i > baseline; --i) {
        const auto& m = messages[i - 1];
        if (m.role == "assistant" && !m.content.empty()) return m.content;
    }
    return {};
}

// 配置字符串 → PermissionMode(与 daemon worker.cpp 的 permission_mode_from_config
// 同语义;那个是 worker.cpp 匿名命名空间函数,这里保留一份本地映射)。
acecode::PermissionMode permission_mode_from_config(const std::string& mode) {
    if (mode == "accept-edits" || mode == "acceptEdits") {
        return acecode::PermissionMode::AcceptEdits;
    }
    if (mode == "plan") return acecode::PermissionMode::Plan;
    if (mode == "yolo") return acecode::PermissionMode::Yolo;
    return acecode::PermissionMode::Default;
}

// prompt 以 '/' 开头时按 cwd 做 skill 命令展开(与 Web POST messages /
// spawn_subagent 同一套 try_expand_skill_command 语义)。
void expand_skill_prompt(const AppConfig& cfg,
                         const std::string& cwd,
                         std::string& prompt,
                         std::string& display_text) {
    if (prompt.empty() || prompt[0] != '/') return;
    SkillRegistry tmp;
    initialize_skill_registry(tmp, cfg, cwd);
    auto expansion = web::try_expand_skill_command(prompt, tmp);
    if (expansion.expanded) {
        display_text = prompt;
        prompt = std::move(expansion.text);
    }
}

} // namespace

int run_print_mode(const HeadlessCliOptions& opts) {
    // ---- prompt 先行:参数错误要在任何重初始化之前廉价失败 ----
    const std::string prompt = build_effective_prompt(opts.prompt);
    if (prompt.empty()) {
        std::cerr << "acecode -p: input must be provided either through stdin or "
                     "as a prompt argument\n"
                     "usage: acecode -p [--yolo] [--permission-mode <m>] "
                     "[--model <name>] [--max-turns <n>] \"<prompt>\"\n";
        return 64;
    }

    // 日志进文件,不镜像 stderr —— stdout/stderr 必须保持干净给管道消费。
    Logger::instance().init_with_rotation(get_logs_dir(), "headless", /*mirror_stderr=*/false);
    Logger::instance().set_level(LogLevel::Dbg);

    headless::set_active(true);

    const std::string cwd = acecode::current_path_utf8();

    std::string hook_config_error;
    acecode::dispatch_startup_before_model_load_hooks(cwd, &hook_config_error);
    if (!hook_config_error.empty()) {
        LOG_WARN("[headless][hooks] " + hook_config_error);
    }

    AppConfig cfg = load_config();
    {
        auto errs = validate_config(cfg);
        if (!errs.empty()) {
            for (const auto& e : errs) std::cerr << "acecode -p: config error: " << e << "\n";
            return 1;
        }
    }

    // --model:显式校验命名 profile 存在。registry 内部对未知名是"警告 +
    // 回退默认"的宽松语义,脚本场景下静默换模型是坑,这里收紧成硬错误。
    if (!opts.model_name.empty()) {
        bool found = false;
        std::string names;
        for (const auto& m : cfg.saved_models) {
            if (m.name == opts.model_name) { found = true; break; }
            if (!names.empty()) names += ", ";
            names += m.name;
        }
        if (!found) {
            std::cerr << "acecode -p: unknown model '" << opts.model_name
                      << "'; available: " << (names.empty() ? "(none)" : names)
                      << "\n";
            return 64;
        }
    }

    // --max-turns → 会话级 agent_loop.max_iterations 覆盖(cfg 是本地副本,
    // 必须在 SessionRegistry 构造之前改,entry 创建时经 set_agent_loop_config
    // 读走)。
    if (opts.max_turns > 0) {
        cfg.agent_loop.max_iterations = opts.max_turns;
    }

    // ---- 网络 / 惰性子系统(与 daemon worker.cpp 同序) ----
    network::proxy_resolver().init(cfg.network);
    network::proxy_resolver().probe_and_maybe_fallback();

    acecode::lsp::init(cfg.lsp, cwd);

    acecode::web_search::init(cfg.web_search);
    acecode::web_search::register_default_backends(
        acecode::web_search::runtime().router(), cfg.web_search);
    {
        auto cached = acecode::web_search::runtime().detector().cached_region();
        acecode::web_search::runtime().router().resolve_active(cached);
        // headless 是短命进程:region 未知时不起后台探测线程(探测结果本轮
        // 用不上还会拖慢退出),让 web_search 首次调用时自行 fallback。
    }

    // workspace 元数据:让 -p 创建的会话在 Web/桌面端的 workspace 列表可见。
    {
        std::string projects_dir = acecode::path_to_utf8(
            acecode::path_from_utf8(acecode::get_acecode_dir()) / "projects");
        acecode::desktop::ensure_workspace_metadata(projects_dir, cwd);
    }

    // ---- hooks ----
    std::string hook_load_error;
    acecode::HookConfig hook_config = acecode::load_hook_config(&hook_load_error);
    if (!hook_load_error.empty()) {
        LOG_WARN("[headless][hooks] " + hook_load_error);
    }
    acecode::HookManager hook_manager(std::move(hook_config));
    {
        std::string trust_error;
        acecode::HookTrustStore trust_store =
            acecode::load_hook_trust_store_from_path(
                acecode::default_hook_trust_state_path(), &trust_error);
        if (!trust_error.empty()) {
            LOG_WARN("[headless][hooks] " + trust_error);
        }
        acecode::HookLoadOptions hook_load;
        hook_load.feature_enabled = cfg.features.hooks;
        hook_load.cwd = cwd;
        hook_load.project_trusted = true;
        hook_manager.refresh_registry(
            acecode::load_hook_registry(hook_load, &trust_store));
    }

    // ---- skills / tools / MCP ----
    acecode::SkillRegistry skill_registry;
    acecode::initialize_skill_registry(skill_registry, cfg, cwd);

    acecode::ToolExecutor tools;
    acecode::register_session_builtin_tools(tools, cfg);
    // async 版 AskUserQuestion:headless::active() 分支在工具内部自动应答,
    // 不会真的走到 prompter。仍注册它是为了让模型看到与 daemon 一致的工具面。
    tools.register_tool(acecode::create_ask_user_question_tool_async());
    tools.register_tool(acecode::create_skills_list_tool(skill_registry, &cfg));
    tools.register_tool(acecode::create_skill_view_tool(skill_registry, &cfg));

    auto subagent_deps = std::make_shared<acecode::SubagentToolDeps>();
    tools.register_tool(acecode::create_spawn_subagent_tool(subagent_deps));
    tools.register_tool(acecode::create_wait_subagent_tool(subagent_deps));

    acecode::daemon::DaemonMcpRuntime mcp_runtime;
    mcp_runtime.start(cfg, tools);

    int exit_code = 1;
    {
        // ---- 权限模板:默认跟随配置;--yolo = dangerous(全放行) ----
        acecode::PermissionManager template_perm;
        template_perm.set_mode(
            permission_mode_from_config(cfg.default_permission_mode));
        if (opts.dangerous_mode) template_perm.set_dangerous(true);

        acecode::SessionRegistryDeps reg_deps;
        reg_deps.provider_accessor = []() -> std::shared_ptr<acecode::LlmProvider> {
            return nullptr; // config 路径下 registry 按 profile 自建 provider
        };
        reg_deps.tools                    = &tools;
        reg_deps.cwd                      = cwd;
        reg_deps.config                   = &cfg;
        reg_deps.skill_registry           = &skill_registry;
        reg_deps.memory_registry          = nullptr;
        reg_deps.memory_cfg               = nullptr;
        reg_deps.project_instructions_cfg = &cfg.project_instructions;
        reg_deps.custom_instructions_cfg  = &cfg.custom_instructions;
        reg_deps.hook_manager             = &hook_manager;
        reg_deps.template_permissions     = &template_perm;

        acecode::SessionRegistry registry(std::move(reg_deps));
        acecode::LocalSessionClient client(registry);
        subagent_deps->registry = &registry;
        subagent_deps->client   = &client;
        subagent_deps->config   = &cfg;

        SessionOptions session_opts;
        session_opts.cwd             = cwd;
        session_opts.model_name      = opts.model_name;
        session_opts.permission_mode = opts.permission_mode;
        session_opts.auto_start      = false;

        std::string session_id;
        try {
            session_id = client.create_session(session_opts);
        } catch (const std::exception& e) {
            std::cerr << "acecode -p: failed to create session: " << e.what() << "\n";
            return 1;
        }

        // provider 兜底检查:没有可用模型配置时 fail fast,而不是让回合
        // 跑起来再报一条模型错误消息。
        {
            auto entry = registry.acquire(session_id);
            if (!entry || !entry->provider_slot) {
                std::cerr << "acecode -p: session initialization failed\n";
                return 1;
            }
            std::shared_ptr<acecode::LlmProvider> p;
            {
                std::lock_guard<std::mutex> lk(entry->provider_slot->mu);
                p = entry->provider_slot->provider;
            }
            if (!p) {
                std::cerr << "acecode -p: no usable model configured; run "
                             "`acecode configure` first\n";
                return 1;
            }
        }

        // ---- 事件订阅:必须在 send_input 之前,否则可能错过 Done ----
        std::mutex wait_mu;
        std::condition_variable wait_cv;
        bool turn_done = false;
        std::string last_error_reason;

        auto sub_id = client.subscribe(
            session_id,
            [&](const SessionEvent& ev) {
                if (ev.kind == SessionEventKind::Done) {
                    std::lock_guard<std::mutex> lk(wait_mu);
                    turn_done = true;
                    wait_cv.notify_all();
                } else if (ev.kind == SessionEventKind::Error) {
                    std::lock_guard<std::mutex> lk(wait_mu);
                    if (ev.payload.contains("reason") && ev.payload["reason"].is_string()) {
                        last_error_reason = ev.payload["reason"].get<std::string>();
                    }
                }
            });

        std::size_t baseline = 0;
        if (auto entry = registry.acquire(session_id); entry && entry->sm) {
            baseline = entry->sm->load_active_messages().size();
        }

        std::string send_text = prompt;
        std::string display_text;
        expand_skill_prompt(cfg, cwd, send_text, display_text);

        std::signal(SIGINT, sigint_handler);

        if (!client.send_input(session_id, send_text, display_text)) {
            std::cerr << "acecode -p: failed to submit prompt\n";
            return 1;
        }

        // ---- 等回合结束。Ctrl+C 一次 = abort 会话(等它收尾落盘);两次 =
        // 立即退出。 ----
        bool aborted = false;
        {
            std::unique_lock<std::mutex> lk(wait_mu);
            while (!turn_done) {
                wait_cv.wait_for(lk, std::chrono::milliseconds(200));
                if (g_interrupt_requested.load() && !aborted) {
                    aborted = true;
                    lk.unlock();
                    client.abort(session_id);
                    std::cerr << "\nacecode -p: interrupted; waiting for the "
                                 "session to persist (Ctrl+C again to force quit)\n";
                    lk.lock();
                }
                if (g_interrupt_count.load() >= 2) {
                    // 第二次 Ctrl+C:用户要的是立刻死。_Exit 跳过静态析构,
                    // 避免卡在 AgentLoop 线程 join / MCP 子进程收尾上。
                    std::cerr << "acecode -p: force quit\n";
                    std::_Exit(130);
                }
            }
        }
        client.unsubscribe(session_id, sub_id);

        // ---- 结果输出 ----
        std::string final_text;
        if (auto entry = registry.acquire(session_id); entry && entry->sm) {
            final_text = last_assistant_text_after(*entry->sm, baseline);
        }

        if (aborted) {
            exit_code = 130;
        } else if (final_text.empty()) {
            std::cerr << "acecode -p: turn finished without an assistant reply"
                      << (last_error_reason.empty()
                              ? std::string{}
                              : " (last error: " + last_error_reason + ")")
                      << "\n";
            exit_code = 1;
        } else {
            std::fwrite(final_text.data(), 1, final_text.size(), stdout);
            if (final_text.empty() || final_text.back() != '\n') {
                std::fputc('\n', stdout);
            }
            std::fflush(stdout);
            exit_code = 0;
        }

        std::cerr << "[acecode] session " << session_id
                  << " saved; resume with: acecode --resume " << session_id << "\n";
    } // registry 析构:abort + join 所有 AgentLoop worker 线程,释放 writer lease

    mcp_runtime.shutdown();
    acecode::lsp::shutdown();
    return exit_code;
}

} // namespace acecode::headless
