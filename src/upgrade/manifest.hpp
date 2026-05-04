#pragma once

#include "version.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace acecode::upgrade {

struct PackageInfo {
    std::string target;
    std::string file;
    std::string sha256;
    std::optional<std::uintmax_t> size;
};

struct ReleaseInfo {
    std::string version;
    std::string published_at;
    std::string notes;
    std::vector<PackageInfo> packages;
};

struct UpdateManifest {
    int schema_version = 0;
    std::string latest;
    std::vector<ReleaseInfo> releases;
};

enum class SelectionStatus {
    UpdateAvailable,
    UpToDate,
    InvalidManifest,
};

struct SelectedPackage {
    std::string version;
    SemVersion parsed_version;
    PackageInfo package;
};

struct SelectionResult {
    SelectionStatus status = SelectionStatus::InvalidManifest;
    std::optional<SelectedPackage> selected;
    std::string error;
};

std::optional<UpdateManifest> parse_update_manifest(const std::string& text,
                                                    std::string* error);
bool is_valid_sha256_hex(const std::string& s);
bool is_safe_package_file(const std::string& file, std::string* error = nullptr);
std::string normalize_update_base_url(const std::string& base_url);
std::string resolve_package_url(const std::string& base_url, const std::string& file);
std::string manifest_url(const std::string& base_url);
std::string current_target();
SelectionResult select_update_package(const UpdateManifest& manifest,
                                      const std::string& current_version,
                                      const std::string& target);

} // namespace acecode::upgrade
