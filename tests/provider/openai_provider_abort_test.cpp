// 覆盖 src/provider/openai_provider.cpp 中 SSE 流式请求的取消路径。
//
// 关注点：当大模型处于"推理"阶段、长时间不发送任何 SSE 字节时，按 Esc 触发的
// abort_flag 必须能在 ~1 秒内取消正在进行的 HTTP 请求。修复前 abort_flag 仅在
// libcurl write callback（收到字节才触发）里被检查，所以静默期内会一直挂起，
// 最坏情况要等 cpr::Timeout（180s）才返回。
//
// 修复方案是把 abort_flag 也接到 libcurl 的 progress / xferinfo callback
// （cpr::ProgressCallback），它每秒触发一次，与是否有数据流无关。
//
// 这里用 httplib 启动一个本地 HTTP server，模拟两种场景：
//   1. 静默服务器：accept 之后睡 60 秒不写任何东西 —— 验证 progress callback
//      能在 1.5 秒内取消请求；
//   2. 正常 SSE 服务器：返回一段含 `[DONE]` 的标准响应 —— 回归保护，确认新增
//      的 progress callback 不影响 happy path。
//
// 备注：httplib 的 server 在 Windows / POSIX 都能跑；端口由 bind_to_any_port
// 选取，避免与开发机已占用端口冲突。

#include <gtest/gtest.h>

#include "provider/openai_provider.hpp"
#include "provider/llm_provider.hpp"
#include "utils/logger.hpp"

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using acecode::ChatMessage;
using acecode::OpenAiCompatProvider;
using acecode::StreamEvent;
using acecode::StreamEventType;
using acecode::ToolDef;

// 启动一个本地 httplib server 的小工具：bind 任意端口、在后台线程 listen，
// 析构时自动 stop。返回端口号供测试拼 base_url。
struct LocalHttpServer {
    httplib::Server svr;
    int port = 0;
    std::thread th;

    explicit LocalHttpServer(std::function<void(httplib::Server&)> setup) {
        setup(svr);
        port = svr.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { svr.listen_after_bind(); });
        // 等服务器进入 listen 状态，避免 client 抢在前面发起连接。
        for (int i = 0; i < 50 && !svr.is_running(); ++i) {
            std::this_thread::sleep_for(10ms);
        }
    }

    ~LocalHttpServer() {
        svr.stop();
        if (th.joinable()) th.join();
    }
};

// 用例 1：模型静默推理期触发取消
//
// 服务器 accept 之后睡 60 秒，期间不写任何字节。
// 此时旧实现只会卡在 libcurl 的 read 阻塞里，abort_flag 永远不会被检查；
// 新实现的 ProgressCallback 必须能在 1.5 秒内取消请求并返回。
TEST(OpenAiProviderAbortTest, CancelDuringSilentReasoningPhase) {
    LocalHttpServer server([](httplib::Server& s) {
        s.Post("/chat/completions", [](const httplib::Request&, httplib::Response& res) {
            // 模拟"思考中、还没有 token"的状态。3s 远大于 1.5s 阈值，
            // 如果 abort 没生效，测试会因为超时失败而不是误判通过；同时
            // 这个 sleep 也决定了 server.stop() 的最长等待时间（httplib
            // 等正在执行的 handler 自然返回），所以不要设得太大。
            std::this_thread::sleep_for(3s);
            res.status = 200;
        });
    });

    std::atomic<bool> abort_flag{false};
    std::vector<StreamEvent> events;
    std::mutex evt_mu;

    OpenAiCompatProvider provider(
        "http://127.0.0.1:" + std::to_string(server.port),
        /*api_key=*/"",
        /*model=*/"test-model"
    );

    ChatMessage msg;
    msg.role = "user";
    msg.content = "hello";
    std::vector<ChatMessage> messages = {msg};
    std::vector<ToolDef> tools;

    auto callback = [&](const StreamEvent& evt) {
        std::lock_guard<std::mutex> lk(evt_mu);
        events.push_back(evt);
    };

    auto t0 = std::chrono::steady_clock::now();
    std::thread worker([&] {
        provider.chat_stream(messages, tools, callback, &abort_flag);
    });

    // 等连接建立、libcurl 真正进入"等响应"状态后再触发取消。
    std::this_thread::sleep_for(200ms);
    abort_flag.store(true);

    worker.join();
    auto elapsed = std::chrono::steady_clock::now() - t0;

    // 1500 ms 阈值来源于 spec：Esc 之后必须在 ~1.5s 内中断（libcurl
    // progress callback 默认 ~1Hz，再加上一点余量）。
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 1500)
        << "abort 在静默推理期应在 1.5 秒内生效，实际耗时="
        << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << " ms";

    // 应该收到且只收到一个 Error 事件，文案稳定为 "Request cancelled"，
    // 且不会有 "Connection failed: ..." 之类的伪装为网络错误的事件泄露。
    int error_count = 0;
    bool saw_request_cancelled = false;
    bool saw_connection_failed = false;
    {
        std::lock_guard<std::mutex> lk(evt_mu);
        for (const auto& evt : events) {
            if (evt.type == StreamEventType::Error) {
                ++error_count;
                if (evt.error == "Request cancelled") saw_request_cancelled = true;
                if (evt.error.rfind("Connection failed:", 0) == 0) saw_connection_failed = true;
            }
        }
    }
    EXPECT_EQ(error_count, 1);
    EXPECT_TRUE(saw_request_cancelled);
    EXPECT_FALSE(saw_connection_failed);
}

// 用例 2：正常 SSE 响应，abort_flag 始终为 false
//
// 回归保护：新增的 ProgressCallback 不能影响 happy path。
// 服务器返回一段最小的 SSE 流，包含一个 content delta 和 [DONE]。
// 期望 callback 收到 Delta("hi") 然后 Done，accumulated.content == "hi"。
TEST(OpenAiProviderAbortTest, HappyPathStreamsContentDelta) {
    LocalHttpServer server([](httplib::Server& s) {
        s.Post("/chat/completions", [](const httplib::Request&, httplib::Response& res) {
            // 标准的 OpenAI SSE 格式：data: {...}\n\n 分隔事件。
            std::string body =
                "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"},\"finish_reason\":\"stop\"}]}\n\n"
                "data: [DONE]\n\n";
            res.set_content(body, "text/event-stream");
            res.status = 200;
        });
    });

    std::atomic<bool> abort_flag{false};
    std::vector<StreamEvent> events;
    std::mutex evt_mu;

    OpenAiCompatProvider provider(
        "http://127.0.0.1:" + std::to_string(server.port),
        /*api_key=*/"",
        /*model=*/"test-model"
    );

    ChatMessage msg;
    msg.role = "user";
    msg.content = "hello";
    std::vector<ChatMessage> messages = {msg};
    std::vector<ToolDef> tools;

    std::string accumulated_content;
    auto callback = [&](const StreamEvent& evt) {
        std::lock_guard<std::mutex> lk(evt_mu);
        events.push_back(evt);
        if (evt.type == StreamEventType::Delta) accumulated_content += evt.content;
    };

    provider.chat_stream(messages, tools, callback, &abort_flag);

    // 期望事件序列：至少一个 Delta("hi")、一个 Done，且没有 Error。
    int delta_count = 0;
    int done_count = 0;
    int error_count = 0;
    std::string first_delta_text;
    {
        std::lock_guard<std::mutex> lk(evt_mu);
        for (const auto& evt : events) {
            if (evt.type == StreamEventType::Delta) {
                if (delta_count == 0) first_delta_text = evt.content;
                ++delta_count;
            } else if (evt.type == StreamEventType::Done) {
                ++done_count;
            } else if (evt.type == StreamEventType::Error) {
                ++error_count;
            }
        }
    }
    EXPECT_EQ(error_count, 0);
    EXPECT_EQ(done_count, 1);
    EXPECT_GE(delta_count, 1);
    EXPECT_EQ(first_delta_text, "hi");
    EXPECT_EQ(accumulated_content, "hi");
}

// 用例 3：静默期取消必须写下可诊断的日志行
//
// 从 acecode.log 角度验证 progress callback（而不是 write callback）赢了
// 取消竞速 —— 因为此场景下服务器根本没写过字节，write callback 从未触发。
// 该用例把 Logger 重定向到临时文件，跑一次和 Case 1 相同的取消序列，然后
// 读回日志文件，断言含 "no-data phase" 关键词。
TEST(OpenAiProviderAbortTest, SilentPhaseCancelWritesDiagnosticLog) {
    namespace fs = std::filesystem;
    auto log_path = fs::temp_directory_path() / "acecode_abort_test.log";
    // 确保起点干净。Logger 以 append 模式打开，不清掉会读到上次残留。
    std::error_code ec;
    fs::remove(log_path, ec);
    acecode::Logger::instance().init(log_path.string());
    acecode::Logger::instance().set_level(acecode::LogLevel::Warn);

    LocalHttpServer server([](httplib::Server& s) {
        s.Post("/chat/completions", [](const httplib::Request&, httplib::Response& res) {
            std::this_thread::sleep_for(3s);
            res.status = 200;
        });
    });

    std::atomic<bool> abort_flag{false};
    OpenAiCompatProvider provider(
        "http://127.0.0.1:" + std::to_string(server.port),
        /*api_key=*/"", /*model=*/"test-model"
    );

    ChatMessage msg;
    msg.role = "user";
    msg.content = "hello";
    std::vector<ChatMessage> messages = {msg};
    std::vector<ToolDef> tools;
    auto noop_cb = [](const StreamEvent&) {};

    std::thread worker([&] {
        provider.chat_stream(messages, tools, noop_cb, &abort_flag);
    });
    std::this_thread::sleep_for(200ms);
    abort_flag.store(true);
    worker.join();

    // 把 Logger 重新指回一个临时文件（或说用尽 flush），以便安全读取。
    // 但 Logger 是 flush-on-each-write 的，我们直接读即可。
    std::ifstream ifs(log_path);
    std::stringstream ss;
    ss << ifs.rdbuf();
    std::string log_contents = ss.str();

    // 新的日志行应当至少包含这两个关键词。"no-data phase" 是本次修复引入
    // 的 discriminator；任何一个没出现都说明诊断路径没被走到。
    EXPECT_NE(log_contents.find("SSE request aborted by user"), std::string::npos)
        << "日志里应该有 abort 告警，实际内容：\n" << log_contents;
    EXPECT_NE(log_contents.find("no-data phase"), std::string::npos)
        << "静默期取消时日志必须带 'no-data phase' 区分符，实际内容：\n" << log_contents;
}

} // namespace
