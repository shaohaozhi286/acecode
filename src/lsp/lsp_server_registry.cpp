#include "lsp_server_registry.hpp"

#include "../utils/logger.hpp"
#include "../utils/utf8_path.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>

namespace acecode::lsp {
namespace {

namespace fs = std::filesystem;

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string file_extension(const std::string& utf8_file) {
    const std::size_t slash = utf8_file.find_last_of("/\\");
    const std::size_t dot = utf8_file.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
    return lower_ascii(utf8_file.substr(dot));
}

bool real_file_exists(const std::string& utf8_path) {
    std::error_code ec;
    return fs::exists(path_from_utf8(utf8_path), ec) && !ec;
}

std::string join_path(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    const char back = dir.back();
    if (back == '/' || back == '\\') return dir + name;
#ifdef _WIN32
    return dir + "\\" + name;
#else
    return dir + "/" + name;
#endif
}

std::string parent_dir(const std::string& utf8_path) {
    const fs::path parent = path_from_utf8(utf8_path).parent_path();
    return path_to_utf8(parent);
}

// 大小写不敏感(Windows)/敏感(POSIX)的前缀包含判断:dir 是否位于
// workspace 之内(含相等)。分隔符统一后比较。
bool dir_within(const std::string& dir, const std::string& workspace) {
    auto canon = [](std::string p) {
        std::replace(p.begin(), p.end(), '\\', '/');
        while (!p.empty() && p.back() == '/') p.pop_back();
#ifdef _WIN32
        p = lower_ascii(p);
#endif
        return p;
    };
    const std::string d = canon(dir);
    const std::string w = canon(workspace);
    if (d == w) return true;
    return d.size() > w.size() && d.compare(0, w.size(), w) == 0 && d[w.size()] == '/';
}

std::string getenv_string(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string{};
}

// node_modules/typescript/lib/tsserver.js:从 start_dir 一路向上找
// (Node 模块解析同款),给 typescript-language-server 指 workspace 自带
// 的 TypeScript。找不到返回空(server 跳过 —— 无项目 TS 时启动无意义)。
std::string find_tsserver_js(const std::string& start_dir) {
    fs::path dir = path_from_utf8(start_dir);
    std::error_code ec;
    for (;;) {
        const fs::path candidate = dir / "node_modules" / "typescript" / "lib" / "tsserver.js";
        if (fs::exists(candidate, ec) && !ec) return path_to_utf8(candidate);
        const fs::path parent = dir.parent_path();
        if (parent == dir) return {};
        dir = parent;
    }
}

// pyright 的解释器探测:VIRTUAL_ENV → root/.venv → root/venv。
std::string find_venv_python(const std::string& root) {
    std::vector<std::string> venv_dirs;
    const std::string active = getenv_string("VIRTUAL_ENV");
    if (!active.empty()) venv_dirs.push_back(active);
    venv_dirs.push_back(join_path(root, ".venv"));
    venv_dirs.push_back(join_path(root, "venv"));
    for (const auto& venv : venv_dirs) {
#ifdef _WIN32
        const std::string python = join_path(join_path(venv, "Scripts"), "python.exe");
#else
        const std::string python = join_path(join_path(venv, "bin"), "python");
#endif
        if (real_file_exists(python)) return python;
    }
    return {};
}

} // namespace

std::vector<LspServerDef> builtin_server_defs() {
    std::vector<LspServerDef> defs;

    {
        LspServerDef clangd;
        clangd.id = "clangd";
        clangd.extensions = {".c", ".cpp", ".cc", ".cxx", ".c++",
                             ".h", ".hpp", ".hh", ".hxx", ".h++"};
        clangd.root.marker_groups = {{"compile_commands.json", "compile_flags.txt", ".clangd"}};
        clangd.command = {"clangd", "--background-index", "--clang-tidy"};
        clangd.builtin_spawn = true;
        defs.push_back(std::move(clangd));
    }
    {
        LspServerDef ts;
        ts.id = "typescript-language-server";
        ts.extensions = {".ts", ".tsx", ".js", ".jsx", ".mjs", ".cjs", ".mts", ".cts"};
        ts.root.marker_groups = {{"package-lock.json", "bun.lockb", "bun.lock",
                                  "pnpm-lock.yaml", "yarn.lock"}};
        ts.root.exclude_markers = {"deno.json", "deno.jsonc"};
        ts.command = {"typescript-language-server", "--stdio"};
        ts.builtin_spawn = true;
        defs.push_back(std::move(ts));
    }
    {
        LspServerDef pyright;
        pyright.id = "pyright";
        pyright.extensions = {".py", ".pyi"};
        pyright.root.marker_groups = {{"pyproject.toml", "setup.py", "setup.cfg",
                                       "requirements.txt", "Pipfile", "pyrightconfig.json"}};
        pyright.command = {"pyright-langserver", "--stdio"};
        pyright.builtin_spawn = true;
        defs.push_back(std::move(pyright));
    }
    {
        LspServerDef gopls;
        gopls.id = "gopls";
        gopls.extensions = {".go"};
        gopls.root.marker_groups = {{"go.work"}, {"go.mod", "go.sum"}};
        gopls.command = {"gopls"};
        gopls.builtin_spawn = true;
        defs.push_back(std::move(gopls));
    }
    {
        LspServerDef rust;
        rust.id = "rust-analyzer";
        rust.extensions = {".rs"};
        rust.root.marker_groups = {{"Cargo.toml", "Cargo.lock"}};
        // 无 Cargo.toml 时 rust-analyzer 没有可用的项目模型,直接不适用。
        // (opencode 还会向上找 [workspace] 段的 workspace 根;v1 用 crate
        // 根即可,rust-analyzer 自身能通过 cargo metadata 发现 workspace。)
        rust.root.fallback_to_workspace = false;
        rust.command = {"rust-analyzer"};
        rust.builtin_spawn = true;
        defs.push_back(std::move(rust));
    }
    return defs;
}

std::vector<LspServerDef> merge_server_defs(const LspConfig& cfg) {
    std::vector<LspServerDef> defs = builtin_server_defs();

    for (const auto& [name, entry] : cfg.servers) {
        auto it = std::find_if(defs.begin(), defs.end(),
                               [&](const LspServerDef& d) { return d.id == name; });
        if (it != defs.end()) {
            if (entry.disabled) {
                defs.erase(it);
                continue;
            }
            if (!entry.command.empty()) {
                it->command = entry.command;
                it->builtin_spawn = false; // 用户接管启动方式,内置特化失效
            }
            if (!entry.extensions.empty()) it->extensions = entry.extensions;
            for (const auto& [k, v] : entry.env) it->env[k] = v;
            if (entry.initialization.is_object() && !entry.initialization.empty()) {
                it->initialization = entry.initialization;
            }
            continue;
        }

        if (entry.disabled) continue; // 禁用一个不存在的名字:无事发生
        if (entry.command.empty()) {
            LOG_WARN("[lsp] config server '" + name +
                     "' has no command and is not a builtin; entry ignored");
            continue;
        }
        LspServerDef custom;
        custom.id = name;
        custom.extensions = entry.extensions;
        custom.command = entry.command;
        custom.env = entry.env;
        custom.initialization = entry.initialization;
        custom.root.fallback_to_workspace = true; // 无 marker → 恒用 workspace
        defs.push_back(std::move(custom));
    }
    return defs;
}

bool extensions_match(const LspServerDef& def, const std::string& utf8_file) {
    if (def.extensions.empty()) return true;
    const std::string ext = file_extension(utf8_file);
    if (ext.empty()) return false;
    for (const auto& candidate : def.extensions) {
        if (lower_ascii(candidate) == ext) return true;
    }
    return false;
}

std::optional<std::string> detect_root(const LspServerDef& def,
                                       const std::string& utf8_file,
                                       const std::string& workspace_cwd,
                                       const FileExistsFn& exists) {
    // 收集探测目录链:文件父目录 → ... → workspace cwd(含)。
    std::vector<std::string> chain;
    std::string dir = parent_dir(utf8_file);
    while (!dir.empty() && dir_within(dir, workspace_cwd)) {
        chain.push_back(dir);
        if (dir_within(workspace_cwd, dir)) break; // 到达 workspace 根
        const std::string parent = parent_dir(dir);
        if (parent == dir) break;
        dir = parent;
    }
    if (chain.empty()) chain.push_back(workspace_cwd);

    for (const auto& level : chain) {
        for (const auto& marker : def.root.exclude_markers) {
            if (exists(join_path(level, marker))) return std::nullopt;
        }
    }

    for (const auto& group : def.root.marker_groups) {
        for (const auto& level : chain) {
            for (const auto& marker : group) {
                if (exists(join_path(level, marker))) return level;
            }
        }
    }

    if (def.root.fallback_to_workspace) return workspace_cwd;
    return std::nullopt;
}

std::optional<ResolvedSpawn> resolve_spawn(const LspServerDef& def,
                                           const std::string& root,
                                           const std::string& workspace_cwd) {
    if (def.command.empty()) return std::nullopt;

    auto binary = which(def.command[0]);
    if (!binary.has_value()) return std::nullopt;

    ResolvedSpawn out;
    out.spawn.argv = def.command;
    out.spawn.argv[0] = *binary;
    out.spawn.cwd = root;
    for (const auto& [k, v] : def.env) out.spawn.extra_env.emplace_back(k, v);
    out.initialization = def.initialization;

    if (def.builtin_spawn && def.id == "typescript-language-server") {
        // 必须用 workspace 自带的 TypeScript;找不到说明这不是 TS 工程,
        // 启动只会得到无意义的全局推断,直接跳过(对齐 opencode)。
        const std::string tsserver = find_tsserver_js(workspace_cwd);
        if (tsserver.empty()) return std::nullopt;
        if (!out.initialization.is_object()) out.initialization = nlohmann::json::object();
        out.initialization["tsserver"] = {{"path", tsserver}};
    }
    if (def.builtin_spawn && def.id == "pyright") {
        const std::string python = find_venv_python(root);
        if (!python.empty()) {
            if (!out.initialization.is_object()) out.initialization = nlohmann::json::object();
            out.initialization["pythonPath"] = python;
        }
    }
    return out;
}

} // namespace acecode::lsp
