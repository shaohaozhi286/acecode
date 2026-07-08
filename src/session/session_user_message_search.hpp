#pragma once

#include "../provider/llm_provider.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace acecode {

struct SessionUserMessageFileSignature {
    bool exists = false;
    std::int64_t mtime = 0;
    std::int64_t size = 0;
};

struct SearchableUserMessage {
    std::string session_id;
    int message_ordinal = 0;
    std::string message_uuid;
    std::string user_text;
    std::vector<std::string> attachment_names;
    std::string attachment_text;
    std::string search_text;
    std::string search_text_norm;
    std::string snippet_text;
};

struct SessionUserMessageSearchResult {
    std::string session_id;
    int message_ordinal = 0;
    int score = 0;
    std::string snippet;
    std::vector<std::string> matched_attachment_names;
};

bool is_searchable_visible_user_message(const ChatMessage& msg);
std::string searchable_user_message_text(const ChatMessage& msg);
std::vector<std::string> searchable_user_message_attachment_names(const ChatMessage& msg);
std::optional<SearchableUserMessage> build_searchable_user_message(
    const std::string& session_id,
    int message_ordinal,
    const ChatMessage& msg);

SessionUserMessageFileSignature session_user_message_file_signature(
    const std::string& jsonl_path);

class SessionUserMessageIndex {
public:
    explicit SessionUserMessageIndex(std::string project_dir);
    ~SessionUserMessageIndex();

    bool initialize(std::string* error = nullptr);

    bool index_appended_message(const std::string& session_id,
                                int message_ordinal,
                                const ChatMessage& msg,
                                const std::string& jsonl_path,
                                const SessionUserMessageFileSignature& before_append,
                                std::string* error = nullptr);

    bool rebuild_session(const std::string& session_id,
                         const std::string& jsonl_path,
                         const std::vector<ChatMessage>& messages,
                         std::string* error = nullptr);

    bool rebuild_session(const std::string& session_id,
                         const std::string& jsonl_path,
                         std::string* error = nullptr);

    bool ensure_session_indexed(const std::string& session_id,
                                const std::string& jsonl_path,
                                std::string* error = nullptr);

    bool ensure_project_indexed(std::string* error = nullptr);

    std::vector<SessionUserMessageSearchResult> search(const std::string& query,
                                                       int limit,
                                                       std::string* error = nullptr);

private:
    bool open(std::string* error);
    bool exec(const char* sql, std::string* error);
    bool source_matches_signature(const std::string& session_id,
                                  const SessionUserMessageFileSignature& signature,
                                  std::string* error);
    bool update_source(const std::string& session_id,
                       const std::string& jsonl_path,
                       const SessionUserMessageFileSignature& signature,
                       std::string* error);
    bool upsert_searchable_message(const SearchableUserMessage& message,
                                   std::string* error);

    std::string project_dir_;
    ::sqlite3* db_ = nullptr;
};

} // namespace acecode
