#include "upgrade.hpp"

#include "apply.hpp"
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

int run_upgrade_command(const AppConfig& config,
                        const std::string& argv0,
                        const std::string& current_version,
                        std::ostream& out,
                        std::ostream& err,
                        bool force) {
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
    DownloadProgressBar progress(out, selected.package.size, stream_is_interactive_terminal(out));
    progress.start();
    DownloadResult dl = download_to_file(
        package_url, package_path, config.upgrade.timeout_ms,
        [&](const DownloadProgress& p) {
            progress.update(p.bytes_written);
        });
    progress.finish(dl.bytes_written);
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

    out << "\n" << styled(out, ConsoleStyle::Cyan, "[4/4] Verifying and preparing install")
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
    fs::path runner_path = make_runner_path(current_process_id());
    std::string runner_error;
    if (!prepare_update_runner(current_exe, runner_path, &runner_error)) {
        err << "acecode upgrade: " << runner_error << "\n";
        return 1;
    }

    ApplyOptions opts;
    opts.parent_pid = current_process_id();
    opts.staging_dir = staging_dir;
    opts.install_dir = install_dir;
    opts.backup_dir = backup_dir;

    if (!launch_update_runner(runner_path, opts, &runner_error)) {
        err << "acecode upgrade: " << runner_error << "\n";
        return 1;
    }

    out << "  Runner  : " << styled(out, ConsoleStyle::Green, "launched") << "\n\n"
        << styled(out, ConsoleStyle::Green, "Update package verified.") << "\n"
        << "ACECode will apply the update after this process exits.\n"
        << "  Staging: " << staging_dir.string() << "\n"
        << "  Backup : " << backup_dir.string() << "\n";
    return 0;
}

} // namespace acecode::upgrade
