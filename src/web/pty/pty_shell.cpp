// resolve_console_shell / pty_backend_kind_name:平台无关纯逻辑,进
// acecode_testable 供单测直接覆盖(spawn 实现在 pty_backend_{win,posix}.cpp)。

#include "pty_backend.hpp"

#include "../../utils/encoding.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace acecode {

const char* pty_backend_kind_name(PtyBackendKind kind) {
    switch (kind) {
        case PtyBackendKind::ConPty:   return "conpty";
        case PtyBackendKind::Winpty:   return "winpty";
        case PtyBackendKind::Pipe:     return "pipe";
        case PtyBackendKind::PosixPty: return "posix";
    }
    return "unknown";
}

std::string resolve_console_shell(const std::string& configured) {
    if (!configured.empty()) return configured;
#ifdef _WIN32
    std::string comspec = getenv_utf8("COMSPEC");
    return comspec.empty() ? "cmd.exe" : comspec;
#else
    std::string shell = getenv_utf8("SHELL");
    return shell.empty() ? "/bin/sh" : shell;
#endif
}

// ── 控制台 shell 目录探测 ───────────────────────────────────────────────

bool is_wsl_system32_bash(const std::string& path) {
    std::string lower;
    lower.reserve(path.size());
    for (char ch : path) {
        char c = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        lower.push_back(c == '/' ? '\\' : c);
    }
    return lower.find("\\system32\\bash.exe") != std::string::npos;
}

namespace {

#ifdef _WIN32
std::string win_git_install_path_from_registry() {
    const HKEY roots[] = {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER};
    for (HKEY root : roots) {
        HKEY key = nullptr;
        if (::RegOpenKeyExW(root, L"SOFTWARE\\GitForWindows", 0, KEY_READ, &key) !=
                ERROR_SUCCESS || !key) {
            continue;
        }
        wchar_t buf[1024];
        DWORD bytes = sizeof(buf);
        DWORD type = 0;
        LONG rc = ::RegQueryValueExW(key, L"InstallPath", nullptr, &type,
                                     reinterpret_cast<LPBYTE>(buf), &bytes);
        ::RegCloseKey(key);
        if (rc == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) &&
                bytes >= sizeof(wchar_t)) {
            std::wstring w(buf, bytes / sizeof(wchar_t));
            while (!w.empty() && w.back() == L'\0') w.pop_back();
            if (!w.empty()) return wide_to_utf8(w);
        }
    }
    return {};
}
#endif

// 含空格的路径在命令行里加双引号(Windows CreateProcessW / winpty 都按命令行解析)。
std::string quote_if_needed(const std::string& path) {
    if (path.find(' ') == std::string::npos) return path;
    return "\"" + path + "\"";
}

}  // namespace

ShellProbe default_shell_probe() {
    ShellProbe p;
    p.exists = [](const std::string& path) {
        if (path.empty()) return false;
        std::error_code ec;
        return std::filesystem::exists(std::filesystem::u8path(path), ec);
    };
    p.getenv = [](const std::string& name) { return getenv_utf8(name.c_str()); };
#ifdef _WIN32
    p.git_install_path = []() { return win_git_install_path_from_registry(); };
#else
    p.git_install_path = []() { return std::string{}; };
#endif
    return p;
}

std::vector<ConsoleShellOption> detect_console_shells(
    const std::string& configured_git_bash_path, const ShellProbe& probe) {
    std::vector<ConsoleShellOption> out;
#ifdef _WIN32
    // PowerShell:pwsh(7)优先,探测不到回退 powershell.exe(System32 必有)。
    {
        ConsoleShellOption ps;
        ps.id = "powershell";
        ps.label = "PowerShell";
        std::vector<std::string> pwsh_candidates;
        const std::string program_files = probe.getenv("ProgramFiles");
        const std::string local_appdata = probe.getenv("LocalAppData");
        if (!program_files.empty())
            pwsh_candidates.push_back(program_files + "\\PowerShell\\7\\pwsh.exe");
        if (!local_appdata.empty())
            pwsh_candidates.push_back(local_appdata + "\\Microsoft\\WindowsApps\\pwsh.exe");
        std::string pwsh;
        for (const auto& c : pwsh_candidates) {
            if (probe.exists(c)) { pwsh = c; break; }
        }
        if (!pwsh.empty()) {
            ps.command = quote_if_needed(pwsh);
            ps.label = "PowerShell 7";
        } else {
            ps.command = "powershell.exe";
        }
        ps.available = true;
        out.push_back(std::move(ps));
    }
    // Git Bash:用户指定路径 → 常见安装位置 → 注册表 InstallPath。排除 WSL。
    {
        ConsoleShellOption gb;
        gb.id = "git-bash";
        gb.label = "Git Bash";
        std::vector<std::string> candidates;
        if (!configured_git_bash_path.empty())
            candidates.push_back(configured_git_bash_path);
        auto add_git_root = [&](const std::string& base) {
            if (!base.empty()) candidates.push_back(base + "\\Git\\bin\\bash.exe");
        };
        add_git_root(probe.getenv("ProgramFiles"));
        add_git_root(probe.getenv("ProgramW6432"));
        add_git_root(probe.getenv("ProgramFiles(x86)"));
        if (std::string la = probe.getenv("LocalAppData"); !la.empty())
            candidates.push_back(la + "\\Programs\\Git\\bin\\bash.exe");
        if (probe.git_install_path) {
            if (std::string gip = probe.git_install_path(); !gip.empty())
                candidates.push_back(gip + "\\bin\\bash.exe");
        }
        std::string found;
        for (const auto& c : candidates) {
            if (is_wsl_system32_bash(c)) continue;  // WSL 的 bash.exe 不是 Git Bash
            if (probe.exists(c)) { found = c; break; }
        }
        if (!found.empty()) {
            gb.command = quote_if_needed(found) + " --login -i";
            gb.available = true;
        } else {
            gb.available = false;
            gb.needs_path = true;
        }
        out.push_back(std::move(gb));
    }
    // cmd(当前默认)。
    {
        ConsoleShellOption c;
        c.id = "cmd";
        c.label = "Command Prompt";
        const std::string comspec = probe.getenv("COMSPEC");
        c.command = comspec.empty() ? "cmd.exe" : comspec;
        c.available = true;
        out.push_back(std::move(c));
    }
#else
    // 默认 $SHELL。
    {
        ConsoleShellOption def;
        def.id = "shell";
        def.label = "Default Shell";
        const std::string sh = probe.getenv("SHELL");
        def.command = sh.empty() ? "/bin/sh" : sh;
        def.available = true;
        out.push_back(std::move(def));
    }
    struct Cand { const char* id; const char* label; std::vector<std::string> paths; };
    const std::vector<Cand> cands = {
        {"bash", "Bash",
         {"/bin/bash", "/usr/bin/bash", "/usr/local/bin/bash", "/opt/homebrew/bin/bash"}},
        {"zsh", "Zsh",
         {"/bin/zsh", "/usr/bin/zsh", "/usr/local/bin/zsh", "/opt/homebrew/bin/zsh"}},
        {"fish", "Fish",
         {"/usr/bin/fish", "/usr/local/bin/fish", "/opt/homebrew/bin/fish"}},
    };
    for (const auto& cand : cands) {
        std::string found;
        for (const auto& p : cand.paths) {
            if (probe.exists(p)) { found = p; break; }
        }
        if (!found.empty()) {
            ConsoleShellOption o;
            o.id = cand.id;
            o.label = cand.label;
            o.command = found;
            o.available = true;
            out.push_back(std::move(o));
        }
    }
    (void)configured_git_bash_path;
#endif
    return out;
}

std::vector<ConsoleShellOption> detect_console_shells(
    const std::string& configured_git_bash_path) {
    return detect_console_shells(configured_git_bash_path, default_shell_probe());
}

std::optional<std::string> resolve_shell_command_by_id(
    const std::string& id, const std::string& configured_git_bash_path,
    const ShellProbe& probe) {
    if (id.empty()) return std::nullopt;
    for (const auto& opt : detect_console_shells(configured_git_bash_path, probe)) {
        if (opt.id == id) {
            if (opt.available && !opt.command.empty()) return opt.command;
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<std::string> resolve_shell_command_by_id(
    const std::string& id, const std::string& configured_git_bash_path) {
    return resolve_shell_command_by_id(id, configured_git_bash_path, default_shell_probe());
}

std::string default_console_shell_id(
    const std::string& configured_default_shell,
    const std::string& configured_git_bash_path, const ShellProbe& probe) {
    if (!configured_default_shell.empty()) {
        for (const auto& opt : detect_console_shells(configured_git_bash_path, probe)) {
            if (opt.id == configured_default_shell && opt.available) {
                return configured_default_shell;
            }
        }
    }
#ifdef _WIN32
    return "cmd";
#else
    return "shell";
#endif
}

std::string default_console_shell_id(
    const std::string& configured_default_shell,
    const std::string& configured_git_bash_path) {
    return default_console_shell_id(configured_default_shell, configured_git_bash_path,
                                    default_shell_probe());
}

} // namespace acecode
