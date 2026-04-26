// 验证 AgentLoop 在 Phase 3 record results 阶段把 ToolResult 的 summary / hunks
// 编码进 ChatMessage.metadata 的 "tool_summary" / "tool_hunks" 子键。
// 这是 restore-tool-calls-on-resume 的写盘端核心 —— 一旦不写,resume 后 tool_result
// 就只能 fold 渲染,失去彩色 diff 与摘要。
//
// 验证手段:用 stub provider 脚本化 [tool_call → task_complete] 两轮,运行结束后
// 直接读 AgentLoop::messages() 的末尾,找 role="tool" 那条,断言它的 metadata。
// 不需要落到磁盘,因为 metadata 注入路径就在 push_back 之前 —— 内存即真相。

#include <gtest/gtest.h>

#include "agent_loop.hpp"
#include "stub_provider.hpp"
#include "tool/task_complete_tool.hpp"
#include "tool/tool_executor.hpp"
#include "tool/diff_utils.hpp"
#include "permissions.hpp"
#include "provider/llm_provider.hpp"
#include "session/tool_metadata_codec.hpp"

#include <nlohmann/json.hpp>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using acecode::AgentLoop;
using acecode::AgentCallbacks;
using acecode::ChatMessage;
using acecode::DiffHunk;
using acecode::DiffLine;
using acecode::DiffLineKind;
using acecode::PermissionManager;
using acecode::PermissionResult;
using acecode::ToolDef;
using acecode::ToolExecutor;
using acecode::ToolImpl;
using acecode::ToolResult;
using acecode::ToolSource;
using acecode::ToolSummary;
using acecode_test::StubLlmProvider;

namespace {

// 构造一个返回固定 summary 的 mock tool(本测试用)。
// summary_provider / hunks_provider 由 lambda 决定每次执行返回什么 —— 让单测能控制。
ToolImpl make_mock_summary_tool(
    const std::string& name,
    std::function<ToolResult()> result_fn) {
    ToolDef def;
    def.name = name;
    def.description = "Mock tool returning a scripted ToolResult for metadata injection tests.";
    def.parameters = {
        {"type", "object"},
        {"properties", nlohmann::json::object()}
    };
    ToolImpl impl;
    impl.definition = def;
    impl.execute = [result_fn](const std::string&, const acecode::ToolContext&) {
        return result_fn();
    };
    impl.is_read_only = true; // 走 read-only 路径,免 permission 弹窗
    impl.source = ToolSource::Builtin;
    return impl;
}

// 与 termination_test 同模式的 fixture —— 用 stub provider + 完成同步。
class MetadataHarness {
public:
    MetadataHarness() {
        AgentCallbacks cb;
        cb.on_busy_changed = [this](bool busy) {
            std::lock_guard<std::mutex> lk(busy_mu_);
            is_busy_ = busy;
            if (!busy) busy_cv_.notify_all();
        };
        cb.on_tool_confirm = [](const std::string&, const std::string&) {
            return PermissionResult::Allow;
        };

        auto provider_accessor =
            [this]() -> std::shared_ptr<acecode::LlmProvider> { return provider_; };

        loop_ = std::make_unique<AgentLoop>(
            provider_accessor, tools_, cb, /*cwd=*/".", perms_);

        tools_.register_tool(acecode::create_task_complete_tool());
    }

    void register_mock(const std::string& name, std::function<ToolResult()> fn) {
        tools_.register_tool(make_mock_summary_tool(name, std::move(fn)));
    }

    void script_call_then_done(const std::string& tool_name) {
        provider_->push_tool_call(tool_name, "{}", "c-1");
        nlohmann::json done_args = {{"summary", "done"}};
        provider_->push_tool_call("task_complete", done_args.dump(), "c-done");
    }

    bool submit_and_wait(const std::string& msg,
                         std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        {
            std::lock_guard<std::mutex> lk(busy_mu_);
            is_busy_ = true;
        }
        loop_->submit(msg);
        std::unique_lock<std::mutex> lk(busy_mu_);
        return busy_cv_.wait_for(lk, timeout, [this] { return !is_busy_; });
    }

    // 找 messages() 中 role=="tool" 的第一条(本测试每次只触发一个工具调用)。
    const ChatMessage* find_tool_msg() const {
        for (const auto& m : loop_->messages()) {
            if (m.role == "tool") return &m;
        }
        return nullptr;
    }

private:
    std::shared_ptr<StubLlmProvider> provider_ = std::make_shared<StubLlmProvider>();
    ToolExecutor tools_;
    PermissionManager perms_;
    std::unique_ptr<AgentLoop> loop_;

    std::mutex busy_mu_;
    std::condition_variable busy_cv_;
    bool is_busy_ = false;
};

ToolSummary make_typical_summary() {
    ToolSummary s;
    s.verb   = "Edited";
    s.object = "src/foo.cpp";
    s.metrics = {{"+", "12"}, {"-", "3"}};
    s.icon   = "✎";
    return s;
}

std::vector<DiffHunk> make_typical_hunks() {
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

// agent_loop 写 tool_summary 到 metadata,且 round-trip 等于原 ToolSummary。
TEST(AgentLoopMetadataInjection, WritesToolSummaryToMetadata) {
    MetadataHarness h;
    auto orig_summary = make_typical_summary();
    h.register_mock("file_edit_mock", [orig_summary]() {
        ToolResult r;
        r.success = true;
        r.output  = "Edited src/foo.cpp";
        r.summary = orig_summary;
        return r;
    });
    h.script_call_then_done("file_edit_mock");
    ASSERT_TRUE(h.submit_and_wait("edit it"));

    const auto* tool_msg = h.find_tool_msg();
    ASSERT_NE(tool_msg, nullptr);
    ASSERT_TRUE(tool_msg->metadata.is_object());
    ASSERT_TRUE(tool_msg->metadata.contains("tool_summary"));

    auto out = acecode::decode_tool_summary(tool_msg->metadata["tool_summary"]);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->verb, orig_summary.verb);
    EXPECT_EQ(out->object, orig_summary.object);
    EXPECT_EQ(out->icon, orig_summary.icon);
    ASSERT_EQ(out->metrics.size(), orig_summary.metrics.size());
    for (size_t i = 0; i < out->metrics.size(); ++i) {
        EXPECT_EQ(out->metrics[i].first,  orig_summary.metrics[i].first);
        EXPECT_EQ(out->metrics[i].second, orig_summary.metrics[i].second);
    }
}

// agent_loop 写 tool_hunks 到 metadata,且 round-trip 等于原 hunks。
TEST(AgentLoopMetadataInjection, WritesToolHunksToMetadata) {
    MetadataHarness h;
    auto orig_hunks = make_typical_hunks();
    h.register_mock("file_edit_mock", [orig_hunks]() {
        ToolResult r;
        r.success = true;
        r.output  = "Edited src/foo.cpp";
        r.hunks   = orig_hunks;
        return r;
    });
    h.script_call_then_done("file_edit_mock");
    ASSERT_TRUE(h.submit_and_wait("edit it"));

    const auto* tool_msg = h.find_tool_msg();
    ASSERT_NE(tool_msg, nullptr);
    ASSERT_TRUE(tool_msg->metadata.is_object());
    ASSERT_TRUE(tool_msg->metadata.contains("tool_hunks"));

    auto out = acecode::decode_tool_hunks(tool_msg->metadata["tool_hunks"]);
    ASSERT_TRUE(out.has_value());
    ASSERT_EQ(out->size(), orig_hunks.size());
    EXPECT_EQ((*out)[0].old_start, orig_hunks[0].old_start);
    EXPECT_EQ((*out)[0].lines.size(), orig_hunks[0].lines.size());
}

// ToolResult 不带 summary/hunks → tool_msg.metadata 不应该出现这两个 key
// (避免空 object 污染)。
TEST(AgentLoopMetadataInjection, SkipsMetadataWhenSummaryAbsent) {
    MetadataHarness h;
    h.register_mock("plain_mock", []() {
        ToolResult r;
        r.success = true;
        r.output  = "ok";
        // 故意不设 summary 与 hunks
        return r;
    });
    h.script_call_then_done("plain_mock");
    ASSERT_TRUE(h.submit_and_wait("run it"));

    const auto* tool_msg = h.find_tool_msg();
    ASSERT_NE(tool_msg, nullptr);
    // metadata 可能是 null(未触碰)或一个不含这两个 key 的对象;两种都接受。
    if (tool_msg->metadata.is_object()) {
        EXPECT_FALSE(tool_msg->metadata.contains("tool_summary"));
        EXPECT_FALSE(tool_msg->metadata.contains("tool_hunks"));
    }
}

// 写 metadata 不能破坏 tool_msg 的其他字段(role/content/tool_call_id)。
// 这是防回归:future-version 如果改动 inject 顺序或写到其他位置,这个测试
// 会兜底捕获 schema 漂移。
TEST(AgentLoopMetadataInjection, MetadataInjectionPreservesOtherFields) {
    MetadataHarness h;
    h.register_mock("file_edit_mock", []() {
        ToolResult r;
        r.success = true;
        r.output  = "Edited foo";
        r.summary = make_typical_summary();
        return r;
    });
    h.script_call_then_done("file_edit_mock");
    ASSERT_TRUE(h.submit_and_wait("edit"));

    const auto* tool_msg = h.find_tool_msg();
    ASSERT_NE(tool_msg, nullptr);
    EXPECT_EQ(tool_msg->role, "tool");
    EXPECT_EQ(tool_msg->content, "Edited foo");
    EXPECT_EQ(tool_msg->tool_call_id, "c-1");
}

// 同时带 summary 与 hunks → 两个键都存在,互不干扰。
TEST(AgentLoopMetadataInjection, WritesBothSummaryAndHunks) {
    MetadataHarness h;
    auto orig_summary = make_typical_summary();
    auto orig_hunks = make_typical_hunks();
    h.register_mock("file_edit_mock", [orig_summary, orig_hunks]() {
        ToolResult r;
        r.success = true;
        r.output  = "Edited";
        r.summary = orig_summary;
        r.hunks   = orig_hunks;
        return r;
    });
    h.script_call_then_done("file_edit_mock");
    ASSERT_TRUE(h.submit_and_wait("edit it"));

    const auto* tool_msg = h.find_tool_msg();
    ASSERT_NE(tool_msg, nullptr);
    ASSERT_TRUE(tool_msg->metadata.contains("tool_summary"));
    ASSERT_TRUE(tool_msg->metadata.contains("tool_hunks"));
}
