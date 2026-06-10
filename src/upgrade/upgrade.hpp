#pragma once

#include "../config/config.hpp"

#include <iosfwd>
#include <string>

namespace acecode::upgrade {

bool apply_upgrade_server_override(AppConfig& config,
                                   const std::string& server,
                                   std::string* error = nullptr);

int run_upgrade_command(const AppConfig& config,
                        const std::string& argv0,
                        const std::string& current_version,
                        std::ostream& out,
                        std::ostream& err,
                        bool force = false);

} // namespace acecode::upgrade
