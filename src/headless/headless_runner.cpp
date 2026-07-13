#include "headless_runner.hpp"

#include "headless_jsonl.hpp"
#include "headless_mode.hpp"

#include "../config/config.hpp"
#include "../connectors/connector_auth_recovery.hpp"
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
#include "../utils/power_inhibitor.hpp"
#include "../utils/utf8_path.hpp"
#include "../web/handlers/skill_command_expander.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
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
#  include <poll.h>
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

std::int64_t now_epoch_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool stdin_is_tty() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(STDIN_FILENO) != 0;
#endif
}

// 等待 stdin 出现首个可读字节,最多 timeout_ms。true = 有数据(或已 EOF /
// 非管道句柄,可安全阻塞读);false = 超时,一个字节都没来。
//
// 为什么需要它:父进程若把 stdin 接成"打开但从不写也从不关"的继承管道
// (CI runner、后台 spawn 的子进程都常见),`stdin 非 TTY → 阻塞读到 EOF`
// 会让 acecode -p 无限挂死。对齐 claude -p 的做法(main.tsx
// peekForStdinData,3s):超时后警告并放弃 stdin,只用位置参数 prompt。
bool wait_for_stdin_first_byte(int timeout_ms) {
#ifdef _WIN32
    HANDLE h = ::GetStdHandle(STD_INPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE || h == nullptr) return false;
    // 只有匿名/命名管道才有"挂起不写"的问题;磁盘文件重定向读到 EOF 立即
    // 返回,直接放行阻塞读。
    if (::GetFileType(h) != FILE_TYPE_PIPE) return true;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    for (;;) {
        DWORD avail = 0;
        if (!::PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) {
            return true; // 管道已断(写端关闭)= EOF,读会立即返回
        }
        if (avail > 0) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        ::Sleep(50);
    }
#else
    struct pollfd pfd{STDIN_FILENO, POLLIN, 0};
    int rc = ::poll(&pfd, 1, timeout_ms);
    return rc > 0; // POLLIN 或 POLLHUP(EOF)都能立即读;0 = 超时
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
// 的经典管道用法(与 claude -p 语义一致)。stdin 首字节等待上限 3s,
// 超时警告并放弃(防父进程继承的死管道把进程挂死),3s 覆盖 curl / 大文件
// jq 这类慢生产者的启动延迟。
std::string build_effective_prompt(const std::string& arg_prompt) {
    std::string piped;
    if (!stdin_is_tty()) {
        if (wait_for_stdin_first_byte(3000)) {
            piped = trim_copy(read_all_stdin());
        } else {
            std::cerr << "acecode -p: warning: no stdin data received in 3s, "
                         "proceeding without it (redirect stdin explicitly with "
                         "< /dev/null or NUL to skip this wait)\n";
        }
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
    g_interrupt_requested.store(false);
    g_interrupt_count.store(0);

    // ---- prompt 先行:参数错误要在任何重初始化之前廉价失败 ----
    const std::string prompt = build_effective_prompt(opts.prompt);
    if (prompt.empty()) {
        std::cerr << "acecode -p: input must be provided either through stdin or "
                     "as a prompt argument\n"
                  << print_mode_usage_line();
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

    // 会话流程收进 lambda:它内部的所有早退(参数/会话错误 return)都必须
    // 落回下方的 mcp/lsp shutdown —— 直接 return 跳过收尾会让 MCP 子进程 /
    // LSP reader 线程在 static 析构期触发 fail-fast(实测 0xC0000409),
    // 退出码被吃掉。registry 在 lambda 内构造,先于 shutdown 析构。
    const int exit_code = [&]() -> int {
        // ---- 权限模板:默认跟随配置;--yolo = dangerous(全放行) ----
        acecode::PermissionManager template_perm;
        template_perm.set_mode(
            permission_mode_from_config(cfg.default_permission_mode));
        if (opts.dangerous_mode) template_perm.set_dangerous(true);

        // headless 无 web server,不设 on_config_refreshed:钩子成功刷新磁盘
        // key 后内存不合并——headless 进程短生命周期、退出前不保存整份
        // config,可接受。声明在 reg_deps/registry 之前,使其析构晚于二者。
        acecode::ConnectorAuthRecovery::Options recovery_opts;
        recovery_opts.load_disk_config = []() { return acecode::load_config(); };
        acecode::ConnectorAuthRecovery auth_recovery(std::move(recovery_opts));

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
        reg_deps.power_guard              = &acecode::process_power_guard();
        reg_deps.auth_recovery            = &auth_recovery;

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

        // meta 探针:--continue 找最近会话 / --session-id 查碰撞都只读磁盘,
        // 复用 registry resume 的同款惰性 SessionManager 用法(不落任何文件)。
        SessionManager meta_probe;
        meta_probe.start_session(cwd, "", "", "", "", "headless");

        std::string session_id;
        const bool resuming = opts.continue_latest || !opts.resume_session_id.empty();
        if (resuming) {
            std::string target = opts.resume_session_id;
            if (target.empty()) {
                // --continue:当前 cwd 项目目录里最近的普通会话(list 最近在前;
                // 跳过 spawn_subagent 子会话 —— 它们不是用户可续接的对话)。
                for (const auto& meta : meta_probe.list_sessions()) {
                    if (meta.parent_session_id.empty()) {
                        target = meta.id;
                        break;
                    }
                }
                if (target.empty()) {
                    std::cerr << "acecode -p: no previous session to continue in "
                                 "this directory (start one without -c first)\n";
                    return 1;
                }
            }
            if (!client.resume_session(target, session_opts)) {
                std::cerr << "acecode -p: cannot resume session '" << target
                          << "': not found in this directory's session store\n";
                return 1;
            }
            session_id = target;
        } else {
            if (!opts.session_id.empty()) {
                // 碰撞即报错而不是静默续写:--session-id 的契约是"创建新会话",
                // 撞上已有 id 十有八九是脚本想 --resume 却写错了 flag。
                if (meta_probe.has_session_file(opts.session_id) ||
                    !meta_probe.load_session_meta(opts.session_id).id.empty()) {
                    std::cerr << "acecode -p: session id '" << opts.session_id
                              << "' already exists; use --resume " << opts.session_id
                              << " to continue it\n";
                    return 1;
                }
                session_opts.preset_session_id = opts.session_id;
            }
            try {
                session_id = client.create_session(session_opts);
            } catch (const std::exception& e) {
                std::cerr << "acecode -p: failed to create session: " << e.what() << "\n";
                return 1;
            }
        }

        const bool stream_json = opts.output_format == "stream-json";
        std::unique_ptr<HeadlessJsonlProjector> jsonl_projector;
        std::unique_ptr<JsonlStreamWriter> jsonl_writer;
        if (stream_json) {
#ifndef _WIN32
            // A closed downstream pipe must become a checked write failure so
            // the session can abort/persist; the default SIGPIPE would kill us.
            std::signal(SIGPIPE, SIG_IGN);
#endif
            jsonl_projector = std::make_unique<HeadlessJsonlProjector>(
                session_id, opts.include_thinking);
            jsonl_writer = std::make_unique<JsonlStreamWriter>(stdout);
        }
        auto write_stream_error = [&](const std::string& name,
                                      const std::string& message,
                                      nlohmann::json details = nlohmann::json::object()) {
            if (!jsonl_projector || !jsonl_writer || jsonl_writer->failed()) return;
            jsonl_writer->write_record(jsonl_projector->make_error_record(
                name, message, std::move(details), now_epoch_ms()));
        };

        // provider 兜底检查:没有可用模型配置时 fail fast,而不是让回合
        // 跑起来再报一条模型错误消息。
        {
            auto entry = registry.acquire(session_id);
            if (!entry || !entry->provider_slot) {
                write_stream_error("SessionInitializationError",
                                   "session initialization failed");
                std::cerr << "acecode -p: session initialization failed\n";
                return 1;
            }
            std::shared_ptr<acecode::LlmProvider> p;
            {
                std::lock_guard<std::mutex> lk(entry->provider_slot->mu);
                p = entry->provider_slot->provider;
            }
            if (!p) {
                write_stream_error("ModelConfigurationError",
                                   "no usable model configured");
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
                if (jsonl_projector && jsonl_writer && !jsonl_writer->failed()) {
                    auto records = jsonl_projector->consume(ev);
                    for (const auto& record : records) {
                        if (!jsonl_writer->write_record(record)) break;
                    }
                    if (jsonl_writer->failed()) wait_cv.notify_all();
                }
                if (ev.kind == SessionEventKind::Done) {
                    std::lock_guard<std::mutex> lk(wait_mu);
                    turn_done = true;
                    wait_cv.notify_all();
                } else if (ev.kind == SessionEventKind::Error) {
                    std::lock_guard<std::mutex> lk(wait_mu);
                    if (ev.payload.contains("reason") && ev.payload["reason"].is_string()) {
                        last_error_reason = ev.payload["reason"].get<std::string>();
                    }
                } else if (ev.kind == SessionEventKind::Message &&
                           ev.payload.value("role", std::string{}) == "error") {
                    std::lock_guard<std::mutex> lk(wait_mu);
                    last_error_reason = ev.payload.value("content", std::string{});
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
            write_stream_error("SubmissionError", "failed to submit prompt");
            std::cerr << "acecode -p: failed to submit prompt\n";
            return 1;
        }

        // ---- 等回合结束。Ctrl+C 一次 = abort 会话(等它收尾落盘);两次 =
        // 立即退出。 ----
        bool aborted = false;
        bool output_failed = false;
        {
            std::unique_lock<std::mutex> lk(wait_mu);
            while (!turn_done) {
                wait_cv.wait_for(lk, std::chrono::milliseconds(200));
                if (jsonl_writer && jsonl_writer->failed() && !output_failed) {
                    output_failed = true;
                    lk.unlock();
                    client.abort(session_id);
                    std::cerr << "acecode -p: " << jsonl_writer->error_message()
                              << "; cancelling the turn\n";
                    lk.lock();
                }
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

        int exit_code = 1;
        if (output_failed) {
            exit_code = 1;
        } else if (aborted) {
            exit_code = 130;
        } else if (final_text.empty() ||
                   (stream_json && !last_error_reason.empty())) {
            std::cerr << "acecode -p: turn finished without an assistant reply"
                      << (last_error_reason.empty()
                              ? std::string{}
                              : " (last error: " + last_error_reason + ")")
                      << "\n";
            exit_code = 1;
        } else {
            exit_code = 0;
        }

        if (aborted && !output_failed) {
            write_stream_error("Interrupted", "interrupted by Ctrl+C",
                               {{"exit_code", 130}});
        }

        if (stream_json) {
            // All completed-part records were emitted from the pre-submit
            // subscription. There is intentionally no terminal result object.
        } else if (opts.output_format == "json") {
            // 脚本消费面:成功失败都输出单个 result 对象(含 session_id 供
            // 下一轮 --resume 链式调用),诊断细节仍走上面的 stderr。
            nlohmann::json out{
                {"type", "result"},
                {"session_id", session_id},
                {"is_error", exit_code != 0},
                {"result", final_text},
            };
            if (aborted) {
                out["error"] = "interrupted";
            } else if (exit_code != 0) {
                out["error"] = last_error_reason.empty() ? "no assistant reply"
                                                         : last_error_reason;
            }
            const std::string dumped = out.dump();
            std::fwrite(dumped.data(), 1, dumped.size(), stdout);
            std::fputc('\n', stdout);
            std::fflush(stdout);
        } else if (exit_code == 0) {
            std::fwrite(final_text.data(), 1, final_text.size(), stdout);
            if (final_text.empty() || final_text.back() != '\n') {
                std::fputc('\n', stdout);
            }
            std::fflush(stdout);
        }
        return exit_code;
    }(); // registry 析构:abort + join 所有 AgentLoop worker 线程,释放 writer lease

    mcp_runtime.shutdown();
    acecode::lsp::shutdown();
    return exit_code;
}

} // namespace acecode::headless
