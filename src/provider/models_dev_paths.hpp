#pragma once

#include <optional>
#include <string>

namespace acecode {

// Locate the bundled models.dev seed directory containing api.json / MANIFEST.json /
// LICENSE. Search order matches the spec: ACECODE_MODELS_DEV_DIR env var first,
// then <executable_dir>/../share/acecode/models_dev (relative to argv0_dir), then
// /usr/share/acecode/models_dev. Returns nullopt when none of the candidates point
// at an existing directory (caller should log a single WARN and continue).
//
// argv0_dir lets unit tests inject a synthetic install layout. Production callers
// pass the runtime executable directory; pass an empty string to skip the
// install-relative candidate.
std::optional<std::string> find_models_dev_dir(const std::string& argv0_dir = "");

} // namespace acecode
