#include "worker.hpp"

#include "../desktop/workspace_registry.hpp"
#include "guid.hpp"
#include "heartbeat.hpp"
#include "platform.hpp"
#include "runtime_files.hpp"
#include "../provider/cwd_model_override.hpp"
#include "../provider/model_resolver.hpp"
#include "../provider/provider_factory.hpp"
#include "../session/local_session_client.hpp"
#include "../session/session_registry.hpp"
#include "../skills/skill_registry.hpp"
#include "../tool/ask_user_question_tool.hpp"
#include "../tool/bash_tool.hpp"
#include "../tool/file_read_tool.hpp"
#include "../tool/file_write_tool.hpp"
#include "../tool/file_edit_tool.hpp"
#include "../tool/grep_tool.hpp"
#include "../tool/glob_tool.hpp"
#include "../tool/task_complete_tool.hpp"
#include "../tool/tool_executor.hpp"
#include "../tool/web_search/runtime.hpp"
#include "../tool/web_search/backend_router.hpp"
#include "../tool/web_search/region_detector.hpp"
#include "../tool/web_search/web_search_tool.hpp"
#include "../network/proxy_resolver.hpp"
#include "../utils/logger.hpp"
#include "../utils/token.hpp"
#include "../web/auth.hpp"
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

    fs::path requested = fs::path(acecode::expand_path(raw));
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
        std::string msg = "[daemon] --cwd path is not a directory: " + effective.string();
        LOG_ERROR(msg);
        if (foreground) std::cerr << msg << "\n";
        return false;
    }

    fs::current_path(effective, dir_ec);
    if (dir_ec) {
        std::string msg = "[daemon] failed to switch --cwd to " + effective.string()
            + ": " + dir_ec.message();
        LOG_ERROR(msg);
        if (foreground) std::cerr << msg << "\n";
        return false;
    }

    std::string msg = "[daemon] cwd=" + fs::current_path().string()
        + " (from --cwd=" + raw + ")";
    LOG_INFO(msg);
    if (foreground) std::cerr << msg << "\n";
    return true;
}

} // namespace

std::string validate_can_start(const WorkerOptions& opts) {
    auto existing_guid = read_guid_file();
    auto existing_pid  = read_pid_file();

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

    // standalone: 若已有 guid + pid 文件,且 pid 仍存活,说明已有 daemon 跑。
    if (existing_guid.has_value() && existing_pid.has_value() &&
        is_pid_alive(*existing_pid)) {
        std::ostringstream oss;
        oss << "another daemon already running (pid=" << *existing_pid
            << " guid=" << *existing_guid << ")";
        return oss.str();
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

    auto reject = validate_can_start(opts);
    if (!reject.empty()) {
        std::cerr << "[daemon] refuse to start: " << reject << "\n";
        return 3;
    }

    // 代理解析器初始化 —— 必须在第一个 cpr 调用前完成。daemon 路径无 TUI,
    // 横幅写到日志(LOG_INFO),insecure 仍走 LOG_WARN(高优先级)。
    network::proxy_resolver().init(cfg.network);
    {
        auto resolved = network::proxy_resolver().effective("https://example.com");
        std::ostringstream oss;
        oss << "[proxy] effective="
            << (resolved.url.empty() ? "direct" : network::redact_credentials(resolved.url))
            << " source=" << resolved.source
            << " mode=" << cfg.network.proxy_mode;
        LOG_INFO(oss.str());
        if (opts.foreground) std::cerr << oss.str() << "\n";
        if (cfg.network.proxy_insecure_skip_verify) {
            LOG_WARN("[proxy] insecure_skip_verify is enabled — TLS chain validation off");
            if (opts.foreground) {
                std::cerr << "[proxy] WARNING: TLS verification disabled for proxied requests\n";
            }
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
    //   - 7 个内置工具 + skills / memory 工具暂留(配置上 v1 daemon 不暴露
    //     skill/memory 命令行入口,工具由 LLM 通过 tool_calls 间接调用 — 留
    //     给后续 change 加 SkillRegistry / MemoryRegistry 真接入)
    //   - PermissionManager (template,SessionRegistry 给每个 session 复制 mode)
    //   - SessionRegistry + LocalSessionClient
    //   - WebServer (HTTP + WebSocket)
    std::string cwd = std::filesystem::current_path().string();
    acecode::desktop::ensure_workspace_metadata(
        (std::filesystem::path(acecode::get_acecode_dir()) / "projects").string(),
        cwd);

    // cfg_mut 已在前面创建(承接 port_override),这里只继续使用,不再重复声明。
    auto cwd_override = acecode::load_cwd_model_override(cwd);
    auto effective_entry = acecode::resolve_effective_model(cfg_mut, cwd_override, std::nullopt);
    auto provider = acecode::create_provider_from_entry(effective_entry);
    if (!provider) {
        LOG_ERROR("[daemon] failed to create LLM provider — daemon will start but new sessions cannot run agent loop until provider is configured");
    }
    std::mutex provider_mu;
    auto provider_accessor =
        [&provider, &provider_mu]() -> std::shared_ptr<acecode::LlmProvider> {
            std::lock_guard<std::mutex> lk(provider_mu);
            return provider;
        };

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

    acecode::ToolExecutor tools;
    tools.register_tool(acecode::create_bash_tool());
    tools.register_tool(acecode::create_file_read_tool());
    tools.register_tool(acecode::create_file_write_tool());
    tools.register_tool(acecode::create_file_edit_tool());
    tools.register_tool(acecode::create_grep_tool());
    tools.register_tool(acecode::create_glob_tool());
    tools.register_tool(acecode::create_task_complete_tool());
    if (cfg.web_search.enabled) {
        tools.register_tool(acecode::web_search::create_web_search_tool(
            acecode::web_search::runtime().router(),
            acecode::web_search::runtime().cfg()));
    }
    // daemon 用 async 版本(走 ToolContext::ask_user_questions → AskUserQuestionPrompter
    // → WS question_request)。TUI 工厂版需要 TuiState/ScreenInteractive,这里没有。
    tools.register_tool(acecode::create_ask_user_question_tool_async());

    acecode::SkillRegistry skill_registry; // 不 scan,v1 daemon 不暴露 skills
    acecode::PermissionManager template_perm;
    if (opts.dangerous) template_perm.set_dangerous(true);

    acecode::SessionRegistryDeps reg_deps;
    reg_deps.provider_accessor    = provider_accessor;
    reg_deps.tools                = &tools;
    reg_deps.cwd                  = cwd;
    reg_deps.config               = &cfg_mut;
    reg_deps.skill_registry       = &skill_registry;
    reg_deps.memory_registry      = nullptr;
    reg_deps.memory_cfg           = nullptr;
    reg_deps.project_instructions_cfg = nullptr;
    reg_deps.template_permissions = &template_perm;

    acecode::SessionRegistry registry(std::move(reg_deps));
    acecode::LocalSessionClient client(registry);

    acecode::web::WebServerDeps web_deps;
    web_deps.web_cfg            = &cfg_mut.web;   // 含 port_override 后的 effective port
    web_deps.daemon_cfg         = &cfg_mut.daemon;
    web_deps.app_config         = &cfg_mut;
    web_deps.config_path        =
        (std::filesystem::path(acecode::get_acecode_dir()) / "config.json").string();
    web_deps.cwd                = cwd;
    web_deps.token              = token;
    web_deps.guid               = guid;
    web_deps.pid                = pid;
    web_deps.start_time_unix_ms = now_unix_ms();
    web_deps.session_client     = &client;
    web_deps.session_registry   = &registry;
    web_deps.skill_registry     = &skill_registry;
    web_deps.provider           = &provider;
    web_deps.provider_mu        = &provider_mu;
    web_deps.dangerous          = opts.dangerous;

    acecode::web::WebServer server(std::move(web_deps));

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

    heartbeat.stop();
    cleanup_runtime_files();
    return rc;
}

void request_worker_termination() {
    request_terminate();
}

} // namespace acecode::daemon
