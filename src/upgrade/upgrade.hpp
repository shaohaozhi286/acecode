#pragma once

#include "../config/config.hpp"

#include <iosfwd>
#include <string>

namespace acecode::upgrade {

int run_upgrade_command(const AppConfig& config,
                        const std::string& argv0,
                        const std::string& current_version,
                        std::ostream& out,
                        std::ostream& err);

} // namespace acecode::upgrade
