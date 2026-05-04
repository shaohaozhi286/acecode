#pragma once

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace acecode::upgrade {

struct ApplyOptions {
    unsigned long parent_pid = 0;
    std::filesystem::path staging_dir;
    std::filesystem::path install_dir;
    std::filesystem::path backup_dir;
};

unsigned long current_process_id();
std::filesystem::path current_executable_path(const std::string& argv0);
std::filesystem::path make_runner_path(unsigned long pid);
std::vector<std::string> build_apply_runner_args(const ApplyOptions& opts);
std::string quote_command_arg(const std::string& arg);
std::optional<ApplyOptions> parse_apply_runner_args(const std::vector<std::string>& args,
                                                   std::string* error);
bool prepare_update_runner(const std::filesystem::path& current_exe,
                           const std::filesystem::path& runner_path,
                           std::string* error);
bool launch_update_runner(const std::filesystem::path& runner_path,
                          const ApplyOptions& opts,
                          std::string* error);
bool apply_staged_update(const std::filesystem::path& staging_dir,
                         const std::filesystem::path& install_dir,
                         const std::filesystem::path& backup_dir,
                         const std::string& target,
                         std::string* error);
int run_apply_update_command(const std::vector<std::string>& args,
                             std::ostream& out,
                             std::ostream& err,
                             const std::string& target);

} // namespace acecode::upgrade
