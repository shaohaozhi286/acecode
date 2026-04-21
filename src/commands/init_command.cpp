#include "init_command.hpp"

#include "../config/config.hpp"

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

std::string skeleton_body_internal(bool claude_exists, bool agent_exists) {
    std::ostringstream oss;
    if (claude_exists || agent_exists) {
        // Prefer mentioning the file that actually exists so the example
        // command is immediately copy-paste-runnable.
        const char* rename_target = claude_exists ? "CLAUDE.md" : "AGENT.md";
        oss << "<!--\n"
            << "  acecode already auto-reads the following files it found in this\n"
            << "  directory: "
            << (claude_exists ? "CLAUDE.md" : "")
            << ((claude_exists && agent_exists) ? ", " : "")
            << (agent_exists ? "AGENT.md" : "")
            << ".\n"
            << "  If you want acecode's native semantics, you can rename it:\n"
            << "    mv " << rename_target << " ACECODE.md\n"
            << "  ACECODE.md wins over AGENT.md and CLAUDE.md when all three are present.\n"
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
bool has_usable_provider(const AppConfig& cfg) {
    if (cfg.provider == "copilot") {
        // Background token refresh handles validity; treat as usable if chosen.
        return true;
    }
    if (cfg.provider == "openai") {
        return !cfg.openai.api_key.empty();
    }
    return false;
}

// Body adapted from claudecodehaha/src/commands/init.ts OLD_INIT_PROMPT with
// CLAUDE.md → ACECODE.md, Claude Code → acecode, and an acecode-specific file
// prefix. The wording ("not obvious instructions", "avoid listing every
// component") carries over verbatim because it is the load-bearing part that
// keeps the LLM from producing boilerplate.
std::string build_init_prompt_body() {
    return
        "Please analyze this codebase and create an ACECODE.md file, which "
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
        "- If there's already an ACECODE.md, suggest improvements to it.\n"
        "- When you make the initial ACECODE.md, do not repeat yourself and "
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
        "# ACECODE.md\n"
        "\n"
        "This file provides guidance to acecode "
        "(https://github.com/tmoonlight/acecode) when working with code in "
        "this repository.\n"
        "```\n";
}

std::string improvement_suffix() {
    return
        "\n"
        "NOTE: ACECODE.md already exists in this directory. Read it first "
        "with the file_read tool, identify specific gaps against what the "
        "codebase actually shows, and apply targeted edits via the "
        "file_edit_tool. If the file is already accurate and well-written, "
        "report that it looks good and do not edit it — do not overwrite "
        "silently.\n";
}

std::string migration_suffix(bool claude_exists, bool agent_exists) {
    std::ostringstream oss;
    oss << "\n"
        << "NOTE: ";
    if (claude_exists && agent_exists) {
        // Match the project_instructions default filenames priority
        // ["ACECODE.md", "AGENT.md", "CLAUDE.md"] so `/init` does not reverse
        // the precedence the loader would honor at read time.
        oss << "Both AGENT.md and CLAUDE.md already exist in this directory "
               "but ACECODE.md does not. Prefer AGENT.md as the primary base "
               "— read it first and adapt its content into a new ACECODE.md "
               "via the file_write_tool. Cross-check CLAUDE.md for any "
               "additional information not already present in AGENT.md. ";
    } else if (agent_exists) {
        oss << "AGENT.md already exists in this directory but ACECODE.md does "
               "not. Read AGENT.md first and adapt its content into a new "
               "ACECODE.md via the file_write_tool rather than writing from "
               "scratch. ";
    } else {
        oss << "CLAUDE.md already exists in this directory but ACECODE.md "
               "does not. Read CLAUDE.md first and adapt its content into a "
               "new ACECODE.md via the file_write_tool rather than writing "
               "from scratch. ";
    }
    oss << "Do not delete or modify the legacy file(s) on disk — leave them "
           "alone. After writing ACECODE.md, tell the user that ACECODE.md "
           "now takes precedence so they can delete the legacy file(s) or "
           "keep them (acecode reads them as a fallback either way).\n";
    return oss.str();
}

void cmd_init(CommandContext& ctx, const std::string& /*args*/) {
    fs::path cwd = fs::path(ctx.agent_loop.cwd());
    std::error_code ec;
    bool acecode_exists = fs::exists(cwd / "ACECODE.md", ec);

    if (!has_usable_provider(ctx.config)) {
        // Offline fallback: write the static skeleton, same refuse-on-exists
        // behavior as the pre-LLM implementation. The skeleton helper already
        // tolerates the case where neither legacy file is present.
        fs::path target = cwd / "ACECODE.md";
        if (acecode_exists) {
            emit(ctx,
                 "ACECODE.md already exists at " + target.generic_string() +
                 " — no model is configured, so /init cannot propose "
                 "improvements. Edit it by hand, or run /configure first and "
                 "re-run /init to get an LLM-driven improvement pass.");
            return;
        }

        std::ofstream ofs(target, std::ios::binary);
        if (!ofs.is_open()) {
            emit(ctx,
                 "Failed to open " + target.generic_string() +
                 " for writing.");
            return;
        }
        ofs << build_acecode_md_skeleton(cwd);
        emit(ctx,
             "Created " + target.generic_string() +
             " (offline skeleton — no model is configured, run /configure to "
             "get a filled-in version).");
        return;
    }

    // LLM-driven path: build the prompt and submit it through the agent loop.
    std::string prompt = build_init_prompt(cwd);

    const std::string ack =
        "[Invoking /init — analyzing codebase and authoring ACECODE.md...]";

    {
        std::lock_guard<std::mutex> lk(ctx.state.mu);
        ctx.state.conversation.push_back({"system", ack, false});
        ctx.state.chat_follow_tail = true;
        if (ctx.state.is_waiting) {
            ctx.state.pending_queue.push_back(prompt);
            return;
        }
        ctx.state.is_waiting = true;
    }

    ctx.agent_loop.submit(prompt);
}

} // namespace

std::string build_acecode_md_skeleton(const fs::path& cwd) {
    std::error_code ec;
    bool claude = fs::exists(cwd / "CLAUDE.md", ec);
    bool agent = fs::exists(cwd / "AGENT.md", ec);
    return skeleton_body_internal(claude, agent);
}

std::string build_init_prompt(const fs::path& cwd) {
    std::error_code ec;
    bool acecode_exists = fs::exists(cwd / "ACECODE.md", ec);
    bool claude_exists = fs::exists(cwd / "CLAUDE.md", ec);
    bool agent_exists = fs::exists(cwd / "AGENT.md", ec);

    std::string out = build_init_prompt_body();
    if (acecode_exists) {
        out += improvement_suffix();
    } else if (claude_exists || agent_exists) {
        out += migration_suffix(claude_exists, agent_exists);
    }
    return out;
}

void register_init_command(CommandRegistry& registry) {
    registry.register_command(
        {"init",
         "Analyze this codebase and generate (or improve) ACECODE.md",
         cmd_init});
}

} // namespace acecode
