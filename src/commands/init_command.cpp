#include "init_command.hpp"

#include "../config/config.hpp"
#include "../utils/utf8_path.hpp"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

namespace fs = std::filesystem;

namespace acecode {

namespace {

void emit(CommandContext& ctx, const std::string& msg) {
    std::lock_guard<std::mutex> lk(ctx.state.mu);
    ctx.state.conversation.push_back({"system", msg, false});
    ctx.state.chat_follow_tail = true;
}

std::string skeleton_body_internal(bool claude_exists) {
    std::ostringstream oss;
    if (claude_exists) {
        oss << "<!--\n"
            << "  acecode found CLAUDE.md in this directory. AGENT.md is the\n"
            << "  native project-instructions file; CLAUDE.md is read only as a\n"
            << "  compatibility fallback when AGENT.md is absent.\n"
            << "  If you want to migrate the legacy file, you can rename it:\n"
            << "    mv CLAUDE.md AGENT.md\n"
            << "-->\n\n";
    }
    oss << "# Project Overview\n\n"
        << "<!-- One paragraph: what is this project, who uses it, and what does acecode need to know when helping? -->\n\n"
        << "# Build Instructions\n\n"
        << "<!-- How to build / run / test. Commands the agent can use. -->\n\n"
        << "# Architecture\n\n"
        << "<!-- Top-level modules, data flow, external services. Keep under 300 lines. -->\n\n"
        << "# Conventions\n\n"
        << "<!-- Coding style, PR conventions, anything agent-facing that is non-obvious from the code. -->\n";
    return oss.str();
}

// Mirrors the preconditions AgentLoop relies on when calling chat_stream, not a
// comprehensive auth probe — false positives just reach AgentLoop which
// surfaces the HTTP error normally. Rationale in design.md D4.
// Body adapted from claudecodehaha/src/commands/init.ts OLD_INIT_PROMPT with
// Claude Code → acecode and an acecode-specific AGENT.md file prefix. The
// wording ("not obvious instructions", "avoid listing every
// component") carries over verbatim because it is the load-bearing part that
// keeps the LLM from producing boilerplate.
std::string build_init_prompt_body() {
    return
        "Please analyze this codebase and create an AGENT.md file, which "
        "will be given to future instances of acecode to operate in this "
        "repository.\n"
        "\n"
        "What to add:\n"
        "1. Commands that will be commonly used, such as how to build, lint, "
        "and run tests. Include the necessary commands to develop in this "
        "codebase, such as how to run a single test.\n"
        "2. High-level code architecture and structure so that future "
        "instances can be productive more quickly. Focus on the \"big "
        "picture\" architecture that requires reading multiple files to "
        "understand.\n"
        "\n"
        "Usage notes:\n"
        "- If there's already an AGENT.md, suggest improvements to it.\n"
        "- When you make the initial AGENT.md, do not repeat yourself and "
        "do not include obvious instructions like \"Provide helpful error "
        "messages to users\", \"Write unit tests for all new utilities\", "
        "\"Never include sensitive information (API keys, tokens) in code or "
        "commits\".\n"
        "- Avoid listing every component or file structure that can be easily "
        "discovered.\n"
        "- Don't include generic development practices.\n"
        "- If there are Cursor rules (in .cursor/rules/ or .cursorrules) or "
        "Copilot rules (in .github/copilot-instructions.md), make sure to "
        "include the important parts.\n"
        "- If there is a README.md, make sure to include the important "
        "parts.\n"
        "- Do not make up information such as \"Common Development Tasks\", "
        "\"Tips for Development\", \"Support and Documentation\" unless this "
        "is expressly included in other files that you read.\n"
        "- Be sure to prefix the file with the following text:\n"
        "\n"
        "```\n"
        "# AGENT.md\n"
        "\n"
        "This file provides guidance to acecode "
        "(https://github.com/tmoonlight/acecode) when working with code in "
        "this repository.\n"
        "```\n";
}

std::string improvement_suffix() {
    return
        "\n"
        "NOTE: AGENT.md already exists in this directory. Read it first "
        "with the file_read tool, identify specific gaps against what the "
        "codebase actually shows, and apply targeted edits via the "
        "file_edit_tool. If the file is already accurate and well-written, "
        "report that it looks good and do not edit it — do not overwrite "
        "silently.\n";
}

std::string migration_suffix(bool claude_exists) {
    std::ostringstream oss;
    oss << "\n"
        << "NOTE: ";
    if (claude_exists) {
        oss << "CLAUDE.md already exists in this directory but AGENT.md "
               "does not. Read CLAUDE.md first and adapt its content into a "
               "new AGENT.md via the file_write_tool rather than writing "
               "from scratch. ";
    }
    oss << "Do not delete or modify CLAUDE.md on disk — leave it alone. After "
           "writing AGENT.md, tell the user that AGENT.md now takes precedence "
           "so they can delete CLAUDE.md or keep it as a fallback.\n";
    return oss.str();
}

void cmd_init(CommandContext& ctx, const std::string& /*args*/) {
    fs::path cwd = path_from_utf8(ctx.agent_loop.cwd());
    std::error_code ec;
    bool agent_exists = fs::exists(cwd / "AGENT.md", ec);

    if (!has_usable_init_provider(ctx.config)) {
        // Offline fallback: write the static skeleton, same refuse-on-exists
        // behavior as the pre-LLM implementation. The skeleton helper already
        // tolerates the case where neither legacy file is present.
        fs::path target = cwd / "AGENT.md";
        if (agent_exists) {
            emit(ctx,
                 "AGENT.md already exists at " + path_to_utf8_generic(target) +
                 " — no model is configured, so /init cannot propose "
                 "improvements. Edit it by hand, or run /configure first and "
                 "re-run /init to get an LLM-driven improvement pass.");
            return;
        }

        std::ofstream ofs(target, std::ios::binary);
        if (!ofs.is_open()) {
            emit(ctx,
                 "Failed to open " + path_to_utf8_generic(target) +
                 " for writing.");
            return;
        }
        ofs << build_agent_md_skeleton(cwd);
        emit(ctx,
             "Created " + path_to_utf8_generic(target) +
             " (offline skeleton — no model is configured, run /configure to "
             "get a filled-in version).");
        return;
    }

    // LLM-driven path: build the prompt and submit it through the agent loop.
    std::string prompt = build_init_prompt(cwd);

    const std::string ack =
        "[Invoking /init — analyzing codebase and authoring AGENT.md...]";

    {
        std::lock_guard<std::mutex> lk(ctx.state.mu);
        ctx.state.conversation.push_back({"system", ack, false});
        ctx.state.chat_follow_tail = true;
        if (ctx.state.is_waiting) {
            ctx.state.pending_queue.push_back(prompt);
            return;
        }
        // 新一轮等待：提前把计时/计数字段重置，否则 on_busy_changed 会因为
        // is_waiting 已为 true 跳过它的重置块，thinking_start_time 停在 0
        // 会让底部 chip 秒数巨大。
        ctx.state.thinking_start_time = std::chrono::steady_clock::now();
        ctx.state.streaming_output_chars = 0;
        ctx.state.last_completion_tokens_authoritative = 0;
        ctx.state.is_waiting = true;
    }

    ctx.agent_loop.submit(prompt);
}

} // namespace

std::string build_agent_md_skeleton(const fs::path& cwd) {
    std::error_code ec;
    bool claude = fs::exists(cwd / "CLAUDE.md", ec);
    return skeleton_body_internal(claude);
}

std::string build_init_prompt(const fs::path& cwd) {
    std::error_code ec;
    bool agent_exists = fs::exists(cwd / "AGENT.md", ec);
    bool claude_exists = fs::exists(cwd / "CLAUDE.md", ec);

    std::string out = build_init_prompt_body();
    if (agent_exists) {
        out += improvement_suffix();
        if (claude_exists) {
            out +=
                "\nAlso read CLAUDE.md and cross-check it for additional "
                "project guidance not already present in AGENT.md. Preserve "
                "AGENT.md as the file you edit; do not delete or modify "
                "CLAUDE.md.\n";
        }
    } else if (claude_exists) {
        out += migration_suffix(claude_exists);
    }
    return out;
}

bool has_usable_init_provider(const AppConfig& cfg) {
    if (cfg.provider == "copilot") {
        // Background token refresh handles validity; treat as usable if chosen.
        return true;
    }
    if (cfg.provider == "codex") return false;
    if (cfg.provider == "openai") {
        return !cfg.openai.api_key.empty();
    }
    return false;
}

void register_init_command(CommandRegistry& registry) {
    registry.register_command(
        {"init",
         "Analyze this codebase and generate (or improve) AGENT.md",
         cmd_init});
}

} // namespace acecode
