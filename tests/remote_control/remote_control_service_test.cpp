#include <gtest/gtest.h>

#include "remote_control/remote_control_service.hpp"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

using acecode::rc::RemoteControlOptions;
using acecode::rc::RemoteControlService;

namespace {

// 每个测试用独立端口,避免并行/重跑时端口残留互踩(同 web_server_smoke_test
// 的 fetch_add 惯例;步长 7 错开常见的连号占用)。
int next_port() {
    static std::atomic<int> next{28411};
    return next.fetch_add(7);
}

struct SubmitCapture {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::string> received;

    void push(const std::string& t) {
        std::lock_guard<std::mutex> lk(mu);
        received.push_back(t);
        cv.notify_all();
    }
    bool wait_for_count(std::size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, timeout, [&] { return received.size() >= n; });
    }
};

std::string base_url(int port) {
    return "http://127.0.0.1:" + std::to_string(port);
}

} // namespace

// 场景:服务启动后的完整入站链路。期望:
//   - /rc/health 无 token 可达(仅活性探测)
//   - 正确 token 的 /rc/send → 200,文本到达 inbound_submit
//   - 错误 token → 401;缺 text 字段 → 400
//   - ?token= 查询参数与 header 等效(channel bridge fetch 不便加 header 时的备选)
TEST(RemoteControlService, ServesHealthAndInboundSend) {
    RemoteControlService service;
    SubmitCapture capture;
    service.hub().set_inbound_submit([&](const std::string& t) { capture.push(t); });

    RemoteControlOptions opts;
    opts.port = next_port();
    opts.token = "tok-123";
    opts.session_id = "sess-1";
    std::string error;
    ASSERT_TRUE(service.start(opts, &error)) << error;

    auto health = cpr::Get(cpr::Url{base_url(opts.port) + "/rc/health"},
                           cpr::Timeout{2000});
    EXPECT_EQ(health.status_code, 200);

    auto ok = cpr::Post(cpr::Url{base_url(opts.port) + "/rc/send"},
                        cpr::Header{{"Content-Type", "application/json"},
                                    {"X-ACECode-RC-Token", "tok-123"}},
                        cpr::Body{nlohmann::json{{"text", u8"来自channel的消息"}}.dump()},
                        cpr::Timeout{2000});
    EXPECT_EQ(ok.status_code, 200);
    ASSERT_TRUE(capture.wait_for_count(1, std::chrono::seconds(5)));
    EXPECT_EQ(capture.received[0], u8"来自channel的消息");

    auto bad_token = cpr::Post(cpr::Url{base_url(opts.port) + "/rc/send"},
                               cpr::Header{{"Content-Type", "application/json"},
                                           {"X-ACECode-RC-Token", "nope"}},
                               cpr::Body{R"({"text":"x"})"},
                               cpr::Timeout{2000});
    EXPECT_EQ(bad_token.status_code, 401);

    auto bad_body = cpr::Post(cpr::Url{base_url(opts.port) + "/rc/send"},
                              cpr::Header{{"Content-Type", "application/json"},
                                          {"X-ACECode-RC-Token", "tok-123"}},
                              cpr::Body{R"({"no_text":1})"},
                              cpr::Timeout{2000});
    EXPECT_EQ(bad_body.status_code, 400);

    auto query_token = cpr::Post(
        cpr::Url{base_url(opts.port) + "/rc/send?token=tok-123"},
        cpr::Header{{"Content-Type", "application/json"}},
        cpr::Body{R"({"text":"via query"})"},
        cpr::Timeout{2000});
    EXPECT_EQ(query_token.status_code, 200);
    ASSERT_TRUE(capture.wait_for_count(2, std::chrono::seconds(5)));

    service.stop();
}

TEST(RemoteControlService, PluginSubmittedUserTextUsesTokenProtectedSessionInputPath) {
    RemoteControlService service;
    SubmitCapture capture;
    service.hub().set_inbound_submit([&](const std::string& t) { capture.push(t); });

    RemoteControlOptions opts;
    opts.port = next_port();
    opts.token = "active-token";
    opts.session_id = "session-from-plugin";
    std::string error;
    ASSERT_TRUE(service.start(opts, &error)) << error;

    auto accepted = cpr::Post(cpr::Url{base_url(opts.port) + "/rc/send"},
                              cpr::Header{{"Content-Type", "application/json"},
                                          {"X-ACECode-RC-Token", "active-token"}},
                              cpr::Body{R"({"text":"from plugin"})"},
                              cpr::Timeout{2000});
    EXPECT_EQ(accepted.status_code, 200);
    ASSERT_TRUE(capture.wait_for_count(1, std::chrono::seconds(5)));
    EXPECT_EQ(capture.received[0], "from plugin");

    auto rejected = cpr::Post(cpr::Url{base_url(opts.port) + "/rc/send"},
                              cpr::Header{{"Content-Type", "application/json"},
                                          {"X-ACECode-RC-Token", "stale-token"}},
                              cpr::Body{R"({"text":"should not arrive"})"},
                              cpr::Timeout{2000});
    EXPECT_EQ(rejected.status_code, 401);
    EXPECT_FALSE(capture.wait_for_count(2, std::chrono::milliseconds(200)));

    service.stop();
}

// 场景:端口已被占用(另一个服务实例在监听)。期望:start 预检立即失败并
// 给出可读错误,不留半启动状态 —— hub 不应处于 enabled。
TEST(RemoteControlService, StartFailsWhenPortInUse) {
    RemoteControlService first;
    RemoteControlOptions opts;
    opts.port = next_port();
    opts.token = "tok-a";
    std::string error;
    ASSERT_TRUE(first.start(opts, &error)) << error;

    RemoteControlService second;
    opts.token = "tok-b";
    EXPECT_FALSE(second.start(opts, &error));
    EXPECT_NE(error.find("in use"), std::string::npos);
    EXPECT_FALSE(second.running());
    EXPECT_FALSE(second.hub().enabled());

    first.stop();
}

// 场景:stop 后同端口重新 start(用户 /remote-control off 再 on)。
// 期望:端口正常释放,二次启动成功。
TEST(RemoteControlService, StopReleasesPortForRestart) {
    RemoteControlService service;
    RemoteControlOptions opts;
    opts.port = next_port();
    opts.token = "tok-1";
    std::string error;
    ASSERT_TRUE(service.start(opts, &error)) << error;
    service.stop();
    EXPECT_FALSE(service.running());

    ASSERT_TRUE(service.start(opts, &error)) << error;
    EXPECT_TRUE(service.running());
    service.stop();
}

// 场景:token 生成器。期望:32 个十六进制字符,且两次生成不同(随机源)。
TEST(RemoteControlService, GeneratedTokenIs32HexChars) {
    std::string a = acecode::rc::generate_remote_control_token();
    std::string b = acecode::rc::generate_remote_control_token();
    EXPECT_EQ(a.size(), 32u);
    EXPECT_NE(a, b);
    for (char c : a) {
        EXPECT_TRUE(std::isxdigit(static_cast<unsigned char>(c))) << a;
    }
}
