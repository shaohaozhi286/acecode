#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace acecode::daemon {

using pid_t_compat = int64_t;

// Return the current process id.
pid_t_compat current_pid();

// Spawn `argv[0]` (executable absolute path) with `argv[1..]` as a fully
// detached background process: no controlling terminal, no console window
// attached, parent returns immediately with the child pid.
//
// Returns the child pid on success, 0 on failure. Logs the underlying error
// via the project logger.
pid_t_compat spawn_detached(const std::vector<std::string>& argv);

// Best-effort liveness check: true if a process with this id currently exists
// (regardless of ownership / user). False if no such process or if the check
// itself failed.
bool is_pid_alive(pid_t_compat pid);

// Request a graceful termination of `pid`. POSIX sends SIGTERM. Windows tries
// CTRL_BREAK_EVENT first, then falls back to TerminateProcess. Blocks for up
// to `wait_ms` waiting for the process to exit. Returns true if the process
// is gone afterwards.
bool terminate_pid(pid_t_compat pid, int wait_ms = 10000);

// Path to the currently-running executable, suitable for re-launching the
// same binary with different argv. Returns empty string on failure.
std::string current_executable_path();

} // namespace acecode::daemon
