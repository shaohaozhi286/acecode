// 覆盖 src/lsp/lsp_service.{hpp,cpp} 的 workspace 边界判定与路径归一
// (对着 fake_lsp_server 真实 spawn,回归 2026-07-08 daemon 日志里的两个线上 bug):
//
//   Bug A(多 workspace):daemon 单进程经 routes_workspaces 服务多个
//     workspace 的会话,但 LSP 单例只认进程启动 cwd。goace 会话的 .go
//     文件因不在 daemon cwd(lsplearn)之下,within_workspace 直接拒绝,
//     gopls 永远不 spawn,lsp 工具永远报 "No LSP server available"。
//     修复:所有入口按调用传入的 session_cwd 判定边界,空串回退进程 cwd。
//
//   Bug B(junction 形态分裂):用户目录迁移后 C:\Users\x 是指向 N:\Users\x
//     的 junction。file_edit 的诊断注入用原始 C: 形态(能连上 server),
//     lsp 工具入口 weakly_canonical 后变 N: 形态,与 C: 形态的 workspace_cwd_
//     前缀比较假性失败 → 已连接的 server 查询时却报 "No LSP server available"。
//     修复:workspace 与文件路径两侧统一 weakly_canonical 后再比较/建 key。

#include <gtest/gtest.h>

#include "lsp/lsp_service.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace acecode::lsp;
namespace fs = std::filesystem;

namespace {

class LspServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = fs::temp_directory_path() /
                   ("acecode_lsp_svc_test_" +
                    std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
                    std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(tmp_dir_);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tmp_dir_, ec);
    }

    // 自定义 server 定义:command 用 fake_lsp_server 绝对路径(which 直通),
    // 扩展名 .zz 避免撞上内置定义;无 root marker → 恒以 workspace 为 root。
    acecode::LspConfig config_with_fake_server() {
        acecode::LspConfig cfg;
        cfg.enabled = true;
        acecode::LspServerOverride fake;
        fake.command = {ACECODE_FAKE_LSP_SERVER_PATH};
        fake.extensions = {".zz"};
        cfg.servers["fake"] = std::move(fake);
        return cfg;
    }

    static std::string write_sample(const fs::path& dir, const char* name) {
        fs::create_directories(dir);
        const fs::path file = dir / name;
        std::ofstream(file) << "sample\n";
        return file.string();
    }

    fs::path tmp_dir_;
};

} // namespace

// 场景(Bug A 回归):service 以 ws_a 为进程 cwd 初始化,文件在 ws_b。
// 期望:不带 session_cwd(空串回退进程 cwd)时拒绝;带 ws_b 时可用,
// 且真实 spawn + 诊断收集走通,连接后再查仍可用。
TEST_F(LspServiceTest, SessionCwdOverridesProcessWorkspace) {
    const fs::path ws_a = tmp_dir_ / "ws_a";
    const fs::path ws_b = tmp_dir_ / "ws_b";
    fs::create_directories(ws_a);
    const std::string file_b = write_sample(ws_b, "hello.zz");

    LspService svc(config_with_fake_server(), ws_a.string());

    // 进程 cwd 边界外 → 不可用(修复前后行为一致,守住边界语义)。
    EXPECT_FALSE(svc.has_server_for(file_b, ""));
    // 会话 cwd 生效 → 可用。修复前这里没有 session_cwd 通道,恒 false。
    EXPECT_TRUE(svc.has_server_for(file_b, ws_b.string()));

    // 真实 spawn:fake server didOpen 后立即推 1 条 ERROR 诊断。
    const auto diags = svc.collect_diagnostics_after_write(
        file_b, std::chrono::milliseconds(5000), AbortProbe{}, ws_b.string());
    EXPECT_FALSE(diags.empty());

    // 连接后再查仍可用(线上表现:连接成功 1 分钟后查询反而被拒)。
    EXPECT_TRUE(svc.has_server_for(file_b, ws_b.string()));
    EXPECT_EQ(svc.connected_snapshot().size(), 1u);
}

#ifdef _WIN32
// 场景(Bug B 回归,Windows-only):workspace 以 junction 形态初始化,
// 查询分别用 junction 形态与 canonical(真实)形态的文件路径。
// 期望:两种形态都可用,且共用同一个 (server, root) slot 不分裂。
// 修复前的 bug 表现:canonical 形态 within_workspace 前缀不匹配 → false。
TEST_F(LspServiceTest, JunctionAndCanonicalFormsShareOneWorkspace) {
    const fs::path real_ws = tmp_dir_ / "real_ws";
    const std::string file_real = write_sample(real_ws, "a.zz");
    const fs::path link_ws = tmp_dir_ / "link_ws";

    // 目录 junction 无需管理员权限;个别受限环境失败则跳过。
    const std::string cmd = "cmd /d /c mklink /J \"" + link_ws.string() + "\" \"" +
                            real_ws.string() + "\" >NUL 2>&1";
    if (std::system(cmd.c_str()) != 0 || !fs::exists(link_ws)) {
        GTEST_SKIP() << "mklink /J unavailable in this environment";
    }
    const std::string file_link = (link_ws / "a.zz").string();

    // 对应线上现场:daemon 以 junction 形态的 cwd 启动(C:\Users\x\proj)。
    LspService svc(config_with_fake_server(), link_ws.string());

    EXPECT_TRUE(svc.has_server_for(file_link, ""));
    // lsp 工具入口 weakly_canonical 后的真实形态 —— 修复前此断言失败。
    EXPECT_TRUE(svc.has_server_for(file_real, ""));

    // 以 junction 形态连接后,canonical 形态查询命中同一 slot。
    const auto diags = svc.collect_diagnostics_after_write(
        file_link, std::chrono::milliseconds(5000), AbortProbe{}, "");
    EXPECT_FALSE(diags.empty());
    EXPECT_TRUE(svc.has_server_for(file_real, ""));
    // 两种形态未把同一 workspace 分裂成两个 root/slot。
    EXPECT_EQ(svc.connected_snapshot().size(), 1u);
}
#endif
