// 覆盖 src/session/session_storage.cpp 的磁盘 schema:
//   - SessionMeta 全字段 roundtrip(含 title / input_draft)
//   - 老版本 .meta.json(无 title 字段)向后兼容
//   - 空 title 必须被序列化路径省略,不污染 JSON
//   - compute_project_hash 稳定且对路径大小写/分隔符不敏感
// 这是 /resume、窗口标题还原、per-project 会话隔离的基础,一旦破坏就会
// 把历史会话错认到不同项目目录,甚至让 --resume 读不到旧标题。

#include <gtest/gtest.h>

#include "session/session_storage.hpp"
#include "utils/utf8_path.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

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
    in.model_preset  = "copilot-fast";
    in.title         = "resume bug";
    in.title_source  = "user";
    in.input_draft   = "continue this refactor";
    in.permission_mode = "plan";
    in.pre_plan_permission_mode = "accept-edits";
    in.turn_count    = 3;
    in.last_token_usage.prompt_tokens = 8000;
    in.last_token_usage.completion_tokens = 1200;
    in.last_token_usage.total_tokens = 9200;
    in.last_token_usage.has_data = true;
    in.session_token_usage.prompt_tokens = 18000;
    in.session_token_usage.completion_tokens = 2200;
    in.session_token_usage.total_tokens = 20200;
    in.session_token_usage.cache_read_tokens = 4000;
    in.session_token_usage.reasoning_tokens = 300;
    in.session_token_usage.has_data = true;
    in.todos = {
        {"1", "Inspect Hermes", "completed"},
        {"2", "Wire checklist UI", "in_progress"},
    };
    in.archived      = true;
    in.no_workspace  = true;
    in.parent_session_id = "20260419-110000-ffff";
    in.loop_id = "loop-1";
    in.loop_run_id = "run-1";

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
    EXPECT_EQ(out.model_preset,  in.model_preset);
    EXPECT_EQ(out.title,         in.title);
    EXPECT_EQ(out.title_source,  in.title_source);
    EXPECT_EQ(out.input_draft,   in.input_draft);
    EXPECT_EQ(out.permission_mode, in.permission_mode);
    EXPECT_EQ(out.pre_plan_permission_mode, in.pre_plan_permission_mode);
    EXPECT_EQ(out.turn_count,    in.turn_count);
    EXPECT_EQ(out.last_token_usage.prompt_tokens, 8000);
    EXPECT_EQ(out.last_token_usage.completion_tokens, 1200);
    EXPECT_EQ(out.last_token_usage.total_tokens, 9200);
    EXPECT_TRUE(out.last_token_usage.has_data);
    EXPECT_EQ(out.session_token_usage.prompt_tokens, 18000);
    EXPECT_EQ(out.session_token_usage.completion_tokens, 2200);
    EXPECT_EQ(out.session_token_usage.total_tokens, 20200);
    EXPECT_EQ(out.session_token_usage.cache_read_tokens, 4000);
    EXPECT_EQ(out.session_token_usage.reasoning_tokens, 300);
    EXPECT_TRUE(out.session_token_usage.has_data);
    ASSERT_EQ(out.todos.size(), 2u);
    EXPECT_EQ(out.todos[0].content, "Inspect Hermes");
    EXPECT_EQ(out.todos[1].status, "in_progress");
    EXPECT_EQ(out.archived,      in.archived);
    EXPECT_EQ(out.no_workspace,  in.no_workspace);
    EXPECT_EQ(out.parent_session_id, in.parent_session_id);
    EXPECT_EQ(out.loop_id, in.loop_id);
    EXPECT_EQ(out.loop_run_id, in.loop_run_id);
}

// 场景:普通会话(非 spawn_subagent 子会话)的 parent_session_id 为空,
// write_meta 必须省略该字段而不是写 ""。一旦回归:老版本读到多余字段无碍,
// 但「常规列表排除子会话」的判断依赖"空 = 主会话",序列化空串本身没错,
// 省略是为了保持老 meta 文件 byte-byte 兼容(与 forked_from 同一约定)。
TEST(SessionStorage, EmptyParentSessionIdIsOmittedOnWrite) {
    auto dir = make_unique_tmp_dir("parent_omit");
    auto meta_path = (dir / "20260419-120000-abcd.meta.json").string();

    SessionMeta in;
    in.id = "20260419-120000-abcd";
    in.cwd = "/tmp/x";
    SessionStorage::write_meta(meta_path, in);

    std::ifstream ifs(meta_path);
    std::string raw((std::istreambuf_iterator<char>(ifs)),
                    std::istreambuf_iterator<char>());
    EXPECT_EQ(raw.find("parent_session_id"), std::string::npos)
        << "空 parent_session_id 不应出现在序列化输出中";

    SessionMeta out = SessionStorage::read_meta(meta_path);
    EXPECT_TRUE(out.parent_session_id.empty());
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
    EXPECT_TRUE(out.model_preset.empty())
        << "legacy meta without 'model_preset' must deserialize to empty preset";
    EXPECT_TRUE(out.title.empty())
        << "legacy meta without 'title' must deserialize to empty title";
    EXPECT_TRUE(out.input_draft.empty())
        << "legacy meta without 'input_draft' must deserialize to empty draft";
    EXPECT_EQ(out.permission_mode, "default")
        << "legacy meta without 'permission_mode' must default to default";
    EXPECT_TRUE(out.pre_plan_permission_mode.empty())
        << "legacy meta without 'pre_plan_permission_mode' must deserialize to empty";
    EXPECT_EQ(out.turn_count, 0)
        << "legacy meta without 'turn_count' must deserialize to zero";
    EXPECT_EQ(out.last_token_usage.total_tokens, 0);
    EXPECT_EQ(out.session_token_usage.total_tokens, 0);
    EXPECT_TRUE(out.todos.empty())
        << "legacy meta without 'todos' must deserialize to an empty checklist";
    EXPECT_TRUE(out.loop_id.empty());
    EXPECT_TRUE(out.loop_run_id.empty());
    EXPECT_FALSE(out.archived)
        << "legacy meta without 'archived' must deserialize to false";
}

TEST(SessionStorage, LegacyMetaWithTitleGetsLegacySource) {
    auto dir = make_unique_tmp_dir("legacy_title_source");
    auto meta_path = (dir / "20260419-121212-abcd.meta.json").string();
    {
        std::ofstream ofs(meta_path, std::ios::binary);
        ofs << R"({
  "id": "20260419-121212-abcd",
  "cwd": "/tmp/project",
  "created_at": "2026-04-19T12:12:12Z",
  "updated_at": "2026-04-19T12:13:12Z",
  "message_count": 1,
  "summary": "",
  "provider": "copilot",
  "model": "gpt-4o",
  "title": "old title"
})";
    }

    SessionMeta out = SessionStorage::read_meta(meta_path);
    EXPECT_EQ(out.title, "old title");
    EXPECT_EQ(out.title_source, "legacy");
}

TEST(SessionStorage, GeneratedProviderErrorTitleReadsAsUntitled) {
    auto dir = make_unique_tmp_dir("generated_error_title");
    auto meta_path = (dir / "generated-error.meta.json").string();

    SessionMeta in;
    in.id = "generated-error";
    in.cwd = "/tmp/project";
    in.title = "  [Error] Connection failed";
    in.title_source = "generated";
    SessionStorage::write_meta(meta_path, in);

    SessionMeta out = SessionStorage::read_meta(meta_path);
    EXPECT_TRUE(out.title.empty());
    EXPECT_TRUE(out.title_source.empty());
}

TEST(SessionStorage, UserProviderErrorShapedTitleIsPreserved) {
    auto dir = make_unique_tmp_dir("user_error_title");
    auto meta_path = (dir / "user-error.meta.json").string();

    SessionMeta in;
    in.id = "user-error";
    in.cwd = "/tmp/project";
    in.title = "[Error] Custom incident label";
    in.title_source = "user";
    SessionStorage::write_meta(meta_path, in);

    SessionMeta out = SessionStorage::read_meta(meta_path);
    EXPECT_EQ(out.title, in.title);
    EXPECT_EQ(out.title_source, "user");
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
    EXPECT_EQ(content.find("\"model_preset\""), std::string::npos)
        << "empty model_preset should be omitted from the serialized JSON; got: " << content;
    EXPECT_EQ(content.find("\"archived\""), std::string::npos)
        << "false archived state should be omitted from the serialized JSON; got: " << content;
    EXPECT_EQ(content.find("\"input_draft\""), std::string::npos)
        << "empty input_draft should be omitted from the serialized JSON; got: " << content;
    EXPECT_EQ(content.find("\"pre_plan_permission_mode\""), std::string::npos)
        << "empty pre_plan_permission_mode should be omitted from the serialized JSON; got: " << content;
    EXPECT_EQ(content.find("\"todos\""), std::string::npos)
        << "empty todos should be omitted from the serialized JSON; got: " << content;
    EXPECT_EQ(content.find("\"loop_execution\""), std::string::npos)
        << "empty LOOP provenance should be omitted from the serialized JSON; got: " << content;
}

// 场景:pre_plan_permission_mode 只能保存非 Plan mode。坏值和 "plan"
// 都会被折回 default,避免 resume 后出现 "Plan 的前一个 mode 还是 Plan"。
TEST(SessionStorage, PrePlanPermissionModeNormalizesInvalidValues) {
    auto dir = make_unique_tmp_dir("pre-plan-normalize");
    auto meta_path = (dir / "pre-plan.meta.json").string();

    {
        std::ofstream ofs(meta_path);
        ofs <<
            "{\n"
            "  \"id\": \"pre-plan\",\n"
            "  \"cwd\": \"/home/shao/acecode\",\n"
            "  \"permission_mode\": \"plan\",\n"
            "  \"pre_plan_permission_mode\": \"plan\"\n"
            "}\n";
    }
    SessionMeta out = SessionStorage::read_meta(meta_path);
    EXPECT_EQ(out.permission_mode, "plan");
    EXPECT_EQ(out.pre_plan_permission_mode, "default");

    {
        std::ofstream ofs(meta_path);
        ofs <<
            "{\n"
            "  \"id\": \"pre-plan\",\n"
            "  \"cwd\": \"/home/shao/acecode\",\n"
            "  \"permission_mode\": \"plan\",\n"
            "  \"pre_plan_permission_mode\": \"ask\"\n"
            "}\n";
    }
    out = SessionStorage::read_meta(meta_path);
    EXPECT_EQ(out.pre_plan_permission_mode, "default");
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
// 但 canonical jsonl 已经有用户消息。list_sessions 需要从 jsonl 补齐,否则 Web 左侧
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
    SessionStorage::write_meta(SessionStorage::meta_path(dir.string(), sid), meta);

    acecode::ChatMessage user;
    user.role = "user";
    user.content = "帮我看一下 websocket 错误";
    SessionStorage::append_message(SessionStorage::session_path(dir.string(), sid), user);

    acecode::ChatMessage assistant;
    assistant.role = "assistant";
    SessionStorage::append_message(SessionStorage::session_path(dir.string(), sid), assistant);

    auto sessions = SessionStorage::list_sessions(dir.string());
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions[0].id, sid);
    EXPECT_EQ(sessions[0].summary, user.content);
    EXPECT_EQ(sessions[0].message_count, 1);
    EXPECT_EQ(sessions[0].turn_count, 1);
}

// 场景:headless `-p --session-id` 允许调用方自定 id(如 "ci-run-42"),
// 文件名不再是 YYYYMMDD-HHMMSS-XXXX 形状。回归背景:list_sessions 原来只认
// 自动生成形状,自定 id 会话在 TUI /resume、Web 列表与 `-p -c` 里全部隐身
//(实测 `-p --session-id cc-cont-test` 后 `-p -c` 报 no previous session)。
// 期望:自定 id 与自动生成 id 同场可见,仍按 updated_at 新前旧后排序;
// 旧 PID 后缀 meta(<canonical>-<pid>.meta.json)继续被当 legacy 数据排除,
// 不能因为宽字符集正则被误吞进列表。
TEST(SessionStorage, ListSessionsIncludesCustomIdSessions) {
    auto dir = make_unique_tmp_dir("custom-id");

    SessionMeta custom;
    custom.id = "ci-run-42";
    custom.cwd = dir.string();
    custom.created_at = "2026-07-09T10:00:00Z";
    custom.updated_at = "2026-07-09T10:00:00Z";
    custom.message_count = 1;
    SessionStorage::write_meta(
        SessionStorage::meta_path(dir.string(), custom.id), custom);

    SessionMeta canonical;
    canonical.id = "20260709-110000-abcd";
    canonical.cwd = dir.string();
    canonical.created_at = "2026-07-09T11:00:00Z";
    canonical.updated_at = "2026-07-09T11:00:00Z";
    canonical.message_count = 1;
    SessionStorage::write_meta(
        SessionStorage::meta_path(dir.string(), canonical.id), canonical);

    // 旧 PID 后缀 meta:文件名匹配宽正则的字符集,但必须被 legacy 分支排除。
    SessionMeta legacy;
    legacy.id = "20260426-100000-abcd";
    legacy.cwd = dir.string();
    legacy.created_at = "2026-04-26T10:00:00Z";
    legacy.updated_at = "2026-04-26T10:00:00Z";
    SessionStorage::write_meta(
        SessionStorage::meta_path(dir.string(), legacy.id, 9999), legacy);

    auto sessions = SessionStorage::list_sessions(dir.string());
    ASSERT_EQ(sessions.size(), 2u);
    EXPECT_EQ(sessions[0].id, canonical.id);  // updated_at 新的在前
    EXPECT_EQ(sessions[1].id, custom.id);
}

// 场景:JSONL 可能因为异常退出留下坏行或未以换行结尾的半行。
// load_messages 只能返回完整、可解析的记录,不能抛异常。
TEST(SessionStorage, LoadMessagesSkipsMalformedAndTrailingPartialLines) {
    auto dir = make_unique_tmp_dir("safe-read");
    const std::string sid = "20260502-101144-61ab";
    const auto path = SessionStorage::session_path(dir.string(), sid);

    acecode::ChatMessage user;
    user.role = "user";
    user.content = "完整消息";
    SessionStorage::append_message(path, user);

    {
        std::ofstream ofs(path, std::ios::app | std::ios::binary);
        ofs << "not-json\n";
        ofs << "{\"role\":\"assistant\",\"content\":\"partial\"}";
    }

    std::vector<acecode::ChatMessage> out;
    ASSERT_NO_THROW(out = SessionStorage::load_messages(path));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].role, "user");
    EXPECT_EQ(out[0].content, "完整消息");
}

// 场景:meta 写入要能替换已经存在的文件。Windows 上 rename 不覆盖目标,
// 因此 write_meta 必须走真正的 replace 语义,否则第二次写会静默保留旧 meta。
TEST(SessionStorage, WriteMetaAtomicallyReplacesExistingFile) {
    auto dir = make_unique_tmp_dir("atomic-meta");
    auto meta_path = (dir / "20260419-120000-abcd.meta.json").string();

    SessionMeta first;
    first.id = "20260419-120000-abcd";
    first.cwd = dir.string();
    first.updated_at = "2026-04-19T12:00:00Z";
    first.summary = "first";
    SessionStorage::write_meta(meta_path, first);

    SessionMeta second = first;
    second.updated_at = "2026-04-19T12:05:00Z";
    second.summary = "second";
    second.message_count = 2;
    SessionStorage::write_meta(meta_path, second);

    auto out = SessionStorage::read_meta(meta_path);
    EXPECT_EQ(out.id, first.id);
    EXPECT_EQ(out.updated_at, second.updated_at);
    EXPECT_EQ(out.summary, "second");
    EXPECT_EQ(out.message_count, 2);
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

TEST(SessionStorage, MetaRoundtripsThroughUtf8ProjectDirectory) {
    auto root = make_unique_tmp_dir("utf8-project");
    auto dir = root / acecode::path_from_utf8(u8"会话目录");
    fs::create_directories(dir);
    std::string project_dir = acecode::path_to_utf8(dir);
    const std::string sid = "20260512-120000-abcd";

    SessionMeta in;
    in.id = sid;
    in.cwd = project_dir;
    in.summary = u8"中文摘要";

    std::string meta_path = SessionStorage::meta_path(project_dir, sid);
    SessionStorage::write_meta(meta_path, in);

    ASSERT_TRUE(fs::exists(acecode::path_from_utf8(meta_path)));
    SessionMeta out = SessionStorage::read_meta(meta_path);
    EXPECT_EQ(out.cwd, project_dir);
    EXPECT_EQ(out.summary, u8"中文摘要");

    auto sessions = SessionStorage::list_sessions(project_dir);
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions[0].summary, u8"中文摘要");
}
