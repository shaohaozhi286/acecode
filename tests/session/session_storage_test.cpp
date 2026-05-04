// 覆盖 src/session/session_storage.cpp 的磁盘 schema:
//   - SessionMeta 全字段 roundtrip(含本次新增的 title)
//   - 老版本 .meta.json(无 title 字段)向后兼容
//   - 空 title 必须被序列化路径省略,不污染 JSON
//   - compute_project_hash 稳定且对路径大小写/分隔符不敏感
// 这是 /resume、窗口标题还原、per-project 会话隔离的基础,一旦破坏就会
// 把历史会话错认到不同项目目录,甚至让 --resume 读不到旧标题。

#include <gtest/gtest.h>

#include "session/session_storage.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;
using acecode::SessionMeta;
using acecode::SessionStorage;

namespace {

// 创建一个每个 TEST 独立、先清空再新建的临时目录,避免跨用例文件污染。
fs::path make_unique_tmp_dir(const std::string& hint) {
    auto base = fs::temp_directory_path() /
                ("acecode_test_" + hint + "_" + std::to_string(::testing::UnitTest::GetInstance()
                    ->current_test_info()->line()));
    fs::remove_all(base);
    fs::create_directories(base);
    return base;
}

}

// 场景:写一个字段齐全(含 title)的 SessionMeta 到磁盘,再读回,
// 所有字段必须逐一一致——这是 write_meta / read_meta 的核心契约。
TEST(SessionStorage, MetaRoundtrip) {
    auto dir = make_unique_tmp_dir("roundtrip");
    auto meta_path = (dir / "20260419-120000-abcd.meta.json").string();

    SessionMeta in;
    in.id            = "20260419-120000-abcd";
    in.cwd           = "/home/shao/acecode";
    in.created_at    = "2026-04-19T12:00:00Z";
    in.updated_at    = "2026-04-19T12:05:00Z";
    in.message_count = 7;
    in.summary       = "fix the resume bug";
    in.provider      = "copilot";
    in.model         = "gpt-4o";
    in.title         = "resume bug";

    SessionStorage::write_meta(meta_path, in);
    SessionMeta out = SessionStorage::read_meta(meta_path);

    EXPECT_EQ(out.id,            in.id);
    EXPECT_EQ(out.cwd,           in.cwd);
    EXPECT_EQ(out.created_at,    in.created_at);
    EXPECT_EQ(out.updated_at,    in.updated_at);
    EXPECT_EQ(out.message_count, in.message_count);
    EXPECT_EQ(out.summary,       in.summary);
    EXPECT_EQ(out.provider,      in.provider);
    EXPECT_EQ(out.model,         in.model);
    EXPECT_EQ(out.title,         in.title);
}

// 场景:手造一份老版本 .meta.json(完全不含 title 字段),read_meta 必须
// 不抛异常且 title 为空。这是"升级到新版后能继续 resume 老会话"的关键。
TEST(SessionStorage, LegacyMetaWithoutTitle) {
    auto dir = make_unique_tmp_dir("legacy");
    auto meta_path = (dir / "legacy.meta.json").string();

    // Hand-crafted legacy meta with no "title" field.
    {
        std::ofstream ofs(meta_path);
        ofs <<
            "{\n"
            "  \"id\": \"legacy-id\",\n"
            "  \"cwd\": \"/home/shao/old\",\n"
            "  \"created_at\": \"2025-01-01T00:00:00Z\",\n"
            "  \"updated_at\": \"2025-01-02T00:00:00Z\",\n"
            "  \"message_count\": 3,\n"
            "  \"summary\": \"old session\",\n"
            "  \"provider\": \"openai\",\n"
            "  \"model\": \"gpt-4\"\n"
            "}\n";
    }

    SessionMeta out;
    ASSERT_NO_THROW(out = SessionStorage::read_meta(meta_path));
    EXPECT_EQ(out.id, "legacy-id");
    EXPECT_EQ(out.message_count, 3);
    EXPECT_TRUE(out.title.empty())
        << "legacy meta without 'title' must deserialize to empty title";
}

// 场景:SessionMeta.title 为空时,write_meta 必须跳过该字段,而不是写成
// `"title": ""`。这保持与 ChatMessage "Empty fields MAY be omitted" 的
// 既有风格一致,也让新老版本互相读写时不会凭空新增字段。
TEST(SessionStorage, EmptyTitleIsOmittedOnWrite) {
    auto dir = make_unique_tmp_dir("omit");
    auto meta_path = (dir / "omit.meta.json").string();

    SessionMeta in;
    in.id = "empty-title";
    in.cwd = "/";
    in.title = "";  // explicitly empty

    SessionStorage::write_meta(meta_path, in);

    std::ifstream ifs(meta_path);
    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
    EXPECT_EQ(content.find("\"title\""), std::string::npos)
        << "empty title should be omitted from the serialized JSON; got: " << content;
}

// 场景:desktop visibility 是 project-level workspace.json marker,不是
// 单个 session .meta.json 字段。TUI/SessionStorage 写 meta 时不能把项目暴露给 Desktop。
TEST(SessionStorage, WriteMetaDoesNotSerializeDesktopVisible) {
    auto dir = make_unique_tmp_dir("desktop-visible");
    auto meta_path = (dir / "desktop-visible.meta.json").string();

    SessionMeta in;
    in.id = "desktop-visible";
    in.cwd = "/home/shao/tui-only";
    in.summary = "tui-only session";

    SessionStorage::write_meta(meta_path, in);

    std::ifstream ifs(meta_path);
    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
    EXPECT_EQ(content.find("desktop_visible"), std::string::npos)
        << "session meta must not carry desktop visibility marker; got: " << content;

    SessionMeta out;
    ASSERT_NO_THROW(out = SessionStorage::read_meta(meta_path));
    EXPECT_EQ(out.id, in.id);
    EXPECT_EQ(out.cwd, in.cwd);
}

// 场景:早期/异常退出的 meta 可能还停在 message_count=0 且 summary 空,
// 但 jsonl 已经有用户消息。list_sessions 需要从 jsonl 补齐,否则 Web 左侧
// 会把说过话的会话显示成裸 session id。
TEST(SessionStorage, ListSessionsBackfillsSummaryAndCountFromJsonl) {
    auto dir = make_unique_tmp_dir("backfill");
    const std::string sid = "20260502-101144-61ab";

    SessionMeta meta;
    meta.id = sid;
    meta.cwd = dir.string();
    meta.created_at = "2026-05-02T10:11:44Z";
    meta.updated_at = "2026-05-02T10:11:45Z";
    meta.provider = "copilot";
    meta.model = "claude";
    meta.message_count = 0;
    SessionStorage::write_meta(SessionStorage::meta_path(dir.string(), sid, 123), meta);

    acecode::ChatMessage user;
    user.role = "user";
    user.content = "帮我看一下 websocket 错误";
    SessionStorage::append_message(SessionStorage::session_path(dir.string(), sid, 123), user);

    acecode::ChatMessage assistant;
    assistant.role = "assistant";
    SessionStorage::append_message(SessionStorage::session_path(dir.string(), sid, 123), assistant);

    auto sessions = SessionStorage::list_sessions(dir.string());
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions[0].id, sid);
    EXPECT_EQ(sessions[0].summary, user.content);
    EXPECT_EQ(sessions[0].message_count, 1);
}

// 场景:同一 cwd 多次调用必须返回完全一致的 16 字符 hex,不同 cwd 必须
// 返回不同 hash。这是 ~/.acecode/projects/<hash>/ 目录隔离的根本保证。
TEST(SessionStorage, ComputeProjectHashStable) {
    const std::string a = SessionStorage::compute_project_hash("/home/shao/acecode");
    const std::string b = SessionStorage::compute_project_hash("/home/shao/acecode");
    EXPECT_EQ(a, b);
    EXPECT_EQ(a.size(), 16u) << "hash is 16 hex chars per session_storage.hpp contract";

    const std::string c = SessionStorage::compute_project_hash("/home/shao/acecode-other");
    EXPECT_NE(a, c);
}

// 场景:Windows 文件系统路径大小写/反斜杠各种写法实际上指向同一目录,
// hash 必须折叠成同一值,否则同一个项目会在 Windows 上产生多份会话历史。
TEST(SessionStorage, ComputeProjectHashIsCaseAndSlashInsensitive) {
    // Windows filesystems are case-insensitive and use backslashes; canonicalization
    // should make these collapse to a single hash.
    const std::string a = SessionStorage::compute_project_hash("C:/Users/Shao/ace");
    const std::string b = SessionStorage::compute_project_hash("c:\\users\\shao\\ace");
    EXPECT_EQ(a, b);
}
