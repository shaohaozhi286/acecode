#pragma once

#include <functional>
#include <string>

namespace acecode::desktop {

struct OpenExternalUrlResult {
    bool ok = false;
    std::string error;
};

using ExternalUrlLauncher = std::function<bool(const std::string&, std::string&)>;

bool is_safe_external_url(const std::string& url);

OpenExternalUrlResult open_external_url(
    const std::string& url,
    ExternalUrlLauncher launcher = {});

} // namespace acecode::desktop
