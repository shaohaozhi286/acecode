// 覆盖 src/lsp/lsp_which.{hpp,cpp}:PATH 可执行探测的纯逻辑核心 which_in()。
//
// LSP server 只在探测命中时启用,探测语义必须与真实 spawn 一致 ——
// 尤其 Windows 上 npm 全局命令是 .cmd shim,漏掉 PATHEXT 拼接会导致
// typescript-language-server / pyright 永远"未安装"。文件系统经
// FileExistsFn 注入,测试不落盘。
//
// 覆盖项:
//   - POSIX 语义(pathext 空):按原名逐目录探测,PATH 顺序优先
//   - Windows 语义:PATHEXT 逐扩展拼接,.cmd shim 命中
//   - 命令已带扩展 → 不重复拼接
//   - 含路径分隔符的命令 → 不遍历 PATH,只按给定路径探测
//   - 找不到 → nullopt;空命令 → nullopt

#include <gtest/gtest.h>

#include "lsp/lsp_which.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>

using acecode::lsp::which_in;

namespace {

// 真实文件系统在 Windows 上大小写不敏感、'/'与'\\'等价;fake 探针把两者
// 都归一(全小写 + '/'),否则 PATHEXT 大写扩展(.CMD)拼出的候选永远
// 匹配不上小写测试数据 —— 这不是产线语义,是测试替身该抹平的差异。
acecode::lsp::FileExistsFn fake_fs(std::set<std::string> files) {
    auto canon = [](std::string p) {
        std::replace(p.begin(), p.end(), '\\', '/');
        std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return p;
    };
    std::set<std::string> normalized;
    for (const auto& f : files) normalized.insert(canon(f));
    return [normalized = std::move(normalized), canon](const std::string& path) {
        return normalized.count(canon(path)) > 0;
    };
}

} // namespace

// 场景:POSIX —— 无 PATHEXT,按原名探测;PATH 中第一个命中的目录胜出。
TEST(LspWhich, PosixFirstPathDirWins) {
    auto fs = fake_fs({"/usr/bin/gopls", "/usr/local/bin/gopls"});
    auto hit = which_in("gopls", {"/usr/local/bin", "/usr/bin"}, {}, fs);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(*hit, "/usr/local/bin/gopls");
}

// 场景:Windows —— 命令无扩展时按 PATHEXT 顺序拼接,命中 .cmd shim。
// 这是 npm 全局安装(typescript-language-server.cmd)的真实形态。
TEST(LspWhich, WindowsPathextResolvesCmdShim) {
    auto fs = fake_fs({"C:\\npm\\typescript-language-server.cmd"});
    auto hit = which_in("typescript-language-server", {"C:\\Windows", "C:\\npm"},
                        {".com", ".exe", ".bat", ".cmd"}, fs);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(*hit, "C:\\npm\\typescript-language-server.cmd");
}

// 场景:Windows —— .exe 优先于 .cmd(PATHEXT 顺序决定)。
TEST(LspWhich, WindowsPathextOrderPrefersExe) {
    auto fs = fake_fs({"C:\\bin\\tool.exe", "C:\\bin\\tool.cmd"});
    auto hit = which_in("tool", {"C:\\bin"}, {".com", ".exe", ".bat", ".cmd"}, fs);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(*hit, "C:\\bin\\tool.exe");
}

// 场景:命令已显式带 .exe 扩展(大小写不敏感)→ 不再拼接 PATHEXT。
TEST(LspWhich, ExplicitExtensionNotDoubled) {
    auto fs = fake_fs({"C:\\bin\\clangd.EXE"});
    auto hit = which_in("clangd.EXE", {"C:\\bin"}, {".com", ".exe"}, fs);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(*hit, "C:\\bin\\clangd.EXE");
}

// 场景:命令含路径分隔符(用户在 config 写了完整路径)→ 不遍历 PATH,
// 直接按该路径(+扩展)探测。
TEST(LspWhich, PathLikeCommandSkipsPathDirs) {
    auto fs = fake_fs({"D:\\tools\\mylang-ls.exe"});
    auto hit = which_in("D:\\tools\\mylang-ls", {"C:\\bin"}, {".exe"}, fs);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(*hit, "D:\\tools\\mylang-ls.exe");
}

// 场景:处处找不到 → nullopt(server 静默跳过的依据);空命令同样 nullopt。
TEST(LspWhich, NotFoundAndEmpty) {
    auto fs = fake_fs({});
    EXPECT_FALSE(which_in("rust-analyzer", {"/usr/bin"}, {}, fs).has_value());
    EXPECT_FALSE(which_in("", {"/usr/bin"}, {}, fs).has_value());
}
