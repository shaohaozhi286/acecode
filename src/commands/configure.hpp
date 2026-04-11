#pragma once

#include "config/config.hpp"

namespace acecode {

// Run the interactive configuration wizard.
// Returns 0 on success, non-zero on error.
int run_configure(const AppConfig& current_config);

} // namespace acecode
