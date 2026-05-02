#include "cli.hpp"

#include "platform.hpp"
#include "runtime_files.hpp"
#include "worker.hpp"
#include "../config/config.hpp"
#include "../skills/default_skill_seeder.hpp"
#include "../utils/logger.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

namespace fs = std::filesystem;

namespace acecode::daemon::cli {

namespace {

void print_help(std::ostream& os) {
    os << "Usage: acecode daemon <subcommand> [options]\n"
       << "\n"
       << "Subcommands:\n"
       << "  start                  spawn worker as detached background process\n"
       << "  stop                   terminate running worker, clean runtime files\n"
       << "  status                 print {pid, port, guid, uptime}\n"
       << "  --foreground           run worker in current console (debug mode)\n"
       << "\n"
       << "Options (advanced):\n"
       << "  --cwd=<PATH>           run worker as if started from PATH\n"
       << "  --port=<N>             override web.port for this worker\n"
       << "  --static-dir=<PATH>    serve front-end assets from PATH\n"
       << "  --supervised --guid=G  launcher-internal: launched by Service supervisor\n"
       << "  -dangerous             skip permission checks (loopback bind only)\n";
}

// 简单 starts_with(C++17 没有 string::starts_with)
bool starts_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

std::string executable_dir_from_path(const std::string& exe_path) {
    if (exe_path.empty()) return "";
    std::error_code ec;
    fs::path abs = fs::weakly_canonical(fs::path(exe_path), ec);
    if (!ec) return abs.parent_path().string();
    return fs::path(exe_path).parent_path().string();
}

void seed_default_skills_if_first_initialization(const std::string& exe_path) {
    bool first_initialization = acecode::consume_acecode_home_created_by_process();
    auto result = acecode::install_default_global_skills_on_first_initialization(
        fs::path(acecode::get_acecode_dir()),
        executable_dir_from_path(exe_path),
        first_initialization);
    if (!result.attempted) return;
    if (!result.error.empty()) {
        LOG_WARN("[skills] Default skill seeding issue: " + result.error);
    }
}

} // namespace

Args parse(const std::vector<std::string>& tokens) {
    Args a;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const auto& t = tokens[i];
        if (t == "start" || t == "stop" || t == "status") {
            if (!a.sub.empty()) {
                a.error = "multiple subcommands specified: " + a.sub + " and " + t;
                return a;
            }
            a.sub = t;
        } else if (t == "--foreground" || t == "-foreground") {
            if (!a.sub.empty() && a.sub != "foreground") {
                a.error = "--foreground cannot combine with " + a.sub;
                return a;
            }
            a.sub = "foreground";
        } else if (t == "--supervised") {
            a.supervised = true;
        } else if (starts_with(t, "--guid=")) {
            a.guid = t.substr(7);
        } else if (starts_with(t, "--port=")) {
            // desktop 父进程预选空闲端口后通过 --port=N 注入。0 视为非法(直接走配置文件)。
            std::string v = t.substr(7);
            try {
                int p = std::stoi(v);
                if (p <= 0 || p > 65535) {
                    a.error = "--port=<N> out of range (1..65535): " + v;
                    return a;
                }
                a.port_override = p;
            } catch (...) {
                a.error = "--port=<N> not an integer: " + v;
                return a;
            }
        } else if (starts_with(t, "--token=")) {
            // desktop 父进程预生成 token,父子两端不必跨进程读 token 文件。
            a.token_override = t.substr(8);
            if (a.token_override.empty()) {
                a.error = "--token=<T> empty value";
                return a;
            }
        } else if (starts_with(t, "--static-dir=")) {
            // dev 模式: desktop 探测到仓库 web/ 后通过这个 flag 让 daemon 走
            // FileSystemAssetSource(每次请求重读),改 web/ 文件 + F5 即生效。
            a.static_dir_override = t.substr(13);
            if (a.static_dir_override.empty()) {
                a.error = "--static-dir=<path> empty value";
                return a;
            }
        } else if (starts_with(t, "--cwd=")) {
            a.cwd_override = t.substr(6);
            if (a.cwd_override.empty()) {
                a.error = "--cwd=<path> empty value";
                return a;
            }
        } else if (t == "--cwd") {
            if (i + 1 >= tokens.size() || tokens[i + 1].empty()) {
                a.error = "--cwd requires a path";
                return a;
            }
            a.cwd_override = tokens[++i];
        } else if (t == "-dangerous") {
            a.dangerous = true;
        } else if (t == "--help" || t == "-h") {
            a.sub = "help";
        } else {
            a.error = "unknown daemon argument: " + t;
            return a;
        }
    }
    if (a.supervised && a.guid.empty()) {
        a.error = "--supervised requires --guid=<G>";
    }
    return a;
}

static int do_foreground(const Args& a, const std::string& exe_path) {
    AppConfig cfg = load_config();
    seed_default_skills_if_first_initialization(exe_path);
    auto errs = validate_config(cfg);
    if (!errs.empty()) {
        for (const auto& e : errs) std::cerr << "config error: " << e << "\n";
        return 5;
    }
    WorkerOptions opts;
    opts.foreground          = true;
    opts.supervised          = a.supervised;
    opts.guid                = a.guid;
    opts.dangerous           = a.dangerous;
    opts.port_override       = a.port_override;
    opts.token_override      = a.token_override;
    opts.static_dir_override = a.static_dir_override;
    opts.cwd_override        = a.cwd_override;
    return run_worker(opts, cfg);
}

static int do_start(const Args& a, const std::string& exe_path) {
    // 检查是否已有 daemon 在跑
    auto existing_pid = read_pid_file();
    if (existing_pid.has_value() && is_pid_alive(*existing_pid)) {
        std::cerr << "daemon already running (pid=" << *existing_pid
                  << "); stop it first or check `acecode daemon status`\n";
        return 6;
    }

    // 派生自身的 detached 副本: <exe> daemon --foreground
    std::vector<std::string> argv = {exe_path, "daemon", "--foreground"};
    if (a.port_override > 0) {
        argv.push_back("--port=" + std::to_string(a.port_override));
    }
    if (!a.token_override.empty()) {
        argv.push_back("--token=" + a.token_override);
    }
    if (!a.static_dir_override.empty()) {
        argv.push_back("--static-dir=" + a.static_dir_override);
    }
    if (!a.cwd_override.empty()) {
        argv.push_back("--cwd=" + a.cwd_override);
    }
    if (a.dangerous) argv.push_back("-dangerous");

    auto child = spawn_detached(argv);
    if (child == 0) {
        std::cerr << "spawn_detached failed; check log\n";
        return 7;
    }
    // POSIX double-fork 时返回 -1 哨兵(中间进程已 reap),真正 worker pid 由
    // worker 自己写到 daemon.pid。这里轮询 daemon.pid 出现 + 进程存活,5s 超时。
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        auto p = read_pid_file();
        if (p.has_value() && is_pid_alive(*p)) {
            std::cout << "daemon started pid=" << *p << "\n";
            return 0;
        }
    }
    std::cerr << "daemon failed to start within 5s; check ~/.acecode/logs/\n";
    return 8;
}

static int do_stop() {
    auto pid = read_pid_file();
    if (!pid.has_value()) {
        std::cerr << "no daemon running (no pid file)\n";
        return 1;
    }
    if (!is_pid_alive(*pid)) {
        std::cerr << "stale pid file (pid=" << *pid << " not alive); cleaning up\n";
        cleanup_runtime_files();
        return 0;
    }
    if (!terminate_pid(*pid, /*wait_ms=*/10000)) {
        std::cerr << "failed to terminate pid=" << *pid << " within 10s\n";
        return 9;
    }
    cleanup_runtime_files();
    std::cout << "daemon stopped (pid=" << *pid << ")\n";
    return 0;
}

static int do_status() {
    auto pid  = read_pid_file();
    auto port = read_port_file();
    auto guid = read_guid_file();
    auto hb   = read_heartbeat();

    if (!pid.has_value() || !is_pid_alive(*pid)) {
        std::cout << "no daemon running\n";
        return 1;
    }

    std::cout << "daemon running\n"
              << "  pid:  " << *pid << "\n";
    if (port.has_value()) std::cout << "  port: " << *port << "\n";
    if (guid.has_value()) std::cout << "  guid: " << *guid << "\n";
    if (hb.has_value()) {
        auto age_ms = now_unix_ms() - hb->timestamp_ms;
        std::cout << "  last_heartbeat_age_ms: " << age_ms << "\n";
    }
    return 0;
}

int run(const std::vector<std::string>& tokens, const std::string& exe_path) {
    Args a = parse(tokens);
    if (!a.error.empty()) {
        std::cerr << a.error << "\n\n";
        print_help(std::cerr);
        return 10;
    }
    if (a.sub.empty() || a.sub == "help") {
        print_help(std::cout);
        return a.sub.empty() ? 11 : 0;
    }

    std::string exe = exe_path.empty() ? current_executable_path() : exe_path;
    if (a.sub != "stop" && a.sub != "status" && exe.empty()) {
        std::cerr << "cannot resolve current executable path\n";
        return 12;
    }

    if (a.sub == "foreground") return do_foreground(a, exe);
    if (a.sub == "start")      return do_start(a, exe);
    if (a.sub == "stop")       return do_stop();
    if (a.sub == "status")     return do_status();

    std::cerr << "unknown subcommand: " << a.sub << "\n";
    return 10;
}

} // namespace acecode::daemon::cli
