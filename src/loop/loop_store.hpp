#pragma once

#include "loop_schedule.hpp"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace acecode::loop {

struct StoreError {
    std::string code;
    std::string message;
    std::optional<ScheduleConflict> conflict;
};

enum class ClaimDisposition { None, Claimed, MissedWorkspaceBusy };

struct ClaimResult {
    ClaimDisposition disposition = ClaimDisposition::None;
    std::optional<LoopDefinition> loop;
    std::optional<LoopRun> run;
};

class LoopStore {
public:
    explicit LoopStore(std::filesystem::path database_path);
    ~LoopStore();

    LoopStore(const LoopStore&) = delete;
    LoopStore& operator=(const LoopStore&) = delete;

    static std::filesystem::path default_database_path();

    bool initialize(StoreError* error = nullptr);
    bool available() const;

    std::vector<LoopDefinition> list_loops(StoreError* error = nullptr) const;
    std::optional<LoopDefinition> get_loop(const std::string& id,
                                           StoreError* error = nullptr) const;
    std::optional<LoopDefinition> create_loop(LoopDefinition value,
                                              std::int64_t now_ms,
                                              StoreError* error = nullptr);
    std::optional<LoopDefinition> update_loop(const std::string& id,
                                              LoopDefinition value,
                                              std::int64_t now_ms,
                                              StoreError* error = nullptr);
    std::optional<LoopDefinition> set_loop_enabled(const std::string& id,
                                                   bool enabled,
                                                   std::int64_t now_ms,
                                                   StoreError* error = nullptr);
    bool delete_loop(const std::string& id, StoreError* error = nullptr);

    std::vector<LoopRun> list_runs(const std::string& loop_id,
                                   int limit = 100,
                                   StoreError* error = nullptr) const;
    std::optional<std::int64_t> earliest_next_run_at(StoreError* error = nullptr) const;

    // Called once at scheduler startup/wake reconciliation. Past occurrences
    // are aggregated as missed and never returned for execution.
    bool record_offline_missed(std::int64_t now_ms,
                               const std::string& owner_id,
                               StoreError* error = nullptr);

    // Claims at most one due occurrence. This transaction also advances the
    // definition's next_run_at and records workspace_busy as missed.
    ClaimResult claim_due(std::int64_t now_ms,
                          const std::string& owner_id,
                          StoreError* error = nullptr);

    bool update_run_state(const std::string& run_id,
                          RunStatus status,
                          std::int64_t now_ms,
                          const std::string& reason = {},
                          const std::string& session_id = {},
                          const std::string& worktree_path = {},
                          const std::string& worktree_branch = {},
                          StoreError* error = nullptr);

    bool interrupt_owner_runs(const std::string& owner_id,
                              std::int64_t now_ms,
                              StoreError* error = nullptr);

private:
    bool ensure_available(StoreError* error) const;
    std::vector<LoopDefinition> list_loops_locked(StoreError* error) const;
    std::optional<LoopDefinition> get_loop_locked(const std::string& id,
                                                  StoreError* error) const;
    bool begin_locked(StoreError* error) const;
    bool commit_locked(StoreError* error) const;
    void rollback_locked() const;

    std::filesystem::path db_path_;
    sqlite3* db_ = nullptr;
    mutable std::mutex mu_;
};

} // namespace acecode::loop
