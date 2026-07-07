// 覆盖 src/lsp/lsp_client.{hpp,cpp}:对着 fake_lsp_server(真实子进程 +
// stdio 管道 + Content-Length 分帧)做端到端集成验证。
//
// 覆盖项:
//   - initialize 握手成功;server_id/root 正确
//   - 握手超时(--slow-init 2000 + 300ms 超时)→ create 返回 nullptr 且
//     不悬挂(进程被清理)
//   - touch_file:首次 didOpen version=0,再次 didChange version 递增
//   - 诊断 push 等待:didOpen 后 wait_for_diagnostics 在 debounce 后返回,
//     diagnostics_for 拿到 fake error
//   - abort 探针:--no-diagnostics 下 5s 等待被 abort 在亚秒内打断
//   - 通用请求:hover 拿到固定应答;MethodNotFound 请求返回 nullopt
//   - shutdown 幂等:双重调用不崩、进程退出
//
// 超时阈值说明:计时断言给了宽裕余量(CI 慢机),只验证数量级
// (亚秒 vs 5 秒全额等待),不追求精确时间。

#include <gtest/gtest.h>

#include "lsp/lsp_client.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace acecode::lsp;
namespace fs = std::filesystem;

namespace {

class LspClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = fs::temp_directory_path() /
                   ("acecode_lsp_test_" +
                    std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
                    std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(tmp_dir_);
        test_file_ = tmp_dir_ / "sample.cpp";
        std::ofstream(test_file_) << "int main() { return 0; }\n";
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tmp_dir_, ec);
    }

    LspClient::CreateOptions options_with(std::vector<std::string> extra_args = {},
                                          int initialize_timeout_ms = 10000) {
        LspClient::CreateOptions options;
        options.server_id = "fake";
        options.root = tmp_dir_.string();
        options.spawn.argv = {ACECODE_FAKE_LSP_SERVER_PATH};
        for (auto& arg : extra_args) options.spawn.argv.push_back(std::move(arg));
        options.spawn.cwd = tmp_dir_.string();
        options.initialize_timeout = std::chrono::milliseconds(initialize_timeout_ms);
        return options;
    }

    fs::path tmp_dir_;
    fs::path test_file_;
};

} // namespace

// 场景:正常握手 → client 非空、alive、元数据正确;shutdown 干净退出且幂等。
TEST_F(LspClientTest, HandshakeAndIdempotentShutdown) {
    std::string error;
    auto client = LspClient::create(options_with(), &error);
    ASSERT_NE(client, nullptr) << error;
    EXPECT_TRUE(client->alive());
    EXPECT_EQ(client->server_id(), "fake");
    client->shutdown();
    EXPECT_FALSE(client->alive());
    client->shutdown(); // 第二次调用必须无害
}

// 场景:server 启动后 2s 才应答 initialize,而客户端只等 300ms →
// create 失败返回 nullptr,且调用在秒级内返回(进程被清理,不悬挂)。
TEST_F(LspClientTest, HandshakeTimeoutFailsFast) {
    const auto start = std::chrono::steady_clock::now();
    std::string error;
    auto client = LspClient::create(options_with({"--slow-init", "2000"}, 300), &error);
    EXPECT_EQ(client, nullptr);
    EXPECT_FALSE(error.empty());
    const auto elapsed = std::chrono::steady_clock::now() - start;
    // 300ms 超时 + 清理余量;远小于 fake 的 2s 慢应答说明没等满。
    EXPECT_LT(elapsed, std::chrono::milliseconds(1900));
}

// 场景:同一文件连续 touch —— 首次 didOpen(version 0),之后 didChange
// version 严格递增。server 的诊断按 version 回显,等待逻辑依赖这一点。
TEST_F(LspClientTest, TouchFileVersionsIncrease) {
    std::string error;
    auto client = LspClient::create(options_with(), &error);
    ASSERT_NE(client, nullptr) << error;

    auto v0 = client->touch_file(test_file_.string());
    ASSERT_TRUE(v0.has_value());
    EXPECT_EQ(*v0, 0);
    EXPECT_EQ(client->open_file_count(), 1);

    auto v1 = client->touch_file(test_file_.string());
    ASSERT_TRUE(v1.has_value());
    EXPECT_EQ(*v1, 1);
    EXPECT_EQ(client->open_file_count(), 1);

    client->shutdown();
}

// 场景:didOpen 后 fake 立即 push 一条 ERROR。wait_for_diagnostics 在
// debounce(150ms)后返回,缓存里能取到该诊断 —— 编辑后诊断注入的
// 核心链路。
TEST_F(LspClientTest, WaitReceivesPushedDiagnostics) {
    std::string error;
    auto client = LspClient::create(options_with(), &error);
    ASSERT_NE(client, nullptr) << error;

    auto version = client->touch_file(test_file_.string());
    ASSERT_TRUE(version.has_value());
    client->wait_for_diagnostics(test_file_.string(), *version,
                                 std::chrono::milliseconds(5000), nullptr);

    auto diags = client->diagnostics_for(test_file_.string());
    ASSERT_EQ(diags.size(), 1u);
    EXPECT_EQ(diags[0]["message"], "fake error");
    EXPECT_EQ(diags[0]["severity"], 1);

    client->shutdown();
}

// 场景:--no-diagnostics 下等待 5s 会全额超时;abort 探针置位后必须在
// 亚秒内(50ms 轮询粒度 + 余量)提前返回 —— 用户 Esc 不能被 LSP 卡住。
TEST_F(LspClientTest, AbortProbeInterruptsWait) {
    std::string error;
    auto client = LspClient::create(options_with({"--no-diagnostics"}), &error);
    ASSERT_NE(client, nullptr) << error;

    auto version = client->touch_file(test_file_.string());
    ASSERT_TRUE(version.has_value());

    std::atomic<bool> abort_flag{false};
    std::thread trigger([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        abort_flag.store(true);
    });
    const auto start = std::chrono::steady_clock::now();
    client->wait_for_diagnostics(test_file_.string(), *version,
                                 std::chrono::milliseconds(5000),
                                 [&] { return abort_flag.load(); });
    const auto elapsed = std::chrono::steady_clock::now() - start;
    trigger.join();
    // abort 150ms 触发 + 50ms 轮询粒度 + CI 余量;全额等待是 5000ms。
    EXPECT_LT(elapsed, std::chrono::milliseconds(2000));

    client->shutdown();
}

// 场景:通用请求 —— hover 拿到 fake 固定应答;未实现方法(server 回
// MethodNotFound 错误)→ request 返回 nullopt 而不是抛错/悬挂。
TEST_F(LspClientTest, GenericRequestAndMethodNotFound) {
    std::string error;
    auto client = LspClient::create(options_with(), &error);
    ASSERT_NE(client, nullptr) << error;

    auto hover = client->request(
        "textDocument/hover",
        {{"textDocument", {{"uri", "file:///x.cpp"}}},
         {"position", {{"line", 0}, {"character", 0}}}},
        std::chrono::milliseconds(5000));
    ASSERT_TRUE(hover.has_value());
    EXPECT_EQ((*hover)["contents"], "fake hover");

    auto missing = client->request("workspace/nonexistent", nlohmann::json::object(),
                                   std::chrono::milliseconds(5000));
    EXPECT_FALSE(missing.has_value());

    client->shutdown();
}
