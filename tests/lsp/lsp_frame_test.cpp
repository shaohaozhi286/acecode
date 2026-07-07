// 覆盖 src/lsp/lsp_frame.{hpp,cpp}:LSP Content-Length 分帧解析器。
//
// 分帧器是 reader 线程与 server 之间唯一的协议边界,粘包/半包处理错一个
// 字节就会导致后续所有消息错位 —— 所以这里逐字节喂入验证状态机。
//
// 覆盖项:
//   - 单条完整消息
//   - 粘包:一次 feed 到达多条消息
//   - 半包:头部/正文分多次到达(含逐字节)
//   - 大小写不敏感的 Content-Length 头 + 额外 Content-Type 头
//   - 宽松 \n\n 头部结束符(个别不规范 server)
//   - 非法 JSON body → 丢弃该帧但流继续
//   - 非法头(无 Content-Length / 非数字 / 超限)→ broken,此后 feed 恒 false
//   - encode_frame 产出的帧可被解析器原样还原(round-trip)

#include <gtest/gtest.h>

#include "lsp/lsp_frame.hpp"

#include <string>
#include <vector>

using acecode::lsp::LspFrameParser;
using acecode::lsp::encode_frame;

namespace {

std::string frame_of(const std::string& body) {
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

std::vector<nlohmann::json> feed_all(LspFrameParser& parser, const std::string& data,
                                     bool* ok_out = nullptr) {
    std::vector<nlohmann::json> messages;
    const bool ok = parser.feed(data.data(), data.size(), [&](nlohmann::json&& m) {
        messages.push_back(std::move(m));
    });
    if (ok_out) *ok_out = ok;
    return messages;
}

} // namespace

// 场景:一条完整消息一次到达 → 恰好回调一次,内容原样。
TEST(LspFrame, SingleMessage) {
    LspFrameParser parser;
    auto messages = feed_all(parser, frame_of(R"({"id":1,"result":"ok"})"));
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0]["id"], 1);
    EXPECT_EQ(messages[0]["result"], "ok");
}

// 场景:粘包 —— 两条完整消息 + 第三条的前半段一次到达。
// 期望:先产出前两条;补齐后产出第三条。
TEST(LspFrame, CoalescedMessagesThenPartial) {
    LspFrameParser parser;
    const std::string third = frame_of(R"({"id":3})");
    std::string data = frame_of(R"({"id":1})") + frame_of(R"({"id":2})") +
                       third.substr(0, third.size() / 2);
    auto messages = feed_all(parser, data);
    ASSERT_EQ(messages.size(), 2u);
    EXPECT_EQ(messages[0]["id"], 1);
    EXPECT_EQ(messages[1]["id"], 2);

    auto more = feed_all(parser, third.substr(third.size() / 2));
    ASSERT_EQ(more.size(), 1u);
    EXPECT_EQ(more[0]["id"], 3);
}

// 场景:极端半包 —— 整帧逐字节喂入。任何一步都不得提前/漏回调。
TEST(LspFrame, ByteByByteFeed) {
    LspFrameParser parser;
    const std::string data = frame_of(R"({"method":"x","params":{}})");
    std::vector<nlohmann::json> messages;
    for (char c : data) {
        ASSERT_TRUE(parser.feed(&c, 1, [&](nlohmann::json&& m) {
            messages.push_back(std::move(m));
        }));
    }
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0]["method"], "x");
}

// 场景:头部字段大小写变体 + 附带 Content-Type 头(LSP 允许)。
TEST(LspFrame, CaseInsensitiveHeaderWithContentType) {
    LspFrameParser parser;
    const std::string body = R"({"id":9})";
    const std::string data = "content-length: " + std::to_string(body.size()) +
                             "\r\nContent-Type: application/vscode-jsonrpc; charset=utf-8\r\n\r\n" +
                             body;
    auto messages = feed_all(parser, data);
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0]["id"], 9);
}

// 场景:不规范 server 用 \n\n 结束头部。解析器宽松接受。
TEST(LspFrame, LenientLfLfHeaderTerminator) {
    LspFrameParser parser;
    const std::string body = R"({"id":7})";
    const std::string data = "Content-Length: " + std::to_string(body.size()) + "\n\n" + body;
    auto messages = feed_all(parser, data);
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0]["id"], 7);
}

// 场景:body 不是合法 JSON。该帧被丢弃(不回调),但后续消息不受影响。
TEST(LspFrame, MalformedJsonBodyDropsFrameButStreamContinues) {
    LspFrameParser parser;
    bool ok = false;
    auto messages = feed_all(parser, frame_of("{not json!") + frame_of(R"({"id":2})"), &ok);
    EXPECT_TRUE(ok);
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0]["id"], 2);
}

// 场景:Content-Length 非数字 → 流损坏;之后任何 feed 都返回 false。
TEST(LspFrame, InvalidContentLengthBreaksStream) {
    LspFrameParser parser;
    bool ok = true;
    feed_all(parser, "Content-Length: abc\r\n\r\n{}", &ok);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(parser.broken());
    // 已损坏的流不再接受任何数据。
    feed_all(parser, frame_of(R"({"id":1})"), &ok);
    EXPECT_FALSE(ok);
}

// 场景:头部块缺 Content-Length 字段 → 流损坏(没有长度无法定界)。
TEST(LspFrame, MissingContentLengthBreaksStream) {
    LspFrameParser parser;
    bool ok = true;
    feed_all(parser, "Content-Type: application/json\r\n\r\n{}", &ok);
    EXPECT_FALSE(ok);
}

// 场景:Content-Length 超过 128MB 上限(kMaxFrameBodyBytes,防失控 server
// 造成无界内存分配)→ 流损坏。
TEST(LspFrame, OversizedContentLengthBreaksStream) {
    LspFrameParser parser;
    bool ok = true;
    feed_all(parser, "Content-Length: 999999999999\r\n\r\n", &ok);
    EXPECT_FALSE(ok);
}

// 场景:encode_frame 产出的帧喂回解析器,消息 round-trip 一致。
// 覆盖非 ASCII(UTF-8 中文)的字节长度计算 —— Content-Length 按字节数
// 而非字符数,算错会导致下一帧错位。
TEST(LspFrame, EncodeRoundTripWithUtf8) {
    LspFrameParser parser;
    nlohmann::json original = {{"method", "textDocument/hover"},
                               {"params", {{"text", "中文诊断消息"}}}};
    auto messages = feed_all(parser, encode_frame(original) + encode_frame(original));
    ASSERT_EQ(messages.size(), 2u);
    EXPECT_EQ(messages[0], original);
    EXPECT_EQ(messages[1], original);
}
