#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

struct sqlite3;

namespace acecode {

enum class ThreadGoalStatus {
    Active,
    Paused,
    BudgetLimited,
    Complete,
};

std::string to_string(ThreadGoalStatus status);
std::optional<ThreadGoalStatus> parse_thread_goal_status(const std::string& status);
bool is_thread_goal_active(ThreadGoalStatus status);
bool is_thread_goal_terminal(ThreadGoalStatus status);

struct ThreadGoal {
    std::string thread_id;
    std::string goal_id;
    std::string objective;
    ThreadGoalStatus status = ThreadGoalStatus::Active;
    std::optional<std::int64_t> token_budget;
    std::int64_t tokens_used = 0;
    std::int64_t time_used_seconds = 0;
    std::int64_t created_at_ms = 0;
    std::int64_t updated_at_ms = 0;
};

struct ThreadGoalAccountingResult {
    std::optional<ThreadGoal> goal;
    bool updated = false;
    bool became_budget_limited = false;
};

class ThreadGoalStore {
public:
    explicit ThreadGoalStore(std::filesystem::path project_dir);
    ~ThreadGoalStore();

    ThreadGoalStore(const ThreadGoalStore&) = delete;
    ThreadGoalStore& operator=(const ThreadGoalStore&) = delete;

    static std::filesystem::path database_path_for_project(
        const std::filesystem::path& project_dir);

    bool initialize(std::string* error = nullptr);
    bool available() const { return db_ != nullptr; }

    std::optional<ThreadGoal> get_thread_goal(
        const std::string& thread_id,
        std::string* error = nullptr) const;

    bool replace_thread_goal(const std::string& thread_id,
                             const std::string& objective,
                             std::optional<std::int64_t> token_budget,
                             ThreadGoalStatus status,
                             std::string* error = nullptr);

    bool update_thread_goal_status(const std::string& thread_id,
                                   const std::string& goal_id,
                                   ThreadGoalStatus status,
                                   std::string* error = nullptr);

    bool update_thread_goal_objective(const std::string& thread_id,
                                      const std::string& goal_id,
                                      const std::string& objective,
                                      std::optional<std::int64_t> token_budget,
                                      std::string* error = nullptr);

    bool pause_active_thread_goal(const std::string& thread_id,
                                  std::string* error = nullptr);

    bool delete_thread_goal(const std::string& thread_id,
                            std::string* error = nullptr);

    ThreadGoalAccountingResult account_thread_goal_usage(
        const std::string& thread_id,
        const std::string& goal_id,
        std::int64_t token_delta,
        std::int64_t time_delta_seconds,
        bool allow_complete,
        std::string* error = nullptr);

    bool copy_goal_reset_usage(const std::string& from_thread_id,
                               const std::string& to_thread_id,
                               std::string* error = nullptr);

private:
    bool ensure_initialized(std::string* error) const;

    std::filesystem::path db_path_;
    sqlite3* db_ = nullptr;
};

std::string trim_goal_objective(const std::string& text);
bool validate_goal_objective(const std::string& text, std::string* error = nullptr);
bool validate_goal_token_budget(std::optional<std::int64_t> budget,
                                std::string* error = nullptr);
nlohmann::json thread_goal_to_json(const ThreadGoal& goal);

} // namespace acecode
