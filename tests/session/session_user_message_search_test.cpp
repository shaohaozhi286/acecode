#include <gtest/gtest.h>

#include "session/session_storage.hpp"
#include "session/session_user_message_search.hpp"

#include <filesystem>
#include <random>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path temp_dir(const std::string& hint) {
    auto dir = fs::temp_directory_path() /
        ("acecode_user_message_search_" + hint + "_" + std::to_string(std::random_device{}()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

acecode::ChatMessage user_message(std::string content) {
    acecode::ChatMessage msg;
    msg.role = "user";
    msg.content = std::move(content);
    return msg;
}

} // namespace

TEST(SessionUserMessageSearch, DisplayTextWinsOverExpandedContent) {
    auto msg = user_message("[SYSTEM: expanded prompt]");
    msg.metadata = nlohmann::json{{"display_text", "/skill inspect"}};

    auto searchable = acecode::build_searchable_user_message("sid", 0, msg);
    ASSERT_TRUE(searchable.has_value());
    EXPECT_EQ(searchable->user_text, "/skill inspect");
    EXPECT_NE(searchable->search_text.find("/skill inspect"), std::string::npos);
    EXPECT_EQ(searchable->search_text.find("[SYSTEM: expanded prompt]"), std::string::npos);
}

TEST(SessionUserMessageSearch, HiddenAndMetaUserMessagesAreExcluded) {
    auto hidden = user_message("hidden goal");
    hidden.metadata = nlohmann::json{{"hidden_goal_context", true}};
    EXPECT_FALSE(acecode::is_searchable_visible_user_message(hidden));
    EXPECT_FALSE(acecode::build_searchable_user_message("sid", 0, hidden).has_value());

    auto meta = user_message("compact boundary");
    meta.is_meta = true;
    EXPECT_FALSE(acecode::is_searchable_visible_user_message(meta));
    EXPECT_FALSE(acecode::build_searchable_user_message("sid", 1, meta).has_value());

    auto compact_summary = user_message("compact summary");
    compact_summary.is_compact_summary = true;
    EXPECT_FALSE(acecode::is_searchable_visible_user_message(compact_summary));
    EXPECT_FALSE(acecode::build_searchable_user_message("sid", 2, compact_summary).has_value());
}

TEST(SessionUserMessageSearch, ExtractsOnlyFileAndImageAttachmentNames) {
    auto msg = user_message("please inspect");
    msg.content_parts = nlohmann::json::array({
        {{"type", "text"}, {"text", "please inspect"}},
        {{"type", "file"}, {"attachment", {{"id", "att_file"}, {"name", "report-final.pdf"}, {"path", "C:/secret/report-final.pdf"}}}},
        {{"type", "image"}, {"attachment", {{"id", "att_img"}, {"name", "diagram.png"}, {"blob_url", "/blob"}}}},
        {{"type", "browser_context"}, {"context", {{"title", "Browser Tab"}}}},
    });

    auto names = acecode::searchable_user_message_attachment_names(msg);
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "report-final.pdf");
    EXPECT_EQ(names[1], "diagram.png");

    auto searchable = acecode::build_searchable_user_message("sid", 2, msg);
    ASSERT_TRUE(searchable.has_value());
    EXPECT_NE(searchable->search_text.find("report-final.pdf"), std::string::npos);
    EXPECT_NE(searchable->search_text.find("diagram.png"), std::string::npos);
    EXPECT_EQ(searchable->search_text.find("C:/secret"), std::string::npos);
    EXPECT_EQ(searchable->search_text.find("att_file"), std::string::npos);
}

TEST(SessionUserMessageSearch, AttachmentOnlyMessageIsSearchable) {
    auto msg = user_message("");
    msg.content_parts = nlohmann::json::array({
        {{"type", "file"}, {"attachment", {{"name", "notes.txt"}}}},
    });

    auto searchable = acecode::build_searchable_user_message("sid", 3, msg);
    ASSERT_TRUE(searchable.has_value());
    EXPECT_TRUE(searchable->user_text.empty());
    EXPECT_EQ(searchable->attachment_text, "notes.txt");
    EXPECT_EQ(searchable->search_text, "notes.txt");
}

TEST(SessionUserMessageIndex, RebuildsAndSearchesUserTextAndAttachmentNames) {
    auto dir = temp_dir("index");
    const std::string project_dir = dir.string();
    const std::string sid = "session-a";
    const std::string jsonl = acecode::SessionStorage::session_path(project_dir, sid);

    auto first = user_message("我们要做 sqlite索引");
    first.uuid = "u1";
    auto hidden = user_message("quarterly-secret");
    hidden.metadata = nlohmann::json{{"hidden_goal_context", true}};
    auto attachment = user_message("");
    attachment.uuid = "u2";
    attachment.content_parts = nlohmann::json::array({
        {{"type", "file"}, {"attachment", {{"name", "report-final.pdf"}}}},
    });
    acecode::SessionStorage::write_messages(jsonl, {first, hidden, attachment});

    acecode::SessionUserMessageIndex index(project_dir);
    std::string error;
    ASSERT_TRUE(index.rebuild_session(sid, jsonl, &error)) << error;

    auto text_matches = index.search("sqlite索引", 10, &error);
    ASSERT_EQ(text_matches.size(), 1u) << error;
    EXPECT_EQ(text_matches[0].session_id, sid);
    EXPECT_NE(text_matches[0].snippet.find("sqlite索引"), std::string::npos);

    auto file_matches = index.search("report-final", 10, &error);
    ASSERT_EQ(file_matches.size(), 1u) << error;
    ASSERT_EQ(file_matches[0].matched_attachment_names.size(), 1u);
    EXPECT_EQ(file_matches[0].matched_attachment_names[0], "report-final.pdf");

    auto hidden_matches = index.search("quarterly-secret", 10, &error);
    EXPECT_TRUE(hidden_matches.empty());
}

TEST(SessionUserMessageIndex, EnsureSessionRebuildsStaleJsonl) {
    auto dir = temp_dir("stale");
    const std::string project_dir = dir.string();
    const std::string sid = "session-b";
    const std::string jsonl = acecode::SessionStorage::session_path(project_dir, sid);

    acecode::SessionStorage::write_messages(jsonl, {user_message("first searchable")});

    acecode::SessionUserMessageIndex index(project_dir);
    std::string error;
    ASSERT_TRUE(index.ensure_session_indexed(sid, jsonl, &error)) << error;
    ASSERT_EQ(index.search("first", 10, &error).size(), 1u);

    acecode::SessionStorage::write_messages(jsonl, {
        user_message("first searchable"),
        user_message("second searchable"),
    });
    ASSERT_TRUE(index.ensure_session_indexed(sid, jsonl, &error)) << error;
    ASSERT_EQ(index.search("second", 10, &error).size(), 1u);
}

TEST(SessionUserMessageIndex, IncrementalAppendUsesFreshSource) {
    auto dir = temp_dir("append");
    const std::string project_dir = dir.string();
    const std::string sid = "session-c";
    const std::string jsonl = acecode::SessionStorage::session_path(project_dir, sid);

    auto first = user_message("first turn");
    acecode::SessionStorage::write_messages(jsonl, {first});

    acecode::SessionUserMessageIndex index(project_dir);
    std::string error;
    ASSERT_TRUE(index.rebuild_session(sid, jsonl, &error)) << error;

    auto before = acecode::session_user_message_file_signature(jsonl);
    auto second = user_message("second turn");
    acecode::SessionStorage::append_message(jsonl, second);
    ASSERT_TRUE(index.index_appended_message(sid, 1, second, jsonl, before, &error)) << error;

    EXPECT_EQ(index.search("first", 10, &error).size(), 1u);
    EXPECT_EQ(index.search("second", 10, &error).size(), 1u);
}

TEST(SessionUserMessageIndex, SearchLimitCountsSessionsNotRawMatchingMessages) {
    auto dir = temp_dir("limit");
    const std::string project_dir = dir.string();
    const std::string busy_sid = "session-many";
    const std::string other_sid = "session-other";
    const std::string busy_jsonl = acecode::SessionStorage::session_path(project_dir, busy_sid);
    const std::string other_jsonl = acecode::SessionStorage::session_path(project_dir, other_sid);

    std::vector<acecode::ChatMessage> busy_messages;
    for (int i = 0; i < 20; ++i) {
        busy_messages.push_back(user_message("needle repeated " + std::to_string(i)));
    }
    acecode::SessionStorage::write_messages(busy_jsonl, busy_messages);
    acecode::SessionStorage::write_messages(other_jsonl, {user_message("needle in other session")});

    acecode::SessionUserMessageIndex index(project_dir);
    std::string error;
    ASSERT_TRUE(index.rebuild_session(busy_sid, busy_jsonl, &error)) << error;
    ASSERT_TRUE(index.rebuild_session(other_sid, other_jsonl, &error)) << error;

    auto matches = index.search("needle", 2, &error);
    ASSERT_EQ(matches.size(), 2u) << error;
    EXPECT_EQ(matches[0].session_id, busy_sid);
    EXPECT_EQ(matches[1].session_id, other_sid);
}
