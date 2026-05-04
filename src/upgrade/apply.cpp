#include "apply.hpp"

#include "package.hpp"
#include "../config/config.hpp"

#include <chrono>
#include <array>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace acecode::upgrade {
namespace {

bool is_same_or_inside(const fs::path& maybe_child, const fs::path& maybe_parent) {
    std::error_code ec;
    fs::path child = fs::weakly_canonical(maybe_child, ec);
    if (ec) child = fs::absolute(maybe_child).lexically_normal();
    fs::path parent = fs::weakly_canonical(maybe_parent, ec);
    if (ec) parent = fs::absolute(maybe_parent).lexically_normal();

    auto c = child.begin();
    for (auto p = parent.begin(); p != parent.end(); ++p, ++c) {
        if (c == child.end() || *p != *c) return false;
    }
    return true;
}

bool remove_directory_contents(const fs::path& dir, std::string* error) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) return true;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        fs::remove_all(entry.path(), ec);
        if (ec) {
            if (error) *error = "failed to remove " + entry.path().string() + ": " + ec.message();
            return false;
        }
    }
    return true;
}

bool move_directory_contents(const fs::path& from, const fs::path& to, std::string* error) {
    std::error_code ec;
    fs::create_directories(to, ec);
    if (ec) {
        if (error) *error = "failed to create directory " + to.string() + ": " + ec.message();
        return false;
    }
    if (!fs::exists(from, ec)) return true;
    for (const auto& entry : fs::directory_iterator(from, ec)) {
        fs::path dest = to / entry.path().filename();
        fs::rename(entry.path(), dest, ec);
        if (ec) {
            if (error) *error = "failed to move " + entry.path().string() + " to " +
                                dest.string() + ": " + ec.message();
            return false;
        }
    }
    return true;
}

bool copy_directory_contents(const fs::path& from, const fs::path& to, std::string* error) {
    std::error_code ec;
    fs::create_directories(to, ec);
    if (ec) {
        if (error) *error = "failed to create install directory: " + ec.message();
        return false;
    }

    for (const auto& entry : fs::recursive_directory_iterator(from, ec)) {
        if (ec) {
            if (error) *error = "failed to walk staged files: " + ec.message();
            return false;
        }
        fs::path rel = fs::relative(entry.path(), from, ec);
        if (ec) {
            if (error) *error = "failed to compute staged relative path: " + ec.message();
            return false;
        }
        fs::path dest = to / rel;
        if (entry.is_directory()) {
            fs::create_directories(dest, ec);
            if (ec) {
                if (error) *error = "failed to create directory " + dest.string() + ": " + ec.message();
                return false;
            }
        } else if (entry.is_regular_file()) {
            fs::create_directories(dest.parent_path(), ec);
            if (ec) {
                if (error) *error = "failed to create directory " +
                                    dest.parent_path().string() + ": " + ec.message();
                return false;
            }
            fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                if (error) *error = "failed to copy " + entry.path().string() + " to " +
                                    dest.string() + ": " + ec.message();
                return false;
            }
        }
    }
    return true;
}

void wait_for_parent(unsigned long pid) {
    if (pid == 0 || pid == current_process_id()) return;
#ifdef _WIN32
    HANDLE h = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!h) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        return;
    }
    ::WaitForSingleObject(h, INFINITE);
    ::CloseHandle(h);
#else
    for (int i = 0; i < 240; ++i) {
        if (::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
#endif
}

} // namespace

unsigned long current_process_id() {
#ifdef _WIN32
    return static_cast<unsigned long>(::GetCurrentProcessId());
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

fs::path current_executable_path(const std::string& argv0) {
    std::error_code ec;
#ifdef _WIN32
    std::vector<char> buf(MAX_PATH);
    DWORD n = ::GetModuleFileNameA(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    while (n == buf.size()) {
        buf.resize(buf.size() * 2);
        n = ::GetModuleFileNameA(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    }
    if (n > 0) {
        return fs::weakly_canonical(fs::path(std::string(buf.data(), n)), ec);
    }
#else
    std::array<char, 4096> buf{};
    ssize_t n = ::readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (n > 0) {
        return fs::weakly_canonical(fs::path(std::string(buf.data(), static_cast<size_t>(n))), ec);
    }
#endif
    fs::path p(argv0);
    fs::path abs = fs::weakly_canonical(p, ec);
    return ec ? fs::absolute(p).lexically_normal() : abs;
}

fs::path make_runner_path(unsigned long pid) {
#ifdef _WIN32
    return fs::temp_directory_path() / ("acecode-update-runner-" + std::to_string(pid) + ".exe");
#else
    return fs::temp_directory_path() / ("acecode-update-runner-" + std::to_string(pid));
#endif
}

std::vector<std::string> build_apply_runner_args(const ApplyOptions& opts) {
    return {
        "--apply-update",
        "--parent-pid", std::to_string(opts.parent_pid),
        "--staging", opts.staging_dir.string(),
        "--install-dir", opts.install_dir.string(),
        "--backup", opts.backup_dir.string(),
    };
}

std::string quote_command_arg(const std::string& arg) {
    if (arg.empty()) return "\"\"";
    bool needs_quote = false;
    for (char c : arg) {
        if (std::isspace(static_cast<unsigned char>(c)) || c == '"') {
            needs_quote = true;
            break;
        }
    }
    if (!needs_quote) return arg;

    std::string out = "\"";
    size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
        } else if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
            backslashes = 0;
        } else {
            out.append(backslashes, '\\');
            backslashes = 0;
            out.push_back(c);
        }
    }
    out.append(backslashes * 2, '\\');
    out.push_back('"');
    return out;
}

std::optional<ApplyOptions> parse_apply_runner_args(const std::vector<std::string>& args,
                                                   std::string* error) {
    ApplyOptions opts;
    for (size_t i = 0; i < args.size(); ++i) {
        auto require_value = [&](const char* flag) -> std::optional<std::string> {
            if (i + 1 >= args.size()) {
                if (error) *error = std::string("missing value for ") + flag;
                return std::nullopt;
            }
            return args[++i];
        };

        if (args[i] == "--parent-pid") {
            auto v = require_value("--parent-pid");
            if (!v) return std::nullopt;
            try {
                opts.parent_pid = std::stoul(*v);
            } catch (...) {
                if (error) *error = "invalid --parent-pid value";
                return std::nullopt;
            }
        } else if (args[i] == "--staging") {
            auto v = require_value("--staging");
            if (!v) return std::nullopt;
            opts.staging_dir = *v;
        } else if (args[i] == "--install-dir") {
            auto v = require_value("--install-dir");
            if (!v) return std::nullopt;
            opts.install_dir = *v;
        } else if (args[i] == "--backup") {
            auto v = require_value("--backup");
            if (!v) return std::nullopt;
            opts.backup_dir = *v;
        } else {
            if (error) *error = "unknown apply-update argument: " + args[i];
            return std::nullopt;
        }
    }

    if (opts.staging_dir.empty() || opts.install_dir.empty() || opts.backup_dir.empty()) {
        if (error) *error = "apply-update requires --staging, --install-dir, and --backup";
        return std::nullopt;
    }
    return opts;
}

bool prepare_update_runner(const fs::path& current_exe,
                           const fs::path& runner_path,
                           std::string* error) {
    std::error_code ec;
    fs::create_directories(runner_path.parent_path(), ec);
    if (ec) {
        if (error) *error = "failed to create runner directory: " + ec.message();
        return false;
    }
    fs::copy_file(current_exe, runner_path, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        if (error) *error = "failed to copy update runner: " + ec.message();
        return false;
    }
    return true;
}

bool launch_update_runner(const fs::path& runner_path,
                          const ApplyOptions& opts,
                          std::string* error) {
    auto args = build_apply_runner_args(opts);
#ifdef _WIN32
    std::string cmd = quote_command_arg(runner_path.string());
    for (const auto& arg : args) {
        cmd += " ";
        cmd += quote_command_arg(arg);
    }
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<char> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back('\0');
    BOOL ok = ::CreateProcessA(
        nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE, 0,
        nullptr, nullptr, &si, &pi);
    if (!ok) {
        if (error) *error = "failed to launch update runner";
        return false;
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return true;
#else
    pid_t pid = ::fork();
    if (pid < 0) {
        if (error) *error = "failed to fork update runner";
        return false;
    }
    if (pid == 0) {
        std::vector<std::string> storage;
        storage.push_back(runner_path.string());
        storage.insert(storage.end(), args.begin(), args.end());
        std::vector<char*> argv;
        for (auto& s : storage) argv.push_back(s.data());
        argv.push_back(nullptr);
        ::execv(runner_path.string().c_str(), argv.data());
        ::_exit(127);
    }
    return true;
#endif
}

bool apply_staged_update(const fs::path& staging_dir,
                         const fs::path& install_dir,
                         const fs::path& backup_dir,
                         const std::string& target,
                         std::string* error) {
    const fs::path data_dir = acecode::get_acecode_dir();
    if (is_same_or_inside(install_dir, data_dir) || is_same_or_inside(data_dir, install_dir)) {
        if (error) *error = "refusing to update an install directory that overlaps ACECode user data";
        return false;
    }

    std::string stage_error;
    auto staged = validate_staged_package(staging_dir, target, &stage_error);
    if (!staged) {
        if (error) *error = stage_error;
        return false;
    }

    std::error_code ec;
    fs::remove_all(backup_dir, ec);
    fs::create_directories(backup_dir, ec);
    if (ec) {
        if (error) *error = "failed to create backup directory: " + ec.message();
        return false;
    }
    fs::create_directories(install_dir, ec);
    if (ec) {
        if (error) *error = "failed to create install directory: " + ec.message();
        return false;
    }

    if (!move_directory_contents(install_dir, backup_dir, error)) {
        std::string ignored;
        (void)move_directory_contents(backup_dir, install_dir, &ignored);
        return false;
    }

    if (!copy_directory_contents(staged->content_root, install_dir, error) ||
        !fs::is_regular_file(install_dir / expected_executable_name_for_target(target))) {
        std::string restore_error;
        (void)remove_directory_contents(install_dir, &restore_error);
        (void)move_directory_contents(backup_dir, install_dir, &restore_error);
        if (error && error->empty()) {
            *error = "failed to verify updated executable";
        }
        return false;
    }

    return true;
}

int run_apply_update_command(const std::vector<std::string>& args,
                             std::ostream& out,
                             std::ostream& err,
                             const std::string& target) {
    std::string parse_error;
    auto opts = parse_apply_runner_args(args, &parse_error);
    if (!opts) {
        err << "acecode update apply failed: " << parse_error << "\n";
        return 64;
    }

    out << "Waiting for ACECode to exit before applying update...\n";
    wait_for_parent(opts->parent_pid);

    std::string apply_error;
    if (!apply_staged_update(opts->staging_dir, opts->install_dir,
                             opts->backup_dir, target, &apply_error)) {
        err << "acecode update apply failed: " << apply_error << "\n"
            << "Backup directory: " << opts->backup_dir.string() << "\n";
        return 1;
    }

    out << "ACECode update applied successfully.\n"
        << "Backup directory: " << opts->backup_dir.string() << "\n";
    return 0;
}

} // namespace acecode::upgrade
