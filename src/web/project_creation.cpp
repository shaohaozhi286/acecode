#include "project_creation.hpp"

#include "../utils/utf8_path.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace acecode::web {

namespace {

bool is_ascii_space(unsigned char c) {
    return c < 0x80 && std::isspace(c) != 0;
}

std::string trim_ascii_space(const std::string& value) {
    std::size_t first = 0;
    while (first < value.size() && is_ascii_space(
        static_cast<unsigned char>(value[first]))) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first && is_ascii_space(
        static_cast<unsigned char>(value[last - 1]))) {
        --last;
    }
    return value.substr(first, last - first);
}

void trim_windows_trailing_characters(std::string& value) {
    while (!value.empty() && (value.back() == ' ' || value.back() == '.')) {
        value.pop_back();
    }
}

bool is_incompatible_project_character(unsigned char c) {
    if (c < 32 || c == 127) return true;
    switch (c) {
        case '<':
        case '>':
        case ':':
        case '"':
        case '/':
        case '\\':
        case '|':
        case '?':
        case '*':
            return true;
        default:
            return false;
    }
}

std::size_t utf8_sequence_length(unsigned char lead) {
    if ((lead & 0x80u) == 0) return 1;
    if ((lead & 0xE0u) == 0xC0u) return 2;
    if ((lead & 0xF0u) == 0xE0u) return 3;
    if ((lead & 0xF8u) == 0xF0u) return 4;
    return 1;
}

std::string truncate_utf8_code_points(const std::string& value,
                                      std::size_t max_code_points) {
    std::size_t offset = 0;
    std::size_t count = 0;
    while (offset < value.size() && count < max_code_points) {
        std::size_t width = utf8_sequence_length(
            static_cast<unsigned char>(value[offset]));
        if (offset + width > value.size()) width = 1;
        offset += width;
        ++count;
    }
    return value.substr(0, offset);
}

std::string ascii_upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return c < 0x80
            ? static_cast<char>(std::toupper(c))
            : static_cast<char>(c);
    });
    return value;
}

bool is_windows_reserved_device_name(const std::string& value) {
    const auto dot = value.find('.');
    std::string base = value.substr(0, dot);
    trim_windows_trailing_characters(base);
    base = ascii_upper(base);
    if (base == "CON" || base == "PRN" || base == "AUX" ||
        base == "NUL" || base == "CLOCK$") {
        return true;
    }
    if (base.size() == 4 && base[3] >= '1' && base[3] <= '9') {
        return base.rfind("COM", 0) == 0 || base.rfind("LPT", 0) == 0;
    }
    return false;
}

std::string normalized_path_text(const fs::path& path) {
    return path_to_utf8_generic(path.lexically_normal());
}

ProjectCreationResult failed_result(ProjectCreationError error,
                                    const std::string& message,
                                    const std::string& requested_name,
                                    const NormalizedProjectDirectoryName& normalized,
                                    const std::string& parent_dir = {},
                                    const std::string& project_dir = {}) {
    ProjectCreationResult result;
    result.error = error;
    result.message = message;
    result.requested_name = requested_name;
    result.directory_name = normalized.value;
    result.parent_dir = parent_dir;
    result.project_dir = project_dir;
    result.sanitized = normalized.changed;
    return result;
}

} // namespace

NormalizedProjectDirectoryName normalize_project_directory_name(
    const std::string& requested_name) {
    std::string source = trim_ascii_space(requested_name);
    std::string normalized;
    normalized.reserve(source.size());

    for (unsigned char c : source) {
        if (is_incompatible_project_character(c)) {
            if (normalized.empty() || normalized.back() != '-') {
                normalized.push_back('-');
            }
            continue;
        }
        normalized.push_back(static_cast<char>(c));
    }

    normalized = trim_ascii_space(normalized);
    trim_windows_trailing_characters(normalized);
    if (normalized.empty() || normalized == "." || normalized == "..") {
        normalized = "project";
    }

    normalized = truncate_utf8_code_points(
        normalized, kProjectDirectoryNameMaxCodePoints);
    trim_windows_trailing_characters(normalized);
    if (normalized.empty()) normalized = "project";

    if (is_windows_reserved_device_name(normalized)) {
        normalized += "-project";
    }

    return {normalized, normalized != requested_name};
}

std::string default_project_parent_directory(
    const std::string& metadata_projects_dir) {
    fs::path metadata = path_from_utf8(metadata_projects_dir).lexically_normal();
    fs::path data_dir = metadata.parent_path();
    if (data_dir.empty()) data_dir = metadata;
    return normalized_path_text(data_dir / "workspaces");
}

const char* project_creation_error_code(ProjectCreationError error) {
    switch (error) {
        case ProjectCreationError::None: return "";
        case ProjectCreationError::NameRequired: return "PROJECT_NAME_REQUIRED";
        case ProjectCreationError::ParentMustBeAbsolute: return "PROJECT_PARENT_ABSOLUTE_REQUIRED";
        case ProjectCreationError::ParentNotFound: return "PROJECT_PARENT_NOT_FOUND";
        case ProjectCreationError::ParentNotDirectory: return "PROJECT_PARENT_NOT_DIRECTORY";
        case ProjectCreationError::TargetExists: return "PROJECT_ALREADY_EXISTS";
        case ProjectCreationError::CreateFailed: return "PROJECT_CREATE_FAILED";
    }
    return "PROJECT_CREATE_FAILED";
}

ProjectCreationResult create_project_directory(
    const std::string& requested_name,
    const std::string& requested_parent_dir,
    const std::string& metadata_projects_dir) {
    const auto normalized = normalize_project_directory_name(requested_name);
    if (trim_ascii_space(requested_name).empty()) {
        return failed_result(ProjectCreationError::NameRequired,
                             "请输入项目名称", requested_name, normalized);
    }

    const std::string default_parent =
        default_project_parent_directory(metadata_projects_dir);
    const bool parent_omitted = trim_ascii_space(requested_parent_dir).empty();
    const bool parent_matches_default = !parent_omitted &&
        path_from_utf8(requested_parent_dir).lexically_normal() ==
            path_from_utf8(default_parent).lexically_normal();
    const bool uses_default_parent = parent_omitted || parent_matches_default;
    const std::string parent_text = uses_default_parent
        ? default_parent
        : requested_parent_dir;
    fs::path parent = path_from_utf8(parent_text).lexically_normal();
    const std::string normalized_parent = normalized_path_text(parent);

    if (!parent.is_absolute()) {
        return failed_result(ProjectCreationError::ParentMustBeAbsolute,
                             "项目位置必须是绝对路径", requested_name,
                             normalized, normalized_parent);
    }

    std::error_code ec;
    if (uses_default_parent) {
        fs::create_directories(parent, ec);
        if (ec) {
            return failed_result(ProjectCreationError::CreateFailed,
                                 "无法创建 ACECode 默认项目目录：" + ec.message(),
                                 requested_name, normalized, normalized_parent);
        }
    }

    ec.clear();
    if (!fs::exists(parent, ec) || ec) {
        return failed_result(ProjectCreationError::ParentNotFound,
                             "所选项目位置不存在", requested_name,
                             normalized, normalized_parent);
    }
    ec.clear();
    if (!fs::is_directory(parent, ec) || ec) {
        return failed_result(ProjectCreationError::ParentNotDirectory,
                             "所选项目位置不是目录", requested_name,
                             normalized, normalized_parent);
    }

    ec.clear();
    fs::path canonical_parent = fs::weakly_canonical(parent, ec);
    if (!ec && !canonical_parent.empty()) parent = canonical_parent;
    const std::string final_parent = normalized_path_text(parent);
    const fs::path target = parent / path_from_utf8(normalized.value);
    const std::string target_text = normalized_path_text(target);

    ec.clear();
    const bool target_exists = fs::exists(target, ec);
    if (target_exists && !ec) {
        return failed_result(ProjectCreationError::TargetExists,
                             "目标目录已存在：" + target_text,
                             requested_name, normalized, final_parent, target_text);
    }

    ec.clear();
    const bool created = fs::create_directory(target, ec);
    if (!created) {
        if (!ec || ec == std::errc::file_exists) {
            return failed_result(ProjectCreationError::TargetExists,
                                 "目标目录已存在：" + target_text,
                                 requested_name, normalized, final_parent, target_text);
        }
        return failed_result(ProjectCreationError::CreateFailed,
                             "创建项目目录失败：" + ec.message(),
                             requested_name, normalized, final_parent, target_text);
    }

    ProjectCreationResult result;
    result.requested_name = requested_name;
    result.directory_name = normalized.value;
    result.parent_dir = final_parent;
    result.project_dir = target_text;
    result.sanitized = normalized.changed;
    return result;
}

} // namespace acecode::web
