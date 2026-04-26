// 验证 ChatMessage.metadata 在 serialize_message / deserialize_message 之间的
// round-trip 在新增 tool_summary / tool_hunks 子键的前提下仍然无损。
//
// session_serializer.cpp 把 metadata 整体作为 nlohmann::json 落盘 —— 形态上
// 这一支测试是冗余守护,但因为 restore-tool-calls-on-resume 把 metadata 升级
// 为"两端共享的视觉字段载体",这里显式列出几个真实形态的 round-trip,可在
// 未来 serializer 改动时迅速捕获形态漂移。

#include <gtest/gtest.h>

#include "session/session_serializer.hpp"
#include "session/tool_metadata_codec.hpp"
#include "tool/tool_executor.hpp"
#include "tool/diff_utils.hpp"
#include "provider/llm_provider.hpp"

#include <nlohmann/json.hpp>

using acecode::ChatMessage;
using acecode::DiffHunk;
using acecode::DiffLine;
using acecode::DiffLineKind;
using acecode::ToolSummary;
using acecode::deserialize_message;
using acecode::encode_tool_hunks;
using acecode::encode_tool_summary;
using acecode::serialize_message;

namespace {

ToolSummary make_summary() {
    ToolSummary s;
    s.verb = "Edited";
    s.object = "src/foo.cpp";
    s.metrics = {{"+", "12"}, {"-", "3"}};
    s.icon = "✎";
    return s;
}

std::vector<DiffHunk> make_hunks() {
    DiffHunk h;
    h.old_start = 1; h.old_count = 2;
    h.new_start = 1; h.new_count = 3;
    DiffLine ctx;
    ctx.kind = DiffLineKind::Context;
    ctx.text = "ctx";
    ctx.old_line_no = 1;
    ctx.new_line_no = 1;
    h.lines.push_back(ctx);
    DiffLine add;
    add.kind = DiffLineKind::Added;
    add.text = "added";
    add.new_line_no = 2;
    h.lines.push_back(add);
    return {h};
}

} // namespace

// 构造 ChatMessage{role:"tool", metadata: {"tool_summary": {...}}},
// serialize → deserialize,metadata.tool_summary round-trip 无损。
TEST(SessionSerializerMetadata, PreservesToolSummary) {
    ChatMessage in;
    in.role = "tool";
    in.content = "Edited foo";
    in.tool_call_id = "c1";
    in.metadata = nlohmann::json::object();
    in.metadata["tool_summary"] = encode_tool_summary(make_summary());

    auto line = serialize_message(in);
    ChatMessage out = deserialize_message(line);

    EXPECT_EQ(out.role, "tool");
    EXPECT_EQ(out.content, "Edited foo");
    EXPECT_EQ(out.tool_call_id, "c1");
    ASSERT_TRUE(out.metadata.is_object());
    ASSERT_TRUE(out.metadata.contains("tool_summary"));

    auto decoded = acecode::decode_tool_summary(out.metadata["tool_summary"]);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->verb, "Edited");
    EXPECT_EQ(decoded->object, "src/foo.cpp");
    EXPECT_EQ(decoded->icon, "✎");
}

// 完整 hunks 数组 round-trip 不丢字段。
TEST(SessionSerializerMetadata, PreservesToolHunks) {
    ChatMessage in;
    in.role = "tool";
    in.content = "Edited";
    in.metadata = nlohmann::json::object();
    in.metadata["tool_hunks"] = encode_tool_hunks(make_hunks());

    auto line = serialize_message(in);
    ChatMessage out = deserialize_message(line);

    ASSERT_TRUE(out.metadata.is_object());
    ASSERT_TRUE(out.metadata.contains("tool_hunks"));

    auto decoded = acecode::decode_tool_hunks(out.metadata["tool_hunks"]);
    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->size(), 1u);
    EXPECT_EQ((*decoded)[0].old_start, 1);
    EXPECT_EQ((*decoded)[0].lines.size(), 2u);
}

// 老 metadata 没 tool_summary/tool_hunks(可能含别的 compact_stats 之类)
// → serialize 路径不影响,deserialize 仍然正确。
TEST(SessionSerializerMetadata, HandlesLegacyMetadataWithoutToolKeys) {
    ChatMessage in;
    in.role = "user";
    in.content = "hi";
    in.metadata = nlohmann::json::object();
    in.metadata["compact_stats"] = {{"tokens_saved", 1234}};
    in.metadata["custom_field"]  = "preserved";

    auto line = serialize_message(in);
    ChatMessage out = deserialize_message(line);

    ASSERT_TRUE(out.metadata.is_object());
    EXPECT_FALSE(out.metadata.contains("tool_summary"));
    EXPECT_FALSE(out.metadata.contains("tool_hunks"));
    EXPECT_TRUE(out.metadata.contains("compact_stats"));
    EXPECT_TRUE(out.metadata.contains("custom_field"));
    EXPECT_EQ(out.metadata["custom_field"].get<std::string>(), "preserved");
}

// summary 与 hunks 两个键并存的 metadata round-trip。
TEST(SessionSerializerMetadata, PreservesBothSummaryAndHunks) {
    ChatMessage in;
    in.role = "tool";
    in.content = "ok";
    in.metadata = nlohmann::json::object();
    in.metadata["tool_summary"] = encode_tool_summary(make_summary());
    in.metadata["tool_hunks"]   = encode_tool_hunks(make_hunks());

    auto line = serialize_message(in);
    ChatMessage out = deserialize_message(line);

    ASSERT_TRUE(out.metadata.is_object());
    EXPECT_TRUE(out.metadata.contains("tool_summary"));
    EXPECT_TRUE(out.metadata.contains("tool_hunks"));
}
