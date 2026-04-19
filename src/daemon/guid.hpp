#pragma once

#include "../utils/uuid.hpp"

#include <string>

namespace acecode::daemon {

// Generate a daemon GUID. Reuses the project's UUIDv4-ish generator so the
// format matches existing session IDs and logs render uniformly. Kept as a
// thin alias so callers signal intent ("this string identifies a supervised
// worker") at the call site.
inline std::string generate_daemon_guid() {
    return acecode::generate_uuid();
}

} // namespace acecode::daemon
