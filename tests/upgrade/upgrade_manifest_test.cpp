#include "upgrade/manifest.hpp"
#include "utils/sha256.hpp"

#include <gtest/gtest.h>

using namespace acecode::upgrade;

TEST(UpgradeVersion, ParsesAndComparesSemanticVersions) {
    auto a = parse_sem_version("0.1.9");
    auto b = parse_sem_version("v0.1.10");
    auto rc = parse_sem_version("1.0.0-rc.1");
    auto final_v = parse_sem_version("1.0.0");

    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    ASSERT_TRUE(rc.has_value());
    ASSERT_TRUE(final_v.has_value());
    EXPECT_LT(compare_sem_version(*a, *b), 0);
    EXPECT_LT(compare_sem_version(*rc, *final_v), 0);
    EXPECT_FALSE(parse_sem_version("0.1").has_value());
}

TEST(UpgradeManifest, SelectsNewestCompatiblePackage) {
    std::string sha = acecode::sha256_hex("package");
    std::string text = R"({
      "schema_version": 1,
      "latest": "0.1.4",
      "releases": [
        {"version": "0.1.4", "packages": [
          {"target": "linux-x64", "file": "acecode-0.1.4-linux-x64.zip", "sha256": ")" + sha + R"("}
        ]},
        {"version": "0.1.3", "packages": [
          {"target": "windows-x64", "file": "acecode-0.1.3-windows-x64.zip", "sha256": ")" + sha + R"("}
        ]},
        {"version": "not-a-version", "packages": [
          {"target": "windows-x64", "file": "ignored.zip", "sha256": ")" + sha + R"("}
        ]}
      ]
    })";

    std::string err;
    auto manifest = parse_update_manifest(text, &err);
    ASSERT_TRUE(manifest.has_value()) << err;
    auto selected = select_update_package(*manifest, "0.1.2", "windows-x64");

    EXPECT_EQ(selected.status, SelectionStatus::UpdateAvailable);
    ASSERT_TRUE(selected.selected.has_value());
    EXPECT_EQ(selected.selected->version, "0.1.3");
    EXPECT_EQ(selected.selected->package.file, "acecode-0.1.3-windows-x64.zip");
}

TEST(UpgradeManifest, ReportsUpToDateWhenNoNewerCompatiblePackageExists) {
    std::string sha = acecode::sha256_hex("package");
    std::string text = R"({
      "schema_version": 1,
      "latest": "0.1.2",
      "releases": [
        {"version": "0.1.2", "packages": [
          {"target": "windows-x64", "file": "acecode-0.1.2-windows-x64.zip", "sha256": ")" + sha + R"("}
        ]}
      ]
    })";

    std::string err;
    auto manifest = parse_update_manifest(text, &err);
    ASSERT_TRUE(manifest.has_value()) << err;
    auto selected = select_update_package(*manifest, "0.1.2", "windows-x64");
    EXPECT_EQ(selected.status, SelectionStatus::UpToDate);
}

TEST(UpgradeManifest, RejectsInvalidManifestAndUnsafePackage) {
    std::string err;
    EXPECT_FALSE(parse_update_manifest(R"({"schema_version": 2, "latest": "1.0.0", "releases": []})", &err));
    EXPECT_NE(err.find("unsupported"), std::string::npos);

    std::string sha = acecode::sha256_hex("package");
    std::string text = R"({
      "schema_version": 1,
      "latest": "0.1.3",
      "releases": [
        {"version": "0.1.3", "packages": [
          {"target": "windows-x64", "file": "../bad.zip", "sha256": ")" + sha + R"("}
        ]}
      ]
    })";
    auto manifest = parse_update_manifest(text, &err);
    ASSERT_TRUE(manifest.has_value()) << err;
    auto selected = select_update_package(*manifest, "0.1.2", "windows-x64");
    EXPECT_EQ(selected.status, SelectionStatus::InvalidManifest);
    EXPECT_NE(selected.error.find("unsafe"), std::string::npos);
}

TEST(UpgradeManifest, ResolvesPackageUnderBaseUrl) {
    EXPECT_TRUE(is_safe_package_file("releases/acecode-0.1.3-windows-x64.zip"));
    EXPECT_FALSE(is_safe_package_file("https://evil.test/a.zip"));
    EXPECT_FALSE(is_safe_package_file("..%2Fencoded-traversal.zip"));
    EXPECT_FALSE(is_safe_package_file("../bad.zip"));
    EXPECT_EQ(manifest_url("https://updates.example.test/ace"),
              "https://updates.example.test/ace/aceupdate.json");
    EXPECT_EQ(resolve_package_url("https://updates.example.test/ace", "pkg.zip"),
              "https://updates.example.test/ace/pkg.zip");
}
