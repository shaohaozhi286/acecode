#pragma once

#include <cstddef>
#include <string>

namespace acecode::web {

inline constexpr std::size_t kProjectDirectoryNameMaxCodePoints = 60;

struct NormalizedProjectDirectoryName {
    std::string value;
    bool changed = false;
};

// Normalize a user-visible project name into one cross-platform path component.
// The Windows rules are intentionally used on every platform so projects can be
// moved between systems without renaming their root directory.
NormalizedProjectDirectoryName normalize_project_directory_name(
    const std::string& requested_name);

// User source trees live beside, not inside, ACECode's hash-indexed projects
// metadata directory: <data-dir>/projects -> <data-dir>/workspaces.
std::string default_project_parent_directory(
    const std::string& metadata_projects_dir);

enum class ProjectCreationError {
    None,
    NameRequired,
    ParentMustBeAbsolute,
    ParentNotFound,
    ParentNotDirectory,
    TargetExists,
    CreateFailed,
};

const char* project_creation_error_code(ProjectCreationError error);

struct ProjectCreationResult {
    ProjectCreationError error = ProjectCreationError::None;
    std::string message;
    std::string requested_name;
    std::string directory_name;
    std::string parent_dir;
    std::string project_dir;
    bool sanitized = false;

    bool ok() const { return error == ProjectCreationError::None; }
};

// Creates exactly one project child. A custom parent must already exist; the
// ACECode default parent is the only parent that is created on demand.
ProjectCreationResult create_project_directory(
    const std::string& requested_name,
    const std::string& requested_parent_dir,
    const std::string& metadata_projects_dir);

} // namespace acecode::web
