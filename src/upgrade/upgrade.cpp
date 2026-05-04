#include "upgrade.hpp"

#include "apply.hpp"
#include "http.hpp"
#include "manifest.hpp"
#include "package.hpp"
#include "../network/proxy_resolver.hpp"
#include "../utils/sha256.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>

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

} // namespace

int run_upgrade_command(const AppConfig& config,
                        const std::string& argv0,
                        const std::string& current_version,
                        std::ostream& out,
                        std::ostream& err) {
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
    if (is_plain_http(base_url)) {
        err << "Warning: update service uses plain HTTP; executable downloads are not encrypted.\n";
    }

    out << "Checking for ACECode updates at " << manifest << "\n";
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

    SelectionResult selection = select_update_package(*parsed_manifest, current_version, target);
    if (selection.status == SelectionStatus::InvalidManifest) {
        err << "acecode upgrade: invalid update manifest: " << selection.error << "\n";
        return 1;
    }
    if (selection.status == SelectionStatus::UpToDate || !selection.selected) {
        out << "ACECode is already up to date (v" << current_version << ").\n";
        return 0;
    }

    const auto& selected = *selection.selected;
    const std::string package_url = resolve_package_url(base_url, selected.package.file);
    out << "Update available: v" << current_version << " -> v" << selected.version << "\n"
        << "Downloading " << package_url << "\n";

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

    DownloadResult dl = download_to_file(package_url, package_path, config.upgrade.timeout_ms);
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

    out << "Extracting package...\n";
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

    out << "Update package verified. ACECode will apply the update after this process exits.\n"
        << "Staging directory: " << staging_dir.string() << "\n"
        << "Backup directory: " << backup_dir.string() << "\n";
    return 0;
}

} // namespace acecode::upgrade
