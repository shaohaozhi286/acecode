#include "feedback/feedback_upload.hpp"

#include "utils/utf8_path.hpp"

#include <gtest/gtest.h>
#include <httplib.h>
#include <zip.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path root;

    explicit TempDir(const std::string& name) {
        root = fs::temp_directory_path() /
               (name + "_" + std::to_string(std::chrono::steady_clock::now()
                                                 .time_since_epoch()
                                                 .count()));
        fs::remove_all(root);
        fs::create_directories(root);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream ofs(path, std::ios::binary);
    ofs << text;
}

std::string read_zip_entry(const fs::path& zip_path, const std::string& entry) {
    int err = 0;
    zip_t* archive = zip_open(acecode::path_to_utf8(zip_path).c_str(), ZIP_RDONLY, &err);
    if (!archive) return {};
    zip_int64_t index = zip_name_locate(archive, entry.c_str(), ZIP_FL_ENC_UTF_8);
    if (index < 0) {
        zip_close(archive);
        return {};
    }
    zip_stat_t st;
    zip_stat_init(&st);
    if (zip_stat_index(archive, static_cast<zip_uint64_t>(index), 0, &st) != 0) {
        zip_close(archive);
        return {};
    }
    zip_file_t* file = zip_fopen_index(archive, static_cast<zip_uint64_t>(index), 0);
    if (!file) {
        zip_close(archive);
        return {};
    }
    std::string out(static_cast<std::size_t>(st.size), '\0');
    zip_int64_t read = zip_fread(file, out.data(), out.size());
    zip_fclose(file);
    zip_close(archive);
    if (read < 0) return {};
    out.resize(static_cast<std::size_t>(read));
    return out;
}

bool zip_entry_exists(const fs::path& zip_path, const std::string& entry) {
    int err = 0;
    zip_t* archive = zip_open(acecode::path_to_utf8(zip_path).c_str(), ZIP_RDONLY, &err);
    if (!archive) return false;
    const bool exists =
        zip_name_locate(archive, entry.c_str(), ZIP_FL_ENC_UTF_8) >= 0;
    zip_close(archive);
    return exists;
}

struct LocalHttpServer {
    httplib::Server svr;
    int port = 0;
    std::thread th;

    explicit LocalHttpServer(std::function<void(httplib::Server&)> setup) {
        setup(svr);
        port = svr.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { svr.listen_after_bind(); });
        for (int i = 0; i < 50 && !svr.is_running(); ++i) {
            std::this_thread::sleep_for(10ms);
        }
    }

    ~LocalHttpServer() {
        svr.stop();
        if (th.joinable()) th.join();
    }

    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port) + "/";
    }
};

} // namespace

TEST(FeedbackUpload, BuildPackageIncludesSessionMetadataAndLogTail) {
    TempDir tmp("acecode_feedback_package");
    const fs::path session = tmp.root / "session.jsonl";
    const fs::path log = tmp.root / "acecode.log";
    write_text(session, "{\"role\":\"user\",\"content\":\"hello\"}\n");
    write_text(log, "0123456789abcdefghijklmnopqrstuvwxyz");

    acecode::feedback::FeedbackPackageRequest req;
    req.feedback_text = "third turn froze";
    req.session_id = "20260618-010203-abcd";
    req.session_jsonl_path = session;
    req.log_path = log;
    req.output_dir = tmp.root / "out";
    req.created_at = "2026-06-18T01:02:03Z";
    req.acecode_version = "test-version";
    req.platform = "windows-x64";
    req.computer_name = "QA BOX";
    req.login_name = "alice@example";
    req.max_log_bytes = 16;

    auto result = acecode::feedback::build_feedback_package(req);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(fs::is_regular_file(result.package_path));
    EXPECT_EQ(result.package_filename,
              "acecode-feedback-20260618-010203-abcd-20260618-010203-windows-x64-QA-BOX-alice-example.zip");
    EXPECT_TRUE(result.log_included);
    EXPECT_EQ(result.log_tail_bytes, 16u);

    EXPECT_EQ(read_zip_entry(result.package_path,
                             "session/20260618-010203-abcd.jsonl"),
              "{\"role\":\"user\",\"content\":\"hello\"}\n");
    EXPECT_EQ(read_zip_entry(result.package_path, "logs/acecode.log.tail.txt"),
              "klmnopqrstuvwxyz");

    auto metadata = nlohmann::json::parse(
        read_zip_entry(result.package_path, "feedback.json"));
    EXPECT_EQ(metadata["feedback_text"], "third turn froze");
    EXPECT_EQ(metadata["source"], "tui");
    EXPECT_EQ(metadata["session_id"], "20260618-010203-abcd");
    EXPECT_EQ(metadata["selected_session_id"], "20260618-010203-abcd");
    EXPECT_EQ(metadata["acecode_version"], "test-version");
    EXPECT_EQ(metadata["platform"], "windows-x64");
    EXPECT_EQ(metadata["computer_name"], "QA BOX");
    EXPECT_EQ(metadata["login_name"], "alice@example");
    EXPECT_TRUE(metadata["log_available"].get<bool>());
    EXPECT_EQ(metadata["log_tail_bytes"], 16);
}

TEST(FeedbackUpload, BuildDesktopPackageCanBeLogOnly) {
    TempDir tmp("acecode_feedback_desktop_log_only");
    const fs::path log = tmp.root / "desktop-2026-06-18.log";
    write_text(log, "desktop log line 1\ndesktop log line 2\n");

    acecode::feedback::FeedbackPackageRequest req;
    req.source = "desktop";
    req.feedback_text = "settings pane froze";
    req.log_path = log;
    req.log_entry_name = "logs/desktop.log.tail.txt";
    req.output_dir = tmp.root / "out";
    req.created_at = "2026-06-18T01:02:03Z";
    req.acecode_version = "test-version";
    req.platform = "windows-x64";
    req.computer_name = "Desk";
    req.login_name = "tester";
    req.max_log_bytes = 19;

    auto result = acecode::feedback::build_feedback_package(req);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(fs::is_regular_file(result.package_path));
    EXPECT_EQ(result.package_filename,
              "acecode-feedback-desktop-20260618-010203-windows-x64-Desk-tester.zip");
    EXPECT_TRUE(result.log_included);
    EXPECT_EQ(read_zip_entry(result.package_path, "logs/desktop.log.tail.txt"),
              "desktop log line 2\n");
    EXPECT_FALSE(zip_entry_exists(result.package_path, "session/session.jsonl"));

    auto metadata = nlohmann::json::parse(
        read_zip_entry(result.package_path, "feedback.json"));
    EXPECT_EQ(metadata["source"], "desktop");
    EXPECT_TRUE(metadata["session_id"].is_null());
    EXPECT_TRUE(metadata["selected_session_id"].is_null());
    EXPECT_EQ(metadata["included_files"].size(), 2u);
    EXPECT_EQ(metadata["included_files"][0], "logs/desktop.log.tail.txt");
    EXPECT_EQ(metadata["included_files"][1], "feedback.json");
}

TEST(FeedbackUpload, BuildDesktopPackageMayIncludeSelectedSession) {
    TempDir tmp("acecode_feedback_desktop_selected_session");
    const fs::path session = tmp.root / "selected.jsonl";
    const fs::path log = tmp.root / "desktop-2026-06-18.log";
    write_text(session, "{\"role\":\"user\",\"content\":\"selected\"}\n");
    write_text(log, "desktop log");

    acecode::feedback::FeedbackPackageRequest req;
    req.source = "desktop";
    req.feedback_text = "selected context";
    req.session_id = "sid-selected";
    req.session_jsonl_path = session;
    req.workspace_hash = "workspace-a";
    req.log_path = log;
    req.log_entry_name = "logs/desktop.log.tail.txt";
    req.output_dir = tmp.root / "out";
    req.created_at = "2026-06-18T01:02:03Z";

    auto result = acecode::feedback::build_feedback_package(req);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(read_zip_entry(result.package_path, "session/sid-selected.jsonl"),
              "{\"role\":\"user\",\"content\":\"selected\"}\n");
    EXPECT_EQ(read_zip_entry(result.package_path, "logs/desktop.log.tail.txt"),
              "desktop log");

    auto metadata = nlohmann::json::parse(
        read_zip_entry(result.package_path, "feedback.json"));
    EXPECT_EQ(metadata["source"], "desktop");
    EXPECT_EQ(metadata["session_id"], "sid-selected");
    EXPECT_EQ(metadata["selected_session_id"], "sid-selected");
    EXPECT_EQ(metadata["workspace_hash"], "workspace-a");
}

TEST(FeedbackUpload, BuildDesktopPackageSucceedsWhenLogIsMissing) {
    TempDir tmp("acecode_feedback_desktop_missing_log");

    acecode::feedback::FeedbackPackageRequest req;
    req.source = "desktop";
    req.log_path = tmp.root / "missing.log";
    req.log_entry_name = "logs/desktop.log.tail.txt";
    req.output_dir = tmp.root / "out";
    req.created_at = "2026-06-18T01:02:03Z";

    auto result = acecode::feedback::build_feedback_package(req);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_FALSE(result.log_included);
    EXPECT_FALSE(zip_entry_exists(result.package_path, "logs/desktop.log.tail.txt"));
    auto metadata = nlohmann::json::parse(
        read_zip_entry(result.package_path, "feedback.json"));
    EXPECT_FALSE(metadata["log_available"].get<bool>());
    EXPECT_TRUE(metadata["session_id"].is_null());
}

TEST(FeedbackUpload, LatestDesktopLogPathPicksNewestDesktopLog) {
    TempDir tmp("acecode_feedback_desktop_latest_log");
    const fs::path logs = tmp.root / "logs";
    const fs::path older = logs / "desktop-2026-06-17.log";
    const fs::path newer = logs / "desktop-2026-06-18.log";
    const fs::path ignored = logs / "daemon-2026-06-19.log";
    write_text(older, "older");
    write_text(newer, "newer");
    write_text(ignored, "ignored");
    fs::last_write_time(older, fs::file_time_type::clock::now() - std::chrono::hours(2));
    fs::last_write_time(newer, fs::file_time_type::clock::now() - std::chrono::hours(1));
    fs::last_write_time(ignored, fs::file_time_type::clock::now());

    auto found = acecode::feedback::latest_desktop_log_path(logs);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->filename(), newer.filename());
}

TEST(FeedbackUpload, PackageFilenameAddsAvailableMachineSuffixFields) {
    EXPECT_EQ(
        acecode::feedback::make_feedback_package_filename(
            "sid", "2026-06-18T01:02:03Z", "windows/x64", "Build Box",
            "\xE7\x94\xA8\xE6\x88\xB7"),
        "acecode-feedback-sid-20260618-010203-windows-x64-Build-Box"
        "-xe7x94xa8xe6x88xb7.zip");

    EXPECT_EQ(
        acecode::feedback::make_feedback_package_filename(
            "sid", "2026-06-18T01:02:03Z", "windows-x64", "", ""),
        "acecode-feedback-sid-20260618-010203-windows-x64.zip");
}

TEST(FeedbackUpload, BuildPackageSucceedsWhenLogIsMissing) {
    TempDir tmp("acecode_feedback_missing_log");
    const fs::path session = tmp.root / "session.jsonl";
    write_text(session, "{}\n");

    acecode::feedback::FeedbackPackageRequest req;
    req.session_id = "sid";
    req.session_jsonl_path = session;
    req.log_path = tmp.root / "missing.log";
    req.output_dir = tmp.root / "out";
    req.created_at = "2026-06-18T01:02:03Z";

    auto result = acecode::feedback::build_feedback_package(req);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_FALSE(result.log_included);
    auto metadata = nlohmann::json::parse(
        read_zip_entry(result.package_path, "feedback.json"));
    EXPECT_FALSE(metadata["log_available"].get<bool>());
    EXPECT_EQ(read_zip_entry(result.package_path, "logs/acecode.log.tail.txt"), "");
}

TEST(FeedbackUpload, UploadPackageSendsGoHttpServerCompatibleMultipart) {
    TempDir tmp("acecode_feedback_upload");
    const fs::path package = tmp.root / "feedback.zip";
    write_text(package, "zip-bytes");

    std::string received_filename;
    std::string received_content;
    LocalHttpServer server([&](httplib::Server& s) {
        s.Post("/", [&](const httplib::Request& req, httplib::Response& res) {
            EXPECT_TRUE(req.is_multipart_form_data());
            auto file = req.get_file_value("file");
            auto filename = req.get_file_value("filename");
            received_filename = file.filename;
            received_content = file.content;
            EXPECT_EQ(filename.content, "feedback.zip");
            res.set_content(R"({"success":true})", "application/json");
        });
    });

    acecode::feedback::FeedbackUploadRequest req;
    req.upload_url = server.base_url();
    req.package_path = package;
    req.package_filename = "feedback.zip";
    req.timeout_ms = 3000;

    auto result = acecode::feedback::upload_feedback_package(req);
    EXPECT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.status_code, 200);
    EXPECT_EQ(received_filename, "feedback.zip");
    EXPECT_EQ(received_content, "zip-bytes");
}

TEST(FeedbackUpload, UploadPackageTreatsSuccessFalseAsFailure) {
    TempDir tmp("acecode_feedback_upload_fail");
    const fs::path package = tmp.root / "feedback.zip";
    write_text(package, "zip-bytes");

    LocalHttpServer server([](httplib::Server& s) {
        s.Post("/", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(R"({"success":false,"error":"denied"})", "application/json");
        });
    });

    acecode::feedback::FeedbackUploadRequest req;
    req.upload_url = server.base_url();
    req.package_path = package;
    req.package_filename = "feedback.zip";
    req.timeout_ms = 3000;

    auto result = acecode::feedback::upload_feedback_package(req);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.status_code, 200);
    EXPECT_NE(result.error.find("denied"), std::string::npos);
}
