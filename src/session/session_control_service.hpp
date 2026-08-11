#pragma once

#include "session_client.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace acecode {

class SessionRegistry;

struct SessionControlScope {
    std::string cwd;
    std::string caller_session_id;
};

struct SessionControlResult {
    bool success = false;
    nlohmann::json value = nlohmann::json::object();
    std::string error;

    static SessionControlResult ok(
        nlohmann::json value = nlohmann::json::object());
    static SessionControlResult fail(std::string error);
};

// In-process domain service shared by model tools and host surfaces. It never
// calls ACECode's HTTP API and always resolves targets inside the caller's
// workspace project directory before touching registry or disk state.
class SessionControlService {
public:
    struct Deps {
        SessionRegistry* registry = nullptr;
        SessionClient* client = nullptr;
    };

    explicit SessionControlService(Deps deps);

    SessionControlResult list(const SessionControlScope& scope,
                              std::size_t page_size = 10,
                              const std::string& cursor = {},
                              bool include_archived = false) const;
    SessionControlResult read(const SessionControlScope& scope,
                              const std::string& session_id,
                              std::size_t max_bytes = 4096) const;
    SessionControlResult wait(const SessionControlScope& scope,
                              const std::string& session_id,
                              std::uint64_t since_seq,
                              int timeout_seconds,
                              const std::atomic<bool>* abort_flag = nullptr) const;

    SessionControlResult create(const SessionControlScope& scope,
                                const std::string& title,
                                const std::string& model_name = {}) const;
    SessionControlResult fork(const SessionControlScope& scope,
                              const std::string& session_id,
                              const std::string& title = {},
                              const std::string& at_message_id = {}) const;
    SessionControlResult send(const SessionControlScope& scope,
                              const std::string& session_id,
                              const std::string& message,
                              bool steer_if_busy) const;
    SessionControlResult interrupt(const SessionControlScope& scope,
                                   const std::string& session_id) const;
    SessionControlResult set_title(const SessionControlScope& scope,
                                   const std::string& session_id,
                                   const std::string& title) const;
    SessionControlResult set_archived(const SessionControlScope& scope,
                                      const std::string& session_id,
                                      bool archived) const;
    SessionControlResult set_pinned(const SessionControlScope& scope,
                                    const std::string& session_id,
                                    bool pinned) const;

private:
    Deps deps_;
};

} // namespace acecode
