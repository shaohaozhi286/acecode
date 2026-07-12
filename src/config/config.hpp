#pragma once

#include "saved_models.hpp"

#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace acecode {

struct OpenAiConfig {
    static constexpr int kDefaultStreamTimeoutMs = 666000;

    std::string base_url = "http://localhost:1234/v1";
    std::string api_key;
    std::string model = "local-model";
    int stream_timeout_ms = kDefaultStreamTimeoutMs;
    // Legacy/global OpenAI-compatible request header templates. Named saved
    // model entries should prefer ModelProfile::request_headers; provider
    // construction uses this map as a fallback when an OpenAI entry has none.
    std::map<std::string, std::string> request_headers;
    // Optional provider id from the bundled models.dev registry (e.g. "anthropic",
    // "openrouter"). Lets resolve_model_context_window() and other catalog-aware
    // call sites pick the correct provider entry even when base_url is a proxy.
    std::optional<std::string> models_dev_provider_id;
};

struct CopilotConfig {
    std::string model = "gpt-4o";
};

struct CodexConfig {
    std::string model = "gpt-5.5";
};

enum class McpTransport {
    Stdio = 0, // launch a child process and talk over its stdio pipes
    Sse,       // HTTP + text/event-stream via mcp::sse_client
    Http,      // MCP Streamable HTTP (currently routed through sse_client)
};

struct McpServerConfig {
    McpTransport transport = McpTransport::Stdio;

    // Stdio-only fields.
    std::string command;                         // required for stdio: executable to launch
    std::vector<std::string> args;               // optional: CLI arguments
    std::map<std::string, std::string> env;      // optional: environment variables

    // SSE / HTTP fields.
    std::string url;                             // required for sse/http: scheme://host[:port]
    std::string sse_endpoint = "/sse";           // path portion of SSE/HTTP endpoint
    std::map<std::string, std::string> headers;  // optional extra request headers
    std::string auth_token;                      // optional bearer token (never logged)
    int timeout_seconds = 30;

    // true = 用户在设置页关掉了这个 server:全 app(所有会话)不连接、不注册它的
    // 工具。持久化到 config.json(仅 true 时写出),daemon 启动时 connect_all 把它
    // 建成 Disabled 态、start_async 跳过;运行时经 /api/mcp/toggle 热切换。
    bool disabled = false;
};

struct SkillsConfig {
    std::vector<std::string> disabled;       // skill names to hide even if present on disk
    std::vector<std::string> external_dirs;  // extra directories to scan (supports ~ and ${ENV})
    bool reuse_opencode = true;              // reuse opencode-compatible skill roots by default
};

struct MemoryConfig {
    bool enabled = true;
    // Hard cap on MEMORY.md size for system-prompt injection. Oversized indexes
    // are truncated in-memory with a marker; the on-disk file is untouched.
    std::size_t max_index_bytes = 32 * 1024;
};

struct ProjectInstructionsConfig {
    bool enabled = true;
    int max_depth = 8;                         // max dirs walked from cwd towards HOME
    std::size_t max_bytes = 256 * 1024;        // per-file cap
    std::size_t max_total_bytes = 1024 * 1024; // aggregate cap for merged text
    // Priority order. Each directory contributes at most the first filename that
    // exists. AGENT.md is native; CLAUDE.md is compat.
    std::vector<std::string> filenames = {"AGENT.md", "CLAUDE.md"};
    // Per-filename gate. Setting this to false removes CLAUDE.md from the
    // effective search list at runtime (overriding its presence in filenames).
    bool read_claude_md = true;
};

inline constexpr std::size_t kCustomInstructionsMaxBytes = 64 * 1024;

struct CustomInstructionsConfig {
    std::string text;

    CustomInstructionsConfig() = default;
    CustomInstructionsConfig(const CustomInstructionsConfig& other) {
        std::lock_guard<std::mutex> lock(other.mu_);
        text = other.text;
    }
    CustomInstructionsConfig& operator=(const CustomInstructionsConfig& other) {
        if (this == &other) return *this;
        std::scoped_lock lock(mu_, other.mu_);
        text = other.text;
        return *this;
    }

    std::string text_snapshot() const {
        std::lock_guard<std::mutex> lock(mu_);
        return text;
    }
    void set_text(std::string value) {
        std::lock_guard<std::mutex> lock(mu_);
        text = std::move(value);
    }

private:
    mutable std::mutex mu_;
};

// 连接器生命周期钩子:一次性外部进程,由 config.json 数据完全描述。
// on_enable    —— 连接器开关从关到开时异步执行。
// on_auth_error —— 聊天请求收到认证形态错误(HTTP 400/401)且模型 base_url
//                  命中 auth_error_scope.base_url_prefix 时执行,退出 0 后
//                  acecode 重读磁盘 saved_models 并重试请求一次。
struct ConnectorHookConfig {
    std::string command;                 // 可执行文件路径(安装脚本写绝对路径)
    std::vector<std::string> args;
    int timeout_ms = 300000;             // 等待钩子进程退出的上限
};

struct ConnectorConfig {
    std::string id;
    std::string name;
    std::string description;
    bool enabled = true;
    std::optional<ConnectorHookConfig> on_enable;      // JSON: hooks.on_enable
    std::optional<ConnectorHookConfig> on_auth_error;  // JSON: hooks.on_auth_error
    std::optional<ConnectorHookConfig> on_startup;     // JSON: hooks.on_startup —— daemon 启动时对 enabled 连接器异步执行一次
    std::string auth_error_base_url_prefix;            // JSON: auth_error_scope.base_url_prefix
};

struct DaemonConfig {
    bool auto_start_on_double_click = false;
    std::string service_name = "ACECodeDaemon";
    int heartbeat_interval_ms = 2000;
    int heartbeat_timeout_ms = 15000;
};

struct WebConfig {
    bool enabled = true;
    std::string bind = "127.0.0.1";
    int port = 28080;
    // Empty = serve embedded assets; non-empty = serve from this filesystem path
    // (development mode for the front-end change).
    std::string static_dir;
};

struct WebUiPreferencesConfig {
    bool show_acecode_avatar = false;
};

struct ModelsDevConfig {
    bool allow_network = false;                          // permit any HTTP request to models.dev
    std::optional<std::string> user_override_path;       // local api.json that beats the bundled snapshot
    bool refresh_on_command_only = true;                 // suppress all startup-time network refresh
};

struct InputHistoryConfig {
    bool enabled = true;        // disable to fall back to pure in-memory history
    int max_entries = 10;       // hard cap on persisted entries per working directory
};

// agent_loop.* control AgentLoop's safety bounds. See
// openspec/changes/align-loop-with-hermes for the termination protocol.
//
// A text-only assistant reply (zero tool calls) ends the loop. This matches
// hermes-agent (`run_agent.py:9823`) and claudecodehaha. ACECode models
// (GPT / Copilot / local LMs) sometimes hedge mid-task with "Would you like
// me to continue?" — when they do, the loop ends and the user manually
// re-prompts (e.g. "继续"). Earlier auto-continue / nudge machinery was
// removed for being more disruptive than helpful (chit-chat regressions,
// model-give-up risk; see hermes-agent #7915).
//
// `task_complete` remains an OPTIONAL explicit terminator: the model can
// call it with a `summary` to render a compact "Done: <summary>" row. It is
// not required — a plain text reply also ends the loop cleanly.
//
// `AskUserQuestion` is NEVER a terminator (its tool_result feeds back to
// the model and the loop continues, exactly like any other tool).
struct AgentLoopConfig {
    int max_iterations = 0; // 0 = unlimited; positive values cap total LLM turns per run()

    // AskUserQuestion 应答策略(openspec/changes/add-ask-question-policy)。
    //   "ask"     = 默认。正常弹 UI 无限期等用户回答。
    //   "deny"    = 不弹 UI,立即返回自动应答让模型自行决策并继续。
    //   "timeout" = 弹 UI 等 question_timeout_seconds 秒,无人回答则自动
    //               采纳每个 question 的第一个选项(工具约定推荐项排第一)。
    // 优先级:goal 无人值守自动应答 > 显式配置(config/CLI) > YOLO 隐式
    // 映射 deny > 默认 ask。非法值在 load_config 归一化为 "ask" 并 LOG_WARN。
    std::string question_policy = "ask";
    int question_timeout_seconds = 60; // 仅 policy=timeout 时读取;clamp [5, 3600]

    // 运行时标记,不序列化:config JSON 显式含 question_policy 键。
    // sparse-on-write 下无法从值区分「默认 ask」与「用户写了 ask」,显式
    // 配置要压制 YOLO 隐式映射就必须单独记账。
    bool question_policy_explicit = false;

    // CLI --question-policy 覆盖,运行时字段,永不序列化。独立于
    // question_policy 存放:若直接覆写上面的配置值,后续任何 save_config
    // (如 /model --default)都会把 CLI 会话级意志意外落盘。
    // 空 = 无覆盖;question_timeout_seconds_cli 仅在 cli 值为 "timeout"
    // 且用户给了冒号秒数时非 0。
    std::string question_policy_cli;
    int question_timeout_seconds_cli = 0;
};

// TUI 渲染策略。绕开 Win10 < 1809 的 conhost / Cmder/ConEmu 在密集 cursor-up
// 序列下产生的画面跳动 —— 详细背景见
// openspec/changes/add-legacy-terminal-fallback/。
//
//   "auto"   = 默认。走 alt-screen,让 TUI 启动时直接撑满终端。
//   "always" = 始终走 alt-screen(\033[?1049h)。
//   "never"  = 始终走 TerminalOutput。
//
// 非法值会在 load_config 中被规范化为 "auto" 并 LOG_WARN。
struct TuiConfig {
    std::string alt_screen_mode = "auto";
    // 把 PgUp / PgDn 当成单行滚动 (等同 Alt+↑/↓). 部分终端 (老 conhost / Cmder /
    // 某些远程 SSH 客户端) 吞掉 Alt+方向键序列, 用户拿不到 Alt+Arrow; 默认打开,
    // 需要整页滚动时可通过 /page-step off 写入 tui.page_keys_single_line=false.
    bool page_keys_single_line = true;
    // TUI 调色板: "auto"(启动时探测终端背景色) / "dark" / "light"。
    std::string theme = "auto";
};

// Network / HTTP client tuning. Drives the system-proxy integration —
// see openspec/changes/respect-system-proxy. proxy_mode is the only field
// callers SHOULD branch on at runtime; the rest are passively consumed by
// network::ProxyResolver.
// Web search tool tunables. See openspec/changes/add-web-search-tool/.
// `backend = "auto"` 走启动时探测的 region 决定(global → duckduckgo,cn →
// bing_cn);显式名字直接使用并跳过探测(但运行时 fallback 仍生效)。
// `bochaai` / `tavily` 为后续 API backend 占位,本期未实现 — 命中时启动
// LOG_WARN 并回退到 auto。
struct WebSearchConfig {
    bool enabled = true;
    // "auto" | "duckduckgo" | "bing_cn" | "bochaai" | "tavily"
    std::string backend = "auto";
    std::string api_key;        // 给将来 API backend 用,本期不读
    int max_results = 5;        // 工具入参 limit 上限(min(limit, max_results, 10))
    int timeout_ms = 8000;      // 单次 backend HTTP 请求超时
};

// 单个 LSP server 的 config 条目(openspec add-lsp-service)。
// 名字命中内置 server(clangd / typescript-language-server / pyright /
// gopls / rust-analyzer)时按字段覆盖内置定义;新名字 = 纯自定义 server,
// 此时 command 必填(argv 形式),extensions 建议提供(空 = 匹配所有文件)。
struct LspServerOverride {
    bool disabled = false;
    std::vector<std::string> command;      // argv;空 = 沿用内置 spawn 逻辑
    std::vector<std::string> extensions;   // 形如 ".rs";空 = 沿用内置定义
    std::map<std::string, std::string> env;
    nlohmann::json initialization;         // initializationOptions 原样透传
};

// LSP 集成总配置。enabled=false 时:lsp 工具不注册、编辑后不注入诊断、
// 不 spawn 任何 server 进程 —— 行为与引入 LSP 前完全一致。
struct LspConfig {
    bool enabled = true;
    std::map<std::string, LspServerOverride> servers;
};

// Worktree 隔离配置(enter_worktree 工具 / --worktree 启动),对齐
// Claude Code settings.json 的 worktree 段。两个字段都是显式配置才生效。
struct WorktreeConfig {
    // 从主仓 symlink 进新 worktree 的目录(如 "node_modules" ".cache"),
    // 避免每个 worktree 重复占磁盘。默认不做任何 symlink。
    std::vector<std::string> symlink_directories;
    // 创建 worktree 时走 git sparse-checkout(cone 模式)只落盘这些路径,
    // 大型 monorepo 明显更快。默认空 = 完整 checkout。
    std::vector<std::string> sparse_paths;
};

// git 感知配置(openspec add-git-context)。enabled=false 时不采集/不注入
// gitStatus 快照,/api/git/* 端点按非仓库处理;系统提示的 git repo 标识行
// 保留(零成本且不泄露仓库状态)。
struct GitContextConfig {
    bool enabled = true;
    int timeout_ms = 3000; // 单条 git 命令超时,clamp [500, 30000]
};

// TUI /remote-control 基座配置(openspec add-remote-control)。
// token 持久化后,channel bridge 跨 ACECode 重启无需重新配对;首次
// /remote-control on 或默认 channel 激活时自动生成并写回。
struct RemoteControlConfig {
    struct ChannelPluginConfig {
        std::string manifest_path;                 // 外部 channel 插件 manifest
        int timeout_ms = 10000;                    // 激活/解除绑定进程超时
        nlohmann::json settings = nlohmann::json::object(); // 透传给插件
    };

    int port = 28190;          // loopback listener 端口(避开 daemon 默认 28080)
    std::string token;         // 空 = 首次启用时生成并持久化
    std::string outbound_url;  // manual 出站 webhook;空 = 仅入站/等待插件
    std::string default_channel;
    std::map<std::string, ChannelPluginConfig> channels;
};

struct AceBrowserPointerCustomConfig {
    int move_duration_ms_min = 180;
    int move_duration_ms_max = 650;
    int click_hold_ms_min = 45;
    int click_hold_ms_max = 120;
    int typing_delay_ms_min = 20;
    int typing_delay_ms_max = 90;
    double jitter_px = 2.0;
    int max_path_points = 80;
};

struct AceBrowserBridgeConfig {
    bool enabled = false;
    // Deprecated compatibility override. New configs do not write this field;
    // the client resolves ace-browser-host next to the acecode executable.
    std::string host_path;
    // "progressive" | "compact" | "full"
    std::string tool_mode = "progressive";
    // "auto" | "dom" | "cdp" | "os"
    std::string default_mode = "auto";
    // "fast" | "normal" | "slow" | "custom"
    std::string pointer_speed = "normal";
    AceBrowserPointerCustomConfig pointer_custom;
    int status_cache_ttl_ms = 2000;
    int tool_timeout_ms = 30000;
    bool os_pointer_enabled = false;
    bool tab_group_enabled = true;
    bool operation_overlay_enabled = true;
    int operation_overlay_watchdog_ms = 10000;
};

struct UpgradeConfig {
    std::string base_url = "http://2017studio.imwork.net:82/aupdate/";
    int timeout_ms = 30000;
};

// Desktop shell (acecode-desktop.exe) — OS notification settings.
// 见 openspec/changes/add-desktop-attention-notifications。
// 目前只在 Windows 桌面壳读取;非 Windows 平台字段不参与运行,但保留解析。
struct DesktopNotificationsConfig {
    bool enabled = true;                 // 总开关
    bool on_question = true;             // AskUserQuestion 触发通知
    bool on_completion = true;           // 回合完成触发通知
    bool suppress_when_focused = true;   // 当前 session 已可见且窗口聚焦时不弹
};

struct DesktopConfig {
    DesktopNotificationsConfig notifications;
    // 关窗(× / Alt+F4 / aceDesktop_closeWindow)默认隐藏到托盘。
    // false 时回到旧行为(关窗即退出)。见 openspec/changes/enhance-desktop-tray-menu。
    bool close_to_tray = true;
};

// Web 控制台(ConsoleDock)配置。见 openspec/changes/add-console-dock。
struct ConsoleConfig {
    // 终端 shell 覆盖(legacy)。空 = 平台默认(Windows: %COMSPEC% 即 cmd;POSIX: $SHELL)。
    // 例:"pwsh" / "powershell" / "/usr/bin/fish"。
    std::string shell;
    // + 旁下拉框选中的默认 shell id(powershell / git-bash / cmd / shell / ...)。
    // 空 = 平台默认。见 detect_console_shells / default_console_shell_id。
    std::string default_shell;
    // 用户指定的 Git Bash bash.exe 完整路径(自动探测不到时填,永久记住)。
    std::string git_bash_path;
};

struct SessionTitleConfig {
    bool enabled = true;
    // Empty = use the current session model. Non-empty = saved_models.name.
    std::string model_name;
    int max_input_bytes = 1000;
    int timeout_ms = 15000;
};

struct NetworkConfig {
    // "auto"   = Windows: WinHTTP-IE → registry → env → direct;
    //            POSIX: env (HTTPS_PROXY/HTTP_PROXY/ALL_PROXY/NO_PROXY).
    // "off"    = force direct, ignore all sources.
    // "manual" = use proxy_url verbatim.
    std::string proxy_mode = "auto";
    std::string proxy_url;        // required when proxy_mode == "manual"
    std::string proxy_no_proxy;   // comma-separated; merged with env NO_PROXY

    // openspec/changes/proxy-fallback-on-unreachable:启动时对解析出的代理做
    // 一次同步 TCP probe,connect 失败就把进程级 fallback flag 置位、横幅显示
    // `auto-fallback`、所有 cpr 走直连。`/proxy refresh` 重跑探测。
    int  proxy_probe_timeout_ms = 1500;  // load_config 时 clamp 到 [200, 10000]
    bool proxy_probe_enabled = true;     // 总开关:false = 完全跳过探测,等价旧行为
};

struct FeaturesConfig {
    // Codex-compatible hooks are enabled by default. Users can set
    // features.hooks=false to disable discovery and execution.
    bool hooks = true;
    // Static, global switch for Web/Desktop completed-turn transcript self-heal.
    // Missing or invalid config keeps this enabled; users can set it false.
    bool completed_turn_self_heal = true;
};

struct AppConfig {
    std::string provider; // active runtime provider: "copilot" or "openai"; empty = not configured
    OpenAiConfig openai;
    CopilotConfig copilot;
    CodexConfig codex;
    int context_window = 128000; // model context window size in tokens
    int max_sessions = 50;       // max saved sessions per project
    // Default permission mode for newly-created daemon/Web/Desktop sessions.
    // Canonical values: default | accept-edits | plan | yolo.
    std::string default_permission_mode = "default";
    std::map<std::string, McpServerConfig> mcp_servers; // MCP stdio servers (optional)
    SkillsConfig skills;                         // skill system configuration (optional)
    MemoryConfig memory;                         // persistent user memory settings
    ProjectInstructionsConfig project_instructions; // AGENT.md / CLAUDE.md loader
    CustomInstructionsConfig custom_instructions; // Desktop/Web user-authored prompt context
    std::vector<ConnectorConfig> connectors;      // user-configured desktop connectors
    DaemonConfig daemon;                         // daemon process supervision settings
    WebConfig web;                               // HTTP/WebSocket server settings
    WebUiPreferencesConfig web_ui;               // Web/Desktop UI-only preferences
    ModelsDevConfig models_dev;                  // bundled models.dev registry behaviour
    InputHistoryConfig input_history;            // per-cwd persistent ↑/↓ history
    AgentLoopConfig agent_loop;                  // agent-loop termination tunables
    NetworkConfig network;                       // proxy / TLS / abort-debug knobs
    FeaturesConfig features;                     // feature flags
    WebSearchConfig web_search;                  // 联网搜索工具配置(参见 add-web-search-tool)
    LspConfig lsp;                               // LSP 集成(参见 add-lsp-service)
    WorktreeConfig worktree;                     // worktree 隔离(enter_worktree / --worktree)
    GitContextConfig git_context;                // git 感知(参见 add-git-context)
    RemoteControlConfig remote_control;          // TUI /remote-control channel 托管
    AceBrowserBridgeConfig ace_browser_bridge;   // browser bridge tools integration
    UpgradeConfig upgrade;                       // explicit self-upgrade command config
    TuiConfig tui;                               // 终端渲染策略(legacy fallback 等)
    DesktopConfig desktop;                       // desktop shell 配置(系统通知等)
    ConsoleConfig console;                       // Web 控制台(PTY shell 覆盖)
    SessionTitleConfig session_title;            // hidden auto session title generation

    // --- model profiles (openspec/changes/model-profiles) ---
    // 用户维护的命名模型列表。
    std::vector<ModelProfile> saved_models;
    // 指向 saved_models 中一个 entry 的 name;空字符串 = 未设定。
    std::string default_model_name;
};

// Build a backwards-compatible ModelProfile from the legacy top-level
// provider/openai/copilot fields. Returns an empty profile when no legacy
// provider is configured.
ModelProfile legacy_model_profile_from_config(const AppConfig& cfg);

// Normalize upgrade.base_url by trimming surrounding whitespace and adding a
// trailing slash when non-empty.
std::string normalize_upgrade_base_url(std::string raw);

// Returns true for non-empty http/https URLs after normalization.
bool is_valid_upgrade_base_url(const std::string& raw);

nlohmann::json connectors_to_json(const std::vector<ConnectorConfig>& connectors);
bool parse_connectors_json(const nlohmann::json& value,
                           std::vector<ConnectorConfig>& out,
                           std::string* error = nullptr);

// 返回「enabled 从 false→true(或新出现)且配置了 on_enable 钩子」的连接器。
// PUT /api/config/connectors 用它决定要异步拉起哪些 on_enable 进程。
std::vector<ConnectorConfig> newly_enabled_connectors(
    const std::vector<ConnectorConfig>& before,
    const std::vector<ConnectorConfig>& after);

// 返回 enabled 且配置了 on_startup 钩子的连接器。daemon 启动时对它们各异步
// 执行一次钩子(登录检查类命令自身保证幂等)。
std::vector<ConnectorConfig> startup_hook_connectors(
    const std::vector<ConnectorConfig>& connectors);

// Load config from ~/.acecode/config.json, with env var overrides.
// Creates default config if missing.
AppConfig load_config();

// True after load_config() created the ACECode home directory during this
// process. consume_acecode_home_created_by_process() returns that value and
// clears it so first-initialization hooks run once.
bool was_acecode_home_created_by_process();
bool consume_acecode_home_created_by_process();
void reset_acecode_home_created_flag_for_test();

// Save config to ~/.acecode/config.json.
// Creates directory if missing, overwrites existing file.
void save_config(const AppConfig& cfg);

// Save config to an explicit file path. Creates parent directory if missing.
// Used by daemon/test code paths that must NOT touch the user's real config —
// e.g. PUT /api/mcp under WebServerFixture writes to a per-test temp file.
void save_config(const AppConfig& cfg, const std::string& explicit_path);

// Refresh only the cross-surface defaults used when creating a fresh session.
// This deliberately does not hot-reload the whole AppConfig: long-lived daemon
// state such as proxy, hooks, memory, and runtime services remains unchanged.
// Returns false on parse/validation errors and leaves cfg unchanged.
bool refresh_default_session_preferences_from_config(
    AppConfig& cfg,
    const std::string& explicit_path = {},
    std::string* error = nullptr);

// Get the path to ~/.acecode/ directory
std::string get_acecode_dir();

// Get the path to ~/.acecode/run/ (creates it if missing on first call site —
// callers are responsible for filesystem::create_directories when needed).
std::string get_run_dir();

// Get the path to ~/.acecode/logs/ (callers handle create_directories).
std::string get_logs_dir();

// Validate runtime-affecting config values. Returns an empty vector on success;
// otherwise each entry is a human-readable error message. Daemon mode callers
// should abort on non-empty result.
std::vector<std::string> validate_config(const AppConfig& cfg);

} // namespace acecode
