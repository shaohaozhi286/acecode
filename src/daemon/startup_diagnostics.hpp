#pragma once

#include <string>

namespace acecode::daemon {

// Writes an early daemon startup diagnostic to the active run directory.
// This is intentionally independent from Logger because startup hooks run
// before the daemon worker initializes rotating logs.
void append_startup_diagnostic(const std::string& message);

} // namespace acecode::daemon
