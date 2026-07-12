#pragma once

#include "../config/config.hpp"

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>

namespace acecode::upgrade {

bool apply_upgrade_server_override(AppConfig& config,
                                   const std::string& server,
                                   std::string* error = nullptr);

enum class UpgradePhase {
    Checking,
    Downloading,
    Verifying,
    Extracting,
    Installing,
    Complete,
};

struct UpgradeProgress {
    UpgradePhase phase = UpgradePhase::Checking;
    std::string current_version;
    std::string target_version;
    std::uintmax_t bytes_downloaded = 0;
    std::optional<std::uintmax_t> bytes_total;
    std::string backup_dir;
};

using UpgradeProgressCallback = std::function<void(const UpgradeProgress&)>;

const char* upgrade_phase_name(UpgradePhase phase);

int run_upgrade_command(const AppConfig& config,
                        const std::string& argv0,
                        const std::string& current_version,
                        std::ostream& out,
                        std::ostream& err,
                        bool force = false,
                        UpgradeProgressCallback progress = {});

} // namespace acecode::upgrade
