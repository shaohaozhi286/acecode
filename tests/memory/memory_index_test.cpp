// 覆盖 src/memory/memory_index.{hpp,cpp}:
// - 从空 MEMORY.md 起把 entries 序列化成索引
// - 既有 MEMORY.md 里的"自定义描述 / 手写注释"保留不动
// - 已不存在的 entry 对应行被移除(避免 dangling 链接)
// - 新增 entry 追加在末尾
// - remove_memory_index_line 只删目标行,其它内容无动于衷

#include <gtest/gtest.h>

#include "memory/memory_index.hpp"
#include "memory/memory_types.hpp"

namespace {

acecode::MemoryEntry make_entry(const std::string& name,
                                const std::string& description,
                                acecode::MemoryType type) {
    acecode::MemoryEntry e;
    e.name = name;
    e.description = description;
    e.type = type;
    return e;
}

} // namespace

// 场景:空索引从零生成每个 entry 一行
TEST(MemoryIndex, RenderFromEmpty) {
    std::vector<acecode::MemoryEntry> entries = {
        make_entry("a", "first entry", acecode::MemoryType::User),
        make_entry("b", "second entry", acecode::MemoryType::Feedback),
    };
    std::string out = acecode::render_memory_index(entries, "");
    EXPECT_NE(out.find("a.md"), std::string::npos);
    EXPECT_NE(out.find("b.md"), std::string::npos);
    EXPECT_NE(out.find("first entry"), std::string::npos);
    EXPECT_NE(out.find("second entry"), std::string::npos);
}

// 场景:用户在 MEMORY.md 顶部加的导言/评论保留
TEST(MemoryIndex, PreservesUserAddedNonEntryLines) {
    std::vector<acecode::MemoryEntry> entries = {
        make_entry("go", "Go expert", acecode::MemoryType::User),
    };
    std::string existing =
        "# My Memory Notes\n"
        "\n"
        "This file is auto-maintained but I put notes here:\n"
        "\n"
        "- [Go expert](go.md) \xE2\x80\x94 out-of-date description\n"
        "\n"
        "Maintenance log: 2026-01-01\n";
    std::string out = acecode::render_memory_index(entries, existing);
    EXPECT_NE(out.find("# My Memory Notes"), std::string::npos);
    EXPECT_NE(out.find("Maintenance log:"), std::string::npos);
    // 过期描述被替换为最新 description
    EXPECT_NE(out.find("Go expert"), std::string::npos);
    // 过期描述应该不再出现(被替换)
    EXPECT_EQ(out.find("out-of-date description"), std::string::npos);
}

// 场景:entry 被删除后,MEMORY.md 里的陈旧行被清理
TEST(MemoryIndex, DropsStaleEntryLines) {
    std::vector<acecode::MemoryEntry> entries = {
        make_entry("keep", "still here", acecode::MemoryType::User),
    };
    std::string existing =
        "- [keep](keep.md) \xE2\x80\x94 still here\n"
        "- [gone](gone.md) \xE2\x80\x94 should disappear\n";
    std::string out = acecode::render_memory_index(entries, existing);
    EXPECT_NE(out.find("keep.md"), std::string::npos);
    EXPECT_EQ(out.find("gone.md"), std::string::npos);
    EXPECT_EQ(out.find("should disappear"), std::string::npos);
}

// 场景:新增 entry 在 entries 里但 MEMORY.md 没有对应行时追加到末尾
TEST(MemoryIndex, AppendsNewEntriesAtEnd) {
    std::vector<acecode::MemoryEntry> entries = {
        make_entry("a", "first", acecode::MemoryType::User),
        make_entry("b", "fresh added", acecode::MemoryType::Project),
    };
    std::string existing = "- [a](a.md) \xE2\x80\x94 first\n";
    std::string out = acecode::render_memory_index(entries, existing);
    std::size_t pos_a = out.find("a.md");
    std::size_t pos_b = out.find("b.md");
    ASSERT_NE(pos_a, std::string::npos);
    ASSERT_NE(pos_b, std::string::npos);
    EXPECT_LT(pos_a, pos_b) << "新增行应该追加在末尾";
}

// 场景:remove_memory_index_line 只移除目标行,其它 entry 保留
TEST(MemoryIndex, RemoveLineTouchesOnlyTarget) {
    std::string existing =
        "- [a](a.md) \xE2\x80\x94 keep me\n"
        "- [b](b.md) \xE2\x80\x94 drop me\n"
        "- [c](c.md) \xE2\x80\x94 keep me too\n"
        "random footer\n";
    std::string out = acecode::remove_memory_index_line(existing, "b");
    EXPECT_NE(out.find("a.md"), std::string::npos);
    EXPECT_NE(out.find("c.md"), std::string::npos);
    EXPECT_EQ(out.find("b.md"), std::string::npos);
    EXPECT_NE(out.find("random footer"), std::string::npos);
}

// 场景:remove 一个不存在的 name 时原文件无变化
TEST(MemoryIndex, RemoveMissingNoop) {
    std::string existing = "- [a](a.md) \xE2\x80\x94 keep\n";
    std::string out = acecode::remove_memory_index_line(existing, "not_there");
    EXPECT_EQ(out, existing);
}

// 场景:仅包含自由 markdown 且无 entries 时,render 不插入 entry 行
TEST(MemoryIndex, NoEntriesKeepsFreeformText) {
    std::vector<acecode::MemoryEntry> entries;
    std::string existing = "# Notes\nNothing automatic here yet.\n";
    std::string out = acecode::render_memory_index(entries, existing);
    EXPECT_NE(out.find("Notes"), std::string::npos);
    EXPECT_EQ(out.find("](.md)"), std::string::npos);
}
