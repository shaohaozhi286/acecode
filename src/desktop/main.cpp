// acecode-desktop: WebView 壳 + N daemon 进程池(每 workspace 一个 daemon)。
//
// 启动流程:
//   1. 扫 .acecode/projects/*  → WorkspaceRegistry
//   2. 读 state.json::last_active_workspace_hash
//   3. pick_active 决定首屏 workspace
//   4. 注册 webview JS bridge: aceDesktop_listWorkspaces / activateWorkspace /
//      renameWorkspace / addWorkspace
//   5. 若 active workspace 存在 → DaemonPool::activate → 拼 URL → navigate
//      若不存在 → navigate 到 about:blank,sidebar 渲染只显示 "+ 添加项目"
//   6. WebHost.run() 阻塞直到窗口关闭
//   7. quit: pool.stop_all() + 写 last_active_workspace_hash
//
// daemon 端零代码改动 — 每个 daemon 仍只服务自己进程的 current_path。

#include "daemon_pool.hpp"
#include "folder_picker.hpp"
#include "pick_active.hpp"
#include "url_builder.hpp"
#include "web_host.hpp"
#include "workspace_registry.hpp"

#include "../config/config.hpp"
#include "../utils/cwd_hash.hpp"
#include "../utils/logger.hpp"
#include "../utils/state_file.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
std::string desktop_exe_dir() {
    wchar_t buf[MAX_PATH] = {0};
    DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return "";
    std::wstring wpath(buf, n);
    std::string path(wpath.begin(), wpath.end()); // ASCII path 兜底
    return fs::path(path).parent_path().string();
}

void show_error(const std::string& msg) {
    std::wstring w(msg.begin(), msg.end());
    ::MessageBoxW(nullptr, w.c_str(), L"ACECode Desktop", MB_ICONERROR | MB_OK);
}
#endif

std::string locate_daemon_exe() {
#ifdef _WIN32
    auto dir = desktop_exe_dir();
    if (dir.empty()) return "";
    fs::path p = fs::path(dir) / "acecode.exe";
    if (fs::exists(p)) return p.string();
    return "";
#else
    return "";
#endif
}

// dev 模式: 探测到仓库 web/ 目录(含 index.html)→ 让 daemon 走 FileSystemAssetSource,
// 改 web/ 文件 + F5 即生效,无需重 build acecode 二进制。
//
// 探测顺序:
//   1. 环境变量 ACECODE_DEV_WEB_DIR(显式指定,绝对路径)
//   2. 自动猜:从 desktop exe 向上 1-4 层找 "web/index.html"
//      build/Release/acecode-desktop.exe → ../web,../../web,../../../web,../../../../web
// 找不到返回空字符串(走默认 embedded asset 路径)。
std::string detect_dev_web_dir() {
#ifdef _WIN32
    if (const char* env = std::getenv("ACECODE_DEV_WEB_DIR"); env && *env) {
        fs::path p = fs::path(env) / "index.html";
        if (fs::exists(p)) return env;
    }
    auto dir = desktop_exe_dir();
    if (dir.empty()) return "";
    fs::path cur = fs::path(dir);
    for (int i = 0; i < 5; ++i) {
        fs::path candidate = cur / "web";
        if (fs::exists(candidate / "index.html")) return candidate.string();
        if (!cur.has_parent_path()) break;
        cur = cur.parent_path();
    }
    return "";
#else
    return "";
#endif
}

std::string projects_dir() {
    return (fs::path(acecode::get_acecode_dir()) / "projects").string();
}

std::string current_cwd() {
    std::error_code ec;
    auto p = fs::current_path(ec);
    if (ec) return "";
    return p.string();
}

// JSON 工具:把 daemon_state 枚举转成前端用的字符串
const char* state_string(acecode::desktop::DaemonState s) {
    switch (s) {
        case acecode::desktop::DaemonState::Stopped:  return "stopped";
        case acecode::desktop::DaemonState::Starting: return "starting";
        case acecode::desktop::DaemonState::Running:  return "running";
        case acecode::desktop::DaemonState::Failed:   return "failed";
    }
    return "stopped";
}

// onboarding splash:registry 全空时显示。极简,只是引导用户点 "添加项目"
// (前端 ace-app 启动时会通过 aceDesktop_listWorkspaces 拿到空列表自动渲染)。
// 这里的 about:blank URL 让 webview 不报错(没 daemon 起的话就没 http URL 可载)。
const char* onboarding_url() { return "about:blank"; }

} // namespace

#ifdef _WIN32
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
#else
int main(int, char**) {
#endif
    using namespace acecode::desktop;

    // desktop 自己的日志路径: ~/.acecode/logs/desktop-<date>.log。和 daemon
    // 日志(daemon-<date>.log)同目录便于一次 tail。mirror_stderr=false 因为
    // WIN32 子系统下 stderr 是 invalid handle。
    acecode::Logger::instance().init_with_rotation(acecode::get_logs_dir(), "desktop", false);
    acecode::Logger::instance().set_level(acecode::LogLevel::Dbg);
    LOG_INFO("[desktop] starting acecode-desktop");

    std::string daemon_exe = locate_daemon_exe();
    if (daemon_exe.empty()) {
#ifdef _WIN32
        show_error("Cannot locate acecode.exe next to acecode-desktop.exe.\n"
                   "Place both binaries in the same directory.");
#endif
        return 1;
    }

    // dev 模式探测 — 找到 web/ 后所有 spawn 出来的 daemon 都拿这个 static_dir
    std::string dev_web_dir = detect_dev_web_dir();
    if (!dev_web_dir.empty()) {
        LOG_INFO("[desktop] dev mode: serving web/ from " + dev_web_dir +
                 " (file changes hot-reload on F5)");
    }

    std::string proj_dir = projects_dir();
    std::error_code ec;
    fs::create_directories(proj_dir, ec); // 首次启动时不存在

    // 1. 扫已有 workspace
    WorkspaceRegistry registry;
    registry.scan(proj_dir);

    // 2. 决定 active workspace
    std::string last_active = acecode::read_last_active_workspace_hash();
    std::string proc_cwd = current_cwd();
    std::string active_hash = pick_active(last_active, proc_cwd, registry);

    // 没有任何已知 workspace 时:把 process cwd 注册成默认 workspace,这样
    // 用户首次安装能直接看到 sidebar 上有一个"当前项目"。
    if (active_hash.empty() && !proc_cwd.empty()) {
        auto m = registry.register_new(proj_dir, proc_cwd);
        active_hash = m.hash;
    }

    // 3. pool + bridge
    DaemonPool pool;
    std::mutex active_mu;
    std::string active_hash_dynamic = active_hash; // 后续切 workspace 时更新

    WebHost host(/*debug=*/true);
    host.set_title("ACECode");
    host.set_size(1280, 820);

    // bridge: 前端 console.error/warn + window.onerror + unhandledrejection 转发
    // 到 desktop 日志(~/.acecode/logs/desktop-<date>.log)。
    host.bind("aceDesktop_logFromWeb", [](const std::string& req) -> std::string {
        try {
            auto arr = nlohmann::json::parse(req);
            if (!arr.is_array() || arr.size() < 2) return "null";
            std::string level = arr[0].is_string() ? arr[0].get<std::string>() : "info";
            std::string msg   = arr[1].is_string() ? arr[1].get<std::string>() : arr[1].dump();
            std::string line  = "[web] " + msg;
            if (level == "error")        LOG_ERROR(line);
            else if (level == "warn")    LOG_WARN(line);
            else                         LOG_INFO(line);
        } catch (...) {
            // 不抛回前端,避免日志通道反过来污染前端 console
        }
        return "null";
    });

    // navigate 前注入 JS: hook console + window 错误事件 → 全部转发回 native。
    // 故意不 hook console.log / console.info,避免噪音(可在前端代码里需要时
    // 显式调 aceDesktop_logFromWeb('info', ...))。
    host.init_script(R"JS(
    (function () {
      if (!window.aceDesktop_logFromWeb) return;
      var send = function (level, args) {
        try {
          var parts = [];
          for (var i = 0; i < args.length; i++) {
            var a = args[i];
            try { parts.push(typeof a === 'string' ? a : JSON.stringify(a)); }
            catch (e) { parts.push(String(a)); }
          }
          window.aceDesktop_logFromWeb(level, parts.join(' '));
        } catch (e) { /* swallow */ }
      };
      var origErr  = console.error.bind(console);
      var origWarn = console.warn.bind(console);
      console.error = function () { send('error', arguments); origErr.apply(console, arguments); };
      console.warn  = function () { send('warn',  arguments); origWarn.apply(console, arguments); };
      window.addEventListener('error', function (e) {
        send('error', ['window.onerror', e.message, e.filename + ':' + e.lineno + ':' + e.colno,
                       e.error && e.error.stack ? e.error.stack : '']);
      });
      window.addEventListener('unhandledrejection', function (e) {
        var r = e.reason;
        send('error', ['unhandledrejection', r && r.message ? r.message : String(r),
                       r && r.stack ? r.stack : '']);
      });
    })();
    )JS");

    // bridge: listWorkspaces
    host.bind("aceDesktop_listWorkspaces", [&](const std::string& /*req*/) -> std::string {
        nlohmann::json arr = nlohmann::json::array();
        auto entries = registry.list();
        std::string cur;
        { std::lock_guard<std::mutex> lk(active_mu); cur = active_hash_dynamic; }
        for (auto& m : entries) {
            auto snap = pool.lookup(m.hash);
            nlohmann::json o;
            o["hash"] = m.hash;
            o["cwd"] = m.cwd;
            o["name"] = m.name;
            o["daemon_state"] = state_string(snap.state);
            o["active"] = (m.hash == cur);
            if (snap.state == DaemonState::Running) {
                o["port"] = snap.port;
                o["token"] = snap.token;
            }
            if (!snap.error.empty()) o["error"] = snap.error;
            arr.push_back(o);
        }
        return arr.dump();
    });

    // bridge: activateWorkspace
    auto activate_fn = [&](const std::string& hash) -> nlohmann::json {
        auto m = registry.get(hash);
        if (!m) {
            return {{"error", "unknown workspace hash"}};
        }
        ActivateRequest req;
        req.hash = m->hash;
        req.cwd = m->cwd;
        req.daemon_exe_path = daemon_exe;
        req.static_dir = dev_web_dir;
        auto r = pool.activate(req);
        if (!r.ok) {
            return {{"error", r.error}};
        }
        { std::lock_guard<std::mutex> lk(active_mu); active_hash_dynamic = m->hash; }
        return {{"port", r.port}, {"token", r.token}};
    };
    host.bind("aceDesktop_activateWorkspace", [&](const std::string& req) -> std::string {
        // webview 给的 args 是 JSON array,如 ["abc123"]
        try {
            auto arr = nlohmann::json::parse(req);
            if (!arr.is_array() || arr.empty() || !arr[0].is_string()) {
                return nlohmann::json{{"error", "missing hash arg"}}.dump();
            }
            return activate_fn(arr[0].get<std::string>()).dump();
        } catch (const std::exception& e) {
            return nlohmann::json{{"error", std::string("parse: ") + e.what()}}.dump();
        }
    });

    // bridge: renameWorkspace
    host.bind("aceDesktop_renameWorkspace", [&](const std::string& req) -> std::string {
        try {
            auto arr = nlohmann::json::parse(req);
            if (!arr.is_array() || arr.size() < 2 ||
                !arr[0].is_string() || !arr[1].is_string()) {
                return nlohmann::json{{"error", "expect [hash, name]"}}.dump();
            }
            std::string h = arr[0].get<std::string>();
            std::string n = arr[1].get<std::string>();
            bool ok = registry.set_name(proj_dir, h, n);
            if (!ok) return nlohmann::json{{"error", "rename failed"}}.dump();
            return nlohmann::json{{"ok", true}}.dump();
        } catch (const std::exception& e) {
            return nlohmann::json{{"error", std::string("parse: ") + e.what()}}.dump();
        }
    });

    // bridge: addWorkspace (folder picker)
    host.bind("aceDesktop_addWorkspace", [&](const std::string& /*req*/) -> std::string {
        auto picked = pick_folder(host.native_window());
        if (!picked) return "null";
        // normalize:把反斜杠改成正斜杠,作为 cwd 持久化(与 hash 算法的输入对齐)
        std::string cwd = *picked;
        for (auto& c : cwd) if (c == '\\') c = '/';
        auto m = registry.register_new(proj_dir, cwd);
        nlohmann::json o;
        o["hash"] = m.hash;
        o["cwd"] = m.cwd;
        o["name"] = m.name;
        return o.dump();
    });

    // 4. spawn active workspace 的 daemon → navigate WebView
    std::string url = onboarding_url();
    if (!active_hash.empty()) {
        auto m = registry.get(active_hash);
        if (m) {
            ActivateRequest req;
            req.hash = m->hash;
            req.cwd = m->cwd;
            req.daemon_exe_path = daemon_exe;
            req.static_dir = dev_web_dir;
            auto r = pool.activate(req);
            if (r.ok) {
                url = build_loopback_url(r.port, r.token);
            } else {
#ifdef _WIN32
                show_error("Failed to start daemon for workspace '" + m->name + "':\n" + r.error);
#endif
                // 不致命退出 — 仍打开 onboarding,用户可重试 / 切其它 workspace
            }
        }
    }
    host.navigate(url);

    // 5. 阻塞主循环
    host.run();

    // 6. quit:写 last_active + stop_all
    {
        std::lock_guard<std::mutex> lk(active_mu);
        if (!active_hash_dynamic.empty()) {
            acecode::write_last_active_workspace_hash(active_hash_dynamic);
        }
    }
    auto failures = pool.stop_all();
    return failures.empty() ? 0 : 100; // 部分失败返回非零便于诊断
}
