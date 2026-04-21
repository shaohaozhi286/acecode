#include "init_command.hpp"

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

void cmd_init(CommandContext& ctx, const std::string& /*args*/) {
    fs::path target = fs::path(ctx.agent_loop.cwd()) / "ACECODE.md";
    std::error_code ec;
    if (fs::exists(target, ec)) {
        emit(ctx, "ACECODE.md already exists at " + target.generic_string() +
                  " — refusing to overwrite. Edit it by hand, or delete it first.");
        return;
    }

    bool claude_exists = fs::exists(fs::path(ctx.agent_loop.cwd()) / "CLAUDE.md", ec);
    bool agent_exists = fs::exists(fs::path(ctx.agent_loop.cwd()) / "AGENT.md", ec);

    std::ofstream ofs(target, std::ios::binary);
    if (!ofs.is_open()) {
        emit(ctx, "Failed to open " + target.generic_string() + " for writing.");
        return;
    }
    ofs << skeleton_body_internal(claude_exists, agent_exists);
    emit(ctx, "Created " + target.generic_string() +
              ". Edit it to describe your project; acecode will read it on the next turn.");
}

} // namespace

std::string build_acecode_md_skeleton(const fs::path& cwd) {
    std::error_code ec;
    bool claude = fs::exists(cwd / "CLAUDE.md", ec);
    bool agent = fs::exists(cwd / "AGENT.md", ec);
    return skeleton_body_internal(claude, agent);
}

void register_init_command(CommandRegistry& registry) {
    registry.register_command(
        {"init", "Generate an ACECODE.md skeleton in the current directory", cmd_init});
}

} // namespace acecode
