#include <gtest/gtest.h>

#include "session/attachment_store.hpp"
#include "utils/utf8_path.hpp"

#include <chrono>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path unique_tmp_dir(const std::string& suffix) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto dir = fs::temp_directory_path() /
        ("acecode_attachment_store_" + suffix + "_" + std::to_string(now));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

} // namespace

TEST(AttachmentStore, SaveLoadAndReadImageAttachment) {
    auto dir = unique_tmp_dir("image");
    const std::string project_dir = acecode::path_to_utf8(dir);

    std::string error;
    auto saved = acecode::save_attachment(
        project_dir,
        "session1",
        "screen.png",
        "image/png",
        std::string{"png-bytes"},
        &error);

    ASSERT_TRUE(saved.has_value()) << error;
    EXPECT_EQ(saved->kind, "image");
    EXPECT_EQ(saved->mime_type, "image/png");
    EXPECT_NE(saved->blob_url.find("/api/sessions/session1/attachments/"), std::string::npos);

    auto loaded = acecode::load_attachment(project_dir, "session1", saved->id, &error);
    ASSERT_TRUE(loaded.has_value()) << error;
    EXPECT_EQ(loaded->id, saved->id);
    EXPECT_EQ(loaded->path, saved->path);

    auto bytes = acecode::read_attachment_bytes(*loaded, acecode::kMaxAttachmentBytes, &error);
    ASSERT_TRUE(bytes.has_value()) << error;
    EXPECT_EQ(*bytes, "png-bytes");

    fs::remove_all(dir);
}

TEST(AttachmentStore, RejectsInvalidAttachmentId) {
    auto dir = unique_tmp_dir("invalid");
    std::string error;
    auto loaded = acecode::load_attachment(
        acecode::path_to_utf8(dir),
        "session1",
        "../nope",
        &error);

    EXPECT_FALSE(loaded.has_value());
    EXPECT_EQ(error, "invalid attachment id");

    fs::remove_all(dir);
}

TEST(AttachmentStore, RejectsLargeImageWhenNormalizationFails) {
    auto dir = unique_tmp_dir("large_image");
    const std::string project_dir = acecode::path_to_utf8(dir);

    std::string error;
    auto saved = acecode::save_attachment(
        project_dir,
        "session1",
        "broken.png",
        "image/png",
        std::string(10u * 1024u * 1024u, 'x'),
        &error);

    EXPECT_FALSE(saved.has_value());
    EXPECT_NE(error.find("image normalization failed"), std::string::npos);

    fs::remove_all(dir);
}
