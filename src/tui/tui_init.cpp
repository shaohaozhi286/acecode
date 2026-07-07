#include "tui/tui_init.hpp"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <termios.h>
#include <unistd.h>
#include <csignal>
#endif

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include "version.hpp"
#include "config/config.hpp"
#include "tui_state.hpp"
#include "tui/tui_helpers.hpp"
#include "tui/terminal_utils.hpp"
#include "tui/tui_context.hpp"
#include "tui/render_mode_factory.hpp"
#include "tui/slash_dropdown.hpp"
#include "tui/theme_palette.hpp"
#include "tui/ctrl_c_exit.hpp"
#include "utils/logger.hpp"
#include "utils/terminal_capability.hpp"
#include "utils/terminal_theme_detect.hpp"
#include "utils/state_file.hpp"
#include "network/proxy_resolver.hpp"
#include "provider/models_dev_registry.hpp"
#include "tool/web_search/runtime.hpp"
#include "tool/web_search/backend_router.hpp"
#include "tool/web_search/region_detector.hpp"
#include "tool/mcp_startup_coordination.hpp"
#include "skills/skill_init.hpp"
#include "skills/skill_registry.hpp"
#include "skills/skill_commands.hpp"
#include "skills/default_skill_seeder.hpp"
#include "commands/opencode_command_registry.hpp"
#include "memory/memory_paths.hpp"
#include "memory/memory_registry.hpp"
#include "upgrade/check.hpp"
#include "commands/builtin_commands.hpp"
#include "commands/command_registry.hpp"
#include "remote_control/remote_control_service.hpp"
#include "hooks/hook_config.hpp"
#include "hooks/hook_manager.hpp"
#include "hooks/hook_payload.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "history/input_history_store.hpp"
#include "desktop/workspace_registry.hpp"

using namespace ftxui;

namespace acecode { namespace tui {

bool ensure_interactive_terminal() {
    std::atexit(reset_cursor);
#ifdef _WIN32
    bool stdin_is_tty = _isatty(_fileno(stdin));
    bool stdout_is_tty = _isatty(_fileno(stdout));
#else
    bool stdin_is_tty = isatty(fileno(stdin));
    bool stdout_is_tty = isatty(fileno(stdout));
#endif
    if (!stdin_is_tty || !stdout_is_tty) {
        std::cerr << "Error: acecode requires an interactive terminal (stdin and stdout must be a TTY).\n"
                  << "If piping input/output, please run acecode directly in a terminal instead.\n";
        return false;
    }
    return true;
}

void set_startup_terminal_title() {
#ifdef _WIN32
    SetConsoleTitleA("acecode v" ACECODE_VERSION);
#else
    std::cout << "\033]0;acecode v" ACECODE_VERSION "\007" << std::flush;
#endif
}

void initialize_logger_for_working_dir(const std::string& working_dir) {
    Logger::instance().init(working_dir + "/acecode.log");
    Logger::instance().set_level(LogLevel::Dbg);
    LOG_INFO("=== acecode started, cwd=" + working_dir + " ===");
}

void initialize_proxy_runtime(const AppConfig& config) {
    network::proxy_resolver().init(config.network);
    network::proxy_resolver().probe_and_maybe_fallback();
    auto resolved = network::proxy_resolver().effective("https://example.com");
    std::string banner;
    if (resolved.source == "auto-fallback") {
        auto fb = network::proxy_resolver().fallback_info_snapshot();
        banner = "Proxy: direct (auto-fallback: " + fb.original_url +
                 " from " + fb.original_source + " unreachable)";
    } else {
        std::string url_disp = resolved.url.empty()
                                   ? std::string("direct")
                                   : network::redact_credentials(resolved.url);
        banner = "Proxy: " + url_disp + " (" + resolved.source + ")";
    }
    std::cerr << banner << std::endl;
    LOG_INFO("[proxy] effective=" +
             (resolved.url.empty() ? "direct" : network::redact_credentials(resolved.url)) +
             " source=" + resolved.source + " mode=" + config.network.proxy_mode);
}

void initialize_models_registry_runtime(const AppConfig& config,
                                        const std::string& argv0_dir) {
    acecode::initialize_registry(config, argv0_dir);
    if (config.models_dev.allow_network && !config.models_dev.refresh_on_command_only) {
        std::thread([] { acecode::refresh_registry_from_network(); }).detach();
    }
}

void initialize_web_search_runtime(const AppConfig& config) {
    web_search::init(config.web_search);
    web_search::register_default_backends(web_search::runtime().router(), config.web_search);
    web_search::Region cached = web_search::runtime().detector().cached_region();
    web_search::runtime().router().resolve_active(cached);
    if (cached == web_search::Region::Unknown) {
        std::thread([]{
            auto r = web_search::runtime().detector().detect_now();
            web_search::runtime().router().resolve_active(r);
        }).detach();
    }
}

std::thread start_tui_update_check(const AppConfig& config,
                                   TuiState& state,
                                   ScreenInteractive& screen) {
    return std::thread([config, &state, &screen] {
        try {
            auto result = acecode::upgrade::check_for_update(config, ACECODE_VERSION);
            if (result.update_available()) {
                std::lock_guard<std::mutex> lk(state.mu);
                state.update_notice = "Update available: v" + result.latest_version +
                                      ". Run acecode update.";
                screen.PostEvent(Event::Custom);
            } else if (result.status != acecode::upgrade::UpdateCheckStatus::UpToDate) {
                LOG_DEBUG(std::string("[upgrade] startup check skipped: ") +
                          acecode::upgrade::update_check_status_name(result.status));
            }
        } catch (const std::exception& e) {
            LOG_DEBUG("[upgrade] startup check failed: " + std::string(e.what()));
        } catch (...) {
            LOG_DEBUG("[upgrade] startup check failed with unknown exception");
        }
    });
}

MemoryConfig initialize_memory_registry(MemoryRegistry& memory_registry,
                                        const AppConfig& config) {
    MemoryConfig runtime_memory_cfg = config.memory;
    std::error_code mkec;
    std::filesystem::create_directories(acecode::get_memory_dir(), mkec);
    if (mkec) {
        LOG_ERROR("[memory] failed to create " + acecode::get_memory_dir().generic_string() +
                  ": " + mkec.message() + " — memory will be disabled this session");
        runtime_memory_cfg.enabled = false;
    } else if (runtime_memory_cfg.enabled) {
        memory_registry.scan();
    }
    return runtime_memory_cfg;
}

void initialize_mcp_servers(McpManager& mcp_manager, const AppConfig& config) {
    if (config.mcp_servers.empty()) return;
    mcp_manager.connect_all(config);
    LOG_INFO("[mcp] Configured " + std::to_string(config.mcp_servers.size()) +
             " server(s); startup will run in the background");
}

void restore_input_history(TuiState& state, const AppConfig& config,
                           const std::string& working_dir) {
    if (!config.input_history.enabled) return;
    std::string ih_path = InputHistoryStore::file_path(
        SessionStorage::get_project_dir(working_dir));
    state.input_history = InputHistoryStore::load(ih_path);
    int cap = config.input_history.max_entries;
    if (cap > 0 && static_cast<int>(state.input_history.size()) > cap) {
        state.input_history.erase(
            state.input_history.begin(),
            state.input_history.begin() +
                (state.input_history.size() - static_cast<size_t>(cap)));
    }
}

void add_startup_messages(TuiState& state, bool dangerous_mode,
                          const McpManager& mcp_manager) {
    if (dangerous_mode) {
        state.conversation.push_back({"system",
            "[DANGEROUS YOLO MODE] Permission and path-safety checks are bypassed. Use with caution. "
            "(AskUserQuestion overlays are still shown when the model needs input.)", false});
    }
    size_t configured = mcp_manager.configured_server_count();
    if (configured > 0) {
        state.conversation.push_back({"system",
            acecode::mcp_background_start_message(configured), false});
    }
}

void start_mcp_servers_async(McpManager& mcp_manager, ToolExecutor& tools,
                             TuiState& state, ScreenInteractive& screen) {
    if (mcp_manager.configured_server_count() == 0) return;
    mcp_manager.set_status_callback([&mcp_manager, &state, &screen](const McpServerInfo& info) {
        auto sidebar_servers = build_mcp_sidebar_servers(mcp_manager);
        auto message = acecode::mcp_status_message(info);
        {
            std::lock_guard<std::mutex> lk(state.mu);
            set_mcp_sidebar_servers_locked(state, std::move(sidebar_servers));
            if (message.has_value()) {
                state.conversation.push_back({"system", *message, false});
            }
        }
        screen.PostEvent(ftxui::Event::Custom);
    });
    mcp_manager.start_async(tools);
}

void maybe_add_legacy_terminal_hint(
    TuiState& state, const AppConfig& config,
    const acecode::TerminalCapabilities& term_caps,
    acecode::tui::ScreenRenderMode render_mode,
    bool force_alt_screen) {
    bool show_legacy_hint =
        render_mode == acecode::tui::ScreenRenderMode::AltScreen &&
        !force_alt_screen &&
        config.tui.alt_screen_mode == "auto" &&
        !term_caps.source_label.empty() &&
        !acecode::read_state_flag("legacy_terminal_hint_shown");
    if (show_legacy_hint) {
        std::string hint = "提示: 检测到 " + term_caps.source_label +
            ",已启用全屏渲染避免画面跳动。可在 ~/.acecode/config.json 设置 "
            "\"tui\": {\"alt_screen_mode\": \"never\"} 关闭。";
        state.conversation.push_back({"system", hint, false});
        acecode::write_state_flag("legacy_terminal_hint_shown", true);
    }
}

PermissionMode permission_mode_from_meta_name(std::string mode) {
    if (mode == "acceptEdits") mode = "accept-edits";
    if (mode == "accept-edits") return PermissionMode::AcceptEdits;
    if (mode == "yolo") return PermissionMode::Yolo;
    if (mode == "plan") return PermissionMode::Plan;
    return PermissionMode::Default;
}

void configure_permissions(PermissionManager& permissions,
                           bool dangerous_mode,
                           const std::string& default_permission_mode) {
    permissions.set_mode(permission_mode_from_meta_name(default_permission_mode));
    if (dangerous_mode) {
        permissions.set_dangerous(true);
        permissions.set_mode(PermissionMode::Yolo);
    }
    permissions.add_rule({"file_write", "*.env", "", RuleAction::Deny, 100});
    permissions.add_rule({"file_edit", "*.env", "", RuleAction::Deny, 100});
    permissions.add_rule({"file_write", ".git/**", "", RuleAction::Deny, 100});
    permissions.add_rule({"file_edit", ".git/**", "", RuleAction::Deny, 100});
    permissions.add_rule({"bash", "", "rm -rf /", RuleAction::Deny, 100});
}

void register_slash_commands(CommandRegistry& cmd_registry,
                             SkillRegistry& skill_registry,
                             const AppConfig& config,
                             const std::string& working_dir) {
    acecode::register_builtin_commands(cmd_registry);
    auto command_keys = acecode::register_opencode_commands_tracked(
        cmd_registry, config, working_dir);
    if (!command_keys.empty()) {
        LOG_INFO("[commands] Registered " + std::to_string(command_keys.size()) +
                 " opencode command slash command(s)");
    }
    auto keys = acecode::register_skill_commands_tracked(cmd_registry, skill_registry);
    if (!keys.empty()) {
        LOG_INFO("[skills] Registered " + std::to_string(keys.size()) + " skill slash command(s)");
    }
}

void run_tui_loop(ScreenInteractive& screen, const ftxui::Component& renderer) {
#ifdef _WIN32
    screen.Post(prepare_windows_ctrl_c_handling_after_ftxui_install);
#endif
    flush_terminal_input_buffer();
    write_terminal_control_sequence(acecode::tui::kBracketedPasteEnableSeq);
    screen.Loop(renderer);
    write_terminal_control_sequence(acecode::tui::kBracketedPasteDisableSeq);
    flush_terminal_input_buffer();
}

void shutdown_after_tui_loop(TuiState& state, AgentLoop& agent_loop,
                             McpManager& mcp_manager,
                             std::atomic<bool>& running,
                             std::atomic<bool>& agent_aborting,
                             std::thread& anim_thread,
                             std::thread& auth_thread,
                             std::thread& update_check_thread,
                             SessionManager& session_manager,
                             const AppConfig& config) {
    g_active_screen.store(nullptr, std::memory_order_release);
#ifdef _WIN32
    SetConsoleCtrlHandler(console_ctrl_handler, FALSE);
#endif
    running = false;
    acecode::rc::remote_control_service().stop();
    agent_aborting = true;
    agent_loop.abort();
    {
        std::lock_guard<std::mutex> lk(state.mu);
        if (state.confirm_pending) {
            state.confirm_pending = false;
            state.confirm_result = PermissionResult::Deny;
            state.confirm_cv.notify_one();
        }
        if (state.ask_pending) {
            state.ask_pending = false;
            state.ask_result_ok = false;
            state.ask_submit_page = false;
            state.ask_submit_focus = 0;
            state.ask_question_option_focus.clear();
            state.ask_answered_questions.clear();
            state.ask_selected_options.clear();
            state.ask_multi_selected_by_question.clear();
            state.ask_custom_answer_selected.clear();
            state.ask_custom_answers.clear();
            state.ask_scroll_offset = 0;
            state.ask_scroll_total_rows = 0;
            state.ask_scroll_visible_rows = 0;
            state.ask_scrollbar_dragging = false;
            state.ask_scroll_to_focus_requested = false;
            state.ask_cv.notify_one();
        }
    }
    agent_loop.shutdown();
    mcp_manager.shutdown();
    state.compact_abort_requested.store(true);
    if (state.compact_thread.joinable()) state.compact_thread.join();
    if (anim_thread.joinable()) anim_thread.join();
    if (auth_thread.joinable()) auth_thread.join();
    if (update_check_thread.joinable()) update_check_thread.join();
    session_manager.finalize();
    session_manager.cleanup_old_sessions(config.max_sessions);
    auto exit_sid = session_manager.current_session_id();
    g_session_manager = nullptr;
    if (!exit_sid.empty()) {
        std::cerr << "\nacecode: session " << exit_sid
                  << " saved. Resume with: acecode --resume " << exit_sid << std::endl;
    }
}

}} // namespace acecode::tui
