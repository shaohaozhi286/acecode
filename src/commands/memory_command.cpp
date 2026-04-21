#include "memory_command.hpp"

#include "../memory/memory_paths.hpp"
#include "../memory/memory_registry.hpp"
#include "../memory/memory_types.hpp"
#include "../utils/logger.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <sstream>

#ifdef _WIN32
#  include <cstdlib>
#else
#  include <sys/wait.h>
#endif

namespace fs = std::filesystem;

namespace acecode {

namespace {

struct ParsedCommand {
    std::string sub;
    std::string rest;
    std::string type_filter; // for list --type=<t>
};

// Trim leading/trailing whitespace.
std::string strip(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

ParsedCommand parse_args(const std::string& args) {
    ParsedCommand out;
    std::string trimmed = strip(args);
    if (trimmed.empty()) {
        out.sub = "list";
        return out;
    }
    std::size_t sp = trimmed.find_first_of(" \t");
    if (sp == std::string::npos) {
        out.sub = trimmed;
    } else {
        out.sub = trimmed.substr(0, sp);
        out.rest = strip(trimmed.substr(sp + 1));
    }

    // Pull out --type=<t> when present in rest.
    const std::string tag = "--type=";
    std::size_t tpos = out.rest.find(tag);
    if (tpos != std::string::npos) {
        std::size_t end = out.rest.find_first_of(" \t", tpos + tag.size());
        out.type_filter = out.rest.substr(tpos + tag.size(),
                                          end == std::string::npos
                                              ? std::string::npos
                                              : end - (tpos + tag.size()));
        // Drop the flag from rest so positional lookups still work.
        std::string before = strip(out.rest.substr(0, tpos));
        std::string after = end == std::string::npos ? std::string{} : strip(out.rest.substr(end + 1));
        if (!before.empty() && !after.empty()) out.rest = before + " " + after;
        else out.rest = before + after;
    }
    return out;
}

void emit(CommandContext& ctx, const std::string& msg) {
    std::lock_guard<std::mutex> lk(ctx.state.mu);
    ctx.state.conversation.push_back({"system", msg, false});
    ctx.state.chat_follow_tail = true;
}

void handle_list(CommandContext& ctx, const ParsedCommand& pc) {
    if (!ctx.memory) { emit(ctx, "Memory is not available in this session."); return; }

    std::optional<MemoryType> filter;
    if (!pc.type_filter.empty()) {
        filter = parse_memory_type(pc.type_filter);
        if (!filter) { emit(ctx, "Invalid type filter: " + pc.type_filter); return; }
    }

    auto entries = ctx.memory->list(filter);
    std::ostringstream oss;
    oss << "Memory directory: " << get_memory_dir().generic_string() << "\n";
    if (entries.empty()) {
        oss << "No memory entries"
            << (filter.has_value() ? " of type " + pc.type_filter : std::string{})
            << " yet.";
    } else {
        oss << entries.size() << " entr" << (entries.size() == 1 ? "y" : "ies") << ":";
        for (const auto& e : entries) {
            oss << "\n  [" << memory_type_to_string(e.type) << "] "
                << e.name << " — " << e.description;
        }
    }
    emit(ctx, oss.str());
}

void handle_view(CommandContext& ctx, const ParsedCommand& pc) {
    if (!ctx.memory) { emit(ctx, "Memory is not available in this session."); return; }
    std::string name = strip(pc.rest);
    if (name.empty()) { emit(ctx, "Usage: /memory view <name>"); return; }
    auto found = ctx.memory->find(name);
    if (!found) { emit(ctx, "No memory entry named '" + name + "'."); return; }

    std::ostringstream oss;
    oss << "[" << memory_type_to_string(found->type) << "] " << found->name << "\n"
        << "Description: " << found->description << "\n"
        << "Path: " << found->path.generic_string() << "\n\n"
        << found->body;
    emit(ctx, oss.str());
}

int run_editor(const fs::path& target) {
    std::string editor;
    const char* env = std::getenv("EDITOR");
    if (env && *env) editor = env;
#ifdef _WIN32
    if (editor.empty()) editor = "notepad";
#else
    if (editor.empty()) editor = "vim";
#endif
    std::string cmd = editor + " \"" + target.string() + "\"";
    return std::system(cmd.c_str());
}

void handle_edit(CommandContext& ctx, const ParsedCommand& pc) {
    if (!ctx.memory) { emit(ctx, "Memory is not available in this session."); return; }
    std::string name = strip(pc.rest);
    if (name.empty()) { emit(ctx, "Usage: /memory edit <name>"); return; }
    std::string err = validate_memory_name(name);
    if (!err.empty()) { emit(ctx, err); return; }

    fs::path target = resolve_memory_entry_path(name);
    if (!fs::exists(target)) {
        emit(ctx, "No memory entry named '" + name + "' (looked for " +
             target.generic_string() + ").");
        return;
    }

    int rc = run_editor(target);
    (void)rc; // rc varies per editor; reload regardless and report path.
    ctx.memory->reload();
    emit(ctx, "Reloaded memory from disk (edited " + target.generic_string() + ").");
}

void handle_forget(CommandContext& ctx, const ParsedCommand& pc) {
    if (!ctx.memory) { emit(ctx, "Memory is not available in this session."); return; }
    std::string name = strip(pc.rest);
    if (name.empty()) { emit(ctx, "Usage: /memory forget <name>"); return; }
    std::string err;
    if (!ctx.memory->remove(name, err)) {
        emit(ctx, "Failed to forget '" + name + "': " + err);
        return;
    }
    emit(ctx, "Forgot '" + name + "'.");
}

void handle_reload(CommandContext& ctx) {
    if (!ctx.memory) { emit(ctx, "Memory is not available in this session."); return; }
    ctx.memory->reload();
    emit(ctx, "Reloaded " + std::to_string(ctx.memory->size()) + " memory entr" +
             (ctx.memory->size() == 1 ? "y." : "ies."));
}

void cmd_memory(CommandContext& ctx, const std::string& args) {
    ParsedCommand pc = parse_args(args);
    if (pc.sub == "list") return handle_list(ctx, pc);
    if (pc.sub == "view") return handle_view(ctx, pc);
    if (pc.sub == "edit") return handle_edit(ctx, pc);
    if (pc.sub == "forget") return handle_forget(ctx, pc);
    if (pc.sub == "reload") return handle_reload(ctx);

    emit(ctx,
         "Usage:\n"
         "  /memory list [--type=<t>]\n"
         "  /memory view <name>\n"
         "  /memory edit <name>\n"
         "  /memory forget <name>\n"
         "  /memory reload");
}

} // namespace

void register_memory_command(CommandRegistry& registry) {
    registry.register_command(
        {"memory", "List, view, edit, forget, or reload persistent user memory", cmd_memory});
}

} // namespace acecode
