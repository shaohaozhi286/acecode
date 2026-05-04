// 覆盖 SessionManager::fork_session_to_new_id 的语义:
//   - retained_prefix 复制行数 == prefix 大小
//   - meta 字段:forked_from / fork_message_id / title / cwd / provider / model
//   - 源 session JSONL 文件 byte-for-byte 不变(spec 强约束)
//   - tool_hunks metadata 透传到新 session JSONL
//   - file_checkpoint 元消息自动过滤(新 session 不继承 checkpoints)
//
// 这些场景对应 openspec session-fork capability "Forked session inherits ..."
// 与 "Fork operations reuse rewind prefix-selection logic" 两条 Requirement。

#include <gtest/gtest.h>

#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session/session_serializer.hpp"
#include "session/file_checkpoint_store.hpp"
#include "session/tool_metadata_codec.hpp"
#include "tool/diff_utils.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using acecode::ChatMessage;
using acecode::SessionManager;
using acecode::SessionStorage;
using acecode::SessionMeta;
using acecode::FileCheckpointStore;
using acecode::FileCheckpointSnapshot;
using acecode::DiffHunk;
using acecode::DiffLine;
using acecode::DiffLineKind;
using acecode::encode_tool_hunks;

namespace {

// 临时 cwd 目录:每个 TEST 独立一个,避免相互污染。
class TempCwd {
public:
    TempCwd() {
        auto base = fs::temp_directory_path() / "acecode_fork_test";
        fs::create_directories(base);
        std::ostringstream oss;
        oss << "session_fork_" << ::testing::UnitTest::GetInstance()->current_test_info()->name();
        path_ = (base / oss.str()).string();
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    ~TempCwd() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const std::string& path() const { return path_; }
private:
    std::string path_;
};

ChatMessage make_user(const std::string& content, const std::string& uuid = "") {
    ChatMessage m;
    m.role = "user";
    m.content = content;
    m.uuid = uuid;
    return m;
}

ChatMessage make_assistant(const std::string& content) {
    ChatMessage m;
    m.role = "assistant";
    m.content = content;
    return m;
}

ChatMessage make_tool(const std::string& tool_call_id, const std::string& output,
                       const std::vector<DiffHunk>* hunks = nullptr) {
    ChatMessage m;
    m.role = "tool";
    m.tool_call_id = tool_call_id;
    m.content = output;
    if (hunks) {
        m.metadata["tool_hunks"] = encode_tool_hunks(*hunks);
    }
    return m;
}

std::string read_file(const std::string& path) {
    std::ifstream ifs(path);
    std::stringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

} // namespace

// 场景:fork_session_to_new_id 写出的 JSONL 行数 == prefix.size()(没漏没多)。
TEST(SessionFork, JsonlLineCountEqualsPrefixSize) {
    TempCwd cwd;
    SessionManager sm;
    sm.start_session(cwd.path(), "openai", "gpt-4");

    std::vector<ChatMessage> prefix = {
        make_user("U1", "u-1"),
        make_assistant("A1"),
        make_user("U2", "u-2"),
    };

    auto new_id = sm.fork_session_to_new_id(prefix, "T", "src-id", "u-2");
    ASSERT_FALSE(new_id.empty());

    auto pdir = SessionStorage::get_project_dir(cwd.path());
    auto candidates = SessionStorage::find_session_files(pdir, new_id);
    ASSERT_FALSE(candidates.empty());
    auto loaded = SessionStorage::load_messages(candidates.front().jsonl_path);
    EXPECT_EQ(loaded.size(), 3u);
    EXPECT_EQ(loaded[0].role, "user");
    EXPECT_EQ(loaded[0].content, "U1");
    EXPECT_EQ(loaded[2].uuid, "u-2");
}

// 场景:meta 字段全填对(forked_from / fork_message_id / title / cwd / provider / model)。
TEST(SessionFork, MetaFieldsCopied) {
    TempCwd cwd;
    SessionManager sm;
    sm.start_session(cwd.path(), "copilot", "gpt-4o");

    auto new_id = sm.fork_session_to_new_id(
        {make_user("hi", "u-1")}, "分叉1:重构 auth", "src-S", "u-1");
    ASSERT_FALSE(new_id.empty());

    auto pdir = SessionStorage::get_project_dir(cwd.path());
    auto candidates = SessionStorage::find_session_files(pdir, new_id);
    ASSERT_FALSE(candidates.empty());
    auto meta = SessionStorage::read_meta(candidates.front().meta_path);

    EXPECT_EQ(meta.id, new_id);
    EXPECT_EQ(meta.cwd, cwd.path());
    EXPECT_EQ(meta.provider, "copilot");
    EXPECT_EQ(meta.model, "gpt-4o");
    EXPECT_EQ(meta.title, "分叉1:重构 auth");
    EXPECT_EQ(meta.forked_from, "src-S");
    EXPECT_EQ(meta.fork_message_id, "u-1");
    EXPECT_EQ(meta.message_count, 1);
}

// 场景:fork 操作不动当前 active session 的 JSONL — 文件 byte-for-byte 不变。
TEST(SessionFork, SourceJsonlUnchanged) {
    TempCwd cwd;
    SessionManager sm;
    sm.start_session(cwd.path(), "openai", "gpt-4");
    sm.on_message(make_user("U1", "u-1"));
    sm.on_message(make_assistant("A1"));

    auto src_id = sm.current_session_id();
    auto pdir = SessionStorage::get_project_dir(cwd.path());
    auto src_candidates = SessionStorage::find_session_files(pdir, src_id);
    ASSERT_FALSE(src_candidates.empty());
    std::string src_jsonl_path = src_candidates.front().jsonl_path;
    std::string before = read_file(src_jsonl_path);
    auto before_size = fs::file_size(src_jsonl_path);

    auto new_id = sm.fork_session_to_new_id(
        {make_user("U1", "u-1")}, "T", src_id, "u-1");
    ASSERT_FALSE(new_id.empty());

    std::string after = read_file(src_jsonl_path);
    auto after_size = fs::file_size(src_jsonl_path);
    EXPECT_EQ(before_size, after_size);
    EXPECT_EQ(before, after);
}

// 场景:retained_prefix 含一条 tool message(带 tool_hunks metadata)→ 新 session
// 的 JSONL 该行也保留 tool_hunks(透传,不丢字段)。
TEST(SessionFork, ToolHunksMetadataPreserved) {
    TempCwd cwd;
    SessionManager sm;
    sm.start_session(cwd.path(), "openai", "gpt-4");

    DiffHunk h;
    h.old_start = 1; h.old_count = 1; h.new_start = 1; h.new_count = 1;
    DiffLine rm; rm.kind = DiffLineKind::Removed; rm.text = "old";
    DiffLine ad; ad.kind = DiffLineKind::Added;   ad.text = "new";
    h.lines = {rm, ad};
    std::vector<DiffHunk> hunks{h};

    auto tool_msg = make_tool("call-1", "ok", &hunks);

    auto new_id = sm.fork_session_to_new_id(
        {make_user("u", "u-1"), tool_msg}, "T", "src", "u-1");
    ASSERT_FALSE(new_id.empty());

    auto pdir = SessionStorage::get_project_dir(cwd.path());
    auto cands = SessionStorage::find_session_files(pdir, new_id);
    ASSERT_FALSE(cands.empty());
    auto loaded = SessionStorage::load_messages(cands.front().jsonl_path);
    ASSERT_EQ(loaded.size(), 2u);
    EXPECT_EQ(loaded[1].role, "tool");
    ASSERT_TRUE(loaded[1].metadata.contains("tool_hunks"));
    auto round = loaded[1].metadata["tool_hunks"];
    EXPECT_TRUE(round.is_array());
    EXPECT_EQ(round.size(), 1u);
    EXPECT_EQ(round[0]["old_start"], 1);
}

// 场景:retained_prefix 含 file_checkpoint 元消息 → 新 session 自动过滤(spec
// 决定:不继承 checkpoints,新 session 从 fork 点开始独立追踪)。
TEST(SessionFork, FileCheckpointMessagesFiltered) {
    TempCwd cwd;
    SessionManager sm;
    sm.start_session(cwd.path(), "openai", "gpt-4");

    // 构造一条 file_checkpoint 元消息(直接造,而不是走 begin_user_turn_checkpoint
    // 来避免 file IO 副作用)
    FileCheckpointSnapshot snap;
    snap.message_uuid = "u-1";
    snap.uuid = "snap-1";
    snap.timestamp = "2026-05-03T10:00:00Z";
    auto checkpoint_msg = FileCheckpointStore::encode_snapshot_message(snap);

    std::vector<ChatMessage> prefix = {
        make_user("u", "u-1"),
        checkpoint_msg,                  // ← 应被过滤
        make_assistant("a"),
    };
    auto new_id = sm.fork_session_to_new_id(prefix, "T", "src", "u-1");
    ASSERT_FALSE(new_id.empty());

    auto pdir = SessionStorage::get_project_dir(cwd.path());
    auto cands = SessionStorage::find_session_files(pdir, new_id);
    ASSERT_FALSE(cands.empty());
    auto loaded = SessionStorage::load_messages(cands.front().jsonl_path);
    EXPECT_EQ(loaded.size(), 2u);  // 只剩 user + assistant
    EXPECT_EQ(loaded[0].role, "user");
    EXPECT_EQ(loaded[1].role, "assistant");
}

// 场景:start_session 没调过(manager 未 started)→ fork 返回空字符串,不写文件。
TEST(SessionFork, NotStartedReturnsEmpty) {
    SessionManager sm;
    auto id = sm.fork_session_to_new_id({make_user("u")}, "T", "src", "x");
    EXPECT_EQ(id, "");
}
