#include <gtest/gtest.h>

#include "session/session_attention.hpp"

using acecode::SessionAttentionRecord;
using acecode::SessionAttentionState;
using acecode::SessionEventKind;
using acecode::apply_session_attention_event;
using acecode::mark_session_attention_read;
using acecode::session_attention_state_for;
using acecode::session_event_has_user_visible_output;

TEST(SessionAttention, BusyStateTakesDisplayPrecedence) {
    SessionAttentionRecord r;
    r.update_cursor = 10;
    r.read_cursor = 0;
    r.busy = true;
    EXPECT_EQ(session_attention_state_for(r), SessionAttentionState::InProgress);
}

TEST(SessionAttention, OutputMakesIdleSessionUnread) {
    SessionAttentionRecord r;
    r = apply_session_attention_event(r, SessionEventKind::Message,
                                      nlohmann::json{{"role", "assistant"}},
                                      4, 1000);
    EXPECT_EQ(r.update_cursor, 4u);
    EXPECT_EQ(session_attention_state_for(r), SessionAttentionState::Unread);
}

TEST(SessionAttention, UserMessageDoesNotMakeSessionUnread) {
    SessionAttentionRecord r;
    r = apply_session_attention_event(r, SessionEventKind::Message,
                                      nlohmann::json{{"role", "user"}},
                                      4, 1000);
    EXPECT_EQ(r.update_cursor, 0u);
    EXPECT_EQ(session_attention_state_for(r), SessionAttentionState::Read);
}

TEST(SessionAttention, ReadAcknowledgementClearsUnreadAtCurrentCursor) {
    SessionAttentionRecord r;
    r.update_cursor = 8;
    r.read_cursor = 2;
    r = mark_session_attention_read(r, 8, 2000);
    EXPECT_EQ(r.read_cursor, 8u);
    EXPECT_EQ(session_attention_state_for(r), SessionAttentionState::Read);
}

TEST(SessionAttention, StaleReadAcknowledgementDoesNotClearNewerOutput) {
    SessionAttentionRecord r;
    r.update_cursor = 10;
    r.read_cursor = 2;
    r = mark_session_attention_read(r, 8, 2000);
    EXPECT_EQ(r.read_cursor, 8u);
    EXPECT_EQ(session_attention_state_for(r), SessionAttentionState::Unread);
}

TEST(SessionAttention, MissingStatusFallsBackToRead) {
    SessionAttentionRecord r;
    EXPECT_EQ(session_attention_state_for(r), SessionAttentionState::Read);
}

TEST(SessionAttention, InteractiveRequestsAreUserVisible) {
    EXPECT_TRUE(session_event_has_user_visible_output(SessionEventKind::PermissionRequest, nlohmann::json::object()));
    EXPECT_TRUE(session_event_has_user_visible_output(SessionEventKind::QuestionRequest, nlohmann::json::object()));
    EXPECT_TRUE(session_event_has_user_visible_output(SessionEventKind::Error, nlohmann::json::object()));
}
