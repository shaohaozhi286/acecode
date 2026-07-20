#include "upgrade.hpp"

#include "apply.hpp"
#include "check.hpp"
#include "console.hpp"
#include "http.hpp"
#include "manifest.hpp"
#include "package.hpp"
#include "../network/proxy_resolver.hpp"
#include "../utils/sha256.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <utility>

namespace fs = std::filesystem;

namespace acecode::upgrade {
namespace {

fs::path unique_update_dir() {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() /
           ("acecode-update-" + std::to_string(current_process_id()) + "-" +
            std::to_string(ticks));
}

bool validate_upgrade_settings(const UpgradeConfig& cfg, std::string* error) {
    if (!acecode::is_valid_upgrade_base_url(cfg.base_url)) {
        if (error) *error = "upgrade.base_url must be a non-empty http or https URL";
        return false;
    }
    if (cfg.timeout_ms < 1000 || cfg.timeout_ms > 120000) {
        if (error) *error = "upgrade.timeout_ms must be between 1000 and 120000";
        return false;
    }
    return true;
}

bool is_plain_http(const std::string& url) {
    return normalize_update_base_url(url).rfind("http://", 0) == 0;
}

std::string format_bytes(std::uintmax_t bytes) {
    constexpr const char* units[] = {"B", "KB", "MB", "GB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < sizeof(units) / sizeof(units[0])) {
        value /= 1024.0;
        ++unit;
    }

    std::ostringstream ss;
    if (unit == 0) {
        ss << bytes << " " << units[unit];
    } else {
        ss << std::fixed << std::setprecision(value >= 10.0 ? 1 : 2)
           << value << " " << units[unit];
    }
    return ss.str();
}

class DownloadProgressBar {
public:
    DownloadProgressBar(std::ostream& out,
                        std::optional<std::uintmax_t> total,
                        bool interactive)
        : out_(out), total_(total), interactive_(interactive) {}

    void start() {
        if (interactive_) {
            render(0, true);
        }
    }

    void update(std::uintmax_t bytes) {
        if (interactive_) {
            render(bytes, false);
        }
    }

    void finish(std::uintmax_t bytes) {
        if (interactive_) {
            render(bytes, true);
            out_ << "\n";
        } else {
            out_ << "      Downloaded " << format_bytes(bytes);
            if (total_ && *total_ > 0) {
                out_ << " / " << format_bytes(*total_);
            }
            out_ << "\n";
        }
    }

private:
    void render(std::uintmax_t bytes, bool force) {
        const auto now = std::chrono::steady_clock::now();
        int percent = -1;
        if (total_ && *total_ > 0) {
            const auto capped = (std::min)(bytes, *total_);
            percent = static_cast<int>((capped * 100) / *total_);
        }

        if (!force && rendered_) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_render_);
            if (percent >= 0 && percent == last_percent_ && elapsed.count() < 80) {
                return;
            }
            if (percent < 0 && elapsed.count() < 160) {
                return;
            }
        }

        constexpr int width = 34;
        std::ostringstream line;
        line << "\r      ";
        if (total_ && *total_ > 0) {
            const auto capped = (std::min)(bytes, *total_);
            const int filled = static_cast<int>((capped * width) / *total_);
            line << "["
                 << std::string(static_cast<size_t>(filled), '#')
                 << std::string(static_cast<size_t>(width - filled), '-')
                 << "] " << std::setw(3) << percent << "%  "
                 << format_bytes(bytes) << " / " << format_bytes(*total_);
        } else {
            line << "Downloaded " << format_bytes(bytes);
        }

        const std::string text = line.str();
        out_ << text;
        if (last_line_len_ > text.size()) {
            out_ << std::string(last_line_len_ - text.size(), ' ');
        }
        out_ << std::flush;
        last_line_len_ = text.size();
        last_percent_ = percent;
        last_render_ = now;
        rendered_ = true;
    }

    std::ostream& out_;
    std::optional<std::uintmax_t> total_;
    bool interactive_ = false;
    bool rendered_ = false;
    int last_percent_ = -1;
    size_t last_line_len_ = 0;
    std::chrono::steady_clock::time_point last_render_{};
};

} // namespace

bool apply_upgrade_server_override(AppConfig& config,
                                   const std::string& server,
                                   std::string* error) {
    const std::string normalized = acecode::normalize_upgrade_base_url(server);
    if (!acecode::is_valid_upgrade_base_url(normalized)) {
        if (error) *error = "upgrade server must be a non-empty http or https URL";
        return false;
    }

    config.upgrade.base_url = normalized;
    return true;
}

const char* upgrade_phase_name(UpgradePhase phase) {
    switch (phase) {
        case UpgradePhase::Checking: return "checking";
        case UpgradePhase::Downloading: return "downloading";
        case UpgradePhase::Verifying: return "verifying";
        case UpgradePhase::Extracting: return "extracting";
        case UpgradePhase::Installing: return "installing";
        case UpgradePhase::Complete: return "complete";
    }
    return "checking";
}

const char* update_check_status_name(UpdateCheckStatus status) {
    switch (status) {
        case UpdateCheckStatus::UpdateAvailable: return "available";
        case UpdateCheckStatus::UpToDate: return "up_to_date";
        case UpdateCheckStatus::InvalidConfig: return "invalid_config";
        case UpdateCheckStatus::UnsupportedTarget: return "unsupported_target";
        case UpdateCheckStatus::ManifestUnavailable: return "manifest_unavailable";
        case UpdateCheckStatus::ManifestInvalid: return "manifest_invalid";
    }
    return "unknown";
}

UpdateCheckResult check_for_update(const AppConfig& config,
                                   const std::string& current_version) {
    UpdateCheckResult result;
    result.current_version = current_version;
    result.target = current_target();

    std::string cfg_error;
    if (!validate_upgrade_settings(config.upgrade, &cfg_error)) {
        result.status = UpdateCheckStatus::InvalidConfig;
        result.error = std::move(cfg_error);
        return result;
    }

    if (result.target.find("unknown") != std::string::npos) {
        result.status = UpdateCheckStatus::UnsupportedTarget;
        result.error = "unsupported platform target: " + result.target;
        return result;
    }

    const std::string base_url = normalize_update_base_url(config.upgrade.base_url);
    result.manifest_url = manifest_url(base_url);

    HttpTextResult manifest_resp = fetch_text(result.manifest_url,
                                              config.upgrade.timeout_ms);
    result.http_status = manifest_resp.status_code;
    if (!manifest_resp.error.empty()) {
        result.status = UpdateCheckStatus::ManifestUnavailable;
        result.error = manifest_resp.error;
        return result;
    }
    if (manifest_resp.status_code != 200) {
        result.status = UpdateCheckStatus::ManifestUnavailable;
        result.error = "manifest request returned HTTP " +
                       std::to_string(manifest_resp.status_code);
        return result;
    }

    std::string parse_error;
    auto parsed_manifest = parse_update_manifest(manifest_resp.body, &parse_error);
    if (!parsed_manifest) {
        result.status = UpdateCheckStatus::ManifestInvalid;
        result.error = parse_error;
        return result;
    }

    SelectionResult selection =
        select_update_package(*parsed_manifest, current_version, result.target);
    if (selection.status == SelectionStatus::InvalidManifest) {
        result.status = UpdateCheckStatus::ManifestInvalid;
        result.error = selection.error;
        return result;
    }
    result.releases.reserve(parsed_manifest->releases.size());
    for (const auto& release : parsed_manifest->releases) {
        result.releases.push_back(UpdateReleaseSummary{
            release.version,
            release.published_at,
            release.notes,
        });
    }
    if (selection.status == SelectionStatus::UpToDate || !selection.selected) {
        result.status = UpdateCheckStatus::UpToDate;
        result.latest_version = parsed_manifest->latest;
        return result;
    }

    result.status = UpdateCheckStatus::UpdateAvailable;
    result.latest_version = selection.selected->version;
    result.package_file = selection.selected->package.file;
    result.package_url = resolve_package_url(base_url, result.package_file);
    result.package_size = selection.selected->package.size;
    return result;
}

int run_upgrade_command(const AppConfig& config,
                        const std::string& argv0,
                        const std::string& current_version,
                        std::ostream& out,
                        std::ostream& err,
                        bool force,
                        UpgradeProgressCallback progress_callback) {
    UpgradeProgress progress_state;
    progress_state.current_version = current_version;
    auto publish_progress = [&]() {
        if (progress_callback) progress_callback(progress_state);
    };

    std::string cfg_error;
    if (!validate_upgrade_settings(config.upgrade, &cfg_error)) {
        err << "acecode upgrade: " << cfg_error << "\n";
        return 64;
    }

    const std::string target = current_target();
    if (target.find("unknown") != std::string::npos) {
        err << "acecode upgrade: unsupported platform target: " << target << "\n";
        return 1;
    }

    network::proxy_resolver().init(config.network);

    const std::string base_url = normalize_update_base_url(config.upgrade.base_url);
    const std::string manifest = manifest_url(base_url);

    out << styled(out, ConsoleStyle::Bold, "ACECode Update") << "\n"
        << "  Current : v" << current_version << "\n"
        << "  Platform: " << target << "\n"
        << "  Manifest: " << manifest << "\n";
    if (force) {
        out << "  Mode    : " << styled(out, ConsoleStyle::Yellow, "force") << "\n";
    }
    if (is_plain_http(base_url)) {
        out << "  " << styled(out, ConsoleStyle::Yellow, "Warning")
            << " : update service uses plain HTTP; downloads are not encrypted.\n";
    }

    progress_state.phase = UpgradePhase::Checking;
    publish_progress();
    out << "\n" << styled(out, ConsoleStyle::Cyan, "[1/4] Checking update manifest...") << "\n";
    HttpTextResult manifest_resp = fetch_text(manifest, config.upgrade.timeout_ms);
    if (!manifest_resp.error.empty()) {
        err << "acecode upgrade: failed to fetch manifest " << manifest
            << ": " << manifest_resp.error << "\n";
        return 1;
    }
    if (manifest_resp.status_code == 404) {
        err << "acecode upgrade: update manifest not found: " << manifest << "\n";
        return 1;
    }
    if (manifest_resp.status_code != 200) {
        err << "acecode upgrade: manifest request returned HTTP "
            << manifest_resp.status_code << ": " << manifest << "\n";
        return 1;
    }

    std::string parse_error;
    auto parsed_manifest = parse_update_manifest(manifest_resp.body, &parse_error);
    if (!parsed_manifest) {
        err << "acecode upgrade: invalid update manifest: " << parse_error << "\n";
        return 1;
    }

    SelectionResult selection = select_update_package(*parsed_manifest, current_version, target, force);
    if (selection.status == SelectionStatus::InvalidManifest) {
        err << "acecode upgrade: invalid update manifest: " << selection.error << "\n";
        return 1;
    }
    if (selection.status == SelectionStatus::UpToDate || !selection.selected) {
        if (force) {
            out << "\nNo compatible update package found for " << target << ".\n";
        } else {
            out << "\n" << styled(out, ConsoleStyle::Green, "ACECode is already up to date")
                << " (v" << current_version << ").\n";
        }
        return 0;
    }

    const auto& selected = *selection.selected;
    progress_state.target_version = selected.version;
    const std::string package_url = resolve_package_url(base_url, selected.package.file);
    out << "\n"
        << styled(out, ConsoleStyle::Cyan,
                  std::string("[2/4] ") + (force ? "Package selected" : "Update available"))
        << "\n"
        << "  Current : v" << current_version << "\n"
        << (force ? "  Target  : v" : "  Latest  : v") << selected.version << "\n"
        << "  Package : " << selected.package.file << "\n";
    if (force) {
        out << "  Force   : " << styled(out, ConsoleStyle::Yellow, "enabled") << "\n";
    }
    if (selected.package.size && *selected.package.size > 0) {
        out << "  Size    : " << format_bytes(*selected.package.size) << "\n";
    }

    fs::path temp_dir = unique_update_dir();
    fs::path package_path = temp_dir / selected.package.file;
    fs::path staging_dir = temp_dir / "staging";
    fs::path backup_dir = temp_dir / "backup";
    std::error_code ec;
    fs::create_directories(package_path.parent_path(), ec);
    if (ec) {
        err << "acecode upgrade: failed to create temp directory: " << ec.message() << "\n";
        return 1;
    }

    out << "\n" << styled(out, ConsoleStyle::Cyan, "[3/4] Downloading package") << "\n"
        << "  URL     : " << package_url << "\n";
    progress_state.phase = UpgradePhase::Downloading;
    progress_state.bytes_downloaded = 0;
    progress_state.bytes_total = selected.package.size;
    publish_progress();
    DownloadProgressBar progress_bar(out, selected.package.size, stream_is_interactive_terminal(out));
    progress_bar.start();
    DownloadResult dl = download_to_file(
        package_url, package_path, config.upgrade.timeout_ms,
        [&](const DownloadProgress& p) {
            progress_bar.update(p.bytes_written);
            progress_state.bytes_downloaded = p.bytes_written;
            publish_progress();
        });
    progress_bar.finish(dl.bytes_written);
    if (!dl.error.empty()) {
        err << "acecode upgrade: failed to download package: " << dl.error << "\n";
        return 1;
    }
    if (dl.status_code < 200 || dl.status_code >= 300) {
        err << "acecode upgrade: package request returned HTTP "
            << dl.status_code << ": " << package_url << "\n";
        return 1;
    }
    if (selected.package.size && dl.bytes_written != *selected.package.size) {
        err << "acecode upgrade: package size mismatch; expected "
            << *selected.package.size << " bytes, got " << dl.bytes_written << "\n";
        return 1;
    }

    progress_state.phase = UpgradePhase::Verifying;
    progress_state.bytes_downloaded = dl.bytes_written;
    publish_progress();
    out << "\n" << styled(out, ConsoleStyle::Cyan, "[4/4] Verifying and installing")
        << "\n";
    std::string sha_error;
    const std::string actual_sha = acecode::sha256_file_hex(package_path.string(), &sha_error);
    if (actual_sha.empty()) {
        err << "acecode upgrade: " << sha_error << "\n";
        return 1;
    }
    if (actual_sha != selected.package.sha256) {
        err << "acecode upgrade: checksum mismatch for " << selected.package.file << "\n"
            << "expected: " << selected.package.sha256 << "\n"
            << "actual:   " << actual_sha << "\n";
        return 1;
    }
    out << "  Checksum: " << styled(out, ConsoleStyle::Green, "OK") << "\n";

    progress_state.phase = UpgradePhase::Extracting;
    publish_progress();
    out << "  Extract : package\n";
    std::string extract_error;
    if (!extract_zip_to_staging(package_path, staging_dir, &extract_error)) {
        err << "acecode upgrade: invalid package: " << extract_error << "\n";
        return 1;
    }
    std::string stage_error;
    if (!validate_staged_package(staging_dir, target, &stage_error)) {
        err << "acecode upgrade: invalid package: " << stage_error << "\n";
        return 1;
    }
    out << "  Package : " << styled(out, ConsoleStyle::Green, "OK") << "\n";

    fs::path current_exe = current_executable_path(argv0);
    fs::path install_dir = current_exe.parent_path();
    progress_state.phase = UpgradePhase::Installing;
    publish_progress();
    out << "  Install : applying update\n";
    std::string apply_error;
    if (!apply_staged_update(staging_dir, install_dir, backup_dir,
                             target, &apply_error)) {
        err << "acecode upgrade: failed to apply update: " << apply_error << "\n"
            << "Backup directory: " << backup_dir.string() << "\n";
        return 1;
    }

    progress_state.phase = UpgradePhase::Complete;
    progress_state.backup_dir = backup_dir.string();
    publish_progress();
    out << "  Install : " << styled(out, ConsoleStyle::Green, "OK") << "\n\n"
        << styled(out, ConsoleStyle::Green, "ACECode update applied successfully.") << "\n"
        << "  Version : v" << selected.version << "\n"
        << "  Backup  : " << backup_dir.string() << "\n";
    return 0;
}

} // namespace acecode::upgrade
