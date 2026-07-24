#include <gtest/gtest.h>

#include "desktop/open_request.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

class OpenRequestTempDir {
public:
    OpenRequestTempDir() {
        path_ = fs::temp_directory_path() /
                ("acecode_open_request_" +
                 std::to_string(std::random_device{}()));
        fs::create_directories(path_);
    }

    ~OpenRequestTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    fs::path request_path() const {
        return path_ / "desktop-open-request.json";
    }

private:
    fs::path path_;
};

acecode::desktop::DesktopOpenRequest request() {
    return {"N:\\Users\\shao\\workspace with spaces",
            "20260724-120000-session"};
}

} // namespace

TEST(DesktopOpenRequest, EncodesAndParsesExactArguments) {
    const auto encoded =
        acecode::desktop::desktop_open_request_arguments(request());
    EXPECT_EQ(encoded, (std::vector<std::string>{
        "--open-workspace",
        request().cwd,
        "--open-session",
        request().session_id,
    }));

    std::vector<std::string> argv{"acecode-desktop.exe", "--webapp"};
    argv.insert(argv.end(), encoded.begin(), encoded.end());
    auto parsed =
        acecode::desktop::parse_desktop_open_request_arguments(argv);
    ASSERT_TRUE(parsed.error.empty()) << parsed.error;
    ASSERT_TRUE(parsed.request.has_value());
    EXPECT_EQ(parsed.request->cwd, request().cwd);
    EXPECT_EQ(parsed.request->session_id, request().session_id);
}

TEST(DesktopOpenRequest, NoRequestIsAllowedButPartialOrDuplicateIsRejected) {
    auto absent = acecode::desktop::parse_desktop_open_request_arguments(
        {"acecode-desktop.exe", "--webapp"});
    EXPECT_FALSE(absent.request.has_value());
    EXPECT_TRUE(absent.error.empty());

    auto partial = acecode::desktop::parse_desktop_open_request_arguments(
        {"acecode-desktop.exe", "--open-workspace", "N:\\repo"});
    EXPECT_FALSE(partial.request.has_value());
    EXPECT_NE(partial.error.find("requires both"), std::string::npos);

    auto duplicate = acecode::desktop::parse_desktop_open_request_arguments(
        {"acecode-desktop.exe",
         "--open-workspace", "N:\\repo",
         "--open-workspace", "N:\\other",
         "--open-session", "sid"});
    EXPECT_FALSE(duplicate.request.has_value());
    EXPECT_NE(duplicate.error.find("duplicate"), std::string::npos);
}

TEST(DesktopOpenRequest, PendingRequestRoundTripsAndIsConsumedOnce) {
    OpenRequestTempDir temp;
    std::string error;
    const std::string path = temp.request_path().string();

    ASSERT_TRUE(acecode::desktop::publish_pending_desktop_open_request(
        request(), &error, path)) << error;
    ASSERT_TRUE(fs::exists(temp.request_path()));

    auto consumed =
        acecode::desktop::take_pending_desktop_open_request(&error, path);
    ASSERT_TRUE(consumed.has_value()) << error;
    EXPECT_EQ(consumed->cwd, request().cwd);
    EXPECT_EQ(consumed->session_id, request().session_id);
    EXPECT_FALSE(fs::exists(temp.request_path()));

    auto second =
        acecode::desktop::take_pending_desktop_open_request(&error, path);
    EXPECT_FALSE(second.has_value());
    EXPECT_TRUE(error.empty());
}

TEST(DesktopOpenRequest, InvalidPendingPayloadIsRemoved) {
    OpenRequestTempDir temp;
    {
        std::ofstream output(temp.request_path(), std::ios::binary);
        output << R"({"version":1,"cwd":"N:\\repo"})";
    }

    std::string error;
    auto consumed =
        acecode::desktop::take_pending_desktop_open_request(
            &error, temp.request_path().string());
    EXPECT_FALSE(consumed.has_value());
    EXPECT_NE(error.find("schema"), std::string::npos);
    EXPECT_FALSE(fs::exists(temp.request_path()));
}

TEST(DesktopOpenRequest, PendingRequestOlderThanCurrentDesktopIsDiscarded) {
    OpenRequestTempDir temp;
    std::string error;
    const std::string path = temp.request_path().string();

    ASSERT_TRUE(acecode::desktop::publish_pending_desktop_open_request(
        request(), &error, path)) << error;

    auto consumed =
        acecode::desktop::take_pending_desktop_open_request(
            &error,
            path,
            (std::numeric_limits<std::int64_t>::max)());
    EXPECT_FALSE(consumed.has_value());
    EXPECT_EQ(error, "ignored stale pending Desktop open request");
    EXPECT_FALSE(fs::exists(temp.request_path()));
}
