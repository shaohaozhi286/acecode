#include "manifest.hpp"

#include "../config/config.hpp"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
#include <sstream>

namespace acecode::upgrade {
namespace {

bool get_required_string(const nlohmann::json& j, const char* key,
                         std::string* out, std::string* error) {
    if (!j.contains(key) || !j[key].is_string()) {
        if (error) *error = std::string("manifest field '") + key + "' must be a string";
        return false;
    }
    *out = j[key].get<std::string>();
    return true;
}

bool has_scheme(const std::string& s) {
    const size_t colon = s.find(':');
    if (colon == std::string::npos) return false;
    if (colon == 0) return false;
    for (size_t i = 0; i < colon; ++i) {
        char c = s[i];
        if (!std::isalpha(static_cast<unsigned char>(c)) &&
            !std::isdigit(static_cast<unsigned char>(c)) &&
            c != '+' && c != '-' && c != '.') {
            return false;
        }
    }
    return true;
}

} // namespace

std::optional<UpdateManifest> parse_update_manifest(const std::string& text,
                                                    std::string* error) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text);
    } catch (const nlohmann::json::parse_error& e) {
        if (error) *error = std::string("invalid JSON: ") + e.what();
        return std::nullopt;
    }

    if (!j.is_object()) {
        if (error) *error = "manifest root must be an object";
        return std::nullopt;
    }
    if (!j.contains("schema_version") || !j["schema_version"].is_number_integer()) {
        if (error) *error = "manifest field 'schema_version' must be an integer";
        return std::nullopt;
    }

    UpdateManifest out;
    out.schema_version = j["schema_version"].get<int>();
    if (out.schema_version != 1) {
        if (error) *error = "unsupported manifest schema_version: " + std::to_string(out.schema_version);
        return std::nullopt;
    }
    if (!get_required_string(j, "latest", &out.latest, error)) return std::nullopt;
    if (!j.contains("releases") || !j["releases"].is_array()) {
        if (error) *error = "manifest field 'releases' must be an array";
        return std::nullopt;
    }

    for (const auto& rj : j["releases"]) {
        if (!rj.is_object()) {
            if (error) *error = "manifest release entries must be objects";
            return std::nullopt;
        }
        ReleaseInfo rel;
        if (!get_required_string(rj, "version", &rel.version, error)) return std::nullopt;
        if (rj.contains("published_at") && rj["published_at"].is_string()) {
            rel.published_at = rj["published_at"].get<std::string>();
        }
        if (rj.contains("notes") && rj["notes"].is_string()) {
            rel.notes = rj["notes"].get<std::string>();
        }
        if (!rj.contains("packages") || !rj["packages"].is_array()) {
            if (error) *error = "manifest release packages must be an array";
            return std::nullopt;
        }
        for (const auto& pj : rj["packages"]) {
            if (!pj.is_object()) {
                if (error) *error = "manifest package entries must be objects";
                return std::nullopt;
            }
            PackageInfo pkg;
            if (!get_required_string(pj, "target", &pkg.target, error)) return std::nullopt;
            if (!get_required_string(pj, "file", &pkg.file, error)) return std::nullopt;
            if (pj.contains("sha256") && pj["sha256"].is_string()) {
                pkg.sha256 = pj["sha256"].get<std::string>();
            }
            if (pj.contains("size")) {
                if (!pj["size"].is_number_unsigned()) {
                    if (error) *error = "manifest package size must be an unsigned integer";
                    return std::nullopt;
                }
                pkg.size = pj["size"].get<std::uintmax_t>();
            }
            rel.packages.push_back(std::move(pkg));
        }
        out.releases.push_back(std::move(rel));
    }
    return out;
}

bool is_valid_sha256_hex(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
        if (std::isalpha(static_cast<unsigned char>(c)) &&
            !std::islower(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

bool is_safe_package_file(const std::string& file, std::string* error) {
    if (file.empty()) {
        if (error) *error = "package file is empty";
        return false;
    }
    if (has_scheme(file)) {
        if (error) *error = "package file must be relative, not an absolute URL";
        return false;
    }
    if (file.front() == '/' || file.front() == '\\') {
        if (error) *error = "package file must not be absolute";
        return false;
    }
    if (file.find('\\') != std::string::npos) {
        if (error) *error = "package file must use URL forward slashes";
        return false;
    }
    if (file.find('?') != std::string::npos || file.find('#') != std::string::npos ||
        file.find('%') != std::string::npos) {
        if (error) *error = "package file must not contain query, fragment, or percent escapes";
        return false;
    }

    std::stringstream ss(file);
    std::string part;
    while (std::getline(ss, part, '/')) {
        if (part.empty() || part == "." || part == "..") {
            if (error) *error = "package file contains unsafe path segment";
            return false;
        }
    }
    return true;
}

std::string normalize_update_base_url(const std::string& base_url) {
    return acecode::normalize_upgrade_base_url(base_url);
}

std::string resolve_package_url(const std::string& base_url, const std::string& file) {
    return normalize_update_base_url(base_url) + file;
}

std::string manifest_url(const std::string& base_url) {
    return resolve_package_url(base_url, "aceupdate.json");
}

std::string current_target() {
#if defined(_WIN32)
    constexpr const char* os = "windows";
#elif defined(__APPLE__)
    constexpr const char* os = "macos";
#elif defined(__linux__)
    constexpr const char* os = "linux";
#else
    constexpr const char* os = "unknown";
#endif

#if defined(_M_X64) || defined(__x86_64__) || defined(__amd64__)
    constexpr const char* arch = "x64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    constexpr const char* arch = "arm64";
#elif defined(_M_IX86) || defined(__i386__)
    constexpr const char* arch = "x86";
#else
    constexpr const char* arch = "unknown";
#endif
    return std::string(os) + "-" + arch;
}

SelectionResult select_update_package(const UpdateManifest& manifest,
                                      const std::string& current_version,
                                      const std::string& target) {
    SelectionResult result;
    auto current = parse_sem_version(current_version);
    if (!current) {
        result.error = "running ACECode version is not semantic: " + current_version;
        return result;
    }

    std::optional<SelectedPackage> best;
    for (const auto& rel : manifest.releases) {
        auto parsed = parse_sem_version(rel.version);
        if (!parsed) {
            continue;
        }
        if (compare_sem_version(*parsed, *current) <= 0) {
            continue;
        }

        for (const auto& pkg : rel.packages) {
            if (pkg.target != target) continue;

            std::string err;
            if (!is_safe_package_file(pkg.file, &err)) {
                result.error = "invalid package file for " + rel.version + ": " + err;
                return result;
            }
            if (!is_valid_sha256_hex(pkg.sha256)) {
                result.error = "package " + pkg.file + " must provide a lowercase 64-hex sha256";
                return result;
            }

            if (!best || compare_sem_version(*parsed, best->parsed_version) > 0) {
                best = SelectedPackage{rel.version, *parsed, pkg};
            }
        }
    }

    if (!best) {
        result.status = SelectionStatus::UpToDate;
        return result;
    }
    result.status = SelectionStatus::UpdateAvailable;
    result.selected = std::move(best);
    return result;
}

} // namespace acecode::upgrade
