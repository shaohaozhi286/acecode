#pragma once
// CLI command routing: version, upgrade, daemon, service, configure, etc.
// Extracted from main.cpp lines 1616-1802.

#include <optional>
#include <string>
#include <vector>

#include "cli/interactive_options.hpp"

namespace acecode { namespace cli {

// Forward declaration from main.cpp
int run_interactive_app(const InteractiveCliOptions& cli,
                        const std::string& argv0_dir);

// Configure process environment (Windows UTF-8 codepage, etc.)
void configure_process_environment();

// Extract argv tail into a vector of strings.
std::vector<std::string> argv_tail(int argc, char* argv[], int start);

// Get executable path from argv[0].
std::string executable_path_from_argv(int argc, char* argv[]);

// Check if an argument is a version request.
bool is_version_command_arg(const std::string& arg);

// Dispatch non-interactive commands (version, upgrade, daemon, service).
// Returns std::nullopt when the TUI interactive app should run.
std::optional<int> dispatch_non_tui_command(int argc, char* argv[]);

// Validate models.dev registry integrity.
int validate_models_registry_command(const std::string& argv0_dir);

// Run one-shot pre-TUI commands (configure, validate-models).
std::optional<int> run_pre_tui_command(const InteractiveCliOptions& cli,
                                       const std::string& argv0_dir);

}} // namespace acecode::cli
