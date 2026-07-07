#pragma once

#include "../config/config.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace acecode {

struct OpencodeCommandInfo {
    std::string name;
    std::string description;
    std::string agent;
    std::string model;
    std::string variant;
    bool subtask = false;
    bool has_subtask = false;
    std::string template_text;
    std::filesystem::path source_path;
};

struct ParsedSlashCommand {
    std::string name;
    std::string args;
};

struct OpencodeCommandExpansionOptions {
    bool wrap_subtask_prompt = true;
};

std::vector<std::filesystem::path> opencode_command_config_roots(
    const AppConfig& config,
    const std::string& working_dir);

std::vector<OpencodeCommandInfo> load_opencode_commands_from_config_roots(
    const std::vector<std::filesystem::path>& config_roots);

std::vector<OpencodeCommandInfo> load_opencode_commands(
    const AppConfig& config,
    const std::string& working_dir);

std::optional<OpencodeCommandInfo> find_opencode_command(
    const AppConfig& config,
    const std::string& working_dir,
    const std::string& name);

std::optional<ParsedSlashCommand> parse_opencode_slash_command(
    const std::string& input);

std::string expand_opencode_command_template(
    const OpencodeCommandInfo& command,
    const std::string& raw_args);

std::string build_opencode_subtask_prompt(
    const OpencodeCommandInfo& command,
    const std::string& expanded_prompt);

std::string expand_opencode_command(
    const OpencodeCommandInfo& command,
    const std::string& raw_args,
    const OpencodeCommandExpansionOptions& options = {});

bool is_web_reserved_builtin_command(const std::string& name);

} // namespace acecode
