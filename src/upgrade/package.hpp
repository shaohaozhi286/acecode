#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace acecode::upgrade {

struct StagedPackage {
    std::filesystem::path content_root;
    std::filesystem::path executable_path;
};

bool is_safe_zip_entry_path(const std::string& entry, std::string* error = nullptr);
std::filesystem::path expected_executable_name_for_target(const std::string& target);
bool extract_zip_to_staging(const std::filesystem::path& zip_path,
                            const std::filesystem::path& staging_dir,
                            std::string* error);
std::optional<StagedPackage> validate_staged_package(const std::filesystem::path& staging_dir,
                                                     const std::string& target,
                                                     std::string* error);

} // namespace acecode::upgrade
