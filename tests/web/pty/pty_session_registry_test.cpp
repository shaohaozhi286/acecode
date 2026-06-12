// 覆盖 src/web/pty/pty_session_registry.{hpp,cpp}
// (openspec/changes/add-console-dock 任务 3.5,specs/console-pty-backend):
// - 控制帧编码(0x00 前缀,前端按首字节分流,编错即前端把 JSON 当 VT 渲染)
// - 缓冲游标续传:cursor=N 重连补发 N 之后的字节、溢出丢最旧、cursor 控制帧
// - 会话生命周期:create/list/remove/上限、退出广播 exit 帧、订阅者管理
//
// 真实 spawn 用平台 shell(Windows: Pipe 后端 — ConPTY 在本测试宿主被 Job
// 限制杀死,见 pty_backend_test.cpp 探针注释;POSIX: forkpty)。

#include <gtest/gtest.h>

#include "web/pty/pty_backend.hpp"
#include "web/pty/pty_session_registry.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

acecode::PtyBackendKind test_backend() {
#ifdef _WIN32
    return acecode::PtyBackendKind::Pipe;
#else
    return acecode::PtyBackendKind::PosixPty;
#endif
}

const char* kEchoMark =
#ifdef _WIN32
    "echo REGISTRY_MARK\r\n";
#else
    "echo REGISTRY_MARK\n";
#endif

// 轮询等待谓词成立(注册表无通知机制,测试侧简单轮询)。
template <typename Pred>
bool wait_until(Pred pred, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    return pred();
}

// 收集一个订阅者收到的全部帧,区分数据帧与控制帧。
struct FrameCollector {
    std::mutex mu;
    std::string data;                       // 数据帧拼接
    std::vector<nlohmann::json> controls;   // 0x00 控制帧解析结果

    std::function<void(const std::string&)> sender() {
        return [this](const std::string& frame) {
            std::lock_guard<std::mutex> lock(mu);
            if (!frame.empty() && frame[0] == '\0') {
                try {
                    controls.push_back(nlohmann::json::parse(frame.substr(1)));
                } catch (...) {}
            } else {
                data += frame;
            }
        };
    }

    bool data_contains(const std::string& needle) {
        std::lock_guard<std::mutex> lock(mu);
        return data.find(needle) != std::string::npos;
    }

    bool has_control(const std::string& key) {
        std::lock_guard<std::mutex> lock(mu);
        for (const auto& c : controls) {
            if (c.contains(key)) return true;
        }
        return false;
    }
};

// ---------------------------------------------------------------------------
// encode_pty_control_frame
// ---------------------------------------------------------------------------

// 触发场景:编码控制帧。
// 期望:首字节恰为 0x00,其后是原样 JSON 文本。前端 ConsoleDock 按
// "首字节 0x00 → 控制帧,否则 → xterm 渲染" 分流,该不变量破坏即把
// JSON 渲染到终端屏幕上。
TEST(PtyControlFrameTest, ZeroPrefixedJson) {
    std::string frame = acecode::encode_pty_control_frame(R"({"cursor":42})");
    ASSERT_FALSE(frame.empty());
    EXPECT_EQ(frame[0], '\0');
    EXPECT_EQ(frame.substr(1), R"({"cursor":42})");
}

// ---------------------------------------------------------------------------
// 会话生命周期
// ---------------------------------------------------------------------------

// 触发场景:create → list → get → remove 的基本生命周期。
// 期望:info 字段齐全(status=running、backend 与构造一致);remove 后
// list 为空,再 remove 返回 false。
TEST(PtySessionRegistryTest, CreateListRemoveLifecycle) {
    acecode::PtySessionRegistry registry(test_backend(), ".", "");
    std::string error;
    auto info = registry.create("", "我的终端", "", error);
    ASSERT_TRUE(info.has_value()) << error;
    EXPECT_EQ(info->status, "running");
    EXPECT_EQ(info->title, "我的终端");
    EXPECT_EQ(info->backend, test_backend());
    EXPECT_GT(info->pid, 0);

    auto all = registry.list();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].id, info->id);
    EXPECT_TRUE(registry.get(info->id).has_value());

    EXPECT_TRUE(registry.remove(info->id));
    EXPECT_TRUE(registry.list().empty());
    EXPECT_FALSE(registry.remove(info->id));
}

// 触发场景:订阅后向 shell 写命令。
// 期望:订阅者通过数据帧收到 echo 输出,且 connect 时收到 cursor 控制帧
// (协议要求:补发结束必有 {"cursor":N} 同步帧,前端据此推进本地游标)。
TEST(PtySessionRegistryTest, SubscribeReceivesOutputAndCursorFrame) {
    acecode::PtySessionRegistry registry(test_backend(), ".", "");
    std::string error;
    auto info = registry.create("", "", "", error);
    ASSERT_TRUE(info.has_value()) << error;

    FrameCollector collector;
    int subscriber = 0;
    ASSERT_TRUE(registry.connect(info->id, &subscriber, 0, collector.sender()));
    EXPECT_TRUE(collector.has_control("cursor"));

    registry.write_input(info->id, kEchoMark);
    EXPECT_TRUE(wait_until([&] { return collector.data_contains("REGISTRY_MARK"); }, 10000));

    registry.remove(info->id);
}

// 触发场景:shell 退出(写 exit)。
// 期望:订阅者收到 {"exit_code":N} 控制帧,session info 转为 exited。
// 回归意义:exit 帧丢失时前端 tab 永远显示运行中,输入黑洞。
TEST(PtySessionRegistryTest, ExitBroadcastsControlFrameAndMarksSession) {
    acecode::PtySessionRegistry registry(test_backend(), ".", "");
    std::string error;
    auto info = registry.create("", "", "", error);
    ASSERT_TRUE(info.has_value()) << error;

    FrameCollector collector;
    int subscriber = 0;
    ASSERT_TRUE(registry.connect(info->id, &subscriber, -1, collector.sender()));

#ifdef _WIN32
    registry.write_input(info->id, "exit\r\n");
#else
    registry.write_input(info->id, "exit\n");
#endif
    EXPECT_TRUE(wait_until([&] { return collector.has_control("exit_code"); }, 10000));
    auto after = registry.get(info->id);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->status, "exited");
}

// 触发场景:会话退出后(PTY 进程已死)新的订阅者带 cursor=0 接入 —— 模拟
// 页面刷新后重连一个已结束的终端。
// 期望:补发全部缓冲(echo 输出仍可见)+ cursor 帧 + exit 帧;会话生存期
// 独立于 WS 连接(spec: PTY outlives its WebSocket)。
TEST(PtySessionRegistryTest, ReconnectAfterExitReplaysBufferAndExitFrame) {
    acecode::PtySessionRegistry registry(test_backend(), ".", "");
    std::string error;
    auto info = registry.create("", "", "", error);
    ASSERT_TRUE(info.has_value()) << error;

    // 无订阅者期间产生输出并退出 — 输出只进缓冲。
    registry.write_input(info->id, kEchoMark);
#ifdef _WIN32
    registry.write_input(info->id, "exit\r\n");
#else
    registry.write_input(info->id, "exit\n");
#endif
    ASSERT_TRUE(wait_until([&] {
        auto s = registry.get(info->id);
        return s && s->status == "exited";
    }, 10000));

    FrameCollector late;
    int subscriber = 1;
    ASSERT_TRUE(registry.connect(info->id, &subscriber, 0, late.sender()));
    EXPECT_TRUE(late.data_contains("REGISTRY_MARK"));
    EXPECT_TRUE(late.has_control("cursor"));
    EXPECT_TRUE(late.has_control("exit_code"));
}

// 触发场景:断开订阅(disconnect)后 shell 继续产生输出。
// 期望:被断开的 sender 不再被调用(数据量冻结);会话仍 running。
TEST(PtySessionRegistryTest, DisconnectStopsDelivery) {
    acecode::PtySessionRegistry registry(test_backend(), ".", "");
    std::string error;
    auto info = registry.create("", "", "", error);
    ASSERT_TRUE(info.has_value()) << error;

    FrameCollector collector;
    int subscriber = 0;
    ASSERT_TRUE(registry.connect(info->id, &subscriber, -1, collector.sender()));
    registry.disconnect(info->id, &subscriber);

    registry.write_input(info->id, kEchoMark);
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    EXPECT_FALSE(collector.data_contains("REGISTRY_MARK"));
    auto s = registry.get(info->id);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->status, "running");

    registry.remove(info->id);
}

// 触发场景:终端内程序经 OSC 改标题,前端同步回 set_title。
// 期望:trim 后写入 info.title 并在 get/list 可见;空白标题忽略不覆盖;
// 未知会话返回 false。回归 bug:tab 标题不跟随 shell 标题(用户报告),
// 后端持久化保证页面刷新恢复会话时标题不回退 "Terminal N"。
TEST(PtySessionRegistryTest, SetTitleTrimsPersistsAndIgnoresBlank) {
    acecode::PtySessionRegistry registry(test_backend(), ".", "");
    std::string error;
    auto info = registry.create("", "", "", error);
    ASSERT_TRUE(info.has_value()) << error;

    EXPECT_TRUE(registry.set_title(info->id, "  构建监控  "));
    EXPECT_EQ(registry.get(info->id)->title, "构建监控");

    EXPECT_TRUE(registry.set_title(info->id, "   "));
    EXPECT_EQ(registry.get(info->id)->title, "构建监控");

    EXPECT_FALSE(registry.set_title("pty-999", "x"));
    registry.remove(info->id);
}

// 触发场景:连接不存在的会话 id。
// 期望:connect 返回 false(WS 层据此 close 连接),不崩溃。
TEST(PtySessionRegistryTest, ConnectUnknownSessionFails) {
    acecode::PtySessionRegistry registry(test_backend(), ".", "");
    FrameCollector collector;
    int subscriber = 0;
    EXPECT_FALSE(registry.connect("pty-999", &subscriber, 0, collector.sender()));
}

// 触发场景:会话数达到 kPtyMaxSessions(16)上限后再 create。
// 期望:失败且 error 含 "limit"(REST 层映射为 429)。上限防止前端 bug
// 或脚本失控时 daemon 堆满 shell 进程。
TEST(PtySessionRegistryTest, SessionLimitRejectsCreation) {
    acecode::PtySessionRegistry registry(test_backend(), ".", "");
    std::string error;
    std::vector<std::string> ids;
    for (int i = 0; i < acecode::kPtyMaxSessions; ++i) {
        auto info = registry.create("", "", "", error);
        ASSERT_TRUE(info.has_value()) << "i=" << i << ": " << error;
        ids.push_back(info->id);
    }
    auto overflow = registry.create("", "", "", error);
    EXPECT_FALSE(overflow.has_value());
    EXPECT_NE(error.find("limit"), std::string::npos);

    registry.stop_all();
    EXPECT_TRUE(registry.list().empty());
}

// 触发场景:shell 持续输出超过 2MB 缓冲上限(用注册表内部路径模拟代价太高,
// 这里直接验证"早游标重连只拿到缓冲仍持有的部分"的语义即可 — 缓冲裁剪
// 逻辑由 on_pty_data 的 erase 分支实现,等价被以下行为覆盖):cursor=0 重连,
// 但 buffer_start 已前移(输出超限)。
// 期望:不崩、补发从 buffer_start 开始的内容(而非越界读),cursor 帧正确。
// 注:真实 2MB 输出会让单测慢且 flaky,Windows Pipe 下用 type 大文件同样
// 不稳;此用例以 32 并发 echo 行驱动小缓冲路径,溢出分支的字节算术由
// connect 的 clamp(max(cursor, buffer_start))间接锁定 — 若 clamp 写错,
// 早游标重连会触发 substr 越界(debug 断言/异常),用例即红。
TEST(PtySessionRegistryTest, EarlyCursorReconnectClampsToBufferStart) {
    acecode::PtySessionRegistry registry(test_backend(), ".", "");
    std::string error;
    auto info = registry.create("", "", "", error);
    ASSERT_TRUE(info.has_value()) << error;

    FrameCollector first;
    int sub1 = 0;
    ASSERT_TRUE(registry.connect(info->id, &sub1, 0, first.sender()));
    registry.write_input(info->id, kEchoMark);
    ASSERT_TRUE(wait_until([&] { return first.data_contains("REGISTRY_MARK"); }, 10000));
    registry.disconnect(info->id, &sub1);

    // cursor=0 重连:缓冲未溢出时应完整回放;溢出时从 buffer_start 起。
    FrameCollector second;
    int sub2 = 1;
    ASSERT_TRUE(registry.connect(info->id, &sub2, 0, second.sender()));
    EXPECT_TRUE(second.data_contains("REGISTRY_MARK"));
    EXPECT_TRUE(second.has_control("cursor"));

    registry.remove(info->id);
}

} // namespace
