// 覆盖 src/lsp/lsp_server_registry.{hpp,cpp}:内置定义、config 合并、
// root 探测(注入假文件系统)与 quote_windows_arg 引号规则。
//
// 覆盖项:
//   - 内置 5 个 server 的 id / 扩展名匹配
//   - config 合并:disabled 删除内置、command 覆盖(内置特化失效)、
//     自定义 server、缺 command 的自定义条目被忽略
//   - detect_root:NearestRoot 语义(最近目录优先)、marker 组顺序
//     (gopls 的 go.work > go.mod)、exclude marker(deno.json 让位)、
//     fallback_to_workspace 开关(rust-analyzer 无 Cargo.toml → 不适用)
//   - quote_windows_arg:空格/引号/反斜杠序列的标准 MSVCRT 规则

#include <gtest/gtest.h>

#include "lsp/lsp_process.hpp"
#include "lsp/lsp_server_registry.hpp"

#include <algorithm>
#include <set>
#include <string>

using namespace acecode::lsp;

namespace {

FileExistsFn fake_fs(std::set<std::string> files) {
    // detect_root 用 join_path 拼 marker 路径,分隔符依平台而定;
    // 统一归一成 '/' 后匹配,测试数据只写 '/'。
    return [files = std::move(files)](const std::string& path) {
        std::string normalized = path;
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        return files.count(normalized) > 0;
    };
}

const LspServerDef* find_def(const std::vector<LspServerDef>& defs, const std::string& id) {
    for (const auto& def : defs) {
        if (def.id == id) return &def;
    }
    return nullptr;
}

#ifdef _WIN32
const std::string kWs = "C:/ws";
const std::string kFile = "C:/ws/src/deep/a.cpp";
#else
const std::string kWs = "/ws";
const std::string kFile = "/ws/src/deep/a.cpp";
#endif

std::string ws_path(const std::string& rel) { return kWs + "/" + rel; }

} // namespace

// 场景:内置注册表包含且仅包含 v1 约定的 5 个 server。
TEST(LspServerRegistry, BuiltinSetIsFixed) {
    auto defs = builtin_server_defs();
    ASSERT_EQ(defs.size(), 5u);
    EXPECT_NE(find_def(defs, "clangd"), nullptr);
    EXPECT_NE(find_def(defs, "typescript-language-server"), nullptr);
    EXPECT_NE(find_def(defs, "pyright"), nullptr);
    EXPECT_NE(find_def(defs, "gopls"), nullptr);
    EXPECT_NE(find_def(defs, "rust-analyzer"), nullptr);
}

// 场景:扩展名匹配大小写不敏感(Windows 上 .CPP 常见);
// 无扩展文件不匹配有扩展约束的 server;自定义 server 扩展为空 = 匹配一切。
TEST(LspServerRegistry, ExtensionsMatching) {
    auto defs = builtin_server_defs();
    const auto* clangd = find_def(defs, "clangd");
    ASSERT_NE(clangd, nullptr);
    EXPECT_TRUE(extensions_match(*clangd, "a.cpp"));
    EXPECT_TRUE(extensions_match(*clangd, "A.CPP"));
    EXPECT_TRUE(extensions_match(*clangd, "x/y.h"));
    EXPECT_FALSE(extensions_match(*clangd, "a.rs"));
    EXPECT_FALSE(extensions_match(*clangd, "Makefile"));

    LspServerDef match_all;
    EXPECT_TRUE(extensions_match(match_all, "anything.xyz"));
}

// 场景:config `"clangd": {"disabled": true}` → 内置 clangd 被移除。
TEST(LspServerRegistry, ConfigDisablesBuiltin) {
    acecode::LspConfig cfg;
    cfg.servers["clangd"].disabled = true;
    auto defs = merge_server_defs(cfg);
    EXPECT_EQ(find_def(defs, "clangd"), nullptr);
    EXPECT_EQ(defs.size(), 4u);
}

// 场景:config 覆盖内置 command → builtin_spawn 特化关闭(用户接管启动),
// extensions 覆盖生效。
TEST(LspServerRegistry, ConfigOverridesBuiltinCommand) {
    acecode::LspConfig cfg;
    cfg.servers["pyright"].command = {"basedpyright-langserver", "--stdio"};
    cfg.servers["pyright"].extensions = {".py"};
    auto defs = merge_server_defs(cfg);
    const auto* pyright = find_def(defs, "pyright");
    ASSERT_NE(pyright, nullptr);
    EXPECT_EQ(pyright->command[0], "basedpyright-langserver");
    EXPECT_FALSE(pyright->builtin_spawn);
    ASSERT_EQ(pyright->extensions.size(), 1u);
}

// 场景:自定义 server(新名字 + command)进入清单,root 兜底 workspace;
// 缺 command 的自定义条目被忽略(LOG_WARN,不 crash 不误注册)。
TEST(LspServerRegistry, CustomServerAndInvalidEntry) {
    acecode::LspConfig cfg;
    cfg.servers["ocaml"].command = {"ocamllsp"};
    cfg.servers["ocaml"].extensions = {".ml"};
    cfg.servers["broken-entry"] = {}; // 无 command、非内置名
    auto defs = merge_server_defs(cfg);
    EXPECT_NE(find_def(defs, "ocaml"), nullptr);
    EXPECT_EQ(find_def(defs, "broken-entry"), nullptr);
    const auto* ocaml = find_def(defs, "ocaml");
    EXPECT_TRUE(ocaml->root.marker_groups.empty());
    EXPECT_TRUE(ocaml->root.fallback_to_workspace);
}

// 场景:NearestRoot —— marker 在多级目录都存在时,离文件最近的目录胜出。
TEST(LspRootDetect, NearestMarkerWins) {
    auto defs = builtin_server_defs();
    const auto* clangd = find_def(defs, "clangd");
    auto fs = fake_fs({ws_path("compile_commands.json"),
                       ws_path("src/compile_commands.json")});
    auto root = detect_root(*clangd, kFile, kWs, fs);
    ASSERT_TRUE(root.has_value());
    // kFile 在 <ws>/src/deep/ 下,最近的 marker 在 <ws>/src。
    std::string normalized = *root;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    EXPECT_EQ(normalized, ws_path("src"));
}

// 场景:所有 marker 缺失 + fallback_to_workspace=true(clangd)→ workspace 兜底。
TEST(LspRootDetect, FallbackToWorkspace) {
    auto defs = builtin_server_defs();
    const auto* clangd = find_def(defs, "clangd");
    auto root = detect_root(*clangd, kFile, kWs, fake_fs({}));
    ASSERT_TRUE(root.has_value());
    EXPECT_EQ(*root, kWs);
}

// 场景:rust-analyzer 无 Cargo.toml → 不适用(fallback_to_workspace=false)。
// 没有项目模型时启动 rust-analyzer 只会空转,必须判不适用。
TEST(LspRootDetect, RustAnalyzerRequiresCargoToml) {
    auto defs = builtin_server_defs();
    const auto* rust = find_def(defs, "rust-analyzer");
#ifdef _WIN32
    const std::string rs_file = "C:/ws/src/main.rs";
#else
    const std::string rs_file = "/ws/src/main.rs";
#endif
    EXPECT_FALSE(detect_root(*rust, rs_file, kWs, fake_fs({})).has_value());
    auto root = detect_root(*rust, rs_file, kWs, fake_fs({ws_path("Cargo.toml")}));
    ASSERT_TRUE(root.has_value());
    EXPECT_EQ(*root, kWs);
}

// 场景:gopls 的 marker 组顺序 —— go.work(第一组)即使在更远的目录,
// 也优先于更近的 go.mod(第二组)。多模块 workspace 的正确 root 语义。
TEST(LspRootDetect, GoplsWorkspaceGroupBeatsNearerGoMod) {
    auto defs = builtin_server_defs();
    const auto* gopls = find_def(defs, "gopls");
#ifdef _WIN32
    const std::string go_file = "C:/ws/mod/pkg/a.go";
#else
    const std::string go_file = "/ws/mod/pkg/a.go";
#endif
    auto fs = fake_fs({ws_path("go.work"), ws_path("mod/go.mod")});
    auto root = detect_root(*gopls, go_file, kWs, fs);
    ASSERT_TRUE(root.has_value());
    EXPECT_EQ(*root, kWs);
}

// 场景:typescript 的 exclude marker —— 探测链上存在 deno.json 时判不适用
// (deno 工程不该被 tsserver 接管)。
TEST(LspRootDetect, TypescriptExcludedByDenoMarker) {
    auto defs = builtin_server_defs();
    const auto* ts = find_def(defs, "typescript-language-server");
#ifdef _WIN32
    const std::string ts_file = "C:/ws/src/a.ts";
#else
    const std::string ts_file = "/ws/src/a.ts";
#endif
    auto fs = fake_fs({ws_path("deno.json"), ws_path("package-lock.json")});
    EXPECT_FALSE(detect_root(*ts, ts_file, kWs, fs).has_value());
}

// 场景:quote_windows_arg 的标准规则 —— 无特殊字符原样;含空格包引号;
// 内部引号转义;结尾反斜杠在闭引号前翻倍(经典坑:`C:\dir\` → `"C:\dir\\"`)。
TEST(LspProcess, QuoteWindowsArg) {
    EXPECT_EQ(quote_windows_arg("clangd"), "clangd");
    EXPECT_EQ(quote_windows_arg("a b"), "\"a b\"");
    EXPECT_EQ(quote_windows_arg("say \"hi\""), "\"say \\\"hi\\\"\"");
    EXPECT_EQ(quote_windows_arg("C:\\dir with space\\"), "\"C:\\dir with space\\\\\"");
    EXPECT_EQ(quote_windows_arg(""), "\"\"");
}
