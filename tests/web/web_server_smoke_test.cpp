// 覆盖 src/web/server.{hpp,cpp} 的 HTTP 路由 + 鉴权路径(spec Section 9 + 11)。
// 起一个真 WebServer 在随机端口 + cpr 客户端打全部 endpoint,验证:
//   - GET /api/health 不需要 token 也能通(loopback)
//   - POST /api/sessions 创建 session + GET 看到它
//   - DELETE 后 GET 看不到
//   - GET /api/sessions/<id>/messages 返回 events+messages
//   - GET /api/skills 全量扫描 workspace 项目链 + 全局根,项目 skill 带 source
//   - GET /api/mcp 返回当前 mcp_servers
//   - POST /api/mcp/reload 返回 501
//   - 远程 IP(非 loopback)模拟 → 这里用 cpr 走 127.0.0.1 不容易模拟,所以
//     远程鉴权由 auth_test.cpp 单元覆盖,这里只验路由 wiring 通的部分
//
// WebSocket 路径不在自动化覆盖范围 — cpr 不带 WS client。WS 协议的客户端->
// 服务端 hello/user_input/decision/abort 由后续 add-web-chat-ui change 的端到端
// 集成测验证。当前 WS 行为依赖 spec 里描述的 hello-binding 协议。

#include <gtest/gtest.h>
#include <httplib.h>
#include <sqlite3.h>

#include "provider/auth/github_auth.hpp"
#include "config/config.hpp"
#include "config/saved_models.hpp"
#include "permissions.hpp"
#include "desktop/workspace_registry.hpp"
#include "hooks/hook_manager.hpp"
#include "loop/loop_store.hpp"
#include "provider/cwd_model_override.hpp"
#include "session/local_session_client.hpp"
#include "session/session_manager.hpp"
#include "session/session_registry.hpp"
#include "session/session_storage.hpp"
#include "session/session_user_message_search.hpp"
#include "session/todo_state.hpp"
#include "session/session_usage_ledger.hpp"
#include "skills/skill_registry.hpp"
#include "tool/tool_executor.hpp"
#include "upgrade/manifest.hpp"
#include "utils/encoding.hpp"
#include "utils/cwd_hash.hpp"
#include "utils/state_file.hpp"
#include "utils/utf8_path.hpp"
#include "web/server.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cpr/cpr.h>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <random>
#include <thread>
#include <zip.h>

using namespace std::chrono_literals;
using nlohmann::json;

namespace {

// 找一个未被占用的端口。简化做法: 用 cpr 试连 0(让 OS 分配)不可行 —
// Crow bind 时要显式数字。这里用一个随机偏移的端口,失败重试几次。
// 为避免和真 daemon/desktop 默认端口冲突,从高位端口起。
int pick_test_port() {
    static std::atomic<int> next{46000};
    return next.fetch_add(7);
}

std::filesystem::path path_from_utf8(const std::string& s) {
#ifdef _WIN32
    return std::filesystem::path(acecode::utf8_to_wide(s));
#else
    return std::filesystem::path(s);
#endif
}

void write_skill_md(const std::filesystem::path& root,
                    const std::string& name,
                    const std::string& description) {
    const auto dir = root / "general" / name;
    std::filesystem::create_directories(dir);
    std::ofstream ofs(dir / "SKILL.md", std::ios::binary);
    ofs << "---\n"
        << "name: " << name << "\n"
        << "description: " << description << "\n"
        << "---\n\n# " << name << "\n";
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    ofs << text;
}

std::string url_encode_component(const std::string& value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value) {
        const bool safe =
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~';
        if (safe) {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('%');
            out.push_back(kHex[ch >> 4]);
            out.push_back(kHex[ch & 0x0f]);
        }
    }
    return out;
}

std::string read_zip_entry(const std::filesystem::path& zip_path,
                           const std::string& entry) {
    int err = 0;
    zip_t* archive = zip_open(acecode::path_to_utf8(zip_path).c_str(), ZIP_RDONLY, &err);
    if (!archive) return {};
    zip_int64_t index = zip_name_locate(archive, entry.c_str(), ZIP_FL_ENC_UTF_8);
    if (index < 0) {
        zip_close(archive);
        return {};
    }
    zip_stat_t st;
    zip_stat_init(&st);
    if (zip_stat_index(archive, static_cast<zip_uint64_t>(index), 0, &st) != 0) {
        zip_close(archive);
        return {};
    }
    zip_file_t* file = zip_fopen_index(archive, static_cast<zip_uint64_t>(index), 0);
    if (!file) {
        zip_close(archive);
        return {};
    }
    std::string out(static_cast<std::size_t>(st.size), '\0');
    zip_int64_t read = zip_fread(file, out.data(), out.size());
    zip_fclose(file);
    zip_close(archive);
    if (read < 0) return {};
    out.resize(static_cast<std::size_t>(read));
    return out;
}

bool zip_entry_exists(const std::filesystem::path& zip_path,
                      const std::string& entry) {
    int err = 0;
    zip_t* archive = zip_open(acecode::path_to_utf8(zip_path).c_str(), ZIP_RDONLY, &err);
    if (!archive) return false;
    const bool exists =
        zip_name_locate(archive, entry.c_str(), ZIP_FL_ENC_UTF_8) >= 0;
    zip_close(archive);
    return exists;
}

class BlockingProvider : public acecode::LlmProvider {
public:
    acecode::ChatResponse chat(const std::vector<acecode::ChatMessage>&,
                               const std::vector<acecode::ToolDef>&) override {
        acecode::ChatResponse resp;
        resp.content = "unused";
        resp.finish_reason = "stop";
        return resp;
    }

    void chat_stream(const std::vector<acecode::ChatMessage>&,
                     const std::vector<acecode::ToolDef>&,
                     const acecode::StreamCallback&,
                     std::atomic<bool>* abort_flag = nullptr) override {
        {
            std::lock_guard<std::mutex> lk(mu_);
            started_ = true;
        }
        cv_.notify_all();
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [&] {
            return released_ || (abort_flag && abort_flag->load());
        });
    }

    bool wait_for_started(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mu_);
        return cv_.wait_for(lk, timeout, [&] { return started_; });
    }

    void release() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            released_ = true;
        }
        cv_.notify_all();
    }

    std::string name() const override { return "blocking-stub"; }
    bool is_authenticated() override { return true; }
    std::string model() const override { return "blocking-stub"; }
    void set_model(const std::string&) override {}

private:
    std::mutex mu_;
    std::condition_variable cv_;
    bool started_ = false;
    bool released_ = false;
};

struct WebServerFixture {
    acecode::ToolExecutor tools;
    acecode::PermissionManager template_perm;
    acecode::SkillRegistry skill_registry;
    acecode::AppConfig cfg;
    acecode::WebConfig web_cfg;
    acecode::DaemonConfig daemon_cfg;
    std::unique_ptr<acecode::desktop::WorkspaceRegistry> workspace_registry;

    std::unique_ptr<acecode::SessionRegistry> registry;
    std::unique_ptr<acecode::LocalSessionClient> client;
    std::unique_ptr<acecode::HookManager> hook_manager;
    std::unique_ptr<acecode::loop::LoopStore> loop_store;
    std::unique_ptr<acecode::web::WebServer> server;

    std::thread server_thread;
    int port = 0;
    std::filesystem::path tmp_dir;
    std::filesystem::path cwd_dir;
    std::filesystem::path projects_dir;
    std::filesystem::path no_workspace_cache_root;
    std::filesystem::path logs_dir;
    std::filesystem::path feedback_dir;
    std::filesystem::path state_file_path;
    std::string cwd;
    std::string project_dir;

    explicit WebServerFixture(
        bool register_default_workspace = true,
        bool native_folder_picker_enabled = false,
        std::function<std::optional<std::string>()> native_folder_picker = {},
        bool attach_skill_registry = true,
        std::function<int(const acecode::AppConfig&,
                          acecode::upgrade::UpgradeProgressCallback,
                          std::string*)> run_update_command = {},
        std::function<std::optional<std::string>(const std::string&)> open_in_explorer = {}) {
        port = pick_test_port();
        web_cfg.bind = "127.0.0.1";
        web_cfg.port = port;
        cfg.web = web_cfg;
        cfg.daemon = daemon_cfg;
        acecode::ModelProfile default_model;
        default_model.name = "fixture-copilot";
        default_model.provider = "copilot";
        default_model.model = "gpt-4o";
        cfg.saved_models.push_back(default_model);

        // PUT /api/mcp 走 save_config 落盘 — 必须指向临时目录,否则
        // 会覆盖真实的 ~/.acecode/config.json(历史 bug,曾把测试用的
        // /usr/bin/python3 -m myserver MCP server 写进用户配置)。
        std::random_device rd;
        tmp_dir = std::filesystem::temp_directory_path() /
                  ("acecode_web_test_" + std::to_string(rd()));
        std::filesystem::create_directories(tmp_dir);
        cwd_dir = tmp_dir / "cwd";
        std::filesystem::create_directories(cwd_dir);
        projects_dir = tmp_dir / "projects";
        std::filesystem::create_directories(projects_dir);
        no_workspace_cache_root = tmp_dir / "cache" / "no-workspace";
        std::filesystem::create_directories(no_workspace_cache_root);
        logs_dir = tmp_dir / "logs";
        std::filesystem::create_directories(logs_dir);
        feedback_dir = tmp_dir / "feedback";
        std::filesystem::create_directories(feedback_dir);
        state_file_path = tmp_dir / "state.json";
        acecode::set_state_file_path_for_test(state_file_path.string());
        cwd = cwd_dir.string();
        workspace_registry = std::make_unique<acecode::desktop::WorkspaceRegistry>();
        if (register_default_workspace) {
            workspace_registry->register_new(projects_dir.string(), cwd);
        } else {
            acecode::desktop::ensure_workspace_metadata(projects_dir.string(), cwd);
        }
        workspace_registry->scan(projects_dir.string());
        project_dir = acecode::SessionStorage::get_project_dir(cwd);
        std::filesystem::remove_all(project_dir);

        acecode::SessionRegistryDeps deps;
        deps.provider_accessor = [] { return std::shared_ptr<acecode::LlmProvider>{}; };
        deps.tools = &tools;
        deps.cwd = cwd;
        deps.no_workspace_cache_root = no_workspace_cache_root.string();
        deps.config = &cfg;
        deps.template_permissions = &template_perm;
        registry = std::make_unique<acecode::SessionRegistry>(std::move(deps));
        client = std::make_unique<acecode::LocalSessionClient>(*registry);
        hook_manager = std::make_unique<acecode::HookManager>(acecode::HookRegistrySnapshot{});
        loop_store = std::make_unique<acecode::loop::LoopStore>(tmp_dir / "loops.sqlite3");
        acecode::loop::StoreError loop_error;
        if (!loop_store->initialize(&loop_error)) {
            throw std::runtime_error("failed to initialize LOOP test store: " + loop_error.message);
        }

        acecode::web::WebServerDeps wdeps;
        wdeps.web_cfg = &cfg.web;
        wdeps.daemon_cfg = &cfg.daemon;
        wdeps.app_config = &cfg;
        wdeps.config_path = (tmp_dir / "config.json").string();
        wdeps.cwd = cwd;
        wdeps.no_workspace_cache_root = no_workspace_cache_root.string();
        wdeps.token = "smoke-token";
        wdeps.logs_dir = logs_dir.string();
        wdeps.feedback_output_dir = feedback_dir.string();
        wdeps.guid = "test-guid-aaaa-bbbb";
        wdeps.pid = 12345;
        wdeps.start_time_unix_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        wdeps.session_client = client.get();
        wdeps.session_registry = registry.get();
        wdeps.hook_manager = hook_manager.get();
        wdeps.projects_dir = projects_dir.string();
        wdeps.workspace_registry = workspace_registry.get();
        wdeps.native_folder_picker_enabled = native_folder_picker_enabled;
        wdeps.native_folder_picker = std::move(native_folder_picker);
        wdeps.open_in_explorer = std::move(open_in_explorer);
        wdeps.run_update_command = std::move(run_update_command);
        wdeps.skill_registry = attach_skill_registry ? &skill_registry : nullptr;
        wdeps.dangerous = false;
        wdeps.loop_store = loop_store.get();

        server = std::make_unique<acecode::web::WebServer>(std::move(wdeps));
        server_thread = std::thread([this] { server->run(); });

        // 等 server 监听就绪 — 用 cpr 探活到 /api/health 通为止,最多 3s
        auto deadline = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(50ms);
            auto r = cpr::Get(cpr::Url{"http://127.0.0.1:" + std::to_string(port) + "/api/health"},
                              cpr::Timeout{500});
            if (r.status_code == 200) return;
        }
        // 起不来的话后续测试会失败 — 不在这里 throw,让 GTest 给清晰报错
    }

    ~WebServerFixture() {
        if (server) server->stop();
        if (server_thread.joinable()) server_thread.join();
        acecode::set_state_file_path_for_test("");
        std::error_code ec;
        std::filesystem::remove_all(project_dir, ec);
        std::filesystem::remove_all(tmp_dir, ec);
    }

    std::string url(const std::string& path) {
        return "http://127.0.0.1:" + std::to_string(port) + path;
    }
};

struct LocalUpdateServer {
    httplib::Server svr;
    int port = 0;
    std::thread th;

    explicit LocalUpdateServer(std::function<void(httplib::Server&)> setup) {
        setup(svr);
        port = svr.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { svr.listen_after_bind(); });
        for (int i = 0; i < 50 && !svr.is_running(); ++i) {
            std::this_thread::sleep_for(10ms);
        }
    }

    ~LocalUpdateServer() {
        svr.stop();
        if (th.joinable()) th.join();
    }

    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port) + "/";
    }
};

std::string update_manifest_for(const std::string& version) {
    return R"({
      "schema_version": 1,
      "latest": ")" + version + R"(",
      "releases": [
        {"version": ")" + version + R"(", "packages": [
          {"target": ")" + acecode::upgrade::current_target() + R"(", "file": "acecode.zip", "sha256": ")" + std::string(64, 'a') + R"("}
        ]}
      ]
    })";
}

struct CwdModelOverrideCleanup {
    std::string cwd;
    ~CwdModelOverrideCleanup() {
        if (!cwd.empty()) acecode::remove_cwd_model_override(cwd);
    }
};

struct RemoveTreeOnExit {
    std::filesystem::path path;
    ~RemoveTreeOnExit() {
        std::error_code ec;
        if (!path.empty()) std::filesystem::remove_all(path, ec);
    }
};

#ifdef _WIN32
constexpr const char* kHomeEnvName = "USERPROFILE";
#else
constexpr const char* kHomeEnvName = "HOME";
#endif

std::optional<std::string> get_env_value(const char* name) {
    const char* value = std::getenv(name);
    if (!value) return std::nullopt;
    return std::string(value);
}

void set_env_value(const char* name, const std::optional<std::string>& value) {
#ifdef _WIN32
    _putenv_s(name, value ? value->c_str() : "");
#else
    if (value) {
        setenv(name, value->c_str(), 1);
    } else {
        unsetenv(name);
    }
#endif
}

struct ScopedHomeOverride {
    std::optional<std::string> old_home;
    std::filesystem::path path;

    explicit ScopedHomeOverride(const std::filesystem::path& new_home)
        : old_home(get_env_value(kHomeEnvName)), path(new_home) {
        std::filesystem::create_directories(path);
        set_env_value(kHomeEnvName, path.string());
    }

    ~ScopedHomeOverride() {
        set_env_value(kHomeEnvName, old_home);
    }
};

struct ScopedEnvOverride {
    std::string name;
    std::optional<std::string> old_value;

    ScopedEnvOverride(const char* env_name, const std::optional<std::string>& value)
        : name(env_name), old_value(get_env_value(env_name)) {
        set_env_value(name.c_str(), value);
    }

    ~ScopedEnvOverride() {
        set_env_value(name.c_str(), old_value);
    }
};

std::string lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string response_header(const cpr::Response& r, const std::string& name) {
    auto want = lower_ascii(name);
    for (const auto& [k, v] : r.header) {
        if (lower_ascii(k) == want) return v;
    }
    return {};
}

} // namespace

// 场景: /api/health 是无 token 探活路径,loopback 总是 200,返回 JSON 含
// guid/pid/port/version/cwd/uptime_seconds 这 6 个字段(spec 9.2)。
TEST(WebServerHttp, HealthEndpointReturnsBasicMetadata) {
    WebServerFixture fx;
    auto r = cpr::Get(cpr::Url{fx.url("/api/health")});
    ASSERT_EQ(r.status_code, 200);
    auto j = json::parse(r.text);
    EXPECT_TRUE(j.contains("guid"));
    EXPECT_TRUE(j.contains("pid"));
    EXPECT_TRUE(j.contains("port"));
    EXPECT_TRUE(j.contains("version"));
    EXPECT_TRUE(j.contains("cwd"));
    EXPECT_TRUE(j.contains("uptime_seconds"));
    EXPECT_EQ(j["guid"], "test-guid-aaaa-bbbb");
    EXPECT_EQ(j["pid"], 12345);
    EXPECT_EQ(j["port"], fx.port);

    // desktop.notifications 默认五个 bool 全 true,health 透传给前端做抑制规则判定。
    // 见 openspec/changes/add-windows-wintoast-completion-notifications。
    ASSERT_TRUE(j.contains("notifications"));
    ASSERT_TRUE(j["notifications"].is_object());
    EXPECT_EQ(j["notifications"]["enabled"], true);
    EXPECT_EQ(j["notifications"]["on_permission"], true);
    EXPECT_EQ(j["notifications"]["on_question"], true);
    EXPECT_EQ(j["notifications"]["on_completion"], true);
    EXPECT_EQ(j["notifications"]["suppress_when_focused"], true);

    ASSERT_TRUE(j.contains("features"));
    ASSERT_TRUE(j["features"].contains("completed_turn_self_heal"));
    EXPECT_EQ(j["features"]["completed_turn_self_heal"]["enabled"], true);
}

TEST(WebServerHttp, DesktopNotificationSettingDefaultsOnAndPersistsChanges) {
    WebServerFixture fx;

    auto initial = cpr::Get(
        cpr::Url{fx.url("/api/config/desktop-notifications")});
    ASSERT_EQ(initial.status_code, 200) << initial.text;
    auto initial_json = json::parse(initial.text);
    EXPECT_EQ(initial_json["enabled"], true);
    EXPECT_EQ(initial_json["on_permission"], true);
    EXPECT_EQ(initial_json["on_question"], true);
    EXPECT_EQ(initial_json["on_completion"], true);

    auto put = cpr::Put(
        cpr::Url{fx.url("/api/config/desktop-notifications")},
        cpr::Header{{"Content-Type", "application/json"}},
        cpr::Body{R"({"enabled":false})"});
    ASSERT_EQ(put.status_code, 200) << put.text;
    auto body = json::parse(put.text);
    EXPECT_EQ(body["enabled"], false);
    EXPECT_FALSE(fx.cfg.desktop.notifications.enabled);

    auto health = cpr::Get(cpr::Url{fx.url("/api/health")});
    ASSERT_EQ(health.status_code, 200) << health.text;
    EXPECT_EQ(json::parse(health.text)["notifications"]["enabled"], false);

    std::ifstream ifs(fx.tmp_dir / "config.json");
    ASSERT_TRUE(ifs.is_open());
    auto saved = json::parse(ifs);
    EXPECT_EQ(saved["desktop"]["notifications"]["enabled"], false);
    EXPECT_FALSE(saved["desktop"]["notifications"].contains("on_permission"));
}

TEST(WebServerHttp, DesktopNotificationSettingRejectsInvalidPayload) {
    WebServerFixture fx;
    auto put = cpr::Put(
        cpr::Url{fx.url("/api/config/desktop-notifications")},
        cpr::Header{{"Content-Type", "application/json"}},
        cpr::Body{R"({"enabled":"yes"})"});
    ASSERT_EQ(put.status_code, 400) << put.text;
    EXPECT_EQ(json::parse(put.text)["error"], "BAD_REQUEST");
    EXPECT_TRUE(fx.cfg.desktop.notifications.enabled);
}

TEST(WebServerHttp, LoopCrudEnableConflictAndRunHistory) {
    WebServerFixture fx;
    const auto workspace = fx.workspace_registry->list().front();
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto body_for = [&](const std::string& name, bool with_workspace) {
        json body{
            {"name", name},
            {"prompt", "Inspect the code and report actionable issues."},
            {"model_name", "fixture-copilot"},
            {"permission_mode", "yolo"},
            {"schedule", {
                {"kind", "interval"},
                {"interval_value", 2},
                {"interval_unit", "hours"},
                {"anchor_ms", now + 3600000},
            }},
        };
        if (with_workspace) {
            body["workspace_hash"] = workspace.hash;
            body["workspace_cwd"] = workspace.cwd;
        }
        return body;
    };

    auto create = cpr::Post(cpr::Url{fx.url("/api/loops")},
                            cpr::Body{body_for("Review code", true).dump()},
                            cpr::Header{{"Content-Type", "application/json"}});
    ASSERT_EQ(create.status_code, 201) << create.text;
    auto created = json::parse(create.text);
    ASSERT_TRUE(created.contains("id"));
    EXPECT_FALSE(created.contains("schedule_expr"));
    const std::string id = created["id"].get<std::string>();

    auto list = cpr::Get(cpr::Url{fx.url("/api/loops")});
    ASSERT_EQ(list.status_code, 200);
    auto listed = json::parse(list.text)["loops"];
    ASSERT_EQ(listed.size(), 1);
    EXPECT_TRUE(listed[0]["latest_run"].is_null());

    acecode::loop::StoreError claim_error;
    auto claimed = fx.loop_store->claim_due(now + 3 * 60 * 60 * 1000,
                                             "web-list-owner", &claim_error);
    ASSERT_TRUE(claim_error.code.empty()) << claim_error.message;
    ASSERT_EQ(claimed.disposition, acecode::loop::ClaimDisposition::Claimed);
    ASSERT_TRUE(claimed.loop.has_value());
    ASSERT_TRUE(claimed.run.has_value());
    EXPECT_EQ(claimed.loop->id, id);

    list = cpr::Get(cpr::Url{fx.url("/api/loops")});
    ASSERT_EQ(list.status_code, 200);
    listed = json::parse(list.text)["loops"];
    ASSERT_EQ(listed.size(), 1);
    ASSERT_TRUE(listed[0]["latest_run"].is_object());
    EXPECT_EQ(listed[0]["latest_run"]["status"], "running");
    EXPECT_EQ(listed[0]["latest_run"]["loop_id"], id);

    auto get = cpr::Get(cpr::Url{fx.url("/api/loops/" + id)});
    ASSERT_EQ(get.status_code, 200);
    EXPECT_EQ(json::parse(get.text)["name"], "Review code");

    auto conflict = cpr::Post(cpr::Url{fx.url("/api/loops")},
                              cpr::Body{body_for("Conflicting review", true).dump()},
                              cpr::Header{{"Content-Type", "application/json"}});
    ASSERT_EQ(conflict.status_code, 409) << conflict.text;
    EXPECT_EQ(json::parse(conflict.text)["error"], "SCHEDULE_CONFLICT");

    // No-workspace LOOPs are intentionally exempt from the workspace guard.
    auto no_workspace = cpr::Post(cpr::Url{fx.url("/api/loops")},
                                  cpr::Body{body_for("Independent review", false).dump()},
                                  cpr::Header{{"Content-Type", "application/json"}});
    ASSERT_EQ(no_workspace.status_code, 201) << no_workspace.text;

    auto disable = cpr::Put(cpr::Url{fx.url("/api/loops/" + id + "/enabled")},
                            cpr::Body{R"({"enabled":false})"},
                            cpr::Header{{"Content-Type", "application/json"}});
    ASSERT_EQ(disable.status_code, 200) << disable.text;
    EXPECT_FALSE(json::parse(disable.text)["enabled"].get<bool>());

    auto runs = cpr::Get(cpr::Url{fx.url("/api/loops/" + id + "/runs")});
    ASSERT_EQ(runs.status_code, 200);
    ASSERT_EQ(json::parse(runs.text)["runs"].size(), 1);
    EXPECT_EQ(json::parse(runs.text)["runs"][0]["status"], "running");

    auto updated_body = body_for("Updated review", true);
    updated_body["enabled"] = false;
    auto update = cpr::Put(cpr::Url{fx.url("/api/loops/" + id)},
                           cpr::Body{updated_body.dump()},
                           cpr::Header{{"Content-Type", "application/json"}});
    ASSERT_EQ(update.status_code, 200) << update.text;
    EXPECT_EQ(json::parse(update.text)["name"], "Updated review");

    auto remove = cpr::Delete(cpr::Url{fx.url("/api/loops/" + id)});
    ASSERT_EQ(remove.status_code, 200);
    EXPECT_TRUE(json::parse(remove.text)["ok"].get<bool>());
    EXPECT_EQ(cpr::Get(cpr::Url{fx.url("/api/loops/" + id)}).status_code, 404);
}

TEST(WebServerHttp, LoopValidationRejectsMissingModelAndUnregisteredWorkspace) {
    WebServerFixture fx;
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    json body{
        {"name", "Bad LOOP"},
        {"prompt", "Do work"},
        {"model_name", "missing-model"},
        {"permission_mode", "yolo"},
        {"schedule", {{"kind", "once"}, {"once_at_ms", now + 60000}}},
    };
    auto missing_model = cpr::Post(cpr::Url{fx.url("/api/loops")},
                                   cpr::Body{body.dump()},
                                   cpr::Header{{"Content-Type", "application/json"}});
    ASSERT_EQ(missing_model.status_code, 400);
    EXPECT_EQ(json::parse(missing_model.text)["error"], "INVALID_MODEL");

    body["model_name"] = "fixture-copilot";
    body["workspace_hash"] = "not-registered";
    body["workspace_cwd"] = fx.cwd;
    auto missing_workspace = cpr::Post(cpr::Url{fx.url("/api/loops")},
                                       cpr::Body{body.dump()},
                                       cpr::Header{{"Content-Type", "application/json"}});
    ASSERT_EQ(missing_workspace.status_code, 400);
    EXPECT_EQ(json::parse(missing_workspace.text)["error"], "INVALID_WORKSPACE");
}

TEST(WebServerHttp, LoopBadJsonReturns400WithoutThrowing) {
    WebServerFixture fx;
    auto create = cpr::Post(cpr::Url{fx.url("/api/loops")},
                            cpr::Body{"{not-json"},
                            cpr::Header{{"Content-Type", "application/json"}});
    ASSERT_EQ(create.status_code, 400);
    EXPECT_EQ(json::parse(create.text)["error"], "BAD_JSON");

    auto enable = cpr::Put(cpr::Url{fx.url("/api/loops/unknown/enabled")},
                           cpr::Body{"{not-json"},
                           cpr::Header{{"Content-Type", "application/json"}});
    ASSERT_EQ(enable.status_code, 400);
    EXPECT_EQ(json::parse(enable.text)["error"], "BAD_REQUEST");
}

void create_opencode_import_db(const std::filesystem::path& db_path,
                               const std::string& cwd) {
    std::filesystem::create_directories(db_path.parent_path());
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open_v2(acecode::path_to_utf8(db_path).c_str(),
                              &db,
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                              nullptr), SQLITE_OK);
    auto exec = [&](const char* sql) {
        char* raw_error = nullptr;
        int rc = sqlite3_exec(db, sql, nullptr, nullptr, &raw_error);
        std::string error = raw_error ? raw_error : "";
        sqlite3_free(raw_error);
        ASSERT_EQ(rc, SQLITE_OK) << error;
    };
    exec(
        "CREATE TABLE project (id TEXT PRIMARY KEY, worktree TEXT);"
        "CREATE TABLE session ("
        "id TEXT PRIMARY KEY, project_id TEXT, directory TEXT, title TEXT,"
        "time_created INTEGER, time_updated INTEGER, time_archived INTEGER, model TEXT"
        ");"
        "CREATE TABLE message ("
        "id TEXT PRIMARY KEY, session_id TEXT, time_created INTEGER, time_updated INTEGER, data TEXT"
        ");"
        "CREATE TABLE part ("
        "id TEXT PRIMARY KEY, message_id TEXT, session_id TEXT, time_created INTEGER, time_updated INTEGER, data TEXT"
        ");"
        "INSERT INTO project(id, worktree) VALUES('proj', '');");

    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "INSERT INTO session(id, project_id, directory, title, time_created, time_updated, time_archived, model) "
        "VALUES('ses-route', 'proj', ?, 'Route import', 1700000000000, 1700000010000, 0, "
        "'{\"providerID\":\"opencode\",\"modelID\":\"big-pickle\"}');",
        -1, &stmt, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(stmt, 1, cwd.c_str(), -1, SQLITE_TRANSIENT), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);
    stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db,
        "INSERT INTO session(id, project_id, directory, title, time_created, time_updated, time_archived, model) "
        "VALUES('ses-route-archived', 'proj', ?, 'Archived route import', 1700000000000, 1700000020000, 1700000030000, "
        "'{\"providerID\":\"opencode\",\"modelID\":\"big-pickle\"}');",
        -1, &stmt, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(stmt, 1, cwd.c_str(), -1, SQLITE_TRANSIENT), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_DONE);
    sqlite3_finalize(stmt);
    exec(
        "INSERT INTO message(id, session_id, time_created, time_updated, data) "
        "VALUES('msg-route', 'ses-route', 1700000000000, 1700000000000, '{\"role\":\"user\"}');"
        "INSERT INTO part(id, message_id, session_id, time_created, time_updated, data) "
        "VALUES('prt-route', 'msg-route', 'ses-route', 1700000000000, 1700000000000, "
        "'{\"type\":\"text\",\"text\":\"hello from opencode\"}');"
        "INSERT INTO message(id, session_id, time_created, time_updated, data) "
        "VALUES('msg-route-archived', 'ses-route-archived', 1700000000000, 1700000000000, '{\"role\":\"user\"}');"
        "INSERT INTO part(id, message_id, session_id, time_created, time_updated, data) "
        "VALUES('prt-route-archived', 'msg-route-archived', 'ses-route-archived', 1700000000000, 1700000000000, "
        "'{\"type\":\"text\",\"text\":\"hello from archived opencode\"}');");
    sqlite3_close(db);
}

TEST(WebServerHttp, HealthReportsCompletedTurnSelfHealFeatureWhenDisabled) {
    WebServerFixture fx;
    fx.cfg.features.completed_turn_self_heal = false;

    auto r = cpr::Get(cpr::Url{fx.url("/api/health")});
    ASSERT_EQ(r.status_code, 200) << r.text;
    auto j = json::parse(r.text);

    ASSERT_TRUE(j.contains("features"));
    ASSERT_TRUE(j["features"].contains("completed_turn_self_heal"));
    EXPECT_EQ(j["features"]["completed_turn_self_heal"]["enabled"], false);
}

TEST(WebServerHttp, UsageEndpointAggregatesLedgerRecords) {
    WebServerFixture fx;

    acecode::UsageLedgerRecord first;
    first.timestamp_ms = acecode::usage_now_unix_ms();
    first.timestamp = acecode::usage_iso8601_from_unix_ms(first.timestamp_ms);
    first.session_id = "usage-session-a";
    first.cwd = fx.cwd;
    first.provider = "openai";
    first.model = "gpt-4o";
    first.model_preset = "gpt-4o";
    first.surface = "web";
    first.usage.prompt_tokens = 1000;
    first.usage.completion_tokens = 250;
    first.usage.total_tokens = 1250;
    first.usage.cache_read_tokens = 300;
    first.usage.has_data = true;
    acecode::append_usage_ledger_record(fx.project_dir, first);

    acecode::UsageLedgerRecord second = first;
    second.session_id = "usage-session-b";
    second.usage.prompt_tokens = 500;
    second.usage.completion_tokens = 100;
    second.usage.total_tokens = 600;
    second.usage.cache_read_tokens = 0;
    second.usage.has_data = false;
    acecode::append_usage_ledger_record(fx.project_dir, second);

    auto r = cpr::Get(cpr::Url{fx.url("/api/usage?days=7")});
    ASSERT_EQ(r.status_code, 200) << r.text;
    auto j = json::parse(r.text);
    ASSERT_TRUE(j["summary"].is_object());
    EXPECT_EQ(j["summary"]["records"], 2);
    EXPECT_EQ(j["summary"]["estimated_records"], 1);
    EXPECT_EQ(j["summary"]["session_count"], 2);
    EXPECT_EQ(j["summary"]["totals"]["total_tokens"], 1850);
    ASSERT_FALSE(j["daily"].empty());
    ASSERT_FALSE(j["models"].empty());
    EXPECT_EQ(j["models"][0]["label"], "gpt-4o");
    ASSERT_FALSE(j["workspaces"].empty());
    EXPECT_EQ(j["metadata"]["days"], 7);
    EXPECT_EQ(j["metadata"]["forward_only"], true);
}

// 场景: 跨端口 Web/Desktop fetch 只接受 loopback Origin,且不能因为 remote_ip
// 是 loopback 就绕过 token。preflight 本身不带 token,实际请求必须带。
TEST(WebServerHttp, CorsCrossOriginLoopbackRequiresToken) {
    WebServerFixture fx;
    const std::string origin = "http://localhost:5173";

    auto no_token = cpr::Get(cpr::Url{fx.url("/api/sessions")},
                             cpr::Header{{"Origin", origin}});
    EXPECT_EQ(no_token.status_code, 401);
    EXPECT_EQ(response_header(no_token, "Access-Control-Allow-Origin"), origin);

    auto with_token = cpr::Get(cpr::Url{fx.url("/api/sessions")},
                               cpr::Header{{"Origin", origin},
                                           {"X-ACECode-Token", "smoke-token"}});
    EXPECT_EQ(with_token.status_code, 200);
    EXPECT_EQ(response_header(with_token, "Access-Control-Allow-Origin"), origin);

    auto bad_origin = cpr::Get(cpr::Url{fx.url("/api/sessions")},
                               cpr::Header{{"Origin", "http://example.com"},
                                           {"X-ACECode-Token", "smoke-token"}});
    EXPECT_EQ(bad_origin.status_code, 401);
    EXPECT_EQ(response_header(bad_origin, "Access-Control-Allow-Origin"), "");
}

// 场景:127.0.0.1 与 localhost 是同一个 loopback daemon 的常见混用方式。
// 同端口 alias 不应被当成跨 daemon fetch/WS,否则无 token 的本机 standalone
// 页面会被误拒。
TEST(WebServerHttp, LoopbackAliasSamePortDoesNotRequireToken) {
    WebServerFixture fx;
    const std::string origin = "http://127.0.0.1:" + std::to_string(fx.port);
    auto r = cpr::Get(cpr::Url{"http://localhost:" + std::to_string(fx.port) + "/api/sessions"},
                      cpr::Header{{"Origin", origin}});
    EXPECT_EQ(r.status_code, 200) << r.text;
    EXPECT_EQ(response_header(r, "Access-Control-Allow-Origin"), origin);
}

TEST(WebServerHttp, HooksEndpointListsTrustsDisablesEnablesAndRefreshesProjectHooks) {
    auto temp_home = std::filesystem::temp_directory_path() /
                     ("acecode_hooks_api_home_" + std::to_string(std::random_device{}()));
    RemoveTreeOnExit cleanup{temp_home};
    ScopedHomeOverride home(temp_home);
    WebServerFixture fx;

    auto empty = cpr::Get(cpr::Url{fx.url("/api/hooks")});
    ASSERT_EQ(empty.status_code, 200) << empty.text;
    EXPECT_TRUE(json::parse(empty.text)["hooks"].empty());

    auto preflight = cpr::Options(
        cpr::Url{fx.url("/api/hooks")},
        cpr::Header{{"Origin", "http://localhost:5173"},
                    {"Access-Control-Request-Method", "GET"}});
    EXPECT_EQ(preflight.status_code, 204);

    write_text(fx.cwd_dir / ".codex" / "hooks.json", R"({
        "hooks": {
            "PreToolUse": [
                {
                    "matcher": "Bash",
                    "hooks": [
                        {
                            "type": "command",
                            "command": "echo hook",
                            "timeout": 5,
                            "statusMessage": "Checking tool"
                        }
                    ]
                }
            ]
        }
    })");

    auto refresh = cpr::Post(cpr::Url{fx.url("/api/hooks/refresh")});
    ASSERT_EQ(refresh.status_code, 200) << refresh.text;
    auto body = json::parse(refresh.text);
    ASSERT_EQ(body["hooks"].size(), 1u);
    const std::string hook_id = body["hooks"][0]["id"].get<std::string>();
    const std::string encoded_hook_id = url_encode_component(hook_id);
    EXPECT_EQ(body["hooks"][0]["event_name"], "PreToolUse");
    EXPECT_EQ(body["hooks"][0]["matcher"], "Bash");
    EXPECT_EQ(body["hooks"][0]["command"], "echo hook");
    EXPECT_EQ(body["hooks"][0]["trust_status"], "pending_review");
    EXPECT_EQ(body["hooks"][0]["pending_review"], true);

    auto trust = cpr::Post(cpr::Url{fx.url("/api/hooks/" + encoded_hook_id + "/trust")});
    ASSERT_EQ(trust.status_code, 200) << trust.text;
    body = json::parse(trust.text);
    ASSERT_EQ(body["hooks"].size(), 1u);
    EXPECT_EQ(body["hooks"][0]["trust_status"], "trusted");
    EXPECT_EQ(body["hooks"][0]["trusted"], true);
    EXPECT_TRUE(std::filesystem::is_regular_file(temp_home / ".acecode" / "hooks_state.json"));

    auto disable = cpr::Post(cpr::Url{fx.url("/api/hooks/" + encoded_hook_id + "/disable")});
    ASSERT_EQ(disable.status_code, 200) << disable.text;
    body = json::parse(disable.text);
    ASSERT_EQ(body["hooks"].size(), 1u);
    EXPECT_EQ(body["hooks"][0]["trust_status"], "disabled");
    EXPECT_EQ(body["hooks"][0]["disabled"], true);

    auto enable = cpr::Post(cpr::Url{fx.url("/api/hooks/" + encoded_hook_id + "/enable")});
    ASSERT_EQ(enable.status_code, 200) << enable.text;
    body = json::parse(enable.text);
    ASSERT_EQ(body["hooks"].size(), 1u);
    EXPECT_EQ(body["hooks"][0]["trust_status"], "trusted");
    EXPECT_EQ(body["hooks"][0]["disabled"], false);

    auto reloaded = cpr::Post(cpr::Url{fx.url("/api/hooks/refresh")});
    ASSERT_EQ(reloaded.status_code, 200) << reloaded.text;
    body = json::parse(reloaded.text);
    ASSERT_EQ(body["hooks"].size(), 1u);
    EXPECT_EQ(body["hooks"][0]["trust_status"], "trusted");

    auto unknown = cpr::Post(cpr::Url{fx.url("/api/hooks/missing-hook/trust")});
    ASSERT_EQ(unknown.status_code, 404) << unknown.text;
    EXPECT_EQ(json::parse(unknown.text)["error"], "HOOK_NOT_FOUND");
}

TEST(WebServerHttp, HooksEndpointTogglesLegacyConfigEnabledFlag) {
    auto temp_home = std::filesystem::temp_directory_path() /
                     ("acecode_hooks_legacy_home_" + std::to_string(std::random_device{}()));
    RemoveTreeOnExit cleanup{temp_home};
    ScopedHomeOverride home(temp_home);

    const auto hooks_path = temp_home / ".acecode" / "hooks.json";
    write_text(hooks_path, R"({
        "version": 1,
        "enabled": true,
        "events": {
            "startup.before_model_load": [
                {"id": "legacy-startup", "command": "node", "args": ["hook.js"]}
            ]
        }
    })");

    WebServerFixture fx;

    auto refresh = cpr::Post(cpr::Url{fx.url("/api/hooks/refresh")});
    ASSERT_EQ(refresh.status_code, 200) << refresh.text;
    auto body = json::parse(refresh.text);
    ASSERT_EQ(body["hooks"].size(), 1u);
    const std::string hook_id = body["hooks"][0]["id"].get<std::string>();
    const std::string encoded_hook_id = url_encode_component(hook_id);
    EXPECT_EQ(body["hooks"][0]["legacy_direct"], true);
    EXPECT_EQ(body["hooks"][0]["trust_status"], "trusted");

    auto disable = cpr::Post(cpr::Url{fx.url("/api/hooks/" + encoded_hook_id + "/disable")});
    ASSERT_EQ(disable.status_code, 200) << disable.text;
    body = json::parse(disable.text);
    ASSERT_EQ(body["hooks"].size(), 1u);
    EXPECT_EQ(body["hooks"][0]["trust_status"], "disabled");
    {
        std::ifstream ifs(hooks_path, std::ios::binary);
        auto disk = json::parse(ifs);
        EXPECT_FALSE(disk["enabled"].get<bool>());
    }

    auto enable = cpr::Post(cpr::Url{fx.url("/api/hooks/" + encoded_hook_id + "/enable")});
    ASSERT_EQ(enable.status_code, 200) << enable.text;
    body = json::parse(enable.text);
    ASSERT_EQ(body["hooks"].size(), 1u);
    EXPECT_EQ(body["hooks"][0]["trust_status"], "trusted");
    {
        std::ifstream ifs(hooks_path, std::ios::binary);
        auto disk = json::parse(ifs);
        EXPECT_TRUE(disk["enabled"].get<bool>());
    }
}

TEST(WebServerHttp, HooksEndpointRejectsManagedHookDisable) {
    auto temp_home = std::filesystem::temp_directory_path() /
                     ("acecode_hooks_managed_home_" + std::to_string(std::random_device{}()));
    RemoveTreeOnExit cleanup{temp_home};
    ScopedHomeOverride home(temp_home);
    WebServerFixture fx;

    acecode::HookSource source;
    source.scope = acecode::HookSourceScope::Managed;
    source.format = acecode::HookSourceFormat::CodexJson;
    source.path = "managed-hooks";
    source.id = acecode::make_hook_source_id(source.scope, source.format, source.path);
    source.managed = true;

    auto snapshot = acecode::parse_codex_hooks_json_source(nlohmann::json::parse(R"({
        "hooks": {
            "SessionStart": [
                {"hooks": [{"type": "command", "command": "echo managed"}]}
            ]
        }
    })"), source);
    acecode::HookTrustStore store;
    acecode::apply_hook_trust_state(snapshot, store, true);
    ASSERT_EQ(snapshot.hooks.size(), 1u);
    const std::string hook_id = snapshot.hooks[0].id;
    fx.hook_manager->refresh_registry(std::move(snapshot));

    auto r = cpr::Post(cpr::Url{fx.url("/api/hooks/" +
                                       url_encode_component(hook_id) +
                                       "/disable")});
    ASSERT_EQ(r.status_code, 409) << r.text;
    EXPECT_EQ(json::parse(r.text)["error"], "HOOK_MANAGED");
}

// 场景: POST /api/sessions 返回 201 + {session_id};立刻 GET /api/sessions
// 应包含这一条 active=true(spec 9.3 + 9.4)。
TEST(WebServerHttp, CreateSessionThenListShowsActive) {
    WebServerFixture fx;
    auto post = cpr::Post(cpr::Url{fx.url("/api/sessions")},
                          cpr::Header{{"Content-Type", "application/json"}},
                          cpr::Body{R"({})"});
    ASSERT_EQ(post.status_code, 201) << post.text;
    auto created = json::parse(post.text);
    auto sid = created["session_id"].get<std::string>();
    EXPECT_FALSE(sid.empty());
    EXPECT_EQ(created["id"], sid);

    auto* entry = fx.registry->lookup(sid);
    ASSERT_NE(entry, nullptr);
    acecode::ChatMessage msg;
    msg.role = "user";
    msg.content = "hello from web";
    entry->sm->on_message(msg);
    entry->sm->finalize();

    auto list = cpr::Get(cpr::Url{fx.url("/api/sessions")});
    ASSERT_EQ(list.status_code, 200);
    auto arr = json::parse(list.text);
    bool found = false;
    int occurrences = 0;
    for (const auto& s : arr) {
        if (s["id"] == sid) {
            occurrences++;
            EXPECT_TRUE(s["active"].get<bool>());
            EXPECT_EQ(s["message_count"], 1);
            EXPECT_EQ(s["attention_state"], "read");
            EXPECT_EQ(s["read_state"], "read");
            EXPECT_TRUE(s.contains("status_cursor"));
            EXPECT_TRUE(s.contains("read_cursor"));
            EXPECT_TRUE(s.contains("update_cursor"));
            found = true;
        }
    }
    EXPECT_TRUE(found) << "新建的 session 必须出现在列表中";
    EXPECT_EQ(occurrences, 1) << "active 与 disk meta 必须合并为同一条";
}

TEST(WebServerHttp, ExportSessionMarkdownWritesVisibleTranscriptToPickedFolder) {
    const auto export_dir = std::filesystem::temp_directory_path() /
                            ("acecode_session_export_success_" + std::to_string(std::random_device{}()));
    RemoveTreeOnExit cleanup{export_dir};
    std::filesystem::create_directories(export_dir);
    WebServerFixture fx(
        true,
        true,
        [export_dir] { return std::optional<std::string>(export_dir.string()); });

    auto post = cpr::Post(cpr::Url{fx.url("/api/sessions")},
                          cpr::Header{{"Content-Type", "application/json"}},
                          cpr::Body{R"({})"});
    ASSERT_EQ(post.status_code, 201) << post.text;
    const auto created = json::parse(post.text);
    const auto sid = created["session_id"].get<std::string>();
    auto* entry = fx.registry->lookup(sid);
    ASSERT_NE(entry, nullptr);
    ASSERT_NE(entry->sm, nullptr);

    acecode::ChatMessage user;
    user.role = "user";
    user.content = "请导出这段会话";
    user.timestamp = "2026-07-12T10:00:00Z";
    entry->sm->on_message(user);
    acecode::ChatMessage assistant;
    assistant.role = "assistant";
    assistant.content = "已完成导出准备。";
    assistant.timestamp = "2026-07-12T10:00:01Z";
    entry->sm->on_message(assistant);
    entry->sm->set_session_title("导出成功测试");

    auto exported = cpr::Post(
        cpr::Url{fx.url("/api/sessions/" + url_encode_component(sid) + "/export-markdown")},
        cpr::Header{{"Content-Type", "application/json"}},
        cpr::Body{json{{"workspace_hash", created.value("workspace_hash", "")}}.dump()});
    ASSERT_EQ(exported.status_code, 200) << exported.text;
    const auto result = json::parse(exported.text);
    EXPECT_TRUE(result["ok"].get<bool>());
    EXPECT_FALSE(result["cancelled"].get<bool>());

    std::vector<std::filesystem::path> files;
    for (const auto& item : std::filesystem::directory_iterator(export_dir)) {
        if (item.is_regular_file()) files.push_back(item.path());
    }
    ASSERT_EQ(files.size(), 1u);
    std::ifstream input(files.front(), std::ios::binary);
    const std::string markdown((std::istreambuf_iterator<char>(input)), {});
    EXPECT_NE(markdown.find("# 导出成功测试"), std::string::npos);
    EXPECT_NE(markdown.find("请导出这段会话"), std::string::npos);
    EXPECT_NE(markdown.find("已完成导出准备。"), std::string::npos);
}

TEST(WebServerHttp, ExportSessionMarkdownCancellationDoesNotCreateFile) {
    const auto export_dir = std::filesystem::temp_directory_path() /
                            ("acecode_session_export_cancel_" + std::to_string(std::random_device{}()));
    RemoveTreeOnExit cleanup{export_dir};
    std::filesystem::create_directories(export_dir);
    WebServerFixture fx(true, true, [] { return std::optional<std::string>{}; });

    auto post = cpr::Post(cpr::Url{fx.url("/api/sessions")},
                          cpr::Header{{"Content-Type", "application/json"}},
                          cpr::Body{R"({})"});
    ASSERT_EQ(post.status_code, 201) << post.text;
    const auto sid = json::parse(post.text)["session_id"].get<std::string>();
    auto exported = cpr::Post(
        cpr::Url{fx.url("/api/sessions/" + url_encode_component(sid) + "/export-markdown")});
    ASSERT_EQ(exported.status_code, 200) << exported.text;
    EXPECT_TRUE(json::parse(exported.text)["cancelled"].get<bool>());
    std::size_t file_count = 0;
    for (const auto& item : std::filesystem::directory_iterator(export_dir)) {
        if (item.is_regular_file()) ++file_count;
    }
    EXPECT_EQ(file_count, 0u);
}

TEST(WebServerHttp, ExportSessionMarkdownReportsPickerUnavailableAndWriteFailure) {
    WebServerFixture unavailable;
    auto post = cpr::Post(cpr::Url{unavailable.url("/api/sessions")},
                          cpr::Header{{"Content-Type", "application/json"}},
                          cpr::Body{R"({})"});
    ASSERT_EQ(post.status_code, 201) << post.text;
    const auto sid = json::parse(post.text)["session_id"].get<std::string>();
    auto unavailable_result = cpr::Post(
        cpr::Url{unavailable.url("/api/sessions/" + url_encode_component(sid) + "/export-markdown")});
    EXPECT_EQ(unavailable_result.status_code, 501);
    EXPECT_EQ(json::parse(unavailable_result.text)["error"], "native folder picker unavailable");

    const auto not_folder = std::filesystem::temp_directory_path() /
                            ("acecode_session_export_not_folder_" + std::to_string(std::random_device{}()));
    RemoveTreeOnExit cleanup{not_folder};
    std::ofstream(not_folder) << "not a folder";
    WebServerFixture write_failure(
        true,
        true,
        [not_folder] { return std::optional<std::string>(not_folder.string()); });
    auto write_post = cpr::Post(cpr::Url{write_failure.url("/api/sessions")},
                                cpr::Header{{"Content-Type", "application/json"}},
                                cpr::Body{R"({})"});
    ASSERT_EQ(write_post.status_code, 201) << write_post.text;
    const auto write_sid = json::parse(write_post.text)["session_id"].get<std::string>();
    auto write_result = cpr::Post(
        cpr::Url{write_failure.url("/api/sessions/" + url_encode_component(write_sid) + "/export-markdown")});
    EXPECT_EQ(write_result.status_code, 400);
    EXPECT_EQ(json::parse(write_result.text)["error"], "destination folder unavailable");
}

// 场景:active session 走 SessionInfo 序列化,也必须保留持久化 meta 中的
// worktree 状态,否则侧栏刷新后当前会话会丢失 worktree 标识。
TEST(WebServerHttp, ActiveWorktreeSessionListIncludesWorktreeState) {
    WebServerFixture fx;
    auto post = cpr::Post(cpr::Url{fx.url("/api/sessions")},
                          cpr::Header{{"Content-Type", "application/json"}},
                          cpr::Body{R"({})"});
    ASSERT_EQ(post.status_code, 201) << post.text;
    const auto sid = json::parse(post.text)["session_id"].get<std::string>();

    auto* entry = fx.registry->lookup(sid);
    ASSERT_NE(entry, nullptr);
    ASSERT_NE(entry->sm, nullptr);
    EXPECT_EQ(entry->sm->ensure_active_session_id(), sid);

    acecode::WorktreeSessionInfo worktree;
    worktree.original_cwd = fx.cwd;
    worktree.worktree_path = fx.cwd + "/.acecode/worktrees/ses-active-list";
    worktree.worktree_name = "ses-active-list";
    worktree.worktree_branch = "worktree-ses-active-list";
    entry->sm->set_active_worktree(worktree);

    auto list = cpr::Get(cpr::Url{fx.url("/api/sessions")});
    ASSERT_EQ(list.status_code, 200) << list.text;
    const auto sessions = json::parse(list.text);
    const auto found = std::find_if(sessions.begin(), sessions.end(), [&](const auto& item) {
        return item.value("id", std::string{}) == sid;
    });
    ASSERT_NE(found, sessions.end());
    EXPECT_TRUE((*found)["active"].get<bool>());
    ASSERT_TRUE(found->contains("worktree"));
    EXPECT_EQ((*found)["worktree"]["name"], "ses-active-list");
    EXPECT_EQ((*found)["worktree"]["branch"], "worktree-ses-active-list");
}

TEST(WebServerHttp, LoopSessionListIncludesOriginWhileActiveAndAfterDestroy) {
    WebServerFixture fx;
    acecode::SessionOptions options;
    options.preset_session_id = "loop-session-list";
    options.loop_execution = true;
    options.loop_id = "loop-1";
    options.loop_run_id = "run-1";
    const auto sid = fx.registry->create(options);
    ASSERT_EQ(sid, "loop-session-list");

    auto find_session = [&](const json& sessions) {
        return std::find_if(sessions.begin(), sessions.end(), [&](const auto& item) {
            return item.value("id", std::string{}) == sid;
        });
    };

    auto list = cpr::Get(cpr::Url{fx.url("/api/sessions")});
    ASSERT_EQ(list.status_code, 200) << list.text;
    auto sessions = json::parse(list.text);
    auto found = find_session(sessions);
    ASSERT_NE(found, sessions.end());
    EXPECT_TRUE((*found)["active"].get<bool>());
    ASSERT_TRUE(found->contains("loop_execution"));
    EXPECT_EQ((*found)["loop_execution"]["loop_id"], "loop-1");
    EXPECT_EQ((*found)["loop_execution"]["run_id"], "run-1");

    auto entry = fx.registry->acquire(sid);
    ASSERT_NE(entry, nullptr);
    ASSERT_NE(entry->sm, nullptr);
    EXPECT_EQ(entry->sm->ensure_active_session_id(), sid);
    entry.reset();
    fx.registry->destroy(sid);

    list = cpr::Get(cpr::Url{fx.url("/api/sessions")});
    ASSERT_EQ(list.status_code, 200) << list.text;
    sessions = json::parse(list.text);
    found = find_session(sessions);
    ASSERT_NE(found, sessions.end());
    EXPECT_FALSE((*found)["active"].get<bool>());
    ASSERT_TRUE(found->contains("loop_execution"));
    EXPECT_EQ((*found)["loop_execution"]["loop_id"], "loop-1");
    EXPECT_EQ((*found)["loop_execution"]["run_id"], "run-1");
}

TEST(WebServerHttp, SessionUserMessageSearchFindsTextAndAttachmentNames) {
    WebServerFixture fx;

    const std::string sid = "20260708-000000-0abc";
    acecode::ChatMessage user;
    user.role = "user";
    user.content = "请帮我设计 sqlite索引 的方案";
    user.uuid = "u1";
    acecode::ChatMessage hidden;
    hidden.role = "user";
    hidden.content = "quarterly-secret";
    hidden.metadata = json{{"hidden_goal_context", true}};
    acecode::ChatMessage attachment;
    attachment.role = "user";
    attachment.content = "";
    attachment.uuid = "u2";
    attachment.content_parts = json::array({
        json{{"type", "file"},
             {"attachment", json{{"id", "att1"},
                                  {"name", "report-final.pdf"},
                                  {"path", "C:/private/report-final.pdf"}}}},
    });
    acecode::SessionStorage::write_messages(
        acecode::SessionStorage::session_path(fx.project_dir, sid),
        {user, hidden, attachment});

    acecode::SessionMeta meta;
    meta.id = sid;
    meta.cwd = fx.cwd;
    meta.created_at = "2026-07-08T00:00:00Z";
    meta.updated_at = "2026-07-08T00:01:00Z";
    meta.message_count = 3;
    meta.turn_count = 2;
    meta.title = "unrelated title";
    meta.summary = "unrelated summary";
    acecode::SessionStorage::write_meta(
        acecode::SessionStorage::meta_path(fx.project_dir, sid), meta);

    auto text = cpr::Get(
        cpr::Url{fx.url("/api/session-search/user-messages")},
        cpr::Parameters{{"q", "sqlite索引"}});
    ASSERT_EQ(text.status_code, 200) << text.text;
    auto text_body = json::parse(text.text);
    ASSERT_TRUE(text_body["matches"].is_array());
    ASSERT_EQ(text_body["matches"].size(), 1u);
    EXPECT_EQ(text_body["matches"][0]["id"], sid);
    EXPECT_EQ(text_body["matches"][0]["workspace_hash"], acecode::compute_cwd_hash(fx.cwd));
    EXPECT_TRUE(text_body["matches"][0].contains("search_match"));
    EXPECT_NE(text_body["matches"][0]["search_match"]["snippet"].get<std::string>().find("sqlite索引"),
              std::string::npos);
    EXPECT_FALSE(text_body["matches"][0].contains("messages"));

    auto file = cpr::Get(
        cpr::Url{fx.url("/api/session-search/user-messages")},
        cpr::Parameters{{"q", "report-final"}});
    ASSERT_EQ(file.status_code, 200) << file.text;
    auto file_body = json::parse(file.text);
    ASSERT_EQ(file_body["matches"].size(), 1u);
    ASSERT_TRUE(file_body["matches"][0]["search_match"]["attachments"].is_array());
    ASSERT_EQ(file_body["matches"][0]["search_match"]["attachments"].size(), 1u);
    EXPECT_EQ(file_body["matches"][0]["search_match"]["attachments"][0], "report-final.pdf");
    EXPECT_EQ(file_body["matches"][0]["search_match"]["snippet"].get<std::string>().find("C:/private"),
              std::string::npos);

    auto hidden_result = cpr::Get(
        cpr::Url{fx.url("/api/session-search/user-messages")},
        cpr::Parameters{{"q", "quarterly-secret"}});
    ASSERT_EQ(hidden_result.status_code, 200) << hidden_result.text;
    EXPECT_TRUE(json::parse(hidden_result.text)["matches"].empty());
}

TEST(WebServerHttp, SessionUserMessageSearchReturnsNoMatchesForEmptyQuery) {
    WebServerFixture fx;
    auto empty = cpr::Get(
        cpr::Url{fx.url("/api/session-search/user-messages")},
        cpr::Parameters{{"q", "   "}});
    ASSERT_EQ(empty.status_code, 200) << empty.text;
    auto body = json::parse(empty.text);
    ASSERT_TRUE(body["matches"].is_array());
    EXPECT_TRUE(body["matches"].empty());
}

// 回归:Crow 的 url_params.get() 返回值已做过一次 URL decode('+'→空格、
// %xx 解码)。修复前路由里又手写 decode 了一遍,查询里的字面 '+' 被二次
// 解码成空格("C++"→"C  ")、'%' 序列被错误展开,内容明明存在却搜不到。
TEST(WebServerHttp, SessionUserMessageSearchKeepsLiteralPlusAndPercent) {
    WebServerFixture fx;

    const std::string sid = "20260708-000001-0abd";
    acecode::ChatMessage user;
    user.role = "user";
    user.content = "帮我优化 C++ 模板,目标覆盖率 95%";
    user.uuid = "u1";
    acecode::SessionStorage::write_messages(
        acecode::SessionStorage::session_path(fx.project_dir, sid), {user});

    acecode::SessionMeta meta;
    meta.id = sid;
    meta.cwd = fx.cwd;
    meta.created_at = "2026-07-08T00:00:00Z";
    meta.updated_at = "2026-07-08T00:01:00Z";
    meta.message_count = 1;
    meta.turn_count = 1;
    meta.title = "unrelated title";
    acecode::SessionStorage::write_meta(
        acecode::SessionStorage::meta_path(fx.project_dir, sid), meta);

    // cpr 会把 "C++" 编码为 q=C%2B%2B,服务端只应 decode 一次。
    auto plus = cpr::Get(
        cpr::Url{fx.url("/api/session-search/user-messages")},
        cpr::Parameters{{"q", "C++"}});
    ASSERT_EQ(plus.status_code, 200) << plus.text;
    ASSERT_EQ(json::parse(plus.text)["matches"].size(), 1u)
        << "字面 '+' 被二次解码成空格,导致查询不命中";

    auto percent = cpr::Get(
        cpr::Url{fx.url("/api/session-search/user-messages")},
        cpr::Parameters{{"q", "95%"}});
    ASSERT_EQ(percent.status_code, 200) << percent.text;
    ASSERT_EQ(json::parse(percent.text)["matches"].size(), 1u)
        << "字面 '%' 被二次解码,导致查询不命中";
}

// 场景: Web 状态栏/设置页切换权限模式走真实 daemon API,必须立即更新
// 当前 active session 的 PermissionManager,否则 Yolo/Plan 会停留在旧状态。
TEST(WebServerHttp, SessionPermissionModeEndpointUpdatesActiveSession) {
    WebServerFixture fx;
    auto post = cpr::Post(cpr::Url{fx.url("/api/sessions")},
                          cpr::Header{{"Content-Type", "application/json"}},
                          cpr::Body{R"({})"});
    ASSERT_EQ(post.status_code, 201) << post.text;
    auto sid = json::parse(post.text)["session_id"].get<std::string>();

    auto get_initial = cpr::Get(cpr::Url{fx.url("/api/sessions/" + sid + "/permissions")});
    ASSERT_EQ(get_initial.status_code, 200) << get_initial.text;
    EXPECT_EQ(json::parse(get_initial.text)["mode"], "default");

    auto put = cpr::Put(cpr::Url{fx.url("/api/sessions/" + sid + "/permissions")},
                        cpr::Header{{"Content-Type", "application/json"}},
                        cpr::Body{R"({"mode":"yolo"})"});
    ASSERT_EQ(put.status_code, 200) << put.text;
    EXPECT_EQ(json::parse(put.text)["mode"], "yolo");

    auto* entry = fx.registry->lookup(sid);
    ASSERT_NE(entry, nullptr);
    ASSERT_NE(entry->perm, nullptr);
    EXPECT_EQ(entry->perm->mode(), acecode::PermissionMode::Yolo);
    EXPECT_TRUE(entry->perm->should_auto_allow("bash", false));

    auto meta = acecode::SessionStorage::read_meta(
        acecode::SessionStorage::meta_path(fx.project_dir, sid));
    EXPECT_EQ(meta.permission_mode, "yolo");

    auto put_plan = cpr::Put(cpr::Url{fx.url("/api/sessions/" + sid + "/permissions")},
                             cpr::Header{{"Content-Type", "application/json"}},
                             cpr::Body{R"({"mode":"plan"})"});
    ASSERT_EQ(put_plan.status_code, 200) << put_plan.text;
    EXPECT_EQ(json::parse(put_plan.text)["mode"], "plan");
    EXPECT_EQ(entry->perm->mode(), acecode::PermissionMode::Plan);
    EXPECT_EQ(entry->perm->pre_plan_mode(), acecode::PermissionMode::Yolo);

    meta = acecode::SessionStorage::read_meta(
        acecode::SessionStorage::meta_path(fx.project_dir, sid));
    EXPECT_EQ(meta.permission_mode, "plan");
    EXPECT_EQ(meta.pre_plan_permission_mode, "yolo");
    EXPECT_TRUE(std::filesystem::exists(entry->sm->current_plan_file_path()));
}

// 场景: 权限模式端点应拒绝未知 mode,避免前端 typo 把 daemon 状态改坏。
TEST(WebServerHttp, SessionPermissionModeRejectsInvalidMode) {
    WebServerFixture fx;
    auto post = cpr::Post(cpr::Url{fx.url("/api/sessions")},
                          cpr::Header{{"Content-Type", "application/json"}},
                          cpr::Body{R"({})"});
    ASSERT_EQ(post.status_code, 201) << post.text;
    auto sid = json::parse(post.text)["session_id"].get<std::string>();

    auto put = cpr::Put(cpr::Url{fx.url("/api/sessions/" + sid + "/permissions")},
                        cpr::Header{{"Content-Type", "application/json"}},
                        cpr::Body{R"({"mode":"always"})"});
    EXPECT_EQ(put.status_code, 400) << put.text;
    auto mode = fx.registry->permission_mode(sid);
    ASSERT_TRUE(mode.has_value());
    EXPECT_EQ(*mode, acecode::PermissionMode::Default);
}

// 场景:管理界面的默认权限模式必须持久化到配置,同步 daemon 新会话模板,
// 并让后续新建 session 继承该模式。
TEST(WebServerHttp, DefaultPermissionModeEndpointAppliesToNewSessions) {
    WebServerFixture fx;

    auto initial = cpr::Get(cpr::Url{fx.url("/api/config/default-permission-mode")});
    ASSERT_EQ(initial.status_code, 200) << initial.text;
    EXPECT_EQ(json::parse(initial.text)["mode"], "default");

    auto put = cpr::Put(cpr::Url{fx.url("/api/config/default-permission-mode")},
                        cpr::Header{{"Content-Type", "application/json"}},
                        cpr::Body{R"({"mode":"accept-edits"})"});
    ASSERT_EQ(put.status_code, 200) << put.text;
    EXPECT_EQ(json::parse(put.text)["mode"], "accept-edits");
    EXPECT_EQ(fx.cfg.default_permission_mode, "accept-edits");
    EXPECT_EQ(fx.registry->default_permission_mode(), acecode::PermissionMode::AcceptEdits);

    auto created = cpr::Post(cpr::Url{fx.url("/api/sessions")},
                             cpr::Header{{"Content-Type", "application/json"}},
                             cpr::Body{R"({})"});
    ASSERT_EQ(created.status_code, 201) << created.text;
    auto sid = json::parse(created.text)["session_id"].get<std::string>();
    auto mode = fx.registry->permission_mode(sid);
    ASSERT_TRUE(mode.has_value());
    EXPECT_EQ(*mode, acecode::PermissionMode::AcceptEdits);
    auto* entry = fx.registry->lookup(sid);
    ASSERT_NE(entry, nullptr);
    ASSERT_NE(entry->sm, nullptr);
    EXPECT_EQ(entry->sm->current_permission_mode(), "accept-edits");

    acecode::ChatMessage msg;
    msg.role = "user";
    msg.content = "persist permission mode";
    entry->sm->on_message(msg);

    auto meta = acecode::SessionStorage::read_meta(
        acecode::SessionStorage::meta_path(fx.project_dir, sid));
    EXPECT_EQ(meta.permission_mode, "accept-edits");

    std::ifstream ifs(fx.tmp_dir / "config.json");
    ASSERT_TRUE(ifs.is_open());
    auto saved = json::parse(ifs);
    EXPECT_EQ(saved["default_permission_mode"], "accept-edits");
}

// 场景:Desktop/Web 首页状态栏选择的权限模式会作为 create-session 显式输入,
// 覆盖全局默认值并初始化新 session 的 PermissionManager/metadata。
TEST(WebServerHttp, CreateSessionAcceptsExplicitPermissionMode) {
    WebServerFixture fx;

    auto put_default = cpr::Put(cpr::Url{fx.url("/api/config/default-permission-mode")},
                                cpr::Header{{"Content-Type", "application/json"}},
                                cpr::Body{R"({"mode":"yolo"})"});
    ASSERT_EQ(put_default.status_code, 200) << put_default.text;

    auto created = cpr::Post(cpr::Url{fx.url("/api/sessions")},
                             cpr::Header{{"Content-Type", "application/json"}},
                             cpr::Body{R"({"permission_mode":"plan"})"});
    ASSERT_EQ(created.status_code, 201) << created.text;
    auto sid = json::parse(created.text)["session_id"].get<std::string>();

    auto mode = fx.registry->permission_mode(sid);
    ASSERT_TRUE(mode.has_value());
    EXPECT_EQ(*mode, acecode::PermissionMode::Plan);
    EXPECT_EQ(fx.registry->default_permission_mode(), acecode::PermissionMode::Yolo);
    auto* entry = fx.registry->lookup(sid);
    ASSERT_NE(entry, nullptr);
    ASSERT_NE(entry->perm, nullptr);
    ASSERT_NE(entry->sm, nullptr);
    EXPECT_EQ(entry->perm->pre_plan_mode(), acecode::PermissionMode::Default);
    EXPECT_EQ(entry->sm->current_permission_mode(), "plan");
    EXPECT_EQ(entry->sm->current_pre_plan_permission_mode(), "default");

    acecode::ChatMessage msg;
    msg.role = "user";
    msg.content = "persist explicit permission mode";
    entry->sm->on_message(msg);

    auto meta = acecode::SessionStorage::read_meta(
        acecode::SessionStorage::meta_path(fx.project_dir, sid));
    EXPECT_EQ(meta.permission_mode, "plan");
    EXPECT_EQ(meta.pre_plan_permission_mode, "default");
}

// 场景:create-session 请求体中的权限模式 typo 不能静默退回 default。
TEST(WebServerHttp, CreateSessionRejectsInvalidExplicitPermissionMode) {
    WebServerFixture fx;

    auto created = cpr::Post(cpr::Url{fx.url("/api/sessions")},
                             cpr::Header{{"Content-Type", "application/json"}},
                             cpr::Body{R"({"permission_mode":"always"})"});
    EXPECT_EQ(created.status_code, 400) << created.text;
    auto body = json::parse(created.text);
    EXPECT_EQ(body["error"], "INVALID_PERMISSION_MODE");
}

// 场景:首页选择"不使用工作区"创建的 session 要出现在兼容会话列表,
// 但不能出现在任何 workspace-scoped 会话列表里。
TEST(WebServerHttp, CreateNoWorkspaceSessionIsListedOutsideWorkspaces) {
    WebServerFixture fx;
    const std::string default_hash = acecode::compute_cwd_hash(fx.cwd);

    auto created = cpr::Post(cpr::Url{fx.url("/api/sessions")},
                             cpr::Header{{"Content-Type", "application/json"}},
                             cpr::Body{R"({"no_workspace":true})"});
    ASSERT_EQ(created.status_code, 201) << created.text;
    auto body = json::parse(created.text);
    const auto sid = body["session_id"].get<std::string>();
    EXPECT_EQ(body["workspace_hash"], "");
    EXPECT_EQ(body["cwd"], "");
    EXPECT_EQ(body["no_workspace"], true);

    auto* entry = fx.registry->lookup(sid);
    ASSERT_NE(entry, nullptr);
    const std::string expected_cwd =
        acecode::no_workspace_session_cwd(sid, fx.no_workspace_cache_root.string());
    EXPECT_TRUE(entry->no_workspace);
    EXPECT_TRUE(entry->workspace_hash.empty());
    EXPECT_EQ(entry->cwd, expected_cwd);
    EXPECT_TRUE(std::filesystem::is_directory(expected_cwd));
    ASSERT_NE(entry->sm, nullptr);
    EXPECT_EQ(entry->sm->ensure_active_session_id(), sid);
    const auto no_workspace_project_dir =
        acecode::SessionStorage::get_project_dir(expected_cwd);
    auto meta = acecode::SessionStorage::read_meta(
        acecode::SessionStorage::meta_path(no_workspace_project_dir, sid));
    EXPECT_TRUE(meta.no_workspace);
    EXPECT_EQ(meta.cwd, expected_cwd);
    EXPECT_TRUE(acecode::SessionStorage::list_sessions(fx.project_dir).empty());

    auto compat_list = cpr::Get(cpr::Url{fx.url("/api/sessions")});
    ASSERT_EQ(compat_list.status_code, 200) << compat_list.text;
    auto compat = json::parse(compat_list.text);
    auto compat_it = std::find_if(compat.begin(), compat.end(), [&](const auto& item) {
        return item.value("id", std::string{}) == sid;
    });
    ASSERT_NE(compat_it, compat.end());
    EXPECT_EQ((*compat_it)["workspace_hash"], "");
    EXPECT_EQ((*compat_it)["cwd"], "");
    EXPECT_EQ((*compat_it)["no_workspace"], true);

    auto workspace_list = cpr::Get(cpr::Url{fx.url("/api/workspaces/" + default_hash + "/sessions")});
    ASSERT_EQ(workspace_list.status_code, 200) << workspace_list.text;
    auto scoped = json::parse(workspace_list.text);
    EXPECT_TRUE(std::none_of(scoped.begin(), scoped.end(), [&](const auto& item) {
        return item.value("id", std::string{}) == sid;
    }));

    fx.client->destroy_session(sid);
    auto inactive_list = cpr::Get(cpr::Url{fx.url("/api/sessions")});
    ASSERT_EQ(inactive_list.status_code, 200) << inactive_list.text;
    auto inactive = json::parse(inactive_list.text);
    auto inactive_it = std::find_if(inactive.begin(), inactive.end(), [&](const auto& item) {
        return item.value("id", std::string{}) == sid;
    });
    ASSERT_NE(inactive_it, inactive.end());
    EXPECT_EQ((*inactive_it)["active"], false);
    EXPECT_EQ((*inactive_it)["workspace_hash"], "");
    EXPECT_EQ((*inactive_it)["cwd"], "");
    EXPECT_EQ((*inactive_it)["no_workspace"], true);

    auto resumed = cpr::Post(cpr::Url{fx.url("/api/sessions/" + sid + "/resume")},
                             cpr::Header{{"Content-Type", "application/json"}},
                             cpr::Body{R"({})"});
    ASSERT_EQ(resumed.status_code, 200) << resumed.text;
    auto resumed_body = json::parse(resumed.text);
    EXPECT_EQ(resumed_body["workspace_hash"], "");
    EXPECT_EQ(resumed_body["cwd"], "");
    EXPECT_EQ(resumed_body["no_workspace"], true);
    auto* resumed_entry = fx.registry->lookup(sid);
    ASSERT_NE(resumed_entry, nullptr);
    EXPECT_TRUE(resumed_entry->no_workspace);
    EXPECT_EQ(resumed_entry->cwd, expected_cwd);

    std::filesystem::remove_all(no_workspace_project_dir);
}

// 场景:daemon 运行期间 TUI/其它进程改写 config.json 的默认模型和默认
// 权限模式,后续 create-session 必须在解析前观察磁盘新值。
TEST(WebServerHttp, CreateSessionRefreshesExternalDefaultPreferences) {
    WebServerFixture fx;

    acecode::AppConfig disk = fx.cfg;
    acecode::ModelProfile slow;
    slow.name = "slow";
    slow.provider = "copilot";
    slow.model = "gpt-slow";
    disk.saved_models.push_back(slow);
    disk.default_model_name = "slow";
    disk.default_permission_mode = "yolo";
    acecode::save_config(disk, (fx.tmp_dir / "config.json").string());

    auto post = cpr::Post(cpr::Url{fx.url("/api/sessions")},
                          cpr::Header{{"Content-Type", "application/json"}},
                          cpr::Body{R"({})"});
    ASSERT_EQ(post.status_code, 201) << post.text;
    auto sid = json::parse(post.text)["session_id"].get<std::string>();

    auto* entry = fx.registry->lookup(sid);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->model_state.name, "slow");
    EXPECT_EQ(entry->model_state.model, "gpt-slow");
    auto mode = fx.registry->permission_mode(sid);
    ASSERT_TRUE(mode.has_value());
    EXPECT_EQ(*mode, acecode::PermissionMode::Yolo);
    EXPECT_EQ(fx.cfg.default_model_name, "slow");
    EXPECT_EQ(fx.cfg.default_permission_mode, "yolo");
}

// 场景:默认权限模式 API 拒绝非法 mode,且不修改内存配置或新会话模板。
TEST(WebServerHttp, DefaultPermissionModeRejectsInvalidMode) {
    WebServerFixture fx;
    fx.cfg.default_permission_mode = "yolo";
    fx.registry->set_default_permission_mode(acecode::PermissionMode::Yolo);

    auto put = cpr::Put(cpr::Url{fx.url("/api/config/default-permission-mode")},
                        cpr::Header{{"Content-Type", "application/json"}},
                        cpr::Body{R"({"mode":"always"})"});
    EXPECT_EQ(put.status_code, 400) << put.text;
    EXPECT_EQ(fx.cfg.default_permission_mode, "yolo");
    EXPECT_EQ(fx.registry->default_permission_mode(), acecode::PermissionMode::Yolo);
}

// 场景: 共享 daemon 暴露 workspace registry,每个 workspace 有独立 session
// lifecycle endpoint。创建/列表响应必须携带 workspace_hash/cwd。
TEST(WebServerHttp, WorkspaceScopedSessionLifecycle) {
    WebServerFixture fx;
    const std::string default_hash = acecode::compute_cwd_hash(fx.cwd);

    auto get_ws = cpr::Get(cpr::Url{fx.url("/api/workspaces")});
    ASSERT_EQ(get_ws.status_code, 200) << get_ws.text;
    auto ws_list = json::parse(get_ws.text);
    ASSERT_TRUE(ws_list.is_array());
    ASSERT_FALSE(ws_list.empty());
    EXPECT_EQ(ws_list[0]["hash"], default_hash);
    EXPECT_EQ(ws_list[0]["cwd"], fx.cwd);

    auto other_cwd_path = fx.tmp_dir / "other-cwd";
    std::filesystem::create_directories(other_cwd_path);
    const std::string other_cwd = other_cwd_path.string();
    const std::string other_hash = acecode::compute_cwd_hash(other_cwd);
    auto post_ws = cpr::Post(cpr::Url{fx.url("/api/workspaces")},
                             cpr::Header{{"Content-Type", "application/json"}},
                             cpr::Body{json{{"cwd", other_cwd}}.dump()});
    ASSERT_EQ(post_ws.status_code, 201) << post_ws.text;
    auto ws = json::parse(post_ws.text);
    EXPECT_EQ(ws["hash"], other_hash);
    EXPECT_EQ(ws["cwd"], other_cwd);

    auto create = cpr::Post(cpr::Url{fx.url("/api/workspaces/" + other_hash + "/sessions")},
                            cpr::Header{{"Content-Type", "application/json"}},
                            cpr::Body{R"({})"});
    ASSERT_EQ(create.status_code, 201) << create.text;
    auto created = json::parse(create.text);
    ASSERT_TRUE(created.contains("session_id"));
    EXPECT_EQ(created["workspace_hash"], other_hash);
    EXPECT_EQ(created["cwd"], other_cwd);

    auto list = cpr::Get(cpr::Url{fx.url("/api/workspaces/" + other_hash + "/sessions")});
    ASSERT_EQ(list.status_code, 200) << list.text;
    auto sessions = json::parse(list.text);
    ASSERT_TRUE(sessions.is_array());
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions[0]["id"], created["session_id"]);
    EXPECT_EQ(sessions[0]["workspace_hash"], other_hash);
    EXPECT_EQ(sessions[0]["cwd"], other_cwd);
}

TEST(WebServerHttp, OpencodeImportRoutesPreviewStartPollAndRejectUnknownWorkspace) {
    auto db_root = std::filesystem::temp_directory_path() /
                   ("acecode_opencode_route_" + std::to_string(std::random_device{}()));
    RemoveTreeOnExit cleanup{db_root};
    const auto db_path = db_root / "opencode.db";

    WebServerFixture fx;
    create_opencode_import_db(db_path, fx.cwd);
    ScopedEnvOverride opencode_db("OPENCODE_DB", acecode::path_to_utf8(db_path));

    auto workspaces_response = cpr::Get(cpr::Url{fx.url("/api/workspaces")});
    ASSERT_EQ(workspaces_response.status_code, 200) << workspaces_response.text;
    auto workspaces = json::parse(workspaces_response.text);
    ASSERT_FALSE(workspaces.empty());
    const std::string hash = workspaces[0]["hash"].get<std::string>();
    const std::string encoded_hash = url_encode_component(hash);

    auto preview = cpr::Get(cpr::Url{fx.url("/api/workspaces/" + encoded_hash + "/opencode-import")});
    ASSERT_EQ(preview.status_code, 200) << preview.text;
    auto preview_json = json::parse(preview.text);
    EXPECT_EQ(preview_json["available"], true);
    EXPECT_EQ(preview_json["count"], 2);
    ASSERT_TRUE(preview_json["sessions"].is_array());
    ASSERT_EQ(preview_json["sessions"].size(), 2u);
    auto archived = std::find_if(preview_json["sessions"].begin(), preview_json["sessions"].end(), [](const auto& session) {
        return session.value("id", std::string{}) == "ses-route-archived";
    });
    ASSERT_NE(archived, preview_json["sessions"].end());
    EXPECT_EQ((*archived)["archived"], true);

    auto unknown_preview = cpr::Get(cpr::Url{fx.url("/api/workspaces/missing/opencode-import")});
    EXPECT_EQ(unknown_preview.status_code, 404);

    auto started = cpr::Post(cpr::Url{fx.url("/api/workspaces/" + encoded_hash + "/opencode-import")},
                             cpr::Header{{"Content-Type", "application/json"}},
                             cpr::Body{json{{"session_ids", json::array({"ses-route"})}}.dump()});
    ASSERT_EQ(started.status_code, 202) << started.text;
    auto started_json = json::parse(started.text);
    EXPECT_EQ(started_json["total"], 1);
    const std::string job_id = started_json["job_id"].get<std::string>();
    ASSERT_FALSE(job_id.empty());

    json status;
    for (int i = 0; i < 20; ++i) {
        std::this_thread::sleep_for(50ms);
        auto poll = cpr::Get(cpr::Url{fx.url("/api/workspaces/" + encoded_hash +
                                             "/opencode-import/" + url_encode_component(job_id))});
        ASSERT_EQ(poll.status_code, 200) << poll.text;
        status = json::parse(poll.text);
        if (status["state"] == "complete" || status["state"] == "failed") break;
    }
    EXPECT_EQ(status["state"], "complete") << status.dump();
    EXPECT_EQ(status["imported"], 1);
    EXPECT_EQ(status["total"], 1);

    auto sessions_response = cpr::Get(cpr::Url{fx.url("/api/workspaces/" + encoded_hash + "/sessions")});
    ASSERT_EQ(sessions_response.status_code, 200) << sessions_response.text;
    auto session_list = json::parse(sessions_response.text);
    ASSERT_FALSE(session_list.empty());
    EXPECT_EQ(session_list[0]["title"], "Route import");
}

// 场景:workspace 下有 .agent/skills 时,GET /api/commands?workspace=<hash>
// 必须同时返回基础 builtin 命令与该 workspace 的 skill,否则前端输入 `/`
// 只能看到空下拉或看不到 skill。
TEST(WebServerHttp, CommandsEndpointReturnsBuiltinsAndWorkspaceSkills) {
    WebServerFixture fx(true, false, {}, false);
    write_skill_md(fx.cwd_dir / ".agent" / "skills", "api-calculator", "Exact math helper");

    auto workspaces = cpr::Get(cpr::Url{fx.url("/api/workspaces")});
    ASSERT_EQ(workspaces.status_code, 200) << workspaces.text;
    auto ws_list = json::parse(workspaces.text);
    ASSERT_TRUE(ws_list.is_array());
    ASSERT_FALSE(ws_list.empty());
    const auto hash = ws_list[0]["hash"].get<std::string>();

    auto r = cpr::Get(cpr::Url{fx.url("/api/commands?workspace=" + hash)});
    ASSERT_EQ(r.status_code, 200) << r.text;
    auto body = json::parse(r.text);

    ASSERT_TRUE(body.contains("builtins"));
    ASSERT_TRUE(body["builtins"].is_array());
    ASSERT_GE(body["builtins"].size(), 2u);
    EXPECT_EQ(body["builtins"][0]["name"].get<std::string>(), "init");
    EXPECT_EQ(body["builtins"][1]["name"].get<std::string>(), "compact");

    ASSERT_TRUE(body.contains("skills"));
    bool found = false;
    for (const auto& skill : body["skills"]) {
        if (skill["name"].get<std::string>() == "api-calculator") {
            found = true;
            EXPECT_EQ(skill["description"].get<std::string>(), "Exact math helper");
        }
    }
    EXPECT_TRUE(found) << r.text;
}

// 场景:显式空 workspace 查询代表「无工作区」。此时全局 skill 仍应进入
// 斜杠补全,但 daemon 启动 cwd 下的项目 skill 不得泄漏；完全省略查询参数
// 仍保留旧客户端只拿 builtins 的兼容响应。
TEST(WebServerHttp, CommandsEndpointReturnsGlobalSkillsForExplicitNoWorkspace) {
    WebServerFixture fx(true, false, {}, false);
    const auto global_root = fx.tmp_dir / "global-skills";
    write_skill_md(global_root, "api-global-only", "Global helper");
    write_skill_md(fx.cwd_dir / ".agent" / "skills",
                   "api-daemon-project-only", "Project helper");
    fx.cfg.skills.external_dirs = {global_root.string()};

    auto explicit_none = cpr::Get(cpr::Url{fx.url("/api/commands?workspace=")});
    ASSERT_EQ(explicit_none.status_code, 200) << explicit_none.text;
    auto body = json::parse(explicit_none.text);
    ASSERT_TRUE(body.contains("commands"));
    EXPECT_TRUE(body["commands"].empty());
    ASSERT_TRUE(body.contains("skills"));

    bool found_global = false;
    bool found_project = false;
    for (const auto& skill : body["skills"]) {
        const auto name = skill["name"].get<std::string>();
        found_global = found_global || name == "api-global-only";
        found_project = found_project || name == "api-daemon-project-only";
    }
    EXPECT_TRUE(found_global) << explicit_none.text;
    EXPECT_FALSE(found_project) << explicit_none.text;

    auto legacy = cpr::Get(cpr::Url{fx.url("/api/commands")});
    ASSERT_EQ(legacy.status_code, 200) << legacy.text;
    auto legacy_body = json::parse(legacy.text);
    EXPECT_FALSE(legacy_body.contains("commands"));
    EXPECT_FALSE(legacy_body.contains("skills"));
}

// 场景: native folder picker API 只给 Desktop 启动的 daemon 用。standalone
// 默认关闭时必须拒绝,且不能调用 picker callback。
TEST(WebServerHttp, NativeFolderPickerEndpointRejectsWhenDisabled) {
    bool called = false;
    WebServerFixture fx(true, false, [&called]() -> std::optional<std::string> {
        called = true;
        return std::string{"should-not-be-called"};
    });

    auto pick = cpr::Post(cpr::Url{fx.url("/api/workspaces/pick-folder")});
    ASSERT_EQ(pick.status_code, 501) << pick.text;
    auto body = json::parse(pick.text);
    EXPECT_EQ(body["error"], "native folder picker unavailable");
    EXPECT_FALSE(called);
}

// 场景: 用户取消 OS 目录选择器时 route 返回 JSON null,不注册新 workspace。
TEST(WebServerHttp, NativeFolderPickerEndpointReturnsNullOnCancel) {
    bool called = false;
    WebServerFixture fx(true, true, [&called]() -> std::optional<std::string> {
        called = true;
        return std::nullopt;
    });
    const auto before = fx.workspace_registry->list().size();

    auto pick = cpr::Post(cpr::Url{fx.url("/api/workspaces/pick-folder")});
    ASSERT_EQ(pick.status_code, 200) << pick.text;
    EXPECT_EQ(pick.text, "null");
    EXPECT_TRUE(called);
    EXPECT_EQ(fx.workspace_registry->list().size(), before);
}

// 场景: Desktop webapp 模式通过 daemon endpoint 调原生 picker。选中目录后
// daemon 应注册 workspace 并返回与 /api/workspaces 相同的 metadata schema。
TEST(WebServerHttp, NativeFolderPickerEndpointRegistersSelectedFolder) {
    auto picked_path = std::filesystem::temp_directory_path() /
        ("acecode_picked_workspace_" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(picked_path);
    RemoveTreeOnExit cleanup{picked_path};

    std::string picked = picked_path.string();
    std::string expected_cwd = picked;
    for (auto& c : expected_cwd) {
        if (c == '\\') c = '/';
    }
    const std::string expected_hash = acecode::compute_cwd_hash(expected_cwd);

    bool called = false;
    WebServerFixture fx(true, true, [&called, picked]() -> std::optional<std::string> {
        called = true;
        return picked;
    });

    auto pick = cpr::Post(cpr::Url{fx.url("/api/workspaces/pick-folder")});
    ASSERT_EQ(pick.status_code, 200) << pick.text;
    EXPECT_TRUE(called);
    auto body = json::parse(pick.text);
    EXPECT_EQ(body["hash"], expected_hash);
    EXPECT_EQ(body["cwd"], expected_cwd);
    EXPECT_EQ(body["available"], true);

    auto get_ws = cpr::Get(cpr::Url{fx.url("/api/workspaces")});
    ASSERT_EQ(get_ws.status_code, 200) << get_ws.text;
    auto workspaces = json::parse(get_ws.text);
    bool found = false;
    for (const auto& ws : workspaces) {
        if (ws.value("hash", "") == expected_hash) {
            found = true;
            EXPECT_EQ(ws["cwd"], expected_cwd);
        }
    }
    EXPECT_TRUE(found);
}

// 场景:新建项目弹窗需要在创建前显示默认位置。该位置是 projects 元数据目录
// 的同级 workspaces,不能把用户源码混进 hash 元数据目录。
TEST(WebServerHttp, ProjectDefaultsExposeSiblingGlobalWorkspaceRoot) {
    WebServerFixture fx;

    auto defaults = cpr::Get(cpr::Url{fx.url("/api/projects/defaults")});
    ASSERT_EQ(defaults.status_code, 200) << defaults.text;
    const auto body = json::parse(defaults.text);
    const auto expected = acecode::path_to_utf8_generic(
        (fx.tmp_dir / "workspaces").lexically_normal());
    EXPECT_EQ(body["parent_dir"], expected);
    EXPECT_FALSE(std::filesystem::exists(fx.tmp_dir / "workspaces"));
}

// 场景:默认位置创建时后端负责安全命名、真正建目录、注册 workspace,并把
// 最终目录名返回给前端。第二次同名不能收养或覆盖已有目录。
TEST(WebServerHttp, CreateProjectNormalizesCreatesRegistersAndRejectsCollision) {
    WebServerFixture fx;
    const json request_body{{"name", " alpha/api:test "}};

    auto created = cpr::Post(cpr::Url{fx.url("/api/projects")},
                             cpr::Body{request_body.dump()},
                             cpr::Header{{"Content-Type", "application/json"}});
    ASSERT_EQ(created.status_code, 201) << created.text;
    const auto body = json::parse(created.text);
    EXPECT_EQ(body["directory_name"], "alpha-api-test");
    EXPECT_EQ(body["name"], "alpha-api-test");
    EXPECT_EQ(body["sanitized"], true);
    const auto expected_path = fx.tmp_dir / "workspaces" / "alpha-api-test";
    EXPECT_TRUE(std::filesystem::is_directory(expected_path));
    std::error_code equivalent_error;
    EXPECT_TRUE(std::filesystem::equivalent(
        path_from_utf8(body["cwd"].get<std::string>()),
        expected_path,
        equivalent_error)) << equivalent_error.message();

    const auto workspace = fx.workspace_registry->get(
        body["hash"].get<std::string>());
    ASSERT_TRUE(workspace.has_value());
    EXPECT_EQ(workspace->cwd, body["cwd"]);

    auto collision = cpr::Post(cpr::Url{fx.url("/api/projects")},
                               cpr::Body{request_body.dump()},
                               cpr::Header{{"Content-Type", "application/json"}});
    ASSERT_EQ(collision.status_code, 409) << collision.text;
    EXPECT_EQ(json::parse(collision.text)["error"], "PROJECT_ALREADY_EXISTS");
    EXPECT_TRUE(std::filesystem::is_directory(expected_path));
}

// 场景:自定义位置必须是已存在的绝对目录。相对路径不能在 daemon cwd 下被
// 悄悄解释,不存在的位置也不能被隐式创建成任意父目录树。
TEST(WebServerHttp, CreateProjectRejectsInvalidCustomParent) {
    WebServerFixture fx;

    auto relative = cpr::Post(cpr::Url{fx.url("/api/projects")},
                              cpr::Body{R"({"name":"demo","parent_dir":"relative"})"},
                              cpr::Header{{"Content-Type", "application/json"}});
    ASSERT_EQ(relative.status_code, 400) << relative.text;
    EXPECT_EQ(json::parse(relative.text)["error"],
              "PROJECT_PARENT_ABSOLUTE_REQUIRED");

    const auto missing_path = fx.tmp_dir / "missing-parent";
    auto missing = cpr::Post(cpr::Url{fx.url("/api/projects")},
                             cpr::Body{json{{"name", "demo"},
                                            {"parent_dir", missing_path.string()}}.dump()},
                             cpr::Header{{"Content-Type", "application/json"}});
    ASSERT_EQ(missing.status_code, 400) << missing.text;
    EXPECT_EQ(json::parse(missing.text)["error"], "PROJECT_PARENT_NOT_FOUND");
    EXPECT_FALSE(std::filesystem::exists(missing_path));
}

// 场景: open-in-explorer 端点与 native folder picker 同款门控 —— 回调未注入
// (standalone daemon / 非 desktop 启动)时必须 501,不能有任何副作用。
// 回归: webapp 兼容模式右键菜单「在资源管理器中打开」首次落地(原先只有
// webview bridge 一条通路,Edge app 模式完全不可用)。
TEST(WebServerHttp, OpenInExplorerEndpointRejectsWhenUnavailable) {
    WebServerFixture fx;

    auto r = cpr::Post(cpr::Url{fx.url("/api/open-in-explorer")},
                       cpr::Header{{"Content-Type", "application/json"}},
                       cpr::Body{R"({"path":"C:/anywhere"})"});
    ASSERT_EQ(r.status_code, 501) << r.text;
    auto body = json::parse(r.text);
    EXPECT_EQ(body["error"], "open in explorer unavailable");
}

// 场景: 请求体缺 path(或为空)时 400,回调不能被调用 —— 校验顺序是
// 门控 → 形参 → 执行,空路径不应该走到执行层。
TEST(WebServerHttp, OpenInExplorerEndpointRequiresPath) {
    bool called = false;
    WebServerFixture fx(true, false, {}, true, {},
                        [&called](const std::string&) -> std::optional<std::string> {
                            called = true;
                            return std::nullopt;
                        });

    auto r = cpr::Post(cpr::Url{fx.url("/api/open-in-explorer")},
                       cpr::Header{{"Content-Type", "application/json"}},
                       cpr::Body{R"({})"});
    ASSERT_EQ(r.status_code, 400) << r.text;
    EXPECT_FALSE(called);

    auto bad = cpr::Post(cpr::Url{fx.url("/api/open-in-explorer")},
                         cpr::Header{{"Content-Type", "application/json"}},
                         cpr::Body{"not json"});
    ASSERT_EQ(bad.status_code, 400) << bad.text;
    EXPECT_FALSE(called);
}

// 场景: 回调成功(返回 nullopt)→ 200 {"ok":true};回调报错(返回错误串,
// 例如路径越出已注册 workspace)→ 400 {"ok":false,"error":...}。错误信息
// 必须原样透传给前端 toast。
TEST(WebServerHttp, OpenInExplorerEndpointForwardsCallbackResult) {
    std::string received_path;
    std::optional<std::string> next_result;
    WebServerFixture fx(true, false, {}, true, {},
                        [&](const std::string& path) -> std::optional<std::string> {
                            received_path = path;
                            return next_result;
                        });

    next_result = std::nullopt;
    auto ok = cpr::Post(cpr::Url{fx.url("/api/open-in-explorer")},
                        cpr::Header{{"Content-Type", "application/json"}},
                        cpr::Body{R"({"path":"D:/proj"})"});
    ASSERT_EQ(ok.status_code, 200) << ok.text;
    EXPECT_EQ(json::parse(ok.text)["ok"], true);
    EXPECT_EQ(received_path, "D:/proj");

    next_result = std::string{"path is outside registered workspaces"};
    auto rejected = cpr::Post(cpr::Url{fx.url("/api/open-in-explorer")},
                              cpr::Header{{"Content-Type", "application/json"}},
                              cpr::Body{R"({"path":"D:/elsewhere"})"});
    ASSERT_EQ(rejected.status_code, 400) << rejected.text;
    auto body = json::parse(rejected.text);
    EXPECT_EQ(body["ok"], false);
    EXPECT_EQ(body["error"], "path is outside registered workspaces");
}

// 场景:desktop bridge 直接改 workspace.json 后,daemon 的 /api/workspaces
// 需要重新扫描磁盘,否则左侧项目名会被旧的 daemon 内存值覆盖。
TEST(WebServerHttp, WorkspaceListRefreshesExternalRename) {
    WebServerFixture fx;
    const std::string hash = acecode::compute_cwd_hash(fx.cwd);

    acecode::desktop::WorkspaceRegistry external;
    external.scan(fx.projects_dir.string());
    ASSERT_TRUE(external.set_name(fx.projects_dir.string(), hash, "renamed-from-desktop"));

    auto get_ws = cpr::Get(cpr::Url{fx.url("/api/workspaces")});
    ASSERT_EQ(get_ws.status_code, 200) << get_ws.text;
    auto ws_list = json::parse(get_ws.text);
    ASSERT_TRUE(ws_list.is_array());
    ASSERT_FALSE(ws_list.empty());
    EXPECT_EQ(ws_list[0]["hash"], hash);
    EXPECT_EQ(ws_list[0]["name"], "renamed-from-desktop");
}

// 场景:在非 daemon 启动 cwd 的 workspace 里 fork,新 session 必须用源
// workspace cwd 装回 registry；否则前端切到新 fork 时 WS 会报 unknown session。
TEST(WebServerHttp, ForkWorkspaceSessionResumesInSourceWorkspace) {
    WebServerFixture fx;

    auto other_cwd_path = fx.tmp_dir / "fork-cwd";
    std::filesystem::create_directories(other_cwd_path);
    const std::string other_cwd = other_cwd_path.string();
    const std::string other_hash = acecode::compute_cwd_hash(other_cwd);

    auto post_ws = cpr::Post(cpr::Url{fx.url("/api/workspaces")},
                             cpr::Header{{"Content-Type", "application/json"}},
                             cpr::Body{json{{"cwd", other_cwd}}.dump()});
    ASSERT_EQ(post_ws.status_code, 201) << post_ws.text;

    auto create = cpr::Post(cpr::Url{fx.url("/api/workspaces/" + other_hash + "/sessions")},
                            cpr::Header{{"Content-Type", "application/json"}},
                            cpr::Body{R"({})"});
    ASSERT_EQ(create.status_code, 201) << create.text;
    const std::string sid = json::parse(create.text)["session_id"].get<std::string>();

    auto* entry = fx.registry->lookup(sid);
    ASSERT_NE(entry, nullptr);
    acecode::ChatMessage msg;
    msg.role = "user";
    msg.content = "fork from workspace";
    msg.uuid = "u-workspace-fork";
    entry->loop->push_message(msg);
    entry->sm->on_message(msg);

    auto fork = cpr::Post(cpr::Url{fx.url("/api/sessions/" + sid + "/fork")},
                          cpr::Header{{"Content-Type", "application/json"}},
                          cpr::Body{json{{"at_message_id", "u-workspace-fork"}}.dump()});
    ASSERT_EQ(fork.status_code, 200) << fork.text;
    auto body = json::parse(fork.text);
    const std::string new_id = body["session_id"].get<std::string>();
    EXPECT_EQ(body["workspace_hash"], other_hash);
    EXPECT_EQ(body["cwd"], other_cwd);

    auto* forked = fx.registry->lookup(new_id);
    ASSERT_NE(forked, nullptr);
    EXPECT_EQ(forked->workspace_hash, other_hash);
    EXPECT_EQ(forked->cwd, other_cwd);
}

// 场景: registry 里留着一个已删除/不可访问的 workspace 时,列表仍可返回,
// 但不允许继续创建新会话,避免工具后续在坏 cwd 上才爆。
TEST(WebServerHttp, UnavailableWorkspaceRejectsSessionCreate) {
    WebServerFixture fx;

    auto missing_cwd_path = fx.tmp_dir / "missing-cwd";
    const std::string missing_cwd = missing_cwd_path.string();
    const std::string missing_hash = acecode::compute_cwd_hash(missing_cwd);

    auto post_ws = cpr::Post(cpr::Url{fx.url("/api/workspaces")},
                             cpr::Header{{"Content-Type", "application/json"}},
                             cpr::Body{json{{"cwd", missing_cwd}}.dump()});
    ASSERT_EQ(post_ws.status_code, 201) << post_ws.text;
    auto ws = json::parse(post_ws.text);
    EXPECT_EQ(ws["hash"], missing_hash);
    EXPECT_FALSE(ws.value("available", true));

    auto create = cpr::Post(cpr::Url{fx.url("/api/workspaces/" + missing_hash + "/sessions")},
                            cpr::Header{{"Content-Type", "application/json"}},
                            cpr::Body{R"({})"});
    ASSERT_EQ(create.status_code, 409) << create.text;
    auto err = json::parse(create.text);
    EXPECT_EQ(err["error"], "workspace path unavailable");
}

// 场景: Desktop 共享 daemon 切换到另一个已注册 workspace 后,文件树 API
// 必须允许浏览该 workspace cwd,不能继续只把 daemon 启动 cwd 当白名单。
TEST(WebServerHttp, FilesEndpointAllowsRegisteredWorkspaceCwd) {
    WebServerFixture fx;

    auto other_cwd_path = fx.tmp_dir / "files-cwd";
    auto src_dir = other_cwd_path / "src";
    std::filesystem::create_directories(src_dir);
    {
        std::ofstream ofs(src_dir / "main.txt");
        ofs << "hello from registered workspace";
    }
    const std::string other_cwd = other_cwd_path.string();

    auto post_ws = cpr::Post(cpr::Url{fx.url("/api/workspaces")},
                             cpr::Header{{"Content-Type", "application/json"}},
                             cpr::Body{json{{"cwd", other_cwd}}.dump()});
    ASSERT_EQ(post_ws.status_code, 201) << post_ws.text;

    auto root = cpr::Get(cpr::Url{fx.url("/api/files")},
                         cpr::Parameters{{"cwd", other_cwd}, {"path", ""}});
    ASSERT_EQ(root.status_code, 200) << root.text;
    auto root_entries = json::parse(root.text);
    bool saw_src = false;
    for (const auto& e : root_entries) {
        if (e.value("name", "") == "src" && e.value("kind", "") == "dir") {
            saw_src = true;
        }
    }
    EXPECT_TRUE(saw_src);

    auto nested = cpr::Get(cpr::Url{fx.url("/api/files")},
                           cpr::Parameters{{"cwd", other_cwd}, {"path", "src"}});
    ASSERT_EQ(nested.status_code, 200) << nested.text;
    auto nested_entries = json::parse(nested.text);
    ASSERT_EQ(nested_entries.size(), 1u);
    EXPECT_EQ(nested_entries[0]["name"], "main.txt");
    EXPECT_EQ(nested_entries[0]["path"], "src/main.txt");

    auto content = cpr::Get(cpr::Url{fx.url("/api/files/content")},
                            cpr::Parameters{{"cwd", other_cwd}, {"path", "src/main.txt"}});
    ASSERT_EQ(content.status_code, 200) << content.text;
    EXPECT_EQ(content.text, "hello from registered workspace");
}

// 场景:/api/files/blob 除图片外也允许浏览器原生可打开的 PDF,供详情面板内嵌预览。
TEST(WebServerHttp, FilesBlobEndpointServesPdfPreview) {
    WebServerFixture fx;

    auto docs_dir = fx.cwd_dir / "docs";
    std::filesystem::create_directories(docs_dir);
    const std::string pdf_body = "%PDF-1.7\n1 0 obj\n<<>>\nendobj\n%%EOF\n";
    {
        std::ofstream ofs(docs_dir / "manual.pdf", std::ios::binary);
        ofs << pdf_body;
    }

    auto pdf = cpr::Get(cpr::Url{fx.url("/api/files/blob")},
                        cpr::Parameters{{"cwd", fx.cwd}, {"path", "docs/manual.pdf"}});
    ASSERT_EQ(pdf.status_code, 200) << pdf.text;
    EXPECT_EQ(pdf.text, pdf_body);
    const auto lower = pdf.header.find("content-type");
    const auto upper = pdf.header.find("Content-Type");
    const std::string content_type =
        lower != pdf.header.end() ? lower->second :
        (upper != pdf.header.end() ? upper->second : "");
    EXPECT_NE(content_type.find("application/pdf"), std::string::npos)
        << "content-type: " << content_type;
}

// /api/files/blob serves modern Office files for browser rendering while
// keeping legacy binary doc/xls formats unsupported.
TEST(WebServerHttp, FilesBlobEndpointServesModernOfficePreviewOnly) {
    WebServerFixture fx;

    auto docs_dir = fx.cwd_dir / "docs";
    std::filesystem::create_directories(docs_dir);
    const std::vector<std::pair<std::string, std::string>> supported = {
        {
            "proposal.docx",
            "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
        },
        {
            "report.xlsx",
            "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
        },
        {
            "macro.xlsm",
            "application/vnd.ms-excel.sheet.macroEnabled.12",
        },
    };

    for (const auto& [name, expected_type] : supported) {
        const std::string body = "office-bytes:" + name;
        {
            std::ofstream ofs(docs_dir / name, std::ios::binary);
            ofs << body;
        }

        auto resp = cpr::Get(cpr::Url{fx.url("/api/files/blob")},
                             cpr::Parameters{{"cwd", fx.cwd}, {"path", "docs/" + name}});
        ASSERT_EQ(resp.status_code, 200) << resp.text;
        EXPECT_EQ(resp.text, body);
        const auto lower = resp.header.find("content-type");
        const auto upper = resp.header.find("Content-Type");
        const std::string content_type =
            lower != resp.header.end() ? lower->second :
            (upper != resp.header.end() ? upper->second : "");
        EXPECT_NE(content_type.find(expected_type), std::string::npos)
            << name << " content-type: " << content_type;
    }

    for (const auto& name : {"legacy.doc", "legacy.xls"}) {
        {
            std::ofstream ofs(docs_dir / name, std::ios::binary);
            ofs << "legacy-office-bytes";
        }
        auto resp = cpr::Get(cpr::Url{fx.url("/api/files/blob")},
                             cpr::Parameters{{"cwd", fx.cwd}, {"path", std::string("docs/") + name}});
        ASSERT_EQ(resp.status_code, 415) << resp.text;
        auto body = json::parse(resp.text);
        EXPECT_EQ(body["error"], "unsupported file type");
    }
}

// 场景:/api/files 遇到中文文件夹/文件名时必须返回 UTF-8 JSON,不能抛异常变 500。
TEST(WebServerHttp, FilesEndpointReturnsUtf8ForChinesePaths) {
    WebServerFixture fx;

    auto other_cwd_path = fx.tmp_dir / "files-cwd-utf8";
    auto chinese_dir = other_cwd_path / path_from_utf8(u8"中文目录");
    std::filesystem::create_directories(chinese_dir);
    {
        std::ofstream ofs(chinese_dir / path_from_utf8(u8"中文文件.txt"), std::ios::binary);
        ofs << "hello utf8";
    }
    const std::string other_cwd = other_cwd_path.string();

    auto post_ws = cpr::Post(cpr::Url{fx.url("/api/workspaces")},
                             cpr::Header{{"Content-Type", "application/json"}},
                             cpr::Body{json{{"cwd", other_cwd}}.dump()});
    ASSERT_EQ(post_ws.status_code, 201) << post_ws.text;

    auto root = cpr::Get(cpr::Url{fx.url("/api/files")},
                         cpr::Parameters{{"cwd", other_cwd}, {"path", ""}});
    ASSERT_EQ(root.status_code, 200) << root.text;
    auto root_entries = json::parse(root.text);
    bool saw_chinese_dir = false;
    for (const auto& e : root_entries) {
        if (e.value("name", "") == u8"中文目录" &&
            e.value("path", "") == u8"中文目录" &&
            e.value("kind", "") == "dir") {
            saw_chinese_dir = true;
        }
    }
    EXPECT_TRUE(saw_chinese_dir);

    auto nested = cpr::Get(cpr::Url{fx.url("/api/files")},
                           cpr::Parameters{{"cwd", other_cwd}, {"path", u8"中文目录"}});
    ASSERT_EQ(nested.status_code, 200) << nested.text;
    auto nested_entries = json::parse(nested.text);
    ASSERT_EQ(nested_entries.size(), 1u);
    EXPECT_EQ(nested_entries[0]["name"], u8"中文文件.txt");
    EXPECT_EQ(nested_entries[0]["path"], u8"中文目录/中文文件.txt");
}

// 场景:workspace pinned-sessions API 持久化有序 session id,并过滤不存在 id。
TEST(WebServerHttp, WorkspacePinnedSessionsPersistAndPruneIds) {
    WebServerFixture fx;
    const std::string hash = acecode::compute_cwd_hash(fx.cwd);

    auto create = cpr::Post(cpr::Url{fx.url("/api/workspaces/" + hash + "/sessions")},
                            cpr::Header{{"Content-Type", "application/json"}},
                            cpr::Body{R"({})"});
    ASSERT_EQ(create.status_code, 201) << create.text;
    const auto created = json::parse(create.text);
    const std::string session_id = created.value("session_id", std::string{});
    ASSERT_FALSE(session_id.empty());

    auto put = cpr::Put(cpr::Url{fx.url("/api/workspaces/" + hash + "/pinned-sessions")},
                        cpr::Header{{"Content-Type", "application/json"}},
                        cpr::Body{json{{"session_ids", json::array({"missing", session_id, session_id})}}.dump()});
    ASSERT_EQ(put.status_code, 200) << put.text;
    auto put_body = json::parse(put.text);
    ASSERT_TRUE(put_body["session_ids"].is_array());
    ASSERT_EQ(put_body["session_ids"].size(), 1u);
    EXPECT_EQ(put_body["session_ids"][0], session_id);

    auto get = cpr::Get(cpr::Url{fx.url("/api/workspaces/" + hash + "/pinned-sessions")});
    ASSERT_EQ(get.status_code, 200) << get.text;
    auto get_body = json::parse(get.text);
    ASSERT_EQ(get_body["session_ids"].size(), 1u);
    EXPECT_EQ(get_body["session_ids"][0], session_id);
}

// 场景:全局 pinned 视觉顺序可跨 workspace 保存,并按当前 pinned 状态裁剪旧项。
TEST(WebServerHttp, PinnedSessionVisualOrderPersistsAcrossWorkspacesAndPrunes) {
    WebServerFixture fx;
    const std::string hash1 = acecode::compute_cwd_hash(fx.cwd);

    const auto other_dir = fx.tmp_dir / "other-workspace";
    std::filesystem::create_directories(other_dir);
    auto post_ws = cpr::Post(cpr::Url{fx.url("/api/workspaces")},
                             cpr::Header{{"Content-Type", "application/json"}},
                             cpr::Body{json{{"cwd", other_dir.string()}}.dump()});
    ASSERT_EQ(post_ws.status_code, 201) << post_ws.text;
    const std::string hash2 = json::parse(post_ws.text).value("hash", std::string{});
    ASSERT_FALSE(hash2.empty());

    auto create1 = cpr::Post(cpr::Url{fx.url("/api/workspaces/" + hash1 + "/sessions")},
                             cpr::Header{{"Content-Type", "application/json"}},
                             cpr::Body{R"({})"});
    ASSERT_EQ(create1.status_code, 201) << create1.text;
    const std::string session1 = json::parse(create1.text).value("session_id", std::string{});
    ASSERT_FALSE(session1.empty());

    auto create2 = cpr::Post(cpr::Url{fx.url("/api/workspaces/" + hash2 + "/sessions")},
                             cpr::Header{{"Content-Type", "application/json"}},
                             cpr::Body{R"({})"});
    ASSERT_EQ(create2.status_code, 201) << create2.text;
    const std::string session2 = json::parse(create2.text).value("session_id", std::string{});
    ASSERT_FALSE(session2.empty());

    auto pin1 = cpr::Put(cpr::Url{fx.url("/api/workspaces/" + hash1 + "/pinned-sessions")},
                         cpr::Header{{"Content-Type", "application/json"}},
                         cpr::Body{json{{"session_ids", json::array({session1})}}.dump()});
    ASSERT_EQ(pin1.status_code, 200) << pin1.text;
    auto pin2 = cpr::Put(cpr::Url{fx.url("/api/workspaces/" + hash2 + "/pinned-sessions")},
                         cpr::Header{{"Content-Type", "application/json"}},
                         cpr::Body{json{{"session_ids", json::array({session2})}}.dump()});
    ASSERT_EQ(pin2.status_code, 200) << pin2.text;

    json order = {
        {"items", json::array({
            json{{"workspace_hash", hash2}, {"session_id", session2}},
            json{{"workspace_hash", hash1}, {"session_id", session1}},
            json{{"workspace_hash", "missing"}, {"session_id", "missing"}},
        })},
    };
    auto put_order = cpr::Put(cpr::Url{fx.url("/api/pinned-sessions/order")},
                              cpr::Header{{"Content-Type", "application/json"}},
                              cpr::Body{order.dump()});
    ASSERT_EQ(put_order.status_code, 200) << put_order.text;
    auto put_body = json::parse(put_order.text);
    ASSERT_EQ(put_body["items"].size(), 2u);
    EXPECT_EQ(put_body["items"][0]["workspace_hash"], hash2);
    EXPECT_EQ(put_body["items"][0]["session_id"], session2);
    EXPECT_EQ(put_body["items"][1]["workspace_hash"], hash1);
    EXPECT_EQ(put_body["items"][1]["session_id"], session1);

    auto unpin1 = cpr::Put(cpr::Url{fx.url("/api/workspaces/" + hash1 + "/pinned-sessions")},
                           cpr::Header{{"Content-Type", "application/json"}},
                           cpr::Body{json{{"session_ids", json::array()}}.dump()});
    ASSERT_EQ(unpin1.status_code, 200) << unpin1.text;

    auto get_order = cpr::Get(cpr::Url{fx.url("/api/pinned-sessions/order")});
    ASSERT_EQ(get_order.status_code, 200) << get_order.text;
    auto get_body = json::parse(get_order.text);
    ASSERT_EQ(get_body["items"].size(), 1u);
    EXPECT_EQ(get_body["items"][0]["workspace_hash"], hash2);
    EXPECT_EQ(get_body["items"][0]["session_id"], session2);
}

// 场景:归档 workspace 会话后,默认列表隐藏它;专用 archived 查询能看到,
// 取消归档后恢复到默认列表。
TEST(WebServerHttp, WorkspaceArchiveSessionHidesFromDefaultList) {
    WebServerFixture fx;
    const std::string hash = acecode::compute_cwd_hash(fx.cwd);

    auto create = cpr::Post(cpr::Url{fx.url("/api/workspaces/" + hash + "/sessions")},
                            cpr::Header{{"Content-Type", "application/json"}},
                            cpr::Body{R"({})"});
    ASSERT_EQ(create.status_code, 201) << create.text;
    const std::string session_id = json::parse(create.text).value("session_id", std::string{});
    ASSERT_FALSE(session_id.empty());

    auto archive = cpr::Put(cpr::Url{fx.url("/api/workspaces/" + hash + "/sessions/" + session_id + "/archive")});
    ASSERT_EQ(archive.status_code, 200) << archive.text;
    auto archived_body = json::parse(archive.text);
    EXPECT_EQ(archived_body["id"], session_id);
    EXPECT_TRUE(archived_body.value("archived", false));

    auto visible = cpr::Get(cpr::Url{fx.url("/api/workspaces/" + hash + "/sessions")});
    ASSERT_EQ(visible.status_code, 200) << visible.text;
    auto visible_sessions = json::parse(visible.text);
    EXPECT_TRUE(std::none_of(visible_sessions.begin(), visible_sessions.end(),
        [&](const json& s) { return s.value("id", std::string{}) == session_id; }));

    auto archived = cpr::Get(cpr::Url{fx.url("/api/workspaces/" + hash + "/sessions?archived=1")});
    ASSERT_EQ(archived.status_code, 200) << archived.text;
    auto archived_sessions = json::parse(archived.text);
    ASSERT_EQ(archived_sessions.size(), 1u);
    EXPECT_EQ(archived_sessions[0]["id"], session_id);
    EXPECT_TRUE(archived_sessions[0].value("archived", false));

    auto unarchive = cpr::Delete(cpr::Url{fx.url("/api/workspaces/" + hash + "/sessions/" + session_id + "/archive")});
    ASSERT_EQ(unarchive.status_code, 200) << unarchive.text;
    EXPECT_FALSE(json::parse(unarchive.text).value("archived", true));

    auto restored = cpr::Get(cpr::Url{fx.url("/api/workspaces/" + hash + "/sessions")});
    ASSERT_EQ(restored.status_code, 200) << restored.text;
    auto restored_sessions = json::parse(restored.text);
    EXPECT_TRUE(std::any_of(restored_sessions.begin(), restored_sessions.end(),
        [&](const json& s) { return s.value("id", std::string{}) == session_id; }));
}

// 场景:工作区归档会话可被永久删除;普通会话拒绝 purge,且成功删除时
// transcript/meta/持久化工具结果/用户消息索引全部清理。
TEST(WebServerHttp, WorkspacePurgePermanentlyDeletesOnlyArchivedSession) {
    WebServerFixture fx;
    const std::string hash = acecode::compute_cwd_hash(fx.cwd);

    auto create = cpr::Post(cpr::Url{fx.url("/api/workspaces/" + hash + "/sessions")},
                            cpr::Header{{"Content-Type", "application/json"}},
                            cpr::Body{R"({})"});
    ASSERT_EQ(create.status_code, 201) << create.text;
    const std::string session_id =
        json::parse(create.text).value("session_id", std::string{});
    ASSERT_FALSE(session_id.empty());

    const auto jsonl_path = path_from_utf8(
        acecode::SessionStorage::session_path(fx.project_dir, session_id));
    const auto meta_path = path_from_utf8(
        acecode::SessionStorage::meta_path(fx.project_dir, session_id));

    auto missing_flag = cpr::Delete(cpr::Url{
        fx.url("/api/workspaces/" + hash + "/sessions/" + session_id)});
    EXPECT_EQ(missing_flag.status_code, 400);

    auto unarchived = cpr::Delete(cpr::Url{
        fx.url("/api/workspaces/" + hash + "/sessions/" + session_id + "?purge=1")});
    EXPECT_EQ(unarchived.status_code, 409) << unarchived.text;
    auto still_visible = cpr::Get(cpr::Url{
        fx.url("/api/workspaces/" + hash + "/sessions")});
    ASSERT_EQ(still_visible.status_code, 200) << still_visible.text;
    const auto still_visible_sessions = json::parse(still_visible.text);
    EXPECT_TRUE(std::any_of(
        still_visible_sessions.begin(),
        still_visible_sessions.end(),
        [&](const json& item) {
            return item.value("id", std::string{}) == session_id;
        }));

    auto archive = cpr::Put(cpr::Url{
        fx.url("/api/workspaces/" + hash + "/sessions/" + session_id + "/archive")});
    ASSERT_EQ(archive.status_code, 200) << archive.text;
    ASSERT_TRUE(std::filesystem::exists(meta_path));

    acecode::ChatMessage indexed_message;
    indexed_message.role = "user";
    indexed_message.content = "archived-purge-needle";
    acecode::SessionStorage::write_messages(
        acecode::SessionStorage::session_path(fx.project_dir, session_id),
        {indexed_message});
    const auto persisted_dir = path_from_utf8(fx.project_dir) / session_id;
    write_text(persisted_dir / "tool-result.json", "{}");
    {
        acecode::SessionUserMessageIndex index(fx.project_dir);
        std::string error;
        ASSERT_TRUE(index.rebuild_session(
            session_id,
            acecode::SessionStorage::session_path(fx.project_dir, session_id),
            &error)) << error;
        ASSERT_EQ(index.search("archived-purge-needle", 10, &error).size(), 1u)
            << error;
    }

    auto purge = cpr::Delete(cpr::Url{
        fx.url("/api/workspaces/" + hash + "/sessions/" + session_id + "?purge=1")});
    EXPECT_EQ(purge.status_code, 204) << purge.text;
    EXPECT_FALSE(std::filesystem::exists(jsonl_path));
    EXPECT_FALSE(std::filesystem::exists(meta_path));
    EXPECT_FALSE(std::filesystem::exists(persisted_dir));
    {
        acecode::SessionUserMessageIndex index(fx.project_dir);
        std::string error;
        EXPECT_TRUE(index.search("archived-purge-needle", 10, &error).empty())
            << error;
    }

    auto archived = cpr::Get(cpr::Url{
        fx.url("/api/workspaces/" + hash + "/sessions?archived=1")});
    ASSERT_EQ(archived.status_code, 200) << archived.text;
    EXPECT_TRUE(json::parse(archived.text).empty());
}

// 场景:工作区 purge 必须解析真实 workspace 与会话,错误目标不触碰磁盘。
TEST(WebServerHttp, WorkspacePurgeRejectsMissingWorkspaceAndSession) {
    WebServerFixture fx;
    const std::string hash = acecode::compute_cwd_hash(fx.cwd);

    auto missing_workspace = cpr::Delete(cpr::Url{
        fx.url("/api/workspaces/missing/sessions/session-a?purge=1")});
    EXPECT_EQ(missing_workspace.status_code, 404);
    EXPECT_EQ(json::parse(missing_workspace.text)["error"], "workspace not found");

    auto missing_session = cpr::Delete(cpr::Url{
        fx.url("/api/workspaces/" + hash + "/sessions/session-a?purge=1")});
    EXPECT_EQ(missing_session.status_code, 404);
    EXPECT_EQ(json::parse(missing_session.text)["error"], "session not found");

    auto invalid_session = cpr::Delete(cpr::Url{
        fx.url("/api/workspaces/" + hash + "/sessions/bad.id?purge=1")});
    EXPECT_EQ(invalid_session.status_code, 400);
}

// 场景:兼容 /api/sessions 路由也遵守归档隐藏与 archived 查询语义。
TEST(WebServerHttp, CompatibilityArchiveSessionHidesFromDefaultList) {
    WebServerFixture fx;

    auto create = cpr::Post(cpr::Url{fx.url("/api/sessions")},
                            cpr::Header{{"Content-Type", "application/json"}},
                            cpr::Body{R"({})"});
    ASSERT_EQ(create.status_code, 201) << create.text;
    const std::string session_id = json::parse(create.text).value("session_id", std::string{});
    ASSERT_FALSE(session_id.empty());

    auto archive = cpr::Put(cpr::Url{fx.url("/api/sessions/" + session_id + "/archive")});
    ASSERT_EQ(archive.status_code, 200) << archive.text;

    auto visible = cpr::Get(cpr::Url{fx.url("/api/sessions")});
    ASSERT_EQ(visible.status_code, 200) << visible.text;
    auto visible_sessions = json::parse(visible.text);
    EXPECT_TRUE(std::none_of(visible_sessions.begin(), visible_sessions.end(),
        [&](const json& s) { return s.value("id", std::string{}) == session_id; }));

    auto archived = cpr::Get(cpr::Url{fx.url("/api/sessions?archived=1")});
    ASSERT_EQ(archived.status_code, 200) << archived.text;
    auto archived_sessions = json::parse(archived.text);
    ASSERT_EQ(archived_sessions.size(), 1u);
    EXPECT_EQ(archived_sessions[0]["id"], session_id);
}

// 场景:无 workspace registry 的归档页回退到兼容路由时,已归档普通会话
// 也能通过显式 purge 永久删除;未归档普通会话仍由既有 guard 拒绝。
TEST(WebServerHttp, CompatibilityPurgeAcceptsArchivedMainSession) {
    WebServerFixture fx;

    auto create = cpr::Post(cpr::Url{fx.url("/api/sessions")},
                            cpr::Header{{"Content-Type", "application/json"}},
                            cpr::Body{R"({})"});
    ASSERT_EQ(create.status_code, 201) << create.text;
    const std::string session_id =
        json::parse(create.text).value("session_id", std::string{});
    ASSERT_FALSE(session_id.empty());

    auto rejected = cpr::Delete(cpr::Url{
        fx.url("/api/sessions/" + session_id + "?purge=1")});
    EXPECT_EQ(rejected.status_code, 400);
    EXPECT_EQ(json::parse(rejected.text)["error"],
              "only subagent sessions can be purged");

    auto archive = cpr::Put(cpr::Url{
        fx.url("/api/sessions/" + session_id + "/archive")});
    ASSERT_EQ(archive.status_code, 200) << archive.text;

    const auto jsonl_path = path_from_utf8(
        acecode::SessionStorage::session_path(fx.project_dir, session_id));
    const auto meta_path = path_from_utf8(
        acecode::SessionStorage::meta_path(fx.project_dir, session_id));
    auto purge = cpr::Delete(cpr::Url{
        fx.url("/api/sessions/" + session_id + "?purge=1")});
    EXPECT_EQ(purge.status_code, 204) << purge.text;
    EXPECT_FALSE(std::filesystem::exists(jsonl_path));
    EXPECT_FALSE(std::filesystem::exists(meta_path));
}

// 场景: shared daemon 为 desktop onboarding 启动时,当前 cwd 只有 hidden
// workspace marker。它可以服务普通 /api/sessions,但不能出现在 /api/workspaces。
TEST(WebServerHttp, HiddenDefaultWorkspaceNotListedOrResolved) {
    WebServerFixture fx(/*register_default_workspace=*/false);
    const std::string hidden_hash = acecode::compute_cwd_hash(fx.cwd);

    auto get_ws = cpr::Get(cpr::Url{fx.url("/api/workspaces")});
    ASSERT_EQ(get_ws.status_code, 200) << get_ws.text;
    auto ws_list = json::parse(get_ws.text);
    ASSERT_TRUE(ws_list.is_array());
    EXPECT_TRUE(ws_list.empty());

    auto scoped = cpr::Get(cpr::Url{fx.url("/api/workspaces/" + hidden_hash + "/sessions")});
    EXPECT_EQ(scoped.status_code, 404) << scoped.text;

    auto local_sessions = cpr::Get(cpr::Url{fx.url("/api/sessions")});
    EXPECT_EQ(local_sessions.status_code, 200) << local_sessions.text;
}

TEST(WebServerHttp, PutSessionTitleRenamesActiveSessionAndPersistsMeta) {
    WebServerFixture fx;
    auto create = cpr::Post(cpr::Url{fx.url("/api/sessions")},
                            cpr::Header{{"Content-Type", "application/json"}},
                            cpr::Body{R"({})"});
    ASSERT_EQ(create.status_code, 201) << create.text;
    const auto sid = json::parse(create.text)["session_id"].get<std::string>();

    auto put = cpr::Put(cpr::Url{fx.url("/api/sessions/" + sid + "/title")},
                        cpr::Header{{"Content-Type", "application/json"}},
                        cpr::Body{R"({"title":"Renamed session"})"});
    ASSERT_EQ(put.status_code, 200) << put.text;
    auto body = json::parse(put.text);
    EXPECT_EQ(body["id"], sid);
    EXPECT_EQ(body["title"], "Renamed session");
    EXPECT_EQ(body["title_source"], "user");

    auto meta = acecode::SessionStorage::read_meta(
        acecode::SessionStorage::meta_path(fx.project_dir, sid, 0));
    EXPECT_EQ(meta.title, "Renamed session");
    EXPECT_EQ(meta.title_source, "user");
}

TEST(WebServerHttp, WorkspacePutSessionTitleRenamesInactiveDiskSession) {
    WebServerFixture fx;
    const std::string sid = "20260610-030405-abcd";
    const std::string hash = acecode::compute_cwd_hash(fx.cwd);
    std::filesystem::create_directories(fx.project_dir);

    acecode::SessionMeta meta;
    meta.id = sid;
    meta.cwd = fx.cwd;
    meta.created_at = "2026-06-10T03:04:05Z";
    meta.updated_at = meta.created_at;
    meta.message_count = 1;
    acecode::SessionStorage::write_meta(
        acecode::SessionStorage::meta_path(fx.project_dir, sid, 0), meta);

    auto put = cpr::Put(cpr::Url{fx.url("/api/workspaces/" + hash + "/sessions/" + sid + "/title")},
                        cpr::Header{{"Content-Type", "application/json"}},
                        cpr::Body{R"({"title":"Disk rename"})"});
    ASSERT_EQ(put.status_code, 200) << put.text;
    auto body = json::parse(put.text);
    EXPECT_EQ(body["id"], sid);
    EXPECT_EQ(body["active"], false);
    EXPECT_EQ(body["title"], "Disk rename");
    EXPECT_EQ(body["title_source"], "user");

    auto out = acecode::SessionStorage::read_meta(
        acecode::SessionStorage::meta_path(fx.project_dir, sid, 0));
    EXPECT_EQ(out.title, "Disk rename");
    EXPECT_EQ(out.title_source, "user");
}

// 场景: hidden workspace hash 不能通过 workspace-scoped endpoint 被直接访问;
// 手动注册后,同一个 cwd 的旧 TUI sessions 会被列出来。
TEST(WebServerHttp, HiddenWorkspaceRejectedUntilRegisteredThenListsExistingSession) {
    WebServerFixture fx;

    auto legacy_cwd_path = fx.tmp_dir / "legacy-tui-cwd";
    std::filesystem::create_directories(legacy_cwd_path);
    const std::string legacy_cwd = legacy_cwd_path.string();
    const std::string legacy_hash = acecode::compute_cwd_hash(legacy_cwd);

    ASSERT_TRUE(acecode::desktop::ensure_workspace_metadata(fx.projects_dir.string(), legacy_cwd));
    auto hidden = cpr::Get(cpr::Url{fx.url("/api/workspaces/" + legacy_hash + "/sessions")});
    ASSERT_EQ(hidden.status_code, 404) << hidden.text;

    const auto legacy_project_dir = std::filesystem::path(acecode::SessionStorage::get_project_dir(legacy_cwd));
    RemoveTreeOnExit cleanup{legacy_project_dir};
    std::filesystem::remove_all(legacy_project_dir);
    std::filesystem::create_directories(legacy_project_dir);

    acecode::SessionMeta meta;
    meta.id = "20260503-010203-abcd";
    meta.cwd = legacy_cwd;
    meta.created_at = "2026-05-03T01:02:03Z";
    meta.updated_at = "2026-05-03T01:03:03Z";
    meta.summary = "old tui chat";
    meta.message_count = 4;
    acecode::SessionStorage::write_meta(
        acecode::SessionStorage::meta_path(legacy_project_dir.string(), meta.id, 0),
        meta);

    auto post_ws = cpr::Post(cpr::Url{fx.url("/api/workspaces")},
                             cpr::Header{{"Content-Type", "application/json"}},
                             cpr::Body{json{{"cwd", legacy_cwd}}.dump()});
    ASSERT_EQ(post_ws.status_code, 201) << post_ws.text;
    auto ws = json::parse(post_ws.text);
    EXPECT_EQ(ws["hash"], legacy_hash);

    auto list = cpr::Get(cpr::Url{fx.url("/api/workspaces/" + legacy_hash + "/sessions")});
    ASSERT_EQ(list.status_code, 200) << list.text;
    auto sessions = json::parse(list.text);
    ASSERT_TRUE(sessions.is_array());
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions[0]["id"], meta.id);
    EXPECT_EQ(sessions[0]["summary"], meta.summary);
    EXPECT_EQ(sessions[0]["workspace_hash"], legacy_hash);
}

// 场景: workspace-scoped create 未显式传 model 时,应从该 workspace cwd 的
// model_override.json 解析 model_name,不能使用 daemon cwd 的 override。
TEST(WebServerHttp, WorkspaceCreateUsesWorkspaceCwdModelOverride) {
    WebServerFixture fx;

    acecode::ModelProfile workspace_model;
    workspace_model.name = "workspace-model";
    workspace_model.provider = "copilot";
    workspace_model.model = "workspace-model";
    fx.cfg.saved_models.push_back(workspace_model);

    auto other_cwd_path = fx.tmp_dir / "model-override-cwd";
    std::filesystem::create_directories(other_cwd_path);
    const std::string other_cwd = other_cwd_path.string();
    const std::string other_hash = acecode::compute_cwd_hash(other_cwd);

    acecode::save_cwd_model_override(other_cwd, "workspace-model");
    CwdModelOverrideCleanup cleanup{other_cwd};

    auto post_ws = cpr::Post(cpr::Url{fx.url("/api/workspaces")},
                             cpr::Header{{"Content-Type", "application/json"}},
                             cpr::Body{json{{"cwd", other_cwd}}.dump()});
    ASSERT_EQ(post_ws.status_code, 201) << post_ws.text;

    auto create = cpr::Post(cpr::Url{fx.url("/api/workspaces/" + other_hash + "/sessions")},
                            cpr::Header{{"Content-Type", "application/json"}},
                            cpr::Body{R"({})"});
    ASSERT_EQ(create.status_code, 201) << create.text;
    auto created = json::parse(create.text);

    auto list = cpr::Get(cpr::Url{fx.url("/api/workspaces/" + other_hash + "/sessions")});
    ASSERT_EQ(list.status_code, 200) << list.text;
    auto sessions = json::parse(list.text);
    ASSERT_TRUE(sessions.is_array());
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions[0]["id"], created["session_id"]);
    EXPECT_EQ(sessions[0]["workspace_hash"], other_hash);
    EXPECT_EQ(sessions[0]["model"], "workspace-model");
}

// 场景: DELETE /api/sessions/:id 返回 204,之后 GET 看不到该 id active=true(spec 9.5)。
TEST(WebServerHttp, DeleteSessionRemovesFromList) {
    WebServerFixture fx;
    auto post = cpr::Post(cpr::Url{fx.url("/api/sessions")},
                          cpr::Header{{"Content-Type", "application/json"}},
                          cpr::Body{R"({})"});
    ASSERT_EQ(post.status_code, 201);
    auto sid = json::parse(post.text)["session_id"].get<std::string>();

    auto del = cpr::Delete(cpr::Url{fx.url("/api/sessions/" + sid)});
    EXPECT_EQ(del.status_code, 204);

    auto list = cpr::Get(cpr::Url{fx.url("/api/sessions")});
    auto arr = json::parse(list.text);
    for (const auto& s : arr) {
        if (s["id"] == sid && s["active"].get<bool>()) {
            FAIL() << "删除后 session 仍然 active";
        }
    }
}

// 场景: spawn_subagent 子会话(meta 带 parent_session_id)不出现在常规
// GET /api/sessions 列表;GET /api/sessions?parent=<id> 反查只返回该父会话
// 的子任务。一旦回归:子会话泄漏进侧栏,或「后台任务」面板拿不到数据。
TEST(WebServerHttp, SubagentSessionsHiddenFromListAndQueryableByParent) {
    WebServerFixture fx;
    const std::string parent_id = "20260705-090000-aaaa";
    const std::string child_id  = "20260705-090100-bbbb";

    // 手造磁盘持久化数据:父会话普通 meta,子会话 meta 带 parent_session_id。
    for (const auto& [id, parent] :
         std::vector<std::pair<std::string, std::string>>{
             {parent_id, ""}, {child_id, parent_id}}) {
        acecode::SessionMeta meta;
        meta.id = id;
        meta.cwd = fx.cwd;
        meta.updated_at = "2026-07-05T09:00:00Z";
        meta.parent_session_id = parent;
        acecode::SessionStorage::write_meta(
            acecode::SessionStorage::meta_path(fx.project_dir, id), meta);
        write_text(path_from_utf8(
            acecode::SessionStorage::session_path(fx.project_dir, id)), "");
    }

    auto list = cpr::Get(cpr::Url{fx.url("/api/sessions")});
    ASSERT_EQ(list.status_code, 200);
    auto arr = json::parse(list.text);
    bool saw_parent = false;
    for (const auto& s : arr) {
        if (s["id"] == parent_id) saw_parent = true;
        EXPECT_NE(s["id"], child_id) << "子会话不应出现在常规列表";
    }
    EXPECT_TRUE(saw_parent);

    auto children = cpr::Get(cpr::Url{fx.url("/api/sessions?parent=" + parent_id)});
    ASSERT_EQ(children.status_code, 200);
    auto child_arr = json::parse(children.text);
    ASSERT_EQ(child_arr.size(), 1u) << child_arr.dump();
    EXPECT_EQ(child_arr[0]["id"], child_id);
    EXPECT_EQ(child_arr[0]["parent_session_id"], parent_id);
}

// 场景: DELETE /api/sessions/:id?purge=1 是「后台任务-清除」——只允许子会话,
// 成功后磁盘 jsonl/meta 永久删除;对主会话 purge 必须 400 拒绝(防误删)。
TEST(WebServerHttp, PurgeDeletesSubagentDiskDataAndRejectsMainSession) {
    WebServerFixture fx;
    const std::string parent_id = "20260705-091000-cccc";
    const std::string child_id  = "20260705-091100-dddd";
    for (const auto& [id, parent] :
         std::vector<std::pair<std::string, std::string>>{
             {parent_id, ""}, {child_id, parent_id}}) {
        acecode::SessionMeta meta;
        meta.id = id;
        meta.cwd = fx.cwd;
        meta.parent_session_id = parent;
        acecode::SessionStorage::write_meta(
            acecode::SessionStorage::meta_path(fx.project_dir, id), meta);
        write_text(path_from_utf8(
            acecode::SessionStorage::session_path(fx.project_dir, id)), "");
    }

    // 主会话 purge → 400,文件原地不动。
    auto bad = cpr::Delete(cpr::Url{fx.url("/api/sessions/" + parent_id + "?purge=1")});
    EXPECT_EQ(bad.status_code, 400);
    EXPECT_TRUE(std::filesystem::exists(path_from_utf8(
        acecode::SessionStorage::meta_path(fx.project_dir, parent_id))));

    // 子会话 purge → 204,jsonl + meta 都消失。
    auto ok = cpr::Delete(cpr::Url{fx.url("/api/sessions/" + child_id + "?purge=1")});
    EXPECT_EQ(ok.status_code, 204);
    EXPECT_FALSE(std::filesystem::exists(path_from_utf8(
        acecode::SessionStorage::meta_path(fx.project_dir, child_id))));
    EXPECT_FALSE(std::filesystem::exists(path_from_utf8(
        acecode::SessionStorage::session_path(fx.project_dir, child_id))));

    // 不带 purge 的 DELETE 语义不变:对磁盘数据无破坏。
    auto plain = cpr::Delete(cpr::Url{fx.url("/api/sessions/" + parent_id)});
    EXPECT_EQ(plain.status_code, 204);
    EXPECT_TRUE(std::filesystem::exists(path_from_utf8(
        acecode::SessionStorage::meta_path(fx.project_dir, parent_id))));
}

// 场景: GET /api/sessions/:id/messages 第一次(无 since)返回 {events:[], messages:[]}。
// 测试不真跑 LLM,所以两个数组都是空 — 但 wire format 必须正确(spec 9.6)。
TEST(WebServerHttp, GetMessagesReturnsEventsAndMessagesSchema) {
    WebServerFixture fx;
    auto post = cpr::Post(cpr::Url{fx.url("/api/sessions")},
                          cpr::Header{{"Content-Type", "application/json"}},
                          cpr::Body{R"({})"});
    ASSERT_EQ(post.status_code, 201);
    auto sid = json::parse(post.text)["session_id"].get<std::string>();

    auto r = cpr::Get(cpr::Url{fx.url("/api/sessions/" + sid + "/messages")});
    ASSERT_EQ(r.status_code, 200);
    auto j = json::parse(r.text);
    EXPECT_TRUE(j.contains("events")) << "首次拉取 schema 应含 events";
    EXPECT_TRUE(j.contains("messages")) << "首次拉取 schema 应含 messages";
    EXPECT_TRUE(j["events"].is_array());
    EXPECT_TRUE(j["messages"].is_array());
}

// 场景:首次打开/切回 Web 会话时,即使 goal_updated 事件已在订阅前发出,
// /messages 也必须携带当前 goal 快照,否则用户看不到停止 /goal 的入口。
TEST(WebServerHttp, GetMessagesIncludesCurrentGoalSnapshot) {
    WebServerFixture fx;
    auto post = cpr::Post(cpr::Url{fx.url("/api/sessions")},
                          cpr::Header{{"Content-Type", "application/json"}},
                          cpr::Body{R"({})"});
    ASSERT_EQ(post.status_code, 201);
    auto sid = json::parse(post.text)["session_id"].get<std::string>();

    auto* entry = fx.registry->lookup(sid);
    ASSERT_NE(entry, nullptr);
    ASSERT_TRUE(entry->sm);
    ASSERT_TRUE(entry->sm->goal_store()->replace_thread_goal(
        sid, "stop this goal from web", std::nullopt, acecode::ThreadGoalStatus::Active));

    auto r = cpr::Get(cpr::Url{fx.url("/api/sessions/" + sid + "/messages")});
    ASSERT_EQ(r.status_code, 200) << r.text;
    auto j = json::parse(r.text);
    ASSERT_TRUE(j.contains("goal")) << r.text;
    EXPECT_EQ(j["goal"]["status"], "active");
    EXPECT_EQ(j["goal"]["objective"], "stop this goal from web");
    ASSERT_TRUE(j.contains("busy")) << r.text;
    EXPECT_FALSE(j["busy"].get<bool>());
}

// 场景:桌面 TodoWrite 底栏清空按钮走 workspace-scoped API,必须同时清掉
// active SessionManager 内存状态和 meta 快照,否则刷新历史后待办事项会回来。
TEST(WebServerHttp, ClearWorkspaceSessionTodosPersistsEmptyState) {
    WebServerFixture fx;
    const std::string hash = acecode::compute_cwd_hash(fx.cwd);
    auto post = cpr::Post(cpr::Url{fx.url("/api/workspaces/" + hash + "/sessions")},
                          cpr::Header{{"Content-Type", "application/json"}},
                          cpr::Body{R"({})"});
    ASSERT_EQ(post.status_code, 201) << post.text;
    auto sid = json::parse(post.text)["session_id"].get<std::string>();

    auto* entry = fx.registry->lookup(sid);
    ASSERT_NE(entry, nullptr);
    ASSERT_TRUE(entry->sm);
    entry->sm->set_todos({
        acecode::TodoItem{"todo-1", "List current folder content", "completed"},
        acecode::TodoItem{"todo-2", "Summarize result", "pending"},
    });
    ASSERT_EQ(entry->sm->current_todos().size(), 2u);

    auto clear = cpr::Delete(cpr::Url{fx.url("/api/workspaces/" + hash + "/sessions/" + sid + "/todos")});
    ASSERT_EQ(clear.status_code, 200) << clear.text;
    auto body = json::parse(clear.text);
    EXPECT_EQ(body["session_id"], sid);
    ASSERT_TRUE(body["todos"].is_array());
    EXPECT_TRUE(body["todos"].empty());
    EXPECT_EQ(body["todo_summary"]["total"], 0);
    EXPECT_EQ(body["todo_summary"]["completed"], 0);
    EXPECT_TRUE(entry->sm->current_todos().empty());

    auto meta = acecode::SessionStorage::read_meta(
        acecode::SessionStorage::meta_path(fx.project_dir, sid));
    EXPECT_TRUE(meta.todos.empty());
}

// 场景: GET /api/sessions/:id/messages?since=N 走 replay-only 路径,返回的是
// 数组(不带 messages 包裹)。SubscribeListener 在事件数=0 时返回空数组 200。
TEST(WebServerHttp, GetMessagesWithSinceReturnsArrayOnly) {
    WebServerFixture fx;
    auto post = cpr::Post(cpr::Url{fx.url("/api/sessions")},
                          cpr::Header{{"Content-Type", "application/json"}},
                          cpr::Body{R"({})"});
    auto sid = json::parse(post.text)["session_id"].get<std::string>();

    auto r = cpr::Get(cpr::Url{fx.url("/api/sessions/" + sid + "/messages?since=100")});
    ASSERT_EQ(r.status_code, 200);
    auto j = json::parse(r.text);
    EXPECT_TRUE(j.is_array());
}

// 场景:POST /api/sessions/:id/messages 只负责把输入交给 daemon 的
// AgentLoop 入队,不依赖当前 WebSocket 连接是否还绑定该会话。
TEST(WebServerHttp, PostMessageQueuesInputInDaemonSession) {
    WebServerFixture fx;
    auto post = cpr::Post(cpr::Url{fx.url("/api/sessions")},
                          cpr::Header{{"Content-Type", "application/json"}},
                          cpr::Body{R"({})"});
    ASSERT_EQ(post.status_code, 201);
    auto sid = json::parse(post.text)["session_id"].get<std::string>();

    auto queued = cpr::Post(cpr::Url{fx.url("/api/sessions/" + sid + "/messages")},
                            cpr::Header{{"Content-Type", "application/json"}},
                            cpr::Body{R"({"text":"hello from http submit","client_message_id":"queued-test-1"})"});
    ASSERT_EQ(queued.status_code, 202) << queued.text;
    EXPECT_TRUE(json::parse(queued.text)["queued"].get<bool>());

    bool found = false;
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline && !found) {
        auto r = cpr::Get(cpr::Url{fx.url("/api/sessions/" + sid + "/messages")});
        ASSERT_EQ(r.status_code, 200) << r.text;
        auto j = json::parse(r.text);
        for (const auto& m : j["messages"]) {
            if (m.value("role", "") == "user" &&
                m.value("content", "") == "hello from http submit" &&
                m.contains("metadata") && m["metadata"].is_object() &&
                m["metadata"].value("client_message_id", "") == "queued-test-1") {
                found = true;
                break;
            }
        }
        if (!found) std::this_thread::sleep_for(20ms);
    }
    EXPECT_TRUE(found) << "HTTP submit should be owned by daemon session";
}

TEST(WebServerHttp, UploadAttachmentAndSubmitContentParts) {
    WebServerFixture fx;
    auto post = cpr::Post(cpr::Url{fx.url("/api/sessions")},
                          cpr::Header{{"Content-Type", "application/json"}},
                          cpr::Body{R"({})"});
    ASSERT_EQ(post.status_code, 201);
    auto sid = json::parse(post.text)["session_id"].get<std::string>();

    auto upload = cpr::Post(
        cpr::Url{fx.url("/api/sessions/" + sid + "/attachments")},
        cpr::Header{{"Content-Type", "application/json"}},
        cpr::Body{R"({"name":"screen.png","mime_type":"image/png","data_base64":"YWJj"})"});
    ASSERT_EQ(upload.status_code, 201) << upload.text;
    auto attachment = json::parse(upload.text)["attachment"];
    ASSERT_TRUE(attachment["id"].is_string());
    EXPECT_EQ(attachment["kind"], "image");
    const std::string attachment_id = attachment["id"].get<std::string>();

    auto blob = cpr::Get(
        cpr::Url{fx.url("/api/sessions/" + sid + "/attachments/" + attachment_id + "/blob")});
    ASSERT_EQ(blob.status_code, 200) << blob.text;
    EXPECT_EQ(blob.text, "abc");

    auto queued = cpr::Post(
        cpr::Url{fx.url("/api/sessions/" + sid + "/messages")},
        cpr::Header{{"Content-Type", "application/json"}},
        cpr::Body{json{
            {"text", "describe this"},
            {"client_message_id", "queued-attachment-test-1"},
            {"attachments", json::array({json{{"id", attachment_id}}})},
        }.dump()});
    ASSERT_EQ(queued.status_code, 202) << queued.text;

    bool found = false;
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline && !found) {
        auto r = cpr::Get(cpr::Url{fx.url("/api/sessions/" + sid + "/messages")});
        ASSERT_EQ(r.status_code, 200) << r.text;
        auto j = json::parse(r.text);
        for (const auto& m : j["messages"]) {
            if (m.value("role", "") != "user" ||
                m.value("content", "") != "describe this" ||
                !m.contains("content_parts") ||
                !m.contains("metadata") || !m["metadata"].is_object() ||
                m["metadata"].value("client_message_id", "") != "queued-attachment-test-1") {
                continue;
            }
            const auto& parts = m["content_parts"];
            if (parts.is_array() && parts.size() == 2 &&
                parts[1].value("type", "") == "image" &&
                parts[1]["attachment"].value("id", "") == attachment_id) {
                found = true;
                break;
            }
        }
        if (!found) std::this_thread::sleep_for(20ms);
    }
    EXPECT_TRUE(found) << "attachment content_parts should be persisted";
}

TEST(WebServerHttp, PostBuiltinCommandRejectsUnknownSession) {
    WebServerFixture fx;
    auto r = cpr::Post(cpr::Url{fx.url("/api/sessions/missing-session/commands")},
                       cpr::Header{{"Content-Type", "application/json"}},
                       cpr::Body{R"({"command":"init"})"});

    EXPECT_EQ(r.status_code, 404);
    auto body = json::parse(r.text);
    EXPECT_EQ(body["error"], "unknown session");
}

TEST(WebServerHttp, PostBuiltinCommandRejectsUnsupportedCommand) {
    WebServerFixture fx;
    auto post = cpr::Post(cpr::Url{fx.url("/api/sessions")},
                          cpr::Header{{"Content-Type", "application/json"}},
                          cpr::Body{R"({})"});
    ASSERT_EQ(post.status_code, 201);
    auto sid = json::parse(post.text)["session_id"].get<std::string>();

    auto r = cpr::Post(cpr::Url{fx.url("/api/sessions/" + sid + "/commands")},
                       cpr::Header{{"Content-Type", "application/json"}},
                       cpr::Body{R"({"command":"model"})"});

    EXPECT_EQ(r.status_code, 400);
    auto body = json::parse(r.text);
    EXPECT_EQ(body["error"], "unsupported command");
    EXPECT_EQ(body["command"], "model");
}

TEST(WebServerHttp, PostSideQuestionValidatesSessionQuestionAndContext) {
    WebServerFixture fx;

    auto invalid = cpr::Post(
        cpr::Url{fx.url("/api/sessions/missing-session/side-question")},
        cpr::Header{{"Content-Type", "application/json"}},
        cpr::Body{R"({"question":42})"});
    ASSERT_EQ(invalid.status_code, 400) << invalid.text;
    EXPECT_EQ(json::parse(invalid.text)["error"], "INVALID_SIDE_QUESTION");

    auto missing = cpr::Post(
        cpr::Url{fx.url("/api/sessions/missing-session/side-question")},
        cpr::Header{{"Content-Type", "application/json"}},
        cpr::Body{R"({"question":"why?"})"});
    ASSERT_EQ(missing.status_code, 404) << missing.text;
    EXPECT_EQ(json::parse(missing.text)["error"], "UNKNOWN_SESSION");

    auto create = cpr::Post(cpr::Url{fx.url("/api/sessions")},
                            cpr::Header{{"Content-Type", "application/json"}},
                            cpr::Body{R"({})"});
    ASSERT_EQ(create.status_code, 201) << create.text;
    auto sid = json::parse(create.text)["session_id"].get<std::string>();

    auto not_ready = cpr::Post(
        cpr::Url{fx.url("/api/sessions/" + sid + "/side-question")},
        cpr::Header{{"Content-Type", "application/json"}},
        cpr::Body{R"({"question":"why?"})"});
    ASSERT_EQ(not_ready.status_code, 409) << not_ready.text;
    EXPECT_EQ(json::parse(not_ready.text)["error"],
              "SIDE_QUESTION_CONTEXT_NOT_READY");
}

// 场景: inactive 磁盘历史不在 registry 内存里时,GET messages 也应能返回
// ChatMessage 历史,让 Web 点击历史会话前可预览/补齐。
TEST(WebServerHttp, GetMessagesForInactiveDiskSessionReturnsHistory) {
    WebServerFixture fx;
    const std::string sid = "20260502-010203-abcd";
    acecode::ChatMessage msg;
    msg.role = "user";
    msg.content = "old disk prompt";
    std::filesystem::create_directories(fx.project_dir);
    acecode::SessionStorage::append_message(
        acecode::SessionStorage::session_path(fx.project_dir, sid, 0), msg);
    acecode::SessionMeta meta;
    meta.id = sid;
    meta.cwd = fx.cwd;
    meta.created_at = "2026-05-02T01:02:03Z";
    meta.updated_at = meta.created_at;
    meta.message_count = 1;
    meta.turn_count = 1;
    meta.permission_mode = "accept-edits";
    meta.last_token_usage.prompt_tokens = 32000;
    meta.last_token_usage.total_tokens = 33000;
    meta.last_token_usage.has_data = true;
    acecode::SessionStorage::write_meta(
        acecode::SessionStorage::meta_path(fx.project_dir, sid, 0), meta);

    auto r = cpr::Get(cpr::Url{fx.url("/api/sessions/" + sid + "/messages")});
    ASSERT_EQ(r.status_code, 200) << r.text;
    auto j = json::parse(r.text);
    ASSERT_TRUE(j.contains("messages"));
    ASSERT_EQ(j["messages"].size(), 1u);
    EXPECT_EQ(j["messages"][0]["content"], "old disk prompt");
    EXPECT_EQ(j["turn_count"], 1);
    EXPECT_EQ(j["permission_mode"], "accept-edits");
    ASSERT_TRUE(j["token_usage"].is_object());
    EXPECT_EQ(j["token_usage"]["prompt_tokens"], 32000);
}

TEST(WebServerHttp, RestoreFileCheckpointRestoresTrackedFile) {
    WebServerFixture fx;
    auto post = cpr::Post(cpr::Url{fx.url("/api/sessions")},
                          cpr::Header{{"Content-Type", "application/json"}},
                          cpr::Body{R"({})"});
    ASSERT_EQ(post.status_code, 201) << post.text;
    auto sid = json::parse(post.text)["session_id"].get<std::string>();

    auto* entry = fx.registry->lookup(sid);
    ASSERT_NE(entry, nullptr);
    ASSERT_NE(entry->sm, nullptr);

    const std::string message_id = "checkpoint-user-1";
    const auto file_path = fx.cwd_dir / "tracked.txt";
    {
        std::ofstream out(file_path, std::ios::binary);
        out << "before\n";
    }
    entry->sm->begin_user_turn_checkpoint(message_id);
    entry->sm->track_file_write_before(file_path.string());
    {
        std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
        out << "after\n";
    }

    auto restore = cpr::Post(cpr::Url{
        fx.url("/api/sessions/" + sid + "/file-checkpoints/" + message_id + "/restore")},
        cpr::Header{{"Content-Type", "application/json"}},
        cpr::Body{R"({})"});
    ASSERT_EQ(restore.status_code, 200) << restore.text;
    auto body = json::parse(restore.text);
    EXPECT_TRUE(body["ok"].get<bool>());
    ASSERT_EQ(body["files_changed"].size(), 1u);

    std::ifstream in(file_path, std::ios::binary);
    std::string restored((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(restored, "before\n");
}

TEST(WebServerHttp, RestoreFileCheckpointRejectsMissingCheckpoint) {
    WebServerFixture fx;
    auto post = cpr::Post(cpr::Url{fx.url("/api/sessions")},
                          cpr::Header{{"Content-Type", "application/json"}},
                          cpr::Body{R"({})"});
    ASSERT_EQ(post.status_code, 201) << post.text;
    auto sid = json::parse(post.text)["session_id"].get<std::string>();

    auto restore = cpr::Post(cpr::Url{
        fx.url("/api/sessions/" + sid + "/file-checkpoints/missing-message/restore")},
        cpr::Header{{"Content-Type", "application/json"}},
        cpr::Body{R"({})"});
    ASSERT_EQ(restore.status_code, 404) << restore.text;
    auto body = json::parse(restore.text);
    EXPECT_EQ(body["error"], "file checkpoint not found");
}

// 场景: POST /api/sessions/:id/resume 把 inactive 磁盘历史恢复进 registry,
// 之后 list_sessions 应标记 active=true。
TEST(WebServerHttp, ResumeDiskSessionActivatesIt) {
    WebServerFixture fx;
    const std::string sid = "20260502-010204-abcd";
    acecode::SessionManager sm;
    sm.start_session(fx.cwd, "test-provider", "test-model", sid);
    acecode::ChatMessage msg;
    msg.role = "user";
    msg.content = "resume me";
    sm.on_message(msg);
    sm.finalize();

    auto post = cpr::Post(cpr::Url{fx.url("/api/sessions/" + sid + "/resume")},
                          cpr::Header{{"Content-Type", "application/json"}},
                          cpr::Body{R"({})"});
    ASSERT_EQ(post.status_code, 200) << post.text;

    auto list = cpr::Get(cpr::Url{fx.url("/api/sessions")});
    ASSERT_EQ(list.status_code, 200);
    auto arr = json::parse(list.text);
    bool found = false;
    for (const auto& s : arr) {
        if (s["id"] == sid) {
            found = true;
            EXPECT_TRUE(s["active"].get<bool>());
        }
    }
    EXPECT_TRUE(found);
}

// 场景: /api/skills 对 daemon workspace 做全量扫描:workspace 项目链下的
// skill 出现在结果里,带 source="project" 与 enabled=true(设置页技能 tab
// 的「项目技能」分组依赖它)。全局根扫的是真实用户目录,内容因机器而异,
// 所以这里只断言项目 skill 的存在与元数据,不断言数组总量。
TEST(WebServerHttp, SkillsListsProjectSkillWithSource) {
    WebServerFixture fx;
    // 在 fixture workspace 的项目链根放一个 skill
    const auto skill_dir = fx.cwd_dir / ".acecode" / "skills" / "fixture-skill";
    std::filesystem::create_directories(skill_dir);
    {
        std::ofstream ofs(skill_dir / "SKILL.md", std::ios::binary);
        ofs << "---\nname: fixture-skill\ndescription: fixture description\n---\n\nbody\n";
    }

    auto r = cpr::Get(cpr::Url{fx.url("/api/skills")});
    ASSERT_EQ(r.status_code, 200);
    auto j = json::parse(r.text);
    ASSERT_TRUE(j.is_array());

    bool found = false;
    for (const auto& o : j) {
        if (o.value("name", "") != "fixture-skill") continue;
        found = true;
        EXPECT_EQ(o["source"], "project");
        EXPECT_TRUE(o["enabled"].get<bool>());
        EXPECT_EQ(o["description"], "fixture description");
    }
    EXPECT_TRUE(found);
}

// 场景: GET /api/skills/root 返回选中 workspace 的有效 skill 目录。
TEST(WebServerHttp, SkillRootReturnsProjectAcecodeSkillsForWorkspace) {
    WebServerFixture fx;
    const auto local_skills = fx.cwd_dir / ".acecode" / "skills";
    std::filesystem::create_directories(local_skills);
    const std::string hash = acecode::compute_cwd_hash(fx.cwd);

    auto r = cpr::Get(cpr::Url{fx.url("/api/skills/root?workspace=" + hash)});
    ASSERT_EQ(r.status_code, 200) << r.text;
    auto body = json::parse(r.text);
    EXPECT_EQ(body["source"], "project_acecode");
    EXPECT_EQ(body["workspace_hash"], hash);
    EXPECT_EQ(std::filesystem::weakly_canonical(path_from_utf8(body["path"].get<std::string>())),
              std::filesystem::weakly_canonical(local_skills));
}

// 场景:首次进入 workspace 时还没有 skill 目录,点击左下角 Skills 入口应先创建
// 项目级 .acecode/skills,再返回可打开的路径。
TEST(WebServerHttp, SkillRootCreatesProjectAcecodeSkillsForWorkspace) {
    WebServerFixture fx;
    const auto local_skills = fx.cwd_dir / ".acecode" / "skills";
    const std::string hash = acecode::compute_cwd_hash(fx.cwd);
    ASSERT_FALSE(std::filesystem::exists(local_skills));

    auto r = cpr::Get(cpr::Url{fx.url("/api/skills/root?workspace=" + hash)});
    ASSERT_EQ(r.status_code, 200) << r.text;
    auto body = json::parse(r.text);
    EXPECT_EQ(body["source"], "project_acecode");
    EXPECT_TRUE(std::filesystem::is_directory(local_skills));
    EXPECT_EQ(std::filesystem::weakly_canonical(path_from_utf8(body["path"].get<std::string>())),
              std::filesystem::weakly_canonical(local_skills));
}

// 场景:未知 workspace 不能通过 /api/skills/root 获取任意文件系统路径。
TEST(WebServerHttp, SkillRootRejectsUnknownWorkspace) {
    WebServerFixture fx;

    auto r = cpr::Get(cpr::Url{fx.url("/api/skills/root?workspace=missing-workspace")});
    ASSERT_EQ(r.status_code, 404) << r.text;
    auto body = json::parse(r.text);
    EXPECT_EQ(body["error"], "workspace not found");
    EXPECT_FALSE(body.contains("path"));
}

// 场景: GET/PUT /api/mcp 互通 — PUT 写入 + GET 读回内容一致(spec 9.8)。
// auth_token 字段不回写(避免暴露)。
TEST(WebServerHttp, McpPutThenGetRoundtrip) {
    WebServerFixture fx;
    json req;
    req["test-server"] = {{"transport", "stdio"},
                          {"command", "/usr/bin/python3"},
                          {"args", json::array({"-m", "myserver"})}};
    auto put = cpr::Put(cpr::Url{fx.url("/api/mcp")},
                        cpr::Header{{"Content-Type", "application/json"}},
                        cpr::Body{req.dump()});
    EXPECT_EQ(put.status_code, 200);

    auto get = cpr::Get(cpr::Url{fx.url("/api/mcp")});
    ASSERT_EQ(get.status_code, 200);
    auto j = json::parse(get.text);
    ASSERT_TRUE(j.contains("test-server"));
    EXPECT_EQ(j["test-server"]["transport"], "stdio");
    EXPECT_EQ(j["test-server"]["command"], "/usr/bin/python3");
}

// 场景: POST /api/mcp/reload 返回 501(v1 未实装,spec 9.9 文档化的限制)。
TEST(WebServerHttp, McpReloadIsNotImplemented) {
    WebServerFixture fx;
    auto r = cpr::Post(cpr::Url{fx.url("/api/mcp/reload")});
    EXPECT_EQ(r.status_code, 501);
}

// 场景: POST /api/mcp/toggle 翻转某 server 的启用态。fixture 未挂 McpManager,
// 所以 applied=false(仅落盘,不热切换),但配置 disabled 应立刻反映到 GET。
// 期望:关闭 → GET 带 disabled:true;再启用 → GET 不带 disabled 键(稀疏)。
// 回归:设置页开关经此端点持久化,漏落盘会让重启后开关状态还原。
TEST(WebServerHttp, McpToggleFlipsDisabledFlag) {
    WebServerFixture fx;
    json req;
    req["toggle-server"] = {{"transport", "stdio"},
                            {"command", "/usr/bin/python3"},
                            {"args", json::array({"-m", "myserver"})}};
    auto put = cpr::Put(cpr::Url{fx.url("/api/mcp")},
                        cpr::Header{{"Content-Type", "application/json"}},
                        cpr::Body{req.dump()});
    ASSERT_EQ(put.status_code, 200);

    // 关闭:落盘 disabled=true,无 manager 时 applied=false。
    auto off = cpr::Post(cpr::Url{fx.url("/api/mcp/toggle")},
                         cpr::Header{{"Content-Type", "application/json"}},
                         cpr::Body{json{{"name", "toggle-server"}, {"enabled", false}}.dump()});
    ASSERT_EQ(off.status_code, 200) << off.text;
    auto off_body = json::parse(off.text);
    EXPECT_EQ(off_body["enabled"], false);
    EXPECT_EQ(off_body["applied"], false);

    auto get_off = cpr::Get(cpr::Url{fx.url("/api/mcp")});
    ASSERT_EQ(get_off.status_code, 200);
    EXPECT_TRUE(json::parse(get_off.text)["toggle-server"].value("disabled", false));

    // 再启用:disabled 键消失。
    auto on = cpr::Post(cpr::Url{fx.url("/api/mcp/toggle")},
                        cpr::Header{{"Content-Type", "application/json"}},
                        cpr::Body{json{{"name", "toggle-server"}, {"enabled", true}}.dump()});
    ASSERT_EQ(on.status_code, 200);
    auto get_on = cpr::Get(cpr::Url{fx.url("/api/mcp")});
    ASSERT_EQ(get_on.status_code, 200);
    EXPECT_FALSE(json::parse(get_on.text)["toggle-server"].contains("disabled"));

    // 未知 server → 404。
    auto missing = cpr::Post(cpr::Url{fx.url("/api/mcp/toggle")},
                             cpr::Header{{"Content-Type", "application/json"}},
                             cpr::Body{json{{"name", "nope"}, {"enabled", false}}.dump()});
    EXPECT_EQ(missing.status_code, 404);
}

// 场景: 未知路由返回 404(Crow 默认行为,这里只是验证我们没把 / 放飞)。
TEST(WebServerHttp, UnknownRouteReturns404) {
    WebServerFixture fx;
    auto r = cpr::Get(cpr::Url{fx.url("/api/does-not-exist")});
    EXPECT_EQ(r.status_code, 404);
}

// 场景:首次进入 Desktop 时,当前 guide version 尚未 dismiss。
TEST(WebServerHttp, GetDesktopOnboardingReturnsUnseenVersion) {
    WebServerFixture fx;
    auto r = cpr::Get(cpr::Url{fx.url("/api/ui/onboarding/desktop")});
    ASSERT_EQ(r.status_code, 200) << r.text;
    auto body = json::parse(r.text);
    EXPECT_EQ(body["guide_version"], 1);
    EXPECT_EQ(body["dismissed"], false);
}

// 场景:dismiss 是幂等操作,且不会覆盖 state.json 中其它字段。
TEST(WebServerHttp, DismissDesktopOnboardingIsDurableAndIdempotent) {
    WebServerFixture fx;
    acecode::write_state_flag("another_flag", true);

    for (int attempt = 0; attempt < 2; ++attempt) {
        auto r = cpr::Post(cpr::Url{fx.url("/api/ui/onboarding/desktop/dismiss")});
        ASSERT_EQ(r.status_code, 200) << r.text;
        auto body = json::parse(r.text);
        EXPECT_EQ(body["guide_version"], 1);
        EXPECT_EQ(body["dismissed"], true);
    }

    EXPECT_TRUE(acecode::read_state_flag("desktop_guided_tour_v1_dismissed"));
    EXPECT_TRUE(acecode::read_state_flag("another_flag"));
}

// 场景:跨端口 Web/Desktop 请求必须带 daemon token。
TEST(WebServerHttp, DesktopOnboardingCrossOriginRequiresToken) {
    WebServerFixture fx;
    const std::string origin = "http://localhost:5173";
    auto denied = cpr::Get(
        cpr::Url{fx.url("/api/ui/onboarding/desktop")},
        cpr::Header{{"Origin", origin}});
    EXPECT_EQ(denied.status_code, 401);

    auto allowed = cpr::Get(
        cpr::Url{fx.url("/api/ui/onboarding/desktop")},
        cpr::Header{{"Origin", origin}, {"X-ACECode-Token", "smoke-token"}});
    EXPECT_EQ(allowed.status_code, 200);
}

// 场景:落盘失败时 endpoint 不得谎报 dismissed=true。
TEST(WebServerHttp, DismissDesktopOnboardingReportsPersistenceFailure) {
    WebServerFixture fx;
    auto directory_target = fx.tmp_dir / "state-directory";
    std::filesystem::create_directories(directory_target);
    acecode::set_state_file_path_for_test(directory_target.string());

    auto r = cpr::Post(cpr::Url{fx.url("/api/ui/onboarding/desktop/dismiss")});
    ASSERT_EQ(r.status_code, 500) << r.text;
    auto body = json::parse(r.text);
    EXPECT_EQ(body["error"], "PERSIST_FAILED");

    acecode::set_state_file_path_for_test(fx.state_file_path.string());
    EXPECT_FALSE(acecode::read_state_flag("desktop_guided_tour_v1_dismissed"));
}

// 场景:GET /api/config/ui-preferences 返回固定隐藏头像的兼容值。
TEST(WebServerHttp, GetUiPreferencesReturnsAvatarDefault) {
    WebServerFixture fx;
    auto r = cpr::Get(cpr::Url{fx.url("/api/config/ui-preferences")});
    ASSERT_EQ(r.status_code, 200) << r.text;
    auto j = json::parse(r.text);
    ASSERT_TRUE(j.contains("show_acecode_avatar"));
    EXPECT_EQ(j["show_acecode_avatar"], false);
}

// 场景:PUT /api/config/ui-preferences 保持兼容,但头像显示固定为 false。
TEST(WebServerHttp, PutUiPreferencesNormalizesAvatarSettingToFalse) {
    WebServerFixture fx;
    json req = {{"show_acecode_avatar", true}};
    auto put = cpr::Put(cpr::Url{fx.url("/api/config/ui-preferences")},
                        cpr::Header{{"Content-Type", "application/json"}},
                        cpr::Body{req.dump()});
    ASSERT_EQ(put.status_code, 200) << put.text;
    auto body = json::parse(put.text);
    EXPECT_EQ(body["show_acecode_avatar"], false);
    EXPECT_FALSE(fx.cfg.web_ui.show_acecode_avatar);

    std::ifstream ifs(fx.tmp_dir / "config.json");
    ASSERT_TRUE(ifs.is_open());
    auto saved = json::parse(ifs);
    EXPECT_FALSE(saved.contains("web_ui"));
}

// 场景:PUT /api/config/ui-preferences 非 bool payload 被拒绝且不改 cfg。
TEST(WebServerHttp, PutUiPreferencesRejectsInvalidAvatarPayload) {
    WebServerFixture fx;
    json req = {{"show_acecode_avatar", "false"}};
    auto r = cpr::Put(cpr::Url{fx.url("/api/config/ui-preferences")},
                      cpr::Header{{"Content-Type", "application/json"}},
                      cpr::Body{req.dump()});
    ASSERT_EQ(r.status_code, 400) << r.text;
    auto j = json::parse(r.text);
    EXPECT_EQ(j["error"], "BAD_REQUEST");
    EXPECT_FALSE(fx.cfg.web_ui.show_acecode_avatar);
}

TEST(WebServerHttp, GetCustomInstructionsReturnsCurrentText) {
    WebServerFixture fx;
    fx.cfg.custom_instructions.set_text("Prefer concise Chinese.");

    auto r = cpr::Get(cpr::Url{fx.url("/api/config/custom-instructions")});
    ASSERT_EQ(r.status_code, 200) << r.text;
    auto j = json::parse(r.text);
    ASSERT_TRUE(j.contains("text"));
    EXPECT_EQ(j["text"], "Prefer concise Chinese.");
}

TEST(WebServerHttp, PutCustomInstructionsPersistsText) {
    WebServerFixture fx;
    json req = {{"text", "Use project-specific terminology."}};
    auto put = cpr::Put(cpr::Url{fx.url("/api/config/custom-instructions")},
                        cpr::Header{{"Content-Type", "application/json"}},
                        cpr::Body{req.dump()});
    ASSERT_EQ(put.status_code, 200) << put.text;
    auto body = json::parse(put.text);
    EXPECT_EQ(body["text"], "Use project-specific terminology.");
    EXPECT_EQ(fx.cfg.custom_instructions.text_snapshot(),
              "Use project-specific terminology.");

    std::ifstream ifs(fx.tmp_dir / "config.json");
    ASSERT_TRUE(ifs.is_open());
    auto saved = json::parse(ifs);
    ASSERT_TRUE(saved.contains("custom_instructions"));
    EXPECT_EQ(saved["custom_instructions"]["text"],
              "Use project-specific terminology.");
}

TEST(WebServerHttp, PutCustomInstructionsRejectsInvalidPayload) {
    WebServerFixture fx;
    fx.cfg.custom_instructions.set_text("keep me");
    json req = {{"text", false}};
    auto r = cpr::Put(cpr::Url{fx.url("/api/config/custom-instructions")},
                      cpr::Header{{"Content-Type", "application/json"}},
                      cpr::Body{req.dump()});
    ASSERT_EQ(r.status_code, 400) << r.text;
    auto j = json::parse(r.text);
    EXPECT_EQ(j["error"], "BAD_REQUEST");
    EXPECT_EQ(fx.cfg.custom_instructions.text_snapshot(), "keep me");
}

TEST(WebServerHttp, GetConnectorsReturnsConfiguredList) {
    WebServerFixture fx;
    fx.cfg.connectors.push_back({
        "alpha-connector",
        "Alpha Connector",
        "Connect alpha providers",
        true,
    });

    auto r = cpr::Get(cpr::Url{fx.url("/api/config/connectors")});
    ASSERT_EQ(r.status_code, 200) << r.text;
    auto j = json::parse(r.text);
    ASSERT_TRUE(j.contains("connectors"));
    ASSERT_EQ(j["connectors"].size(), 1u);
    EXPECT_EQ(j["connectors"][0]["id"], "alpha-connector");
    EXPECT_EQ(j["connectors"][0]["name"], "Alpha Connector");
    EXPECT_EQ(j["connectors"][0]["enabled"], true);
}

TEST(WebServerHttp, PutConnectorsPersistsConfiguredList) {
    WebServerFixture fx;
    json req = {{"connectors", json::array({
        {
            {"id", "alpha-connector"},
            {"name", "Alpha Connector"},
            {"description", "Connect alpha providers"},
            {"enabled", true},
        },
        {
            {"id", "beta-connector"},
            {"name", "Beta Connector"},
            {"description", "Remote conversation channel"},
            {"enabled", false},
        },
    })}};

    auto put = cpr::Put(cpr::Url{fx.url("/api/config/connectors")},
                        cpr::Header{{"Content-Type", "application/json"}},
                        cpr::Body{req.dump()});
    ASSERT_EQ(put.status_code, 200) << put.text;
    auto body = json::parse(put.text);
    ASSERT_EQ(body["connectors"].size(), 2u);
    ASSERT_EQ(fx.cfg.connectors.size(), 2u);
    EXPECT_EQ(fx.cfg.connectors[1].id, "beta-connector");
    EXPECT_FALSE(fx.cfg.connectors[1].enabled);

    std::ifstream ifs(fx.tmp_dir / "config.json");
    ASSERT_TRUE(ifs.is_open());
    auto saved = json::parse(ifs);
    ASSERT_TRUE(saved.contains("connectors"));
    EXPECT_EQ(saved["connectors"][1]["id"], "beta-connector");
    EXPECT_EQ(saved["connectors"][1]["enabled"], false);
}

TEST(WebServerHttp, PutConnectorsRejectsDuplicateIds) {
    WebServerFixture fx;
    fx.cfg.connectors.push_back({"keep", "Keep", "Existing connector", true});
    json req = {{"connectors", json::array({
        {{"id", "same"}, {"name", "One"}, {"description", ""}, {"enabled", true}},
        {{"id", "same"}, {"name", "Two"}, {"description", ""}, {"enabled", false}},
    })}};

    auto r = cpr::Put(cpr::Url{fx.url("/api/config/connectors")},
                      cpr::Header{{"Content-Type", "application/json"}},
                      cpr::Body{req.dump()});
    ASSERT_EQ(r.status_code, 400) << r.text;
    auto j = json::parse(r.text);
    EXPECT_EQ(j["error"], "BAD_REQUEST");
    ASSERT_EQ(fx.cfg.connectors.size(), 1u);
    EXPECT_EQ(fx.cfg.connectors[0].id, "keep");
}

// 场景:GET /api/config/upgrade 返回当前升级服务 URL 默认值。
TEST(WebServerHttp, GetUpgradeConfigReturnsDefaultBaseUrl) {
    WebServerFixture fx;
    auto r = cpr::Get(cpr::Url{fx.url("/api/config/upgrade")});
    ASSERT_EQ(r.status_code, 200) << r.text;
    auto j = json::parse(r.text);
    ASSERT_TRUE(j.contains("base_url"));
    EXPECT_EQ(j["base_url"], "http://2017studio.imwork.net:82/aupdate/");
}

// 场景:PUT /api/config/upgrade 规范化 URL、更新内存并落盘。
TEST(WebServerHttp, PutUpgradeConfigPersistsNormalizedBaseUrl) {
    WebServerFixture fx;
    json req = {{"base_url", " https://updates.example.test/ace "}};
    auto put = cpr::Put(cpr::Url{fx.url("/api/config/upgrade")},
                        cpr::Header{{"Content-Type", "application/json"}},
                        cpr::Body{req.dump()});
    ASSERT_EQ(put.status_code, 200) << put.text;
    auto body = json::parse(put.text);
    EXPECT_EQ(body["base_url"], "https://updates.example.test/ace/");
    EXPECT_EQ(fx.cfg.upgrade.base_url, "https://updates.example.test/ace/");

    std::ifstream ifs(fx.tmp_dir / "config.json");
    ASSERT_TRUE(ifs.is_open());
    auto saved = json::parse(ifs);
    ASSERT_TRUE(saved.contains("upgrade"));
    EXPECT_EQ(saved["upgrade"]["base_url"], "https://updates.example.test/ace/");
}

// 场景:PUT /api/config/upgrade 非 http(s) URL 被拒绝且不改 cfg。
TEST(WebServerHttp, PutUpgradeConfigRejectsInvalidBaseUrl) {
    WebServerFixture fx;
    const std::string before = fx.cfg.upgrade.base_url;
    json req = {{"base_url", "ftp://updates.example.test/ace"}};
    auto r = cpr::Put(cpr::Url{fx.url("/api/config/upgrade")},
                      cpr::Header{{"Content-Type", "application/json"}},
                      cpr::Body{req.dump()});
    ASSERT_EQ(r.status_code, 400) << r.text;
    auto j = json::parse(r.text);
    EXPECT_EQ(j["error"], "BAD_REQUEST");
    EXPECT_EQ(fx.cfg.upgrade.base_url, before);
}

TEST(WebServerHttp, DesktopFeedbackRecentSessionsReturnsNewestFirst) {
    WebServerFixture fx;
    const std::string older_id = "20260618-010000-abcd";
    const std::string newer_id = "20260618-020000-abce";

    acecode::SessionMeta older;
    older.id = older_id;
    older.cwd = fx.cwd;
    older.created_at = "2026-06-18T01:00:00Z";
    older.updated_at = "2026-06-18T01:00:00Z";
    older.title = "Older";
    acecode::SessionMeta newer;
    newer.id = newer_id;
    newer.cwd = fx.cwd;
    newer.created_at = "2026-06-18T02:00:00Z";
    newer.updated_at = "2026-06-18T02:00:00Z";
    newer.title = "Newer";
    acecode::SessionStorage::write_meta(
        acecode::SessionStorage::meta_path(fx.project_dir, older_id), older);
    acecode::SessionStorage::write_meta(
        acecode::SessionStorage::meta_path(fx.project_dir, newer_id), newer);
    write_text(path_from_utf8(acecode::SessionStorage::session_path(fx.project_dir, older_id)), "{}\n");
    write_text(path_from_utf8(acecode::SessionStorage::session_path(fx.project_dir, newer_id)), "{}\n");

    auto r = cpr::Get(cpr::Url{fx.url("/api/feedback/desktop/recent-sessions?limit=1")});
    ASSERT_EQ(r.status_code, 200) << r.text;
    auto body = json::parse(r.text);
    ASSERT_TRUE(body["sessions"].is_array());
    ASSERT_EQ(body["sessions"].size(), 1u);
    EXPECT_EQ(body["sessions"][0]["id"], newer_id);
}

TEST(WebServerHttp, DesktopFeedbackUploadsLogOnlyWithoutSession) {
    std::filesystem::path received_zip;
    LocalUpdateServer upload_server([&](httplib::Server& s) {
        s.Post("/", [&](const httplib::Request& req, httplib::Response& res) {
            EXPECT_TRUE(req.is_multipart_form_data());
            auto file = req.get_file_value("file");
            received_zip = std::filesystem::temp_directory_path() /
                           ("acecode_desktop_feedback_log_only_" +
                            std::to_string(std::chrono::steady_clock::now()
                                               .time_since_epoch()
                                               .count()) + ".zip");
            write_text(received_zip, file.content);
            res.set_content(R"({"success":true})", "application/json");
        });
    });

    WebServerFixture fx;
    fx.cfg.upgrade.base_url = upload_server.base_url();
    write_text(fx.logs_dir / "desktop-2026-06-18.log", "desktop latest log");

    auto r = cpr::Post(cpr::Url{fx.url("/api/feedback/desktop")},
                       cpr::Header{{"Content-Type", "application/json"}},
                       cpr::Body{json{{"feedback_text", "desktop froze"}}.dump()});
    ASSERT_EQ(r.status_code, 200) << r.text;
    auto body = json::parse(r.text);
    EXPECT_TRUE(body["ok"].get<bool>());
    EXPECT_TRUE(body["selected_session_id"].is_null());
    EXPECT_EQ(read_zip_entry(received_zip, "logs/desktop.log.tail.txt"),
              "desktop latest log");
    EXPECT_FALSE(zip_entry_exists(received_zip, "session/session.jsonl"));
    auto metadata = json::parse(read_zip_entry(received_zip, "feedback.json"));
    EXPECT_EQ(metadata["source"], "desktop");
    EXPECT_EQ(metadata["feedback_text"], "desktop froze");
    EXPECT_TRUE(metadata["session_id"].is_null());
    EXPECT_TRUE(metadata["selected_session_id"].is_null());
    EXPECT_TRUE(metadata["log_available"].get<bool>());
    std::error_code ec;
    std::filesystem::remove(received_zip, ec);
}

TEST(WebServerHttp, DesktopFeedbackUploadsSelectedSessionOnly) {
    const std::string sid = "20260618-030000-abcf";
    std::filesystem::path received_zip;
    LocalUpdateServer upload_server([&](httplib::Server& s) {
        s.Post("/", [&](const httplib::Request& req, httplib::Response& res) {
            auto file = req.get_file_value("file");
            received_zip = std::filesystem::temp_directory_path() /
                           ("acecode_desktop_feedback_selected_" +
                            std::to_string(std::chrono::steady_clock::now()
                                               .time_since_epoch()
                                               .count()) + ".zip");
            write_text(received_zip, file.content);
            res.set_content(R"({"success":true})", "application/json");
        });
    });

    WebServerFixture fx;
    fx.cfg.upgrade.base_url = upload_server.base_url();
    write_text(fx.logs_dir / "desktop-2026-06-18.log", "desktop log");
    write_text(path_from_utf8(acecode::SessionStorage::session_path(fx.project_dir, sid)),
               "{\"role\":\"user\",\"content\":\"selected\"}\n");
    acecode::SessionMeta meta;
    meta.id = sid;
    meta.cwd = fx.cwd;
    meta.created_at = "2026-06-18T03:00:00Z";
    meta.updated_at = "2026-06-18T03:00:00Z";
    acecode::SessionStorage::write_meta(
        acecode::SessionStorage::meta_path(fx.project_dir, sid), meta);

    auto r = cpr::Post(cpr::Url{fx.url("/api/feedback/desktop")},
                       cpr::Header{{"Content-Type", "application/json"}},
                       cpr::Body{json{{"feedback_text", "with selected session"},
                                      {"session_id", sid},
                                      {"workspace_hash", acecode::compute_cwd_hash(fx.cwd)}}.dump()});
    ASSERT_EQ(r.status_code, 200) << r.text;
    auto body = json::parse(r.text);
    EXPECT_EQ(body["selected_session_id"], sid);
    EXPECT_EQ(read_zip_entry(received_zip, "session/" + sid + ".jsonl"),
              "{\"role\":\"user\",\"content\":\"selected\"}\n");
    EXPECT_FALSE(zip_entry_exists(received_zip, "session/other.jsonl"));
    auto metadata = json::parse(read_zip_entry(received_zip, "feedback.json"));
    EXPECT_EQ(metadata["source"], "desktop");
    EXPECT_EQ(metadata["selected_session_id"], sid);
    EXPECT_EQ(metadata["workspace_hash"], acecode::compute_cwd_hash(fx.cwd));
    std::error_code ec;
    std::filesystem::remove(received_zip, ec);
}

TEST(WebServerHttp, DesktopFeedbackSucceedsWithoutDesktopLog) {
    std::filesystem::path received_zip;
    LocalUpdateServer upload_server([&](httplib::Server& s) {
        s.Post("/", [&](const httplib::Request& req, httplib::Response& res) {
            auto file = req.get_file_value("file");
            received_zip = std::filesystem::temp_directory_path() /
                           ("acecode_desktop_feedback_missing_log_" +
                            std::to_string(std::chrono::steady_clock::now()
                                               .time_since_epoch()
                                               .count()) + ".zip");
            write_text(received_zip, file.content);
            res.set_content(R"({"success":true})", "application/json");
        });
    });

    WebServerFixture fx;
    fx.cfg.upgrade.base_url = upload_server.base_url();

    auto r = cpr::Post(cpr::Url{fx.url("/api/feedback/desktop")},
                       cpr::Header{{"Content-Type", "application/json"}},
                       cpr::Body{json{{"feedback_text", "no log"}}.dump()});
    ASSERT_EQ(r.status_code, 200) << r.text;
    EXPECT_FALSE(zip_entry_exists(received_zip, "logs/desktop.log.tail.txt"));
    auto metadata = json::parse(read_zip_entry(received_zip, "feedback.json"));
    EXPECT_FALSE(metadata["log_available"].get<bool>());
    std::error_code ec;
    std::filesystem::remove(received_zip, ec);
}

TEST(WebServerHttp, DesktopFeedbackRejectsInvalidUpgradeUrlBeforeUpload) {
    WebServerFixture fx;
    fx.cfg.upgrade.base_url = "file:///tmp/feedback";
    write_text(fx.logs_dir / "desktop-2026-06-18.log", "desktop log");

    auto r = cpr::Post(cpr::Url{fx.url("/api/feedback/desktop")},
                       cpr::Header{{"Content-Type", "application/json"}},
                       cpr::Body{json{{"feedback_text", "bad config"}}.dump()});
    ASSERT_EQ(r.status_code, 400) << r.text;
    auto body = json::parse(r.text);
    EXPECT_EQ(body["error"], "BAD_REQUEST");
    EXPECT_TRUE(std::filesystem::is_empty(fx.feedback_dir));
}

TEST(WebServerHttp, DesktopFeedbackUploadFailureReturnsRetainedPackagePath) {
    LocalUpdateServer upload_server([](httplib::Server& s) {
        s.Post("/", [](const httplib::Request&, httplib::Response& res) {
            res.status = 500;
            res.set_content("nope", "text/plain");
        });
    });

    WebServerFixture fx;
    fx.cfg.upgrade.base_url = upload_server.base_url();
    write_text(fx.logs_dir / "desktop-2026-06-18.log", "desktop log");

    auto r = cpr::Post(cpr::Url{fx.url("/api/feedback/desktop")},
                       cpr::Header{{"Content-Type", "application/json"}},
                       cpr::Body{json{{"feedback_text", "upload fails"}}.dump()});
    ASSERT_EQ(r.status_code, 502) << r.text;
    auto body = json::parse(r.text);
    EXPECT_EQ(body["error"], "UPLOAD_FAILED");
    ASSERT_TRUE(body.contains("package_path"));
    const auto retained = path_from_utf8(body["package_path"].get<std::string>());
    EXPECT_TRUE(std::filesystem::is_regular_file(retained));
}

// 场景:GET /api/update/status 只检查 manifest,返回有新版状态。
TEST(WebServerHttp, GetUpdateStatusReportsAvailableVersion) {
    LocalUpdateServer update_server([](httplib::Server& s) {
        s.Get("/aceupdate.json", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(update_manifest_for("9.9.9"), "application/json");
        });
        s.Get("/acecode.zip", [](const httplib::Request&, httplib::Response& res) {
            res.status = 500;
        });
    });
    WebServerFixture fx;
    fx.cfg.upgrade.base_url = update_server.base_url();
    fx.cfg.upgrade.timeout_ms = 3000;

    auto r = cpr::Get(cpr::Url{fx.url("/api/update/status")});
    ASSERT_EQ(r.status_code, 200) << r.text;
    auto j = json::parse(r.text);
    EXPECT_EQ(j["status"], "available");
    EXPECT_EQ(j["update_available"], true);
    EXPECT_EQ(j["latest_version"], "9.9.9");
    EXPECT_EQ(j["package_file"], "acecode.zip");
}

// 场景:POST 创建可轮询 GUI 升级任务,成功后保留重启提示与 latest-job 状态。
TEST(WebServerHttp, PostUpdateStartPublishesSuccessfulGuiJob) {
    LocalUpdateServer update_server([](httplib::Server& s) {
        s.Get("/aceupdate.json", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(update_manifest_for("9.9.9"), "application/json");
        });
    });
    std::atomic<bool> called{false};
    WebServerFixture fx(
        true,
        false,
        {},
        true,
        [&](const acecode::AppConfig&,
            acecode::upgrade::UpgradeProgressCallback publish,
            std::string*) {
            called.store(true);
            acecode::upgrade::UpgradeProgress downloading;
            downloading.phase = acecode::upgrade::UpgradePhase::Downloading;
            downloading.current_version = "0.0.0-test";
            downloading.target_version = "9.9.9";
            downloading.bytes_downloaded = 50;
            downloading.bytes_total = 100;
            publish(downloading);
            acecode::upgrade::UpgradeProgress installing = downloading;
            installing.phase = acecode::upgrade::UpgradePhase::Installing;
            publish(installing);
            acecode::upgrade::UpgradeProgress complete = installing;
            complete.phase = acecode::upgrade::UpgradePhase::Complete;
            complete.backup_dir = "C:/Temp/acecode-backup";
            publish(complete);
            return 0;
        });
    fx.cfg.upgrade.base_url = update_server.base_url();
    fx.cfg.upgrade.timeout_ms = 3000;

    auto r = cpr::Post(cpr::Url{fx.url("/api/update/start")});
    ASSERT_EQ(r.status_code, 202) << r.text;
    auto j = json::parse(r.text);
    EXPECT_EQ(j["started"], true);
    EXPECT_EQ(j["latest_version"], "9.9.9");
    ASSERT_TRUE(j.contains("job_id"));
    const std::string job_id = j["job_id"].get<std::string>();

    json status;
    for (int i = 0; i < 50; ++i) {
        auto poll = cpr::Get(cpr::Url{fx.url("/api/update/jobs/" + job_id)});
        ASSERT_EQ(poll.status_code, 200) << poll.text;
        status = json::parse(poll.text);
        if (status["state"] == "succeeded") break;
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_TRUE(called.load());
    EXPECT_EQ(status["state"], "succeeded");
    EXPECT_EQ(status["phase"], "complete");
    EXPECT_EQ(status["restart_required"], true);
    EXPECT_EQ(status["percent"], 50);
    EXPECT_EQ(status["backup_dir"], "C:/Temp/acecode-backup");

    auto latest = cpr::Get(cpr::Url{fx.url("/api/update/job")});
    ASSERT_EQ(latest.status_code, 200) << latest.text;
    EXPECT_EQ(json::parse(latest.text)["job_id"], job_id);
}

// 场景:两个页面同时点更新,后端只允许第一个任务运行。
TEST(WebServerHttp, PostUpdateStartRejectsConcurrentGuiJob) {
    LocalUpdateServer update_server([](httplib::Server& s) {
        s.Get("/aceupdate.json", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(update_manifest_for("9.9.9"), "application/json");
        });
    });
    struct Gate {
        std::mutex mu;
        std::condition_variable cv;
        bool started = false;
        bool release = false;
    };
    auto gate = std::make_shared<Gate>();
    WebServerFixture fx(
        true,
        false,
        {},
        true,
        [gate](const acecode::AppConfig&,
               acecode::upgrade::UpgradeProgressCallback publish,
               std::string*) {
            acecode::upgrade::UpgradeProgress progress;
            progress.phase = acecode::upgrade::UpgradePhase::Downloading;
            progress.target_version = "9.9.9";
            publish(progress);
            std::unique_lock<std::mutex> lock(gate->mu);
            gate->started = true;
            gate->cv.notify_all();
            gate->cv.wait(lock, [&] { return gate->release; });
            return 0;
        });
    fx.cfg.upgrade.base_url = update_server.base_url();
    fx.cfg.upgrade.timeout_ms = 3000;

    auto first = cpr::Post(cpr::Url{fx.url("/api/update/start")});
    ASSERT_EQ(first.status_code, 202) << first.text;
    const auto first_body = json::parse(first.text);
    {
        std::unique_lock<std::mutex> lock(gate->mu);
        ASSERT_TRUE(gate->cv.wait_for(lock, 2s, [&] { return gate->started; }));
    }

    auto second = cpr::Post(cpr::Url{fx.url("/api/update/start")});
    ASSERT_EQ(second.status_code, 409) << second.text;
    const auto second_body = json::parse(second.text);
    EXPECT_EQ(second_body["error"], "UPDATE_IN_PROGRESS");
    EXPECT_EQ(second_body["job"]["job_id"], first_body["job_id"]);

    {
        std::lock_guard<std::mutex> lock(gate->mu);
        gate->release = true;
    }
    gate->cv.notify_all();
    for (int i = 0; i < 50; ++i) {
        auto poll = cpr::Get(cpr::Url{fx.url(
            "/api/update/jobs/" + first_body["job_id"].get<std::string>())});
        ASSERT_EQ(poll.status_code, 200) << poll.text;
        if (json::parse(poll.text)["state"] == "succeeded") break;
        std::this_thread::sleep_for(10ms);
    }
}

// 场景:失败状态带错误且允许显式重试创建新 job id。
TEST(WebServerHttp, FailedUpdateJobCanBeRetried) {
    LocalUpdateServer update_server([](httplib::Server& s) {
        s.Get("/aceupdate.json", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(update_manifest_for("9.9.9"), "application/json");
        });
    });
    auto attempts = std::make_shared<std::atomic<int>>(0);
    WebServerFixture fx(
        true,
        false,
        {},
        true,
        [attempts](const acecode::AppConfig&,
                   acecode::upgrade::UpgradeProgressCallback publish,
                   std::string* error) {
            const int attempt = attempts->fetch_add(1);
            acecode::upgrade::UpgradeProgress progress;
            progress.phase = attempt == 0
                ? acecode::upgrade::UpgradePhase::Verifying
                : acecode::upgrade::UpgradePhase::Complete;
            progress.target_version = "9.9.9";
            publish(progress);
            if (attempt == 0) {
                if (error) *error = "checksum mismatch";
                return 1;
            }
            return 0;
        });
    fx.cfg.upgrade.base_url = update_server.base_url();
    fx.cfg.upgrade.timeout_ms = 3000;

    auto first = cpr::Post(cpr::Url{fx.url("/api/update/start")});
    ASSERT_EQ(first.status_code, 202) << first.text;
    const std::string first_id = json::parse(first.text)["job_id"];
    json failed;
    for (int i = 0; i < 50; ++i) {
        auto poll = cpr::Get(cpr::Url{fx.url("/api/update/jobs/" + first_id)});
        ASSERT_EQ(poll.status_code, 200) << poll.text;
        failed = json::parse(poll.text);
        if (failed["state"] == "failed") break;
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_EQ(failed["state"], "failed");
    EXPECT_EQ(failed["phase"], "verifying");
    EXPECT_EQ(failed["restart_required"], false);
    EXPECT_EQ(failed["error"], "checksum mismatch");

    auto retry = cpr::Post(cpr::Url{fx.url("/api/update/start")});
    ASSERT_EQ(retry.status_code, 202) << retry.text;
    EXPECT_NE(json::parse(retry.text)["job_id"], first_id);
}

// 场景: POST /api/sessions body 是非法 JSON → 400 + error JSON,不影响 server。
TEST(WebServerHttp, CreateSessionWithBadJsonReturns400) {
    WebServerFixture fx;
    auto r = cpr::Post(cpr::Url{fx.url("/api/sessions")},
                       cpr::Header{{"Content-Type", "application/json"}},
                       cpr::Body{"{not json"});
    EXPECT_EQ(r.status_code, 400);
    auto j = json::parse(r.text);
    EXPECT_TRUE(j.contains("error"));
}

// ------------------- saved_models CRUD route smoke tests -------------------
// 触发场景:Task 4 新增 4 个 endpoint(POST /api/models / PUT / DELETE /
// POST /api/config/default-model)。pure helper test 已覆盖 parse + 状态码
// 映射,这里补 route-level wiring:鉴权 + 落盘 + JSON 形态。配置文件指向
// fixture 的 tmp_dir,绝不会污染真实 ~/.acecode/config.json。

// 场景:POST /api/models 成功 → 200 + body 含 name/provider/model/api_key,
// 管理界面用 api_key 回填可显隐切换的输入框。
TEST(WebServerHttp, PostModelsCreatesSavedEntryWithApiKey) {
    WebServerFixture fx;
    json req = {
        {"name", "smoke-openai"},
        {"provider", "openai"},
        {"model", "llama-3"},
        {"base_url", "http://localhost:1234/v1"},
        {"api_key", "sk-secret"},
        {"request_headers", {
            {"Authorization", "Bearer {env:ACE_TOKEN}"},
            {"X-Team", "acecode"}
        }},
    };
    auto r = cpr::Post(cpr::Url{fx.url("/api/models")},
                       cpr::Header{{"Content-Type", "application/json"}},
                       cpr::Body{req.dump()});
    ASSERT_EQ(r.status_code, 200) << r.text;
    auto j = json::parse(r.text);
    EXPECT_EQ(j["name"], "smoke-openai");
    EXPECT_EQ(j["provider"], "openai");
    EXPECT_EQ(j["model"], "llama-3");
    EXPECT_EQ(j["base_url"], "http://localhost:1234/v1");
    EXPECT_EQ(j["api_key"], "sk-secret");
    EXPECT_EQ(j["request_headers"]["Authorization"], "Bearer {env:ACE_TOKEN}");
    EXPECT_EQ(j["request_headers"]["X-Team"], "acecode");

    // 落盘后 cfg 内存应已追加此条目
    ASSERT_EQ(fx.cfg.saved_models.size(), 2u);
    EXPECT_EQ(fx.cfg.saved_models.back().name, "smoke-openai");
    EXPECT_EQ(fx.cfg.saved_models.back().api_key, "sk-secret");
    EXPECT_EQ(fx.cfg.saved_models.back().request_headers.at("X-Team"), "acecode");
}

// 场景:POST /api/models 创建 Anthropic saved model。
TEST(WebServerHttp, PostModelsCreatesAnthropicEntry) {
    WebServerFixture fx;
    json req = {
        {"name", "smoke-claude"},
        {"provider", "anthropic"},
        {"model", "claude-test"},
        {"base_url", "https://api.anthropic.com/v1"},
        {"api_key", "sk-ant-secret"},
        {"request_headers", {
            {"anthropic-beta", "prompt-caching-2024-07-31"}
        }},
    };
    auto r = cpr::Post(cpr::Url{fx.url("/api/models")},
                       cpr::Header{{"Content-Type", "application/json"}},
                       cpr::Body{req.dump()});
    ASSERT_EQ(r.status_code, 200) << r.text;
    auto j = json::parse(r.text);
    EXPECT_EQ(j["name"], "smoke-claude");
    EXPECT_EQ(j["provider"], "anthropic");
    EXPECT_EQ(j["base_url"], "https://api.anthropic.com/v1");
    EXPECT_EQ(j["api_key"], "sk-ant-secret");
    EXPECT_EQ(j["request_headers"]["anthropic-beta"],
              "prompt-caching-2024-07-31");

    ASSERT_EQ(fx.cfg.saved_models.size(), 2u);
    EXPECT_EQ(fx.cfg.saved_models.back().provider, "anthropic");
    EXPECT_EQ(fx.cfg.saved_models.back().request_headers.at("anthropic-beta"),
              "prompt-caching-2024-07-31");
}

// 场景:PUT /api/models/<name> 省略 request_headers 时保留旧模板,
// 显式发送 {} 时清空旧模板。
TEST(WebServerHttp, PutModelsPreservesAndClearsRequestHeaders) {
    WebServerFixture fx;
    json create_req = {
        {"name", "gateway-openai"},
        {"provider", "openai"},
        {"model", "llama-3"},
        {"base_url", "http://localhost:1234/v1"},
        {"api_key", "sk-secret"},
        {"request_headers", {{"X-Team", "acecode"}}},
    };
    auto create = cpr::Post(cpr::Url{fx.url("/api/models")},
                            cpr::Header{{"Content-Type", "application/json"}},
                            cpr::Body{create_req.dump()});
    ASSERT_EQ(create.status_code, 200) << create.text;

    json preserve_req = {
        {"name", "gateway-openai"},
        {"provider", "openai"},
        {"model", "llama-3.1"},
        {"base_url", "http://localhost:4321/v1"},
    };
    auto preserve = cpr::Put(cpr::Url{fx.url("/api/models/gateway-openai")},
                             cpr::Header{{"Content-Type", "application/json"}},
                             cpr::Body{preserve_req.dump()});
    ASSERT_EQ(preserve.status_code, 200) << preserve.text;
    auto preserved = json::parse(preserve.text);
    EXPECT_EQ(preserved["api_key"], "sk-secret");
    EXPECT_EQ(preserved["request_headers"]["X-Team"], "acecode");
    ASSERT_EQ(fx.cfg.saved_models.size(), 2u);
    EXPECT_EQ(fx.cfg.saved_models.back().api_key, "sk-secret");
    EXPECT_EQ(fx.cfg.saved_models.back().request_headers.at("X-Team"), "acecode");

    json clear_req = preserve_req;
    clear_req["request_headers"] = json::object();
    auto clear = cpr::Put(cpr::Url{fx.url("/api/models/gateway-openai")},
                          cpr::Header{{"Content-Type", "application/json"}},
                          cpr::Body{clear_req.dump()});
    ASSERT_EQ(clear.status_code, 200) << clear.text;
    auto cleared = json::parse(clear.text);
    EXPECT_FALSE(cleared.contains("request_headers"));
    EXPECT_TRUE(fx.cfg.saved_models.back().request_headers.empty());
}

// 场景: PUT 重命名当前默认 saved model 时,default_model_name 同步改到新名。
TEST(WebServerHttp, PutModelsRenamingDefaultUpdatesDefaultName) {
    WebServerFixture fx;
    fx.cfg.default_model_name = "fixture-copilot";

    json req = {
        {"name", "fixture-copilot-v2"},
        {"provider", "copilot"},
        {"model", "gpt-5"},
    };
    auto r = cpr::Put(cpr::Url{fx.url("/api/models/fixture-copilot")},
                      cpr::Header{{"Content-Type", "application/json"}},
                      cpr::Body{req.dump()});
    ASSERT_EQ(r.status_code, 200) << r.text;
    auto j = json::parse(r.text);
    EXPECT_EQ(j["name"], "fixture-copilot-v2");
    EXPECT_EQ(fx.cfg.default_model_name, "fixture-copilot-v2");
    ASSERT_EQ(fx.cfg.saved_models.size(), 1u);
    EXPECT_EQ(fx.cfg.saved_models[0].name, "fixture-copilot-v2");
}

// 场景:外部登录器遗留 readonly=true 时,Desktop PUT 仍按普通 saved model
// 更新,不再返回 403 更新拒绝。成功更新后 legacy 标记自然清除。
TEST(WebServerHttp, PutModelsUpdatesLegacyReadonlyProfile) {
    WebServerFixture fx;
    ASSERT_EQ(fx.cfg.saved_models.size(), 1u);
    fx.cfg.saved_models[0].readonly = true;

    json req = {
        {"name", "fixture-copilot"},
        {"provider", "copilot"},
        {"model", "gpt-5"},
        {"capabilities", {"tool_use"}},
    };
    auto r = cpr::Put(cpr::Url{fx.url("/api/models/fixture-copilot")},
                      cpr::Header{{"Content-Type", "application/json"}},
                      cpr::Body{req.dump()});
    ASSERT_EQ(r.status_code, 200) << r.text;
    auto body = json::parse(r.text);
    EXPECT_EQ(body["model"], "gpt-5");
    EXPECT_EQ(body["capabilities"], json::array({"tool_use"}));
    ASSERT_EQ(fx.cfg.saved_models.size(), 1u);
    EXPECT_EQ(fx.cfg.saved_models[0].model, "gpt-5");
    EXPECT_FALSE(fx.cfg.saved_models[0].readonly);
}

// 场景:POST /api/models/probe 只接受 OpenAI-compatible 探测参数。这里走
// 400 分支,不发真实网络请求,用于固定 route wiring + 错误码。
TEST(WebServerHttp, PostModelsProbeRejectsUnsupportedProvider) {
    WebServerFixture fx;
    json req = {
        {"provider", "anthropic"},
        {"base_url", "http://localhost:1234/v1"},
    };
    auto r = cpr::Post(cpr::Url{fx.url("/api/models/probe")},
                       cpr::Header{{"Content-Type", "application/json"}},
                       cpr::Body{req.dump()});
    ASSERT_EQ(r.status_code, 400) << r.text;
    auto j = json::parse(r.text);
    EXPECT_EQ(j["error"], "UNKNOWN_PROVIDER");
}

// 场景:POST /api/models/probe 对 OpenAI-compatible /models 请求发送自定义 header。
TEST(WebServerHttp, PostModelsProbeAppliesCustomRequestHeaders) {
    std::mutex mu;
    std::string seen_probe_header;
    LocalUpdateServer upstream([&](httplib::Server& s) {
        s.Get("/models", [&](const httplib::Request& req, httplib::Response& res) {
            {
                std::lock_guard<std::mutex> lk(mu);
                seen_probe_header = req.get_header_value("X-Probe");
            }
            res.set_content(R"({"data":[{"id":"probe-model"}]})", "application/json");
        });
    });

    WebServerFixture fx;
    json req = {
        {"provider", "openai"},
        {"base_url", upstream.base_url()},
        {"api_key", "sk-probe"},
        {"request_headers", {{"X-Probe", "acecode"}}},
    };
    auto r = cpr::Post(cpr::Url{fx.url("/api/models/probe")},
                       cpr::Header{{"Content-Type", "application/json"}},
                       cpr::Body{req.dump()});
    ASSERT_EQ(r.status_code, 200) << r.text;
    auto j = json::parse(r.text);
    ASSERT_TRUE(j["models"].is_array());
    ASSERT_EQ(j["models"].size(), 1u);
    EXPECT_EQ(j["models"][0], "probe-model");

    std::lock_guard<std::mutex> lk(mu);
    EXPECT_EQ(seen_probe_header, "acecode");
}

// 场景:probe header 模板引用不存在的环境变量时,在发网前返回 400。
TEST(WebServerHttp, PostModelsProbeRejectsMissingRequestHeaderEnv) {
    ScopedEnvOverride missing("ACE_MISSING_PROBE_HEADER", std::nullopt);
    WebServerFixture fx;
    json req = {
        {"provider", "openai"},
        {"base_url", "http://127.0.0.1:1/v1"},
        {"api_key", "sk-probe"},
        {"request_headers", {{"X-Probe", "{env:ACE_MISSING_PROBE_HEADER}"}}},
    };
    auto r = cpr::Post(cpr::Url{fx.url("/api/models/probe")},
                       cpr::Header{{"Content-Type", "application/json"}},
                       cpr::Body{req.dump()});
    ASSERT_EQ(r.status_code, 400) << r.text;
    auto j = json::parse(r.text);
    EXPECT_EQ(j["error"], "INVALID_REQUEST_HEADER");
    EXPECT_NE(j["message"].get<std::string>().find("ACE_MISSING_PROBE_HEADER"),
              std::string::npos);
}

// 场景:Copilot 模型探测需要先通过 desktop/web 登录;无 token 时必须走
// 401,且不能尝试真实网络。
TEST(WebServerHttp, PostModelsProbeCopilotRequiresAuthWhenNoToken) {
    auto temp_home = std::filesystem::temp_directory_path() /
                     ("acecode_copilot_home_" + std::to_string(std::random_device{}()));
    RemoveTreeOnExit cleanup{temp_home};
    ScopedHomeOverride home(temp_home);
    WebServerFixture fx;
    json req = {{"provider", "copilot"}};

    auto r = cpr::Post(cpr::Url{fx.url("/api/models/probe")},
                       cpr::Header{{"Content-Type", "application/json"}},
                       cpr::Body{req.dump()});
    ASSERT_EQ(r.status_code, 401) << r.text;
    auto j = json::parse(r.text);
    EXPECT_EQ(j["error"], "COPILOT_AUTH_REQUIRED");
}

// 场景:desktop/web Copilot auth 状态和退出只读写 github_token 文件,
// 不碰 saved_models,也不泄漏 token 内容。
TEST(WebServerHttp, CopilotAuthStatusAndLogoutUseSavedTokenOnly) {
    auto temp_home = std::filesystem::temp_directory_path() /
                     ("acecode_copilot_auth_home_" + std::to_string(std::random_device{}()));
    RemoveTreeOnExit cleanup{temp_home};
    ScopedHomeOverride home(temp_home);
    WebServerFixture fx;

    auto initial = cpr::Get(cpr::Url{fx.url("/api/copilot/auth")});
    ASSERT_EQ(initial.status_code, 200) << initial.text;
    auto initial_body = json::parse(initial.text);
    EXPECT_EQ(initial_body["has_token"], false);
    EXPECT_EQ(initial_body["authenticated"], false);
    EXPECT_EQ(initial.text.find("gho-secret"), std::string::npos);

    acecode::save_github_token("gho-secret-test");
    auto with_token = cpr::Get(cpr::Url{fx.url("/api/copilot/auth")});
    ASSERT_EQ(with_token.status_code, 200) << with_token.text;
    auto with_token_body = json::parse(with_token.text);
    EXPECT_EQ(with_token_body["has_token"], true);
    EXPECT_EQ(with_token_body["authenticated"], true);
    EXPECT_EQ(with_token.text.find("gho-secret-test"), std::string::npos);
    ASSERT_EQ(fx.cfg.saved_models.size(), 1u);

    auto logout = cpr::Delete(cpr::Url{fx.url("/api/copilot/auth")});
    ASSERT_EQ(logout.status_code, 200) << logout.text;
    EXPECT_FALSE(acecode::has_saved_github_token());
    EXPECT_EQ(fx.cfg.saved_models.size(), 1u);
}

// 场景:POST /api/models 重名 → 409 NAME_TAKEN(saved_models_editor 校验)。
// 第一次提交建立基线;第二次同 name 必须被拒,且 cfg 不重复。
TEST(WebServerHttp, PostModelsDuplicateNameRejectedWith409) {
    WebServerFixture fx;
    json req = {
        {"name", "dup-name"},
        {"provider", "copilot"},
        {"model", "gpt-4o"},
    };
    auto first = cpr::Post(cpr::Url{fx.url("/api/models")},
                           cpr::Header{{"Content-Type", "application/json"}},
                           cpr::Body{req.dump()});
    ASSERT_EQ(first.status_code, 200) << first.text;

    auto second = cpr::Post(cpr::Url{fx.url("/api/models")},
                            cpr::Header{{"Content-Type", "application/json"}},
                            cpr::Body{req.dump()});
    ASSERT_EQ(second.status_code, 409) << second.text;
    auto j = json::parse(second.text);
    EXPECT_EQ(j["error"], "NAME_TAKEN");
    // cfg 不应出现两条同 name 条目
    EXPECT_EQ(fx.cfg.saved_models.size(), 2u);
}

// 场景:POST /api/config/default-model body name 不在 saved_models
// → 404 NOT_FOUND;cfg.default_model_name 不变。
// 回归表现:用户在 picker 里随便输入一个名字也能被 set 成 default,然后
// 启动时 model_resolver 解析失败。
TEST(WebServerHttp, PostDefaultModelUnknownNameReturns404) {
    WebServerFixture fx;
    fx.cfg.default_model_name = "previous-default";

    json req = {{"name", "totally-not-a-real-model"}};
    auto r = cpr::Post(cpr::Url{fx.url("/api/config/default-model")},
                       cpr::Header{{"Content-Type", "application/json"}},
                       cpr::Body{req.dump()});
    ASSERT_EQ(r.status_code, 404) << r.text;
    auto j = json::parse(r.text);
    EXPECT_EQ(j["error"], "NOT_FOUND");
    EXPECT_EQ(fx.cfg.default_model_name, "previous-default")
        << "校验失败时不应改 default_model_name";
}

// 场景: DELETE 默认 saved model 不再被 default 指针保护;删除成功后同步清空
// default_model_name,避免配置里留下悬空默认指针。
TEST(WebServerHttp, DeleteDefaultModelClearsDefaultModelName) {
    WebServerFixture fx;
    fx.cfg.default_model_name = "fixture-copilot";

    auto r = cpr::Delete(cpr::Url{fx.url("/api/models/fixture-copilot")});
    ASSERT_EQ(r.status_code, 200) << r.text;
    EXPECT_TRUE(fx.cfg.saved_models.empty());
    EXPECT_TRUE(fx.cfg.default_model_name.empty());
}

// 场景: active idle session 引用的 saved model 被删除时,删除成功;随后
// session model state 暴露 deleted=true,让 UI 显示 name (deleted)。
TEST(WebServerHttp, DeleteIdleActiveSessionModelReturnsDeletedState) {
    WebServerFixture fx;
    acecode::ModelProfile fast;
    fast.name = "fast";
    fast.provider = "copilot";
    fast.model = "fast-model";
    fx.cfg.saved_models.push_back(fast);

    acecode::SessionOptions opts;
    opts.model_name = "fast";
    const std::string id = fx.registry->create(opts);

    auto r = cpr::Delete(cpr::Url{fx.url("/api/models/fast")});
    ASSERT_EQ(r.status_code, 200) << r.text;
    ASSERT_EQ(fx.cfg.saved_models.size(), 1u);
    EXPECT_EQ(fx.cfg.saved_models[0].name, "fixture-copilot");

    auto state = cpr::Get(cpr::Url{fx.url("/api/sessions/" + id + "/model")});
    ASSERT_EQ(state.status_code, 200) << state.text;
    auto j = json::parse(state.text);
    EXPECT_EQ(j["name"], "fast");
    EXPECT_EQ(j["deleted"], true);
    EXPECT_EQ(j["provider"], "");
    EXPECT_EQ(j["model"], "");
}

// 场景: active busy session 正在使用某 saved model 时,DELETE 返回 409 且
// 不删除 profile,也不改 default_model_name。
TEST(WebServerHttp, DeleteBusyActiveSessionModelReturnsConflict) {
    WebServerFixture fx;
    acecode::ModelProfile busy;
    busy.name = "busy-fast";
    busy.provider = "copilot";
    busy.model = "busy-model";
    fx.cfg.saved_models.push_back(busy);
    fx.cfg.default_model_name = "busy-fast";

    acecode::SessionOptions opts;
    opts.model_name = "busy-fast";
    const std::string id = fx.registry->create(opts);
    auto entry = fx.registry->acquire(id);
    ASSERT_TRUE(entry);
    auto blocker = std::make_shared<BlockingProvider>();
    {
        ASSERT_TRUE(entry->provider_slot);
        std::lock_guard<std::mutex> lk(entry->provider_slot->mu);
        entry->provider_slot->provider = blocker;
    }

    entry->loop->submit("hold provider");
    ASSERT_TRUE(blocker->wait_for_started(2s));

    auto r = cpr::Delete(cpr::Url{fx.url("/api/models/busy-fast")});
    blocker->release();
    for (int i = 0; i < 100 && entry->loop->is_busy(); ++i) {
        std::this_thread::sleep_for(10ms);
    }

    ASSERT_EQ(r.status_code, 409) << r.text;
    auto j = json::parse(r.text);
    EXPECT_EQ(j["error"], "MODEL_IN_USE");
    EXPECT_NE(std::find_if(fx.cfg.saved_models.begin(),
                           fx.cfg.saved_models.end(),
                           [](const acecode::ModelProfile& p) {
                               return p.name == "busy-fast";
                           }),
              fx.cfg.saved_models.end());
    EXPECT_EQ(fx.cfg.default_model_name, "busy-fast");
}
