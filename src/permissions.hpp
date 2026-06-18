#pragma once

#include <string>
#include <vector>
#include <set>
#include <mutex>
#include <algorithm>
#include <cstring>
#include <atomic>

namespace acecode {

// Rule action for the permission rules engine
enum class RuleAction { Allow, Deny };

// A permission rule that matches tool calls by tool name, path, and/or command pattern
struct PermissionRule {
    std::string tool_pattern;    // glob: "bash", "file_*", "*"
    std::string path_pattern;    // glob: "src/**", "*.env", "" (any)
    std::string command_pattern; // prefix: "git ", "rm -rf /", "" (any)
    RuleAction action = RuleAction::Allow;
    int priority = 0;            // higher = evaluated first
};

// Permission mode for the current session
enum class PermissionMode {
    Default,      // Prompt for write/exec tools, auto-allow read-only
    AcceptEdits,  // Also auto-allow file_write, file_edit (still ask for bash)
    Yolo,         // Auto-allow tools; first external file write still confirms
    Plan          // Explore and write only the active plan file before approval
};

// Result of a permission check
enum class PermissionResult {
    Allow,        // Execute the tool
    Deny,         // Reject this tool call
    AlwaysAllow   // Allow + remember for this tool for the rest of the session
};

// Manages tool permission decisions
class PermissionManager {
public:
    void set_mode(PermissionMode mode) {
        const PermissionMode current = mode_.load(std::memory_order_relaxed);
        if (mode != PermissionMode::Yolo || current != PermissionMode::Yolo) {
            yolo_external_file_write_confirmed_.store(false, std::memory_order_relaxed);
        }
        if (mode == PermissionMode::Plan) {
            if (current != PermissionMode::Plan) {
                pre_plan_mode_.store(current, std::memory_order_relaxed);
                has_pre_plan_mode_.store(true, std::memory_order_relaxed);
            }
        } else {
            has_pre_plan_mode_.store(false, std::memory_order_relaxed);
        }
        mode_.store(mode, std::memory_order_relaxed);
    }
    PermissionMode mode() const { return mode_.load(std::memory_order_relaxed); }

    PermissionMode pre_plan_mode() const {
        if (!has_pre_plan_mode_.load(std::memory_order_relaxed)) {
            return PermissionMode::Default;
        }
        PermissionMode mode = pre_plan_mode_.load(std::memory_order_relaxed);
        return mode == PermissionMode::Plan ? PermissionMode::Default : mode;
    }

    void set_pre_plan_mode(PermissionMode mode) {
        if (mode == PermissionMode::Plan) mode = PermissionMode::Default;
        pre_plan_mode_.store(mode, std::memory_order_relaxed);
        has_pre_plan_mode_.store(true, std::memory_order_relaxed);
    }

    PermissionMode restore_pre_plan_mode() {
        PermissionMode restored = pre_plan_mode();
        mode_.store(restored, std::memory_order_relaxed);
        has_pre_plan_mode_.store(false, std::memory_order_relaxed);
        clear_session_allows();
        return restored;
    }

    // Enable/disable dangerous mode (bypasses ALL checks including path safety)
    void set_dangerous(bool enabled) { dangerous_mode_.store(enabled, std::memory_order_relaxed); }
    bool is_dangerous() const { return dangerous_mode_.load(std::memory_order_relaxed); }

    bool yolo_external_file_write_confirmed() const {
        return yolo_external_file_write_confirmed_.load(std::memory_order_relaxed);
    }

    void mark_yolo_external_file_write_confirmed() {
        yolo_external_file_write_confirmed_.store(true, std::memory_order_relaxed);
    }

    void clear_yolo_external_file_write_confirmation() {
        yolo_external_file_write_confirmed_.store(false, std::memory_order_relaxed);
    }

    // Add a permission rule
    void add_rule(const PermissionRule& rule) {
        std::lock_guard<std::mutex> lk(mu_);
        rules_.push_back(rule);
    }

    // Check if a tool should be auto-allowed (no user prompt needed)
    // path and command are optional context for rule matching
    bool should_auto_allow(const std::string& tool_name, bool is_read_only,
                           const std::string& path = "",
                           const std::string& command = "") const {
        // Dangerous mode: everything is auto-allowed unconditionally
                if (dangerous_mode_.load(std::memory_order_relaxed)) return true;

        // Check rules (priority-ordered, first match wins)
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!rules_.empty()) {
                // Build sorted copy by descending priority
                auto sorted = rules_;
                std::sort(sorted.begin(), sorted.end(),
                    [](const PermissionRule& a, const PermissionRule& b) {
                        return a.priority > b.priority;
                    });
                for (const auto& rule : sorted) {
                    if (rule_matches(rule, tool_name, path, command)) {
                        if (rule.action == RuleAction::Deny) return false;
                        if (rule.action == RuleAction::Allow) return true;
                    }
                }
            }
        }

        // Yolo mode: tools are auto-allowed by default. AgentLoop may still
        // force one confirmation before the first external file write.
        if (mode_.load(std::memory_order_relaxed) == PermissionMode::Yolo) return true;

        // Read-only tools are always auto-allowed
        if (is_read_only) return true;

        // memory_write is auto-allowed in every non-Yolo mode because its
        // tool implementation hard-locks the target path to
        // ~/.acecode/memory/ and rejects anything else — the PermissionManager
        // doesn't need to prompt on top of that.
        if (tool_name == "memory_write") return true;

        // task_complete is a zero-side-effect terminator signal. Even though
        // its ToolImpl already sets is_read_only=true (so the read-only branch
        // above would catch it), we also name-match here so future refactors
        // that toggle that flag can't accidentally turn task_complete into a
        // tool that prompts the user mid-loop.
        if (tool_name == "task_complete") return true;

        // TodoWrite mutates only session-local checklist state so it must run
        // sequentially, but it should never interrupt the user with a file/exec
        // permission prompt.
        if (tool_name == "TodoWrite") return true;

        // Browser bridge tools are governed by their own config switch and
        // runtime health checks. Marking mutating browser tools non-read-only
        // keeps them sequential in AgentLoop without adding a confirmation
        // prompt before every browser action.
        if (tool_name.rfind("browser_", 0) == 0) return true;

        // Session-level always-allow for this tool
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (session_allowed_.count(tool_name)) return true;
        }

        // AcceptEdits mode: auto-allow file tools (but not bash)
        if (mode_.load(std::memory_order_relaxed) == PermissionMode::AcceptEdits) {
            if (tool_name == "file_write" || tool_name == "file_edit") {
                return true;
            }
        }

        return false;
    }

    // Record that user chose "always allow" for a tool
    void add_session_allow(const std::string& tool_name) {
        std::lock_guard<std::mutex> lk(mu_);
        session_allowed_.insert(tool_name);
    }

    // Check if a tool has session-level always-allow
    bool has_session_allow(const std::string& tool_name) const {
        std::lock_guard<std::mutex> lk(mu_);
        return session_allowed_.count(tool_name) > 0;
    }

    // Clear all session allows (e.g., on mode change)
    void clear_session_allows() {
        std::lock_guard<std::mutex> lk(mu_);
        session_allowed_.clear();
        clear_yolo_external_file_write_confirmation();
    }

    // Cycle to next permission mode
    PermissionMode cycle_mode() {
        PermissionMode next = PermissionMode::Default;
        switch (mode_.load(std::memory_order_relaxed)) {
            case PermissionMode::Default:     next = PermissionMode::AcceptEdits; break;
            case PermissionMode::AcceptEdits: next = PermissionMode::Yolo; break;
            case PermissionMode::Yolo:        next = PermissionMode::Plan; break;
            case PermissionMode::Plan:        next = PermissionMode::Default; break;
        }
        set_mode(next);
        clear_session_allows();
        return next;
    }

    static const char* mode_name(PermissionMode m) {
        switch (m) {
            case PermissionMode::Default:     return "default";
            case PermissionMode::AcceptEdits: return "accept-edits";
            case PermissionMode::Yolo:        return "yolo";
            case PermissionMode::Plan:        return "plan";
        }
        return "unknown";
    }

    static const char* mode_description(PermissionMode m) {
        switch (m) {
            case PermissionMode::Default:     return "Prompt for write/exec tools";
            case PermissionMode::AcceptEdits: return "Auto-allow file edits, prompt for bash";
            case PermissionMode::Yolo:        return "Auto-allow tools; confirm first external file write";
            case PermissionMode::Plan:        return "Plan first, approve before coding";
        }
        return "";
    }

private:
    // Simple glob match: supports * (any non-/) and ** (any including /)
    // Empty pattern matches everything
    static bool glob_match(const std::string& pattern, const std::string& text) {
        if (pattern.empty()) return true;
        return glob_match_impl(pattern.c_str(), text.c_str());
    }

    static bool glob_match_impl(const char* p, const char* t) {
        while (*p) {
            if (p[0] == '*' && p[1] == '*') {
                // ** matches any sequence including /
                p += 2;
                if (*p == '/') p++; // skip optional / after **
                if (!*p) return true;
                for (const char* s = t; *s; s++) {
                    if (glob_match_impl(p, s)) return true;
                }
                return glob_match_impl(p, t + strlen(t)); // match empty
            }
            if (*p == '*') {
                // * matches any sequence except /
                p++;
                for (const char* s = t; *s && *s != '/'; s++) {
                    if (glob_match_impl(p, s)) return true;
                }
                return glob_match_impl(p, t); // * can match empty
            }
            if (*p == '?') {
                if (!*t || *t == '/') return false;
                p++; t++;
                continue;
            }
            // Case-insensitive char compare
            char pc = *p, tc = *t;
            if (pc >= 'A' && pc <= 'Z') pc += 32;
            if (tc >= 'A' && tc <= 'Z') tc += 32;
            if (pc != tc) return false;
            p++; t++;
        }
        return *t == '\0';
    }

    // Normalize path separators to /
    static std::string normalize_path(const std::string& path) {
        std::string result = path;
        for (auto& c : result) {
            if (c == '\\') c = '/';
        }
        return result;
    }

    // Check if a rule matches the given context
    static bool rule_matches(const PermissionRule& rule,
                             const std::string& tool_name,
                             const std::string& path,
                             const std::string& command) {
        // Tool pattern must match
        if (!rule.tool_pattern.empty() && !glob_match(rule.tool_pattern, tool_name)) {
            return false;
        }
        // Path pattern must match (if rule has one and path is provided)
        if (!rule.path_pattern.empty() && !path.empty()) {
            std::string norm = normalize_path(path);
            if (!glob_match(rule.path_pattern, norm)) return false;
        } else if (!rule.path_pattern.empty() && path.empty()) {
            return false; // rule requires path but none provided
        }
        // Command pattern: prefix match
        if (!rule.command_pattern.empty() && !command.empty()) {
            if (command.substr(0, rule.command_pattern.size()) != rule.command_pattern) {
                return false;
            }
        } else if (!rule.command_pattern.empty() && command.empty()) {
            return false; // rule requires command but none provided
        }
        return true;
    }

    std::atomic<PermissionMode> mode_{PermissionMode::Default};
    std::atomic<PermissionMode> pre_plan_mode_{PermissionMode::Default};
    std::atomic<bool> has_pre_plan_mode_{false};
    std::atomic<bool> dangerous_mode_{false};
    std::atomic<bool> yolo_external_file_write_confirmed_{false};
    mutable std::mutex mu_;
    std::set<std::string> session_allowed_;
    std::vector<PermissionRule> rules_;
};

} // namespace acecode
