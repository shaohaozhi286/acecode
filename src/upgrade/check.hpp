#pragma once

#include "../config/config.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace acecode::upgrade {

enum class UpdateCheckStatus {
    UpdateAvailable,
    UpToDate,
    InvalidConfig,
    UnsupportedTarget,
    ManifestUnavailable,
    ManifestInvalid,
};

struct UpdateCheckResult {
    UpdateCheckStatus status = UpdateCheckStatus::ManifestUnavailable;
    std::string current_version;
    std::string latest_version;
    std::string target;
    std::string manifest_url;
    std::string package_file;
    std::string package_url;
    std::optional<std::uintmax_t> package_size;
    long http_status = 0;
    std::string error;

    bool update_available() const {
        return status == UpdateCheckStatus::UpdateAvailable;
    }
};

const char* update_check_status_name(UpdateCheckStatus status);

UpdateCheckResult check_for_update(const AppConfig& config,
                                   const std::string& current_version);

} // namespace acecode::upgrade
