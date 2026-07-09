#include "worker.hpp"

#include "../desktop/folder_picker.hpp"
#include "../desktop/open_in_explorer.hpp"
#include "../tool/spawn_subagent_tool.hpp"
#include "../desktop/workspace_registry.hpp"
#include "guid.hpp"
#include "heartbeat.hpp"
#include "mcp_runtime.hpp"
#include "platform.hpp"
#include "runtime_files.hpp"
#include "../provider/cwd_model_override.hpp"
#include "../provider/copilot_provider.hpp"
#include "../provider/model_pool_status.hpp"
#include "../provider/model_resolver.hpp"
#include "../provider/provider_factory.hpp"
#include "../hooks/hook_config.hpp"
#include "../hooks/hook_manager.hpp"
#include "../hooks/hook_payload.hpp"
#include "../session/local_session_client.hpp"
#include "../session/session_registry.hpp"
#include "../skills/skill_registry.hpp"
#include "../skills/skill_init.hpp"
#include "../tool/ask_user_question_tool.hpp"
#include "../tool/bash_tool.hpp"
#include "../tool/builtin_tool_registry.hpp"
#include "../tool/file_read_tool.hpp"
#include "../tool/file_write_tool.hpp"
#include "../tool/file_edit_tool.hpp"
#include "../tool/grep_tool.hpp"
#include "../tool/glob_tool.hpp"
#include "../tool/goal_tool.hpp"
#include "../tool/skill_view_tool.hpp"
#include "../tool/skills_tool.hpp"
#include "../tool/task_complete_tool.hpp"
#include "../tool/tool_executor.hpp"
#include "../lsp/lsp_service.hpp"
#include "../tool/web_search/runtime.hpp"
#include "../tool/web_search/backend_router.hpp"
#include "../tool/web_search/region_detector.hpp"
#include "../tool/web_search/web_search_tool.hpp"
#include "../network/proxy_resolver.hpp"
#include "../utils/logger.hpp"
#include "../utils/paths.hpp"
#include "../utils/token.hpp"
#include "../utils/utf8_path.hpp"
#include "../web/auth.hpp"
#include "../web/pty/pty_session_registry.hpp"
#include "../web/server.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace acecode::daemon {

namespace {

// 终止信号 → 唤醒主循环退出。POSIX 与 Windows 各有一套。
// 文件级而非 anon-namespace,因为 ServiceMain 的 SCM 控制 handler 也要触发它
// (经 worker.hpp 暴露的 request_worker_termination)。
std::mutex              g_term_mu;
std::condition_variable g_term_cv;
std::atomic<bool>       g_term_requested{false};

void request_terminate() {
    g_term_requested.store(true);
    g_term_cv.notify_all();
}

#ifdef _WIN32
BOOL WINAPI win_console_handler(DWORD ctrl) {
    switch (ctrl) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            request_terminate();
            return TRUE;
        default:
            return FALSE;
    }
}
#else
extern "C" void posix_term_handler(int /*signo*/) {
    request_terminate();
}
#endif

void install_term_handlers() {
#ifdef _WIN32
    ::SetConsoleCtrlHandler(win_console_handler, TRUE);
#else
    std::signal(SIGTERM, posix_term_handler);
    std::signal(SIGINT,  posix_term_handler);
#endif
}

bool apply_cwd_override(const std::string& raw, bool foreground) {
    if (raw.empty()) return true;

    namespace fs = std::filesystem;

    fs::path requested = acecode::path_from_utf8(acecode::expand_path(raw));
    std::error_code ec;
    fs::path effective = requested.is_absolute()
        ? requested
        : fs::absolute(requested, ec);
    if (ec) effective = requested;

    auto canonical = fs::weakly_canonical(effective, ec);
    if (!ec && !canonical.empty()) {
        effective = canonical;
    }

    std::error_code dir_ec;
    if (!fs::exists(effective, dir_ec) || !fs::is_directory(effective, dir_ec)) {
        std::string msg = "[daemon] --cwd path is not a directory: " + acecode::path_to_utf8(effective);
        LOG_ERROR(msg);
        if (foreground) std::cerr << msg << "\n";
        return false;
    }

    fs::current_path(effective, dir_ec);
    if (dir_ec) {
        std::string msg = "[daemon] failed to switch --cwd to " + acecode::path_to_utf8(effective)
            + ": " + dir_ec.message();
        LOG_ERROR(msg);
        if (foreground) std::cerr << msg << "\n";
        return false;
    }

    std::string msg = "[daemon] cwd=" + acecode::path_to_utf8(fs::current_path())
        + " (from --cwd=" + raw + ")";
    LOG_INFO(msg);
    if (foreground) std::cerr << msg << "\n";
    return true;
}

acecode::PermissionMode permission_mode_from_config(const std::string& mode) {
    if (mode == "accept-edits" || mode == "acceptEdits") {
        return acecode::PermissionMode::AcceptEdits;
    }
    if (mode == "plan") return acecode::PermissionMode::Plan;
    if (mode == "yolo") return acecode::PermissionMode::Yolo;
    return acecode::PermissionMode::Default;
}

} // namespace

std::string validate_can_start(const WorkerOptions& opts,
                               int heartbeat_timeout_ms) {
    auto existing_guid = read_guid_file();

    if (opts.supervised) {
        // launcher 派 GUID 进来。如果磁盘已有 guid 但跟 launcher 派的不一致,
        // 视为另一个 launcher 已抢占了 daemon.guid,拒启。
        if (existing_guid.has_value() && !existing_guid->empty() &&
            *existing_guid != opts.guid) {
            return "another supervised worker already owns daemon.guid (expected="
                   + opts.guid + " actual=" + *existing_guid + ")";
        }
        return {};
    }

    RuntimeValidationOptions validation_options;
    validation_options.heartbeat_timeout_ms = heartbeat_timeout_ms;
    RuntimeSnapshot snapshot = read_runtime_snapshot();
    RuntimeReuseCheck reuse = validate_runtime_snapshot_for_reuse(snapshot, validation_options);

    // standalone: 只有当 runtime file bundle 仍描述一个当前可用 daemon 时拒启。
    if (existing_guid.has_value() && reuse.reusable && snapshot.pid.has_value()) {
        std::ostringstream oss;
        oss << "another daemon already running (pid=" << *snapshot.pid
            << " guid=" << *existing_guid << ")";
        return oss.str();
    }
    if ((existing_guid.has_value() || snapshot.pid.has_value()) && !reuse.reusable) {
        LOG_WARN("[daemon] ignoring stale runtime files during startup: " + reuse.reason);
    }
    return {};
}

int run_worker(const WorkerOptions& opts, const AppConfig& cfg) {
    // daemon 模式日志切换(spec 12.1-12.3): 写到 ~/.acecode/logs/daemon-{date}.log,
    // 跨午夜自动滚动文件;foreground=true 时同时镜像到 stderr。必须放在 preflight
    // 之前,否则启动期校验失败时不会留下任何日志记录。
    Logger::instance().init_with_rotation(get_logs_dir(), "daemon", opts.foreground);
    Logger::instance().set_level(LogLevel::Dbg);

    if (!apply_cwd_override(opts.cwd_override, opts.foreground)) {
        return 13;
    }

    // 启动前硬安全校验(spec 11.3)。token 还没生成,这里先做 dangerous 检查;
    // 非 loopback 缺 token 的检查放在 token 生成后,因为我们生成 token = 总是
    // 满足。但若用户后续传入外部配置开关禁用 token,应在那里失败。当前 v1
    // 总是生成 token → 非 loopback 也通过 preflight。
    auto preflight_dangerous_only = acecode::web::preflight_bind_check(
        cfg.web.bind, /*server_token=*/"placeholder", opts.dangerous);
    if (!preflight_dangerous_only.empty()) {
        std::cerr << preflight_dangerous_only << "\n";
        return 2;
    }

    auto reject = validate_can_start(opts, cfg.daemon.heartbeat_timeout_ms);
    if (!reject.empty()) {
        std::cerr << "[daemon] refuse to start: " << reject << "\n";
        return 3;
    }

    // 代理解析器初始化 —— 必须在第一个 cpr 调用前完成。daemon 路径无 TUI,
    // 横幅写到日志(LOG_INFO)。
    network::proxy_resolver().init(cfg.network);
    network::proxy_resolver().probe_and_maybe_fallback();
    {
        auto resolved = network::proxy_resolver().effective("https://example.com");
        std::ostringstream oss;
        if (resolved.source == "auto-fallback") {
            auto fb = network::proxy_resolver().fallback_info_snapshot();
            oss << "[proxy] effective=direct (auto-fallback: " << fb.original_url
                << " from " << fb.original_source << " unreachable; reason=" << fb.reason << ")";
        } else {
            oss << "[proxy] effective="
                << (resolved.url.empty() ? "direct" : network::redact_credentials(resolved.url))
                << " source=" << resolved.source
                << " mode=" << cfg.network.proxy_mode;
        }
        LOG_INFO(oss.str());
        if (opts.foreground) std::cerr << oss.str() << "\n";
    }

    // 模型池负载监控:仅当配置里存在 PUB 池模型时才起 30s 轮询,避免在没有这些
    // 模型的机器上无谓地打外网接口。停在 worker 收尾段(server.run() 返回后)。
    {
        bool has_pub = false;
        for (const auto& m : cfg.saved_models) {
            if (acecode::is_pub_model(m.model)) { has_pub = true; break; }
        }
        if (has_pub) {
            LOG_INFO("[model_pool] PUB model(s) configured; starting 30s load monitor");
            acecode::model_pool_status_service().start();
        }
    }

    ensure_run_dir();

    // GUID: supervised 用 launcher 派的;standalone 自己生成。
    std::string guid = opts.supervised ? opts.guid : generate_daemon_guid();
    std::int64_t pid = current_pid();

    // 整个 daemon 路径只用 cfg_mut 一份本地可变副本: /api/mcp PUT 要写、
    // desktop 父进程注入的 port_override 也要写。原本只服务前者,现在两用合一,
    // 后续所有引用都从 cfg_mut 取(原代码部分位置仍引用 const cfg,见底部 web_deps)。
    AppConfig cfg_mut = cfg;
    if (opts.port_override > 0) {
        cfg_mut.web.port = opts.port_override;
    }
    if (!opts.static_dir_override.empty()) {
        cfg_mut.web.static_dir = opts.static_dir_override;
    }

    // 写运行时产物。顺序: guid → pid → port → token,失败立刻退出。
    if (!write_guid_file(guid))                 { std::cerr << "write guid failed\n"; return 4; }
    if (!write_pid_file(pid))                   { std::cerr << "write pid failed\n"; return 4; }
    if (!write_port_file(cfg_mut.web.port))     { std::cerr << "write port failed\n"; return 4; }

    std::string token = !opts.token_override.empty()
        ? opts.token_override
        : acecode::generate_auth_token();
    if (token.empty() || !write_token(token)) {
        std::cerr << "write token failed\n";
        return 4;
    }

    {
        std::ostringstream oss;
        oss << "[daemon] worker started pid=" << pid
            << " guid=" << guid
            << " bind=" << cfg_mut.web.bind << ":" << cfg_mut.web.port
            << (opts.supervised ? " mode=supervised" : " mode=standalone");
        LOG_INFO(oss.str());
        if (opts.foreground) std::cerr << oss.str() << "\n";
    }

    // 心跳
    HeartbeatWriter heartbeat(pid, guid, cfg.daemon.heartbeat_interval_ms);
    heartbeat.start();

    install_term_handlers();

    // ----- 装配 daemon-side 的 Provider / Tools / SessionRegistry -----
    // 这一段重现了 main.cpp 在 TUI 路径下的初始化,但缩到 daemon 必要项:
    //   - LlmProvider (与 TUI 等价的三层解析)
    //   - 7 个内置工具 + skills 工具; project instructions 通过 SessionRegistry
    //     接到每个 AgentLoop,与 TUI 保持一致
    //   - PermissionManager (template,SessionRegistry 给每个 session 复制 mode)
    //   - SessionRegistry + LocalSessionClient
    //   - WebServer (HTTP + WebSocket)
    std::string cwd = acecode::current_path_utf8();
    std::string projects_dir =
        acecode::path_to_utf8(acecode::path_from_utf8(acecode::get_acecode_dir()) / "projects");
    acecode::desktop::ensure_workspace_metadata(projects_dir, cwd);
    acecode::desktop::WorkspaceRegistry workspace_registry;
    workspace_registry.scan(projects_dir);

    std::string hook_config_error;
    acecode::HookConfig hook_config = acecode::load_hook_config(&hook_config_error);
    if (!hook_config_error.empty()) {
        LOG_WARN("[hooks] " + hook_config_error);
    }
    acecode::HookManager hook_manager(std::move(hook_config));
    {
        std::string trust_error;
        acecode::HookTrustStore trust_store =
            acecode::load_hook_trust_store_from_path(
                acecode::default_hook_trust_state_path(), &trust_error);
        if (!trust_error.empty()) {
            LOG_WARN("[hooks] " + trust_error);
        }
        acecode::HookLoadOptions hook_load;
        hook_load.feature_enabled = cfg.features.hooks;
        hook_load.cwd = cwd;
        hook_load.project_trusted = true;
        hook_manager.refresh_registry(
            acecode::load_hook_registry(hook_load, &trust_store));
    }

    // cfg_mut 已在前面创建(承接 port_override),这里只继续使用,不再重复声明。
    auto cwd_override = acecode::load_cwd_model_override(cwd);
    auto effective_entry = acecode::resolve_effective_model(cfg_mut, cwd_override, std::nullopt);
    auto provider = acecode::create_provider_from_entry(effective_entry, &cfg_mut);
    if (!provider) {
        LOG_ERROR("[daemon] failed to create LLM provider — daemon will start but new sessions cannot run agent loop until provider is configured");
    } else if (effective_entry.provider == "copilot") {
        auto copilot = std::dynamic_pointer_cast<acecode::CopilotProvider>(provider);
        if (copilot && !copilot->try_silent_auth()) {
            LOG_WARN("[daemon] Copilot silent auth failed; run `acecode configure` to re-authenticate before using Copilot in Web UI");
        }
    }
    {
        auto payload = acecode::build_startup_models_loaded_payload(cwd, effective_entry, provider);
        hook_manager.dispatch(acecode::kHookEventStartupModelsLoaded, payload, cwd);
    }
    std::mutex provider_mu;
    auto provider_accessor =
        [&provider, &provider_mu]() -> std::shared_ptr<acecode::LlmProvider> {
            std::lock_guard<std::mutex> lk(provider_mu);
            return provider;
        };

    // ---- Init LSP runtime (daemon path, openspec add-lsp-service) ----
    // 惰性子系统:init 本身不 spawn 任何进程,首个匹配文件的编辑/查询才会。
    acecode::lsp::init(cfg.lsp, cwd);

    // ---- Init web search runtime (daemon path) ----
    // 与 TUI 共用同一份 state.json 的 region 缓存,所以两侧探测结果互通。
    acecode::web_search::init(cfg.web_search);
    acecode::web_search::register_default_backends(
        acecode::web_search::runtime().router(), cfg.web_search);
    {
        auto cached = acecode::web_search::runtime().detector().cached_region();
        acecode::web_search::runtime().router().resolve_active(cached);
        if (cached == acecode::web_search::Region::Unknown) {
            std::thread([]{
                auto r = acecode::web_search::runtime().detector().detect_now();
                acecode::web_search::runtime().router().resolve_active(r);
            }).detach();
        }
    }

    // SkillRegistry 必须在 register_tool 之前创建,因为 skills_list / skill_view
    // tools 持有 registry 引用。与 TUI 走同一份扫描根逻辑(see src/skills/skill_init.cpp),
    // 让 GET /api/skills 与 GET /api/commands 看到的 skill 集合与 TUI `/skills` 一致。
    acecode::SkillRegistry skill_registry;
    acecode::initialize_skill_registry(skill_registry, cfg, cwd);

    acecode::ToolExecutor tools;
    acecode::register_session_builtin_tools(tools, cfg_mut);
    // daemon 用 async 版本(走 ToolContext::ask_user_questions → AskUserQuestionPrompter
    // → WS question_request)。TUI 工厂版需要 TuiState/ScreenInteractive,这里没有。
    tools.register_tool(acecode::create_ask_user_question_tool_async());

    // skills_list / skill_view 让 LLM 按需加载 SKILL.md(配合 expand-webui-skill-commands
    // 的轻量提示策略 — daemon expander 不再 inject SKILL.md body,LLM 看到提示后用
    // 这两个 tool 自己取)。
    tools.register_tool(acecode::create_skills_list_tool(skill_registry, &cfg_mut));
    tools.register_tool(acecode::create_skill_view_tool(skill_registry, &cfg_mut));

    // spawn_subagent / wait_subagent:daemon 专属(TUI 无 SessionRegistry 不注册)。
    // ToolExecutor 先于 SessionRegistry 构造,deps 用 shared_ptr 延迟回填 —— 回填
    // 发生在 server 启动前,不存在工具已被调用的并发窗口。
    auto subagent_deps = std::make_shared<acecode::SubagentToolDeps>();
    tools.register_tool(acecode::create_spawn_subagent_tool(subagent_deps));
    tools.register_tool(acecode::create_wait_subagent_tool(subagent_deps));

    acecode::daemon::DaemonMcpRuntime mcp_runtime;
    mcp_runtime.start(cfg_mut, tools);

    acecode::PermissionManager template_perm;
    template_perm.set_mode(permission_mode_from_config(cfg_mut.default_permission_mode));
    if (opts.dangerous) template_perm.set_dangerous(true);

    acecode::SessionRegistryDeps reg_deps;
    reg_deps.provider_accessor    = provider_accessor;
    reg_deps.tools                = &tools;
    reg_deps.cwd                  = cwd;
    reg_deps.config               = &cfg_mut;
    reg_deps.skill_registry       = &skill_registry;
    reg_deps.memory_registry      = nullptr;
    reg_deps.memory_cfg           = nullptr;
    reg_deps.project_instructions_cfg = &cfg_mut.project_instructions;
    reg_deps.custom_instructions_cfg = &cfg_mut.custom_instructions;
    reg_deps.hook_manager         = &hook_manager;
    reg_deps.template_permissions = &template_perm;

    acecode::SessionRegistry registry(std::move(reg_deps));
    acecode::LocalSessionClient client(registry);
    subagent_deps->registry = &registry;
    subagent_deps->client   = &client;
    subagent_deps->config   = &cfg_mut;

    // 控制台 PTY 注册表(add-console-dock):启动期探测一次 backend,
    // 析构时 stop_all 杀掉全部 shell(栈对象,server.run() 返回后回收)。
    // 默认 shell:+ 旁下拉框选中的 default_shell(探测可用)→ 平台默认 → legacy
    // console.shell。per-create 覆盖由 REST /api/pty 的 shell 参数注入。
    std::string default_shell_id = acecode::default_console_shell_id(
        cfg_mut.console.default_shell, cfg_mut.console.git_bash_path);
    std::string default_shell_cmd =
        acecode::resolve_shell_command_by_id(default_shell_id, cfg_mut.console.git_bash_path)
            .value_or(acecode::resolve_console_shell(cfg_mut.console.shell));
    acecode::PtySessionRegistry pty_registry(
        acecode::detect_pty_backend(), cwd, default_shell_cmd);
    LOG_INFO(std::string("[daemon] console backend=") +
             acecode::pty_backend_kind_name(pty_registry.backend()) +
             " shell=" + pty_registry.shell());

    acecode::web::WebServerDeps web_deps;
    web_deps.web_cfg            = &cfg_mut.web;   // 含 port_override 后的 effective port
    web_deps.daemon_cfg         = &cfg_mut.daemon;
    web_deps.app_config         = &cfg_mut;
    web_deps.config_path        =
        acecode::path_to_utf8(acecode::path_from_utf8(acecode::get_acecode_dir()) / "config.json");
    web_deps.cwd                = cwd;
    web_deps.projects_dir       = projects_dir;
    web_deps.token              = token;
    web_deps.guid               = guid;
    web_deps.pid                = pid;
    web_deps.start_time_unix_ms = now_unix_ms();
    web_deps.session_client     = &client;
    web_deps.session_registry   = &registry;
    web_deps.hook_manager       = &hook_manager;
    web_deps.tools              = &tools;
    web_deps.mcp_manager        = &mcp_runtime.manager();
    web_deps.workspace_registry = &workspace_registry;
    web_deps.native_folder_picker_enabled = opts.native_folder_picker_enabled;
    if (opts.native_folder_picker_enabled) {
        web_deps.native_folder_picker = [] {
            return acecode::desktop::pick_folder(nullptr);
        };
        // webapp 兼容模式右键菜单的「在资源管理器中打开」。允许范围 = 已注册
        // workspace + 本 daemon 的 cwd(兜底覆盖 registry 为空的 onboarding 场景)
        // + 全局 skills 目录(设置页「打开全局 Skill 目录」按钮的目标)。
        web_deps.open_in_explorer =
            [&workspace_registry, cwd](const std::string& path) -> std::optional<std::string> {
            std::vector<std::string> roots;
            for (const auto& m : workspace_registry.list()) {
                if (!m.cwd.empty()) roots.push_back(m.cwd);
            }
            roots = acecode::desktop::append_allowed_open_root(std::move(roots), cwd);
            roots = acecode::desktop::append_allowed_open_root(
                std::move(roots),
                acecode::path_to_utf8(
                    acecode::path_from_utf8(acecode::get_acecode_dir()) / "skills"));
            auto result = acecode::desktop::open_directory_in_file_manager(path, roots);
            if (result.ok) return std::nullopt;
            return result.error.empty() ? std::string("failed to open directory") : result.error;
        };
    }
    web_deps.skill_registry     = &skill_registry;
    web_deps.provider           = &provider;
    web_deps.provider_mu        = &provider_mu;
    web_deps.dangerous          = opts.dangerous;
    web_deps.pty_registry       = &pty_registry;

    acecode::web::WebServer server(std::move(web_deps));

    // 子会话 spawn 后登记到 WebServer,给它挂常驻状态监听器,使其 busy 能广播
    // session_status(否则未被 WS 订阅的子会话永不广播,父会话前端在 wait=true
    // 阻塞期间发现不了它,子代理的权限请求冒泡不到主会话)。on_spawn 只在 turn
    // 内(server.run() 之后)被调,捕获 &server 安全。
    subagent_deps->on_spawn =
        [&server](const std::string& child_id, const std::string& /*prompt*/) {
            server.track_subagent(child_id);
        };

    // 信号 / 终止 → 主循环退出。Crow app.run() 阻塞跑;另起个观察线程在
    // term 信号时调 server.stop() 让 Crow 退出。这样我们就在主线程上 join。
    std::thread watcher([&server] {
        std::unique_lock<std::mutex> lk(g_term_mu);
        g_term_cv.wait(lk, [] { return g_term_requested.load(); });
        server.stop();
    });

    int rc = server.run();
    request_terminate(); // 唤醒 watcher(防 server 自然退出但信号还没来)
    if (watcher.joinable()) watcher.join();

    LOG_INFO("[daemon] worker shutting down");
    if (opts.foreground) std::cerr << "[daemon] shutting down\n";

    mcp_runtime.shutdown();
    acecode::lsp::shutdown(); // 逐 client 协议级退出,超时强杀
    acecode::model_pool_status_service().stop(); // 幂等;未 start 过也安全

    heartbeat.stop();
    cleanup_runtime_files();
    return rc;
}

void request_worker_termination() {
    request_terminate();
}

} // namespace acecode::daemon
