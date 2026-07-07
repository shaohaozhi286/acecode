#pragma once

// LSP server 注册表:内置 server 定义(探测已安装的可执行文件,绝不
// 自动下载)+ config.lsp.servers 按名合并。root 探测与合并逻辑是纯函数
// (文件存在性经 FileExistsFn 注入),单测不落真实文件系统。
// 内置定义的 marker/命令与 opencode packages/opencode/src/lsp/server.ts
// 对应条目对齐(v1 收录 clangd / typescript-language-server / pyright /
// gopls / rust-analyzer)。

#include "lsp_process.hpp"
#include "lsp_which.hpp"
#include "../config/config.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace acecode::lsp {

struct LspRootRule {
    // 依序尝试的 marker 组:每组做一次 NearestRoot(从文件目录向上找,
    // 止于 workspace cwd),第一组命中即返回。gopls 用两组表达
    // 「go.work 优先于 go.mod」。
    std::vector<std::vector<std::string>> marker_groups;
    // 向上探测途中命中任一 exclude marker → 该 server 对此文件不适用
    // (typescript 遇 deno.json 让位给 deno 的规则)。
    std::vector<std::string> exclude_markers;
    // 所有组都未命中时:true → 用 workspace cwd 兜底;false → 不适用
    // (rust-analyzer 无 Cargo.toml 就没有启动意义)。
    bool fallback_to_workspace = true;
};

struct LspServerDef {
    std::string id;
    std::vector<std::string> extensions; // 空 = 匹配所有文件(仅自定义 server)
    LspRootRule root;
    // argv。builtin 的 argv[0] 是命令名,resolve_spawn 时经 which 解析;
    // config 覆盖/自定义的 command 同样走 which(允许写完整路径)。
    std::vector<std::string> command;
    std::map<std::string, std::string> env;
    nlohmann::json initialization;
    // true = resolve_spawn 走该 id 的内置特化(tsserver 路径探测 /
    // pyright venv 探测)。config 覆盖 command 后转为 false(纯通用逻辑)。
    bool builtin_spawn = false;
};

struct ResolvedSpawn {
    LspSpawnOptions spawn;
    nlohmann::json initialization;
};

std::vector<LspServerDef> builtin_server_defs();

// 内置定义 + config 合并。非法条目(自定义无 command)LOG_WARN 后跳过。
std::vector<LspServerDef> merge_server_defs(const LspConfig& cfg);

bool extensions_match(const LspServerDef& def, const std::string& utf8_file);

// root 探测。utf8_file 必须位于 workspace_cwd 之下(调用方保证)。
// 返回 nullopt = 此 server 对该文件不适用。
std::optional<std::string> detect_root(const LspServerDef& def,
                                       const std::string& utf8_file,
                                       const std::string& workspace_cwd,
                                       const FileExistsFn& exists);

// 探测可执行文件并组装 spawn 参数。探测不到返回 nullopt(静默跳过)。
std::optional<ResolvedSpawn> resolve_spawn(const LspServerDef& def,
                                           const std::string& root,
                                           const std::string& workspace_cwd);

} // namespace acecode::lsp
