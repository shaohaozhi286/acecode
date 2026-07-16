#include <gtest/gtest.h>

#include "web/project_creation.hpp"

#include <filesystem>
#include <random>
#include <string>

namespace fs = std::filesystem;

namespace {

class ProjectCreationTempDir {
public:
    ProjectCreationTempDir() {
        path_ = fs::temp_directory_path() /
            ("acecode_project_creation_" + std::to_string(std::random_device{}()));
        fs::create_directories(path_);
    }

    ~ProjectCreationTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

} // namespace

TEST(ProjectCreationName, ReplacesCrossPlatformIncompatibleCharacters) {
    const auto result = acecode::web::normalize_project_directory_name(
        "  my/api:test?  ");
    EXPECT_EQ(result.value, "my-api-test-");
    EXPECT_TRUE(result.changed);
}
TEST(ProjectCreationName, AdjustsWindowsReservedDeviceNames) {
    EXPECT_EQ(acecode::web::normalize_project_directory_name("CON").value,
              "CON-project");
    EXPECT_EQ(acecode::web::normalize_project_directory_name("lpt9.txt").value,
              "lpt9.txt-project");
    EXPECT_EQ(acecode::web::normalize_project_directory_name("COM0").value,
              "COM0");
}

TEST(ProjectCreationName, TrimsTrailingDotsAndUsesFallback) {
    EXPECT_EQ(acecode::web::normalize_project_directory_name("demo...  ").value,
              "demo");
    EXPECT_EQ(acecode::web::normalize_project_directory_name("...").value,
              "project");
}

TEST(ProjectCreationName, TruncatesAtUnicodeCodePointBoundary) {
    std::string requested;
    for (int i = 0; i < 61; ++i) requested += "项";
    const auto result = acecode::web::normalize_project_directory_name(requested);
    std::string expected;
    for (int i = 0; i < 60; ++i) expected += "项";
    EXPECT_EQ(result.value, expected);
    EXPECT_TRUE(result.changed);
}

TEST(ProjectCreationDirectory, UsesSiblingWorkspaceRootByDefault) {
    ProjectCreationTempDir temp;
    const fs::path metadata = temp.path() / "projects";
    fs::create_directories(metadata);

    const auto result = acecode::web::create_project_directory(
        "alpha", "", metadata.string());

    ASSERT_TRUE(result.ok()) << result.message;
    EXPECT_TRUE(fs::is_directory(temp.path() / "workspaces" / "alpha"));
    EXPECT_EQ(fs::path(result.parent_dir).filename(), "workspaces");
    EXPECT_EQ(result.directory_name, "alpha");
}

TEST(ProjectCreationDirectory, CreatesUnderExistingCustomParent) {
    ProjectCreationTempDir temp;
    const fs::path metadata = temp.path() / "projects";
    const fs::path custom = temp.path() / "chosen";
    fs::create_directories(metadata);
    fs::create_directories(custom);

    const auto result = acecode::web::create_project_directory(
        "beta/project", custom.string(), metadata.string());

    ASSERT_TRUE(result.ok()) << result.message;
    EXPECT_TRUE(fs::is_directory(custom / "beta-project"));
    EXPECT_TRUE(result.sanitized);
}

TEST(ProjectCreationDirectory, RejectsRelativeOrMissingCustomParent) {
    ProjectCreationTempDir temp;
    const fs::path metadata = temp.path() / "projects";
    fs::create_directories(metadata);

    const auto relative = acecode::web::create_project_directory(
        "alpha", "relative-parent", metadata.string());
    EXPECT_EQ(relative.error,
              acecode::web::ProjectCreationError::ParentMustBeAbsolute);

    const auto missing = acecode::web::create_project_directory(
        "alpha", (temp.path() / "missing").string(), metadata.string());
    EXPECT_EQ(missing.error,
              acecode::web::ProjectCreationError::ParentNotFound);
}

TEST(ProjectCreationDirectory, RefusesExistingTargetWithoutAdoptingIt) {
    ProjectCreationTempDir temp;
    const fs::path metadata = temp.path() / "projects";
    const fs::path custom = temp.path() / "chosen";
    fs::create_directories(metadata);
    fs::create_directories(custom / "alpha");

    const auto result = acecode::web::create_project_directory(
        "alpha", custom.string(), metadata.string());

    EXPECT_EQ(result.error, acecode::web::ProjectCreationError::TargetExists);
    EXPECT_EQ(acecode::web::project_creation_error_code(result.error),
              std::string("PROJECT_ALREADY_EXISTS"));
}
