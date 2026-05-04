// 覆盖 src/web/server.{hpp,cpp} 的 HTTP 路由 + 鉴权路径(spec Section 9 + 11)。
// 起一个真 WebServer 在随机端口 + cpr 客户端打全部 endpoint,验证:
//   - GET /api/health 不需要 token 也能通(loopback)
//   - POST /api/sessions 创建 session + GET 看到它
//   - DELETE 后 GET 看不到
//   - GET /api/sessions/<id>/messages 返回 events+messages
//   - GET /api/skills 返回空数组(daemon 路径不 scan skills)
//   - GET /api/mcp 返回当前 mcp_servers
//   - POST /api/mcp/reload 返回 501
//   - 远程 IP(非 loopback)模拟 → 这里用 cpr 走 127.0.0.1 不容易模拟,所以
//     远程鉴权由 auth_test.cpp 单元覆盖,这里只验路由 wiring 通的部分
//
// WebSocket 路径不在自动化覆盖范围 — cpr 不带 WS client。WS 协议的客户端->
// 服务端 hello/user_input/decision/abort 由后续 add-web-chat-ui change 的端到端
// 集成测验证。当前 WS 行为依赖 spec 里描述的 hello-binding 协议。

#include <gtest/gtest.h>

#include "permissions.hpp"
#include "desktop/workspace_registry.hpp"
#include "provider/cwd_model_override.hpp"
#include "session/local_session_client.hpp"
#include "session/session_manager.hpp"
#include "session/session_registry.hpp"
#include "session/session_storage.hpp"
#include "skills/skill_registry.hpp"
#include "tool/tool_executor.hpp"
#include "utils/cwd_hash.hpp"
#include "web/server.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cpr/cpr.h>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <random>
#include <thread>

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
    std::unique_ptr<acecode::web::WebServer> server;

    std::thread server_thread;
    int port = 0;
    std::filesystem::path tmp_dir;
    std::filesystem::path cwd_dir;
    std::filesystem::path projects_dir;
    std::string cwd;
    std::string project_dir;

    explicit WebServerFixture(bool register_default_workspace = true) {
        port = pick_test_port();
        web_cfg.bind = "127.0.0.1";
        web_cfg.port = port;
        cfg.web = web_cfg;
        cfg.daemon = daemon_cfg;

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
        deps.config = &cfg;
        deps.template_permissions = &template_perm;
        registry = std::make_unique<acecode::SessionRegistry>(std::move(deps));
        client = std::make_unique<acecode::LocalSessionClient>(*registry);

        acecode::web::WebServerDeps wdeps;
        wdeps.web_cfg = &cfg.web;
        wdeps.daemon_cfg = &cfg.daemon;
        wdeps.app_config = &cfg;
        wdeps.config_path = (tmp_dir / "config.json").string();
        wdeps.cwd = cwd;
        wdeps.token = "smoke-token";
        wdeps.guid = "test-guid-aaaa-bbbb";
        wdeps.pid = 12345;
        wdeps.start_time_unix_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        wdeps.session_client = client.get();
        wdeps.session_registry = registry.get();
        wdeps.projects_dir = projects_dir.string();
        wdeps.workspace_registry = workspace_registry.get();
        wdeps.skill_registry = &skill_registry;
        wdeps.dangerous = false;

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
        std::error_code ec;
        std::filesystem::remove_all(project_dir, ec);
        std::filesystem::remove_all(tmp_dir, ec);
    }

    std::string url(const std::string& path) {
        return "http://127.0.0.1:" + std::to_string(port) + path;
    }
};

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
            found = true;
        }
    }
    EXPECT_TRUE(found) << "新建的 session 必须出现在列表中";
    EXPECT_EQ(occurrences, 1) << "active 与 disk meta 必须合并为同一条";
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
                            cpr::Body{R"({"text":"hello from http submit"})"});
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
                m.value("content", "") == "hello from http submit") {
                found = true;
                break;
            }
        }
        if (!found) std::this_thread::sleep_for(20ms);
    }
    EXPECT_TRUE(found) << "HTTP submit should be owned by daemon session";
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
    acecode::SessionStorage::write_meta(
        acecode::SessionStorage::meta_path(fx.project_dir, sid, 0), meta);

    auto r = cpr::Get(cpr::Url{fx.url("/api/sessions/" + sid + "/messages")});
    ASSERT_EQ(r.status_code, 200) << r.text;
    auto j = json::parse(r.text);
    ASSERT_TRUE(j.contains("messages"));
    ASSERT_EQ(j["messages"].size(), 1u);
    EXPECT_EQ(j["messages"][0]["content"], "old disk prompt");
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

// 场景: /api/skills 在 daemon 模式不 scan skills,返回空数组(spec 9.7)。
// 后续 daemon 接入 SkillRegistry::scan 时,这个测试需更新但不该破坏。
TEST(WebServerHttp, SkillsReturnsEmptyArrayWhenNotScanned) {
    WebServerFixture fx;
    auto r = cpr::Get(cpr::Url{fx.url("/api/skills")});
    ASSERT_EQ(r.status_code, 200);
    auto j = json::parse(r.text);
    EXPECT_TRUE(j.is_array());
    EXPECT_EQ(j.size(), 0u);
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

// 场景: 未知路由返回 404(Crow 默认行为,这里只是验证我们没把 / 放飞)。
TEST(WebServerHttp, UnknownRouteReturns404) {
    WebServerFixture fx;
    auto r = cpr::Get(cpr::Url{fx.url("/api/does-not-exist")});
    EXPECT_EQ(r.status_code, 404);
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
