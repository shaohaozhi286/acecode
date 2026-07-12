#include "config/config.hpp"
#include "upgrade/check.hpp"
#include "upgrade/http.hpp"
#include "upgrade/manifest.hpp"
#include "upgrade/upgrade.hpp"
#include "utils/sha256.hpp"

#include <gtest/gtest.h>
#include <httplib.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

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

acecode::AppConfig upgrade_config_for(const LocalHttpServer& server) {
    acecode::AppConfig cfg;
    cfg.network.proxy_mode = "off";
    cfg.upgrade.base_url = server.base_url();
    cfg.upgrade.timeout_ms = 3000;
    return cfg;
}

std::string manifest_for(const std::string& version,
                         const std::string& target,
                         const std::string& file,
                         const std::string& sha) {
    return R"({
      "schema_version": 1,
      "latest": ")" + version + R"(",
      "releases": [
        {"version": ")" + version + R"(", "packages": [
          {"target": ")" + target + R"(", "file": ")" + file + R"(", "sha256": ")" + sha + R"("}
        ]}
      ]
    })";
}

} // namespace

TEST(UpgradeHttp, NoUpdateReturnsSuccessWithoutTui) {
    LocalHttpServer server([](httplib::Server& s) {
        s.Get("/aceupdate.json", [](const httplib::Request&, httplib::Response& res) {
            std::string sha = acecode::sha256_hex("pkg");
            res.set_content(manifest_for("0.1.2", acecode::upgrade::current_target(),
                                         "pkg.zip", sha), "application/json");
        });
    });

    std::ostringstream out;
    std::ostringstream err;
    int code = acecode::upgrade::run_upgrade_command(
        upgrade_config_for(server), "acecode-test", "0.1.2", out, err);

    EXPECT_EQ(code, 0);
    EXPECT_NE(out.str().find("already up to date"), std::string::npos);
}

TEST(UpgradeHttp, Manifest404ReturnsActionableError) {
    LocalHttpServer server([](httplib::Server&) {});

    std::ostringstream out;
    std::ostringstream err;
    int code = acecode::upgrade::run_upgrade_command(
        upgrade_config_for(server), "acecode-test", "0.1.2", out, err);

    EXPECT_NE(code, 0);
    EXPECT_NE(err.str().find("manifest not found"), std::string::npos);
}

TEST(UpgradeHttp, DownloadFailureReturnsBeforeApply) {
    LocalHttpServer server([](httplib::Server& s) {
        s.Get("/aceupdate.json", [](const httplib::Request&, httplib::Response& res) {
            std::string sha = acecode::sha256_hex("pkg");
            res.set_content(manifest_for("9.9.9", acecode::upgrade::current_target(),
                                         "pkg.zip", sha), "application/json");
        });
        s.Get("/pkg.zip", [](const httplib::Request&, httplib::Response& res) {
            res.status = 500;
            res.set_content("nope", "text/plain");
        });
    });

    std::ostringstream out;
    std::ostringstream err;
    std::vector<acecode::upgrade::UpgradeProgress> progress;
    int code = acecode::upgrade::run_upgrade_command(
        upgrade_config_for(server), "acecode-test", "0.1.2", out, err, false,
        [&](const acecode::upgrade::UpgradeProgress& item) {
            progress.push_back(item);
        });

    EXPECT_NE(code, 0);
    EXPECT_NE(err.str().find("package request returned HTTP 500"), std::string::npos);
    ASSERT_GE(progress.size(), 2U);
    EXPECT_EQ(progress.front().phase, acecode::upgrade::UpgradePhase::Checking);
    EXPECT_EQ(progress.back().phase, acecode::upgrade::UpgradePhase::Downloading);
    EXPECT_EQ(progress.back().target_version, "9.9.9");
    EXPECT_STREQ(acecode::upgrade::upgrade_phase_name(progress.back().phase),
                 "downloading");
}

TEST(UpgradeHttp, DownloadAcceptHeaderAllowsCommonZipMimeTypes) {
    LocalHttpServer server([](httplib::Server& s) {
        s.Get("/pkg.zip", [](const httplib::Request& req, httplib::Response& res) {
            const std::string accept = req.get_header_value("Accept");
            if (accept.find("application/x-zip-compressed") == std::string::npos ||
                accept.find("*/*") == std::string::npos) {
                res.status = 406;
                return;
            }
            res.set_content("zip", "application/x-zip-compressed");
        });
    });

    const auto output = std::filesystem::temp_directory_path() /
        ("acecode-download-accept-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         ".zip");
    std::uintmax_t last_progress = 0;
    int progress_calls = 0;
    auto result = acecode::upgrade::download_to_file(
        server.base_url() + "pkg.zip", output, 3000,
        [&](const acecode::upgrade::DownloadProgress& progress) {
            last_progress = progress.bytes_written;
            ++progress_calls;
        });

    EXPECT_TRUE(result.error.empty());
    EXPECT_EQ(result.status_code, 200);
    EXPECT_EQ(result.bytes_written, 3u);
    EXPECT_GT(progress_calls, 0);
    EXPECT_EQ(last_progress, 3u);
    std::error_code ec;
    EXPECT_EQ(std::filesystem::file_size(output, ec), 3u);
    std::filesystem::remove(output, ec);
}

TEST(UpgradeHttp, ChecksumMismatchReturnsBeforeExtraction) {
    LocalHttpServer server([](httplib::Server& s) {
        s.Get("/aceupdate.json", [](const httplib::Request&, httplib::Response& res) {
            std::string sha = acecode::sha256_hex("expected");
            res.set_content(manifest_for("9.9.9", acecode::upgrade::current_target(),
                                         "pkg.zip", sha), "application/json");
        });
        s.Get("/pkg.zip", [](const httplib::Request&, httplib::Response& res) {
            res.set_content("actual", "application/zip");
        });
    });

    std::ostringstream out;
    std::ostringstream err;
    std::vector<acecode::upgrade::UpgradeProgress> progress;
    int code = acecode::upgrade::run_upgrade_command(
        upgrade_config_for(server), "acecode-test", "0.1.2", out, err, false,
        [&](const acecode::upgrade::UpgradeProgress& item) {
            progress.push_back(item);
        });

    EXPECT_NE(code, 0);
    EXPECT_NE(err.str().find("checksum mismatch"), std::string::npos);
    ASSERT_FALSE(progress.empty());
    EXPECT_EQ(progress.back().phase, acecode::upgrade::UpgradePhase::Verifying);
    EXPECT_EQ(progress.back().bytes_downloaded, std::string("actual").size());
}

TEST(UpgradeHttp, UpdateCheckReportsAvailableWithoutDownloadingPackage) {
    bool package_requested = false;
    LocalHttpServer server([&](httplib::Server& s) {
        s.Get("/aceupdate.json", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(manifest_for("9.9.9", acecode::upgrade::current_target(),
                                         "acecode.zip", std::string(64, 'a')),
                            "application/json");
        });
        s.Get("/acecode.zip", [&](const httplib::Request&, httplib::Response& res) {
            package_requested = true;
            res.status = 500;
        });
    });

    auto result = acecode::upgrade::check_for_update(
        upgrade_config_for(server), "0.1.2");

    EXPECT_EQ(result.status, acecode::upgrade::UpdateCheckStatus::UpdateAvailable);
    EXPECT_TRUE(result.update_available());
    EXPECT_EQ(result.latest_version, "9.9.9");
    EXPECT_EQ(result.package_file, "acecode.zip");
    EXPECT_FALSE(package_requested);
}

TEST(UpgradeHttp, UpdateCheckReportsUpToDate) {
    LocalHttpServer server([](httplib::Server& s) {
        s.Get("/aceupdate.json", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(manifest_for("0.1.2", acecode::upgrade::current_target(),
                                         "acecode.zip", std::string(64, 'a')),
                            "application/json");
        });
    });

    auto result = acecode::upgrade::check_for_update(
        upgrade_config_for(server), "0.1.2");

    EXPECT_EQ(result.status, acecode::upgrade::UpdateCheckStatus::UpToDate);
    EXPECT_FALSE(result.update_available());
}

TEST(UpgradeHttp, UpdateCheckRejectsInvalidConfigBeforeNetwork) {
    acecode::AppConfig cfg;
    cfg.upgrade.base_url = "ftp://updates.example.test/";

    auto result = acecode::upgrade::check_for_update(cfg, "0.1.2");

    EXPECT_EQ(result.status, acecode::upgrade::UpdateCheckStatus::InvalidConfig);
    EXPECT_FALSE(result.error.empty());
}

TEST(UpgradeHttp, UpdateCheckReportsManifestFailure) {
    LocalHttpServer server([](httplib::Server&) {});

    auto result = acecode::upgrade::check_for_update(
        upgrade_config_for(server), "0.1.2");

    EXPECT_EQ(result.status, acecode::upgrade::UpdateCheckStatus::ManifestUnavailable);
    EXPECT_EQ(result.http_status, 404);
}

TEST(UpgradeHttp, ServerOverrideNormalizesAndPersistsThroughConfigSave) {
    acecode::AppConfig cfg;
    cfg.upgrade.base_url = "http://old.example.test/updates/";

    std::string error;
    ASSERT_TRUE(acecode::upgrade::apply_upgrade_server_override(
        cfg, " https://updates.example.test/ace ", &error));
    EXPECT_TRUE(error.empty());
    EXPECT_EQ(cfg.upgrade.base_url, "https://updates.example.test/ace/");

    const auto output = std::filesystem::temp_directory_path() /
        ("acecode-upgrade-server-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         ".json");
    acecode::save_config(cfg, output.string());

    std::ifstream ifs(output);
    ASSERT_TRUE(ifs.is_open());
    auto j = nlohmann::json::parse(ifs);
    EXPECT_EQ(j["upgrade"]["base_url"], "https://updates.example.test/ace/");

    std::error_code ec;
    std::filesystem::remove(output, ec);
}

TEST(UpgradeHttp, ServerOverrideRejectsInvalidUrlWithoutChangingConfig) {
    acecode::AppConfig cfg;
    cfg.upgrade.base_url = "http://old.example.test/updates/";

    std::string error;
    EXPECT_FALSE(acecode::upgrade::apply_upgrade_server_override(
        cfg, "ftp://updates.example.test/ace", &error));
    EXPECT_NE(error.find("http or https"), std::string::npos);
    EXPECT_EQ(cfg.upgrade.base_url, "http://old.example.test/updates/");

    error.clear();
    EXPECT_FALSE(acecode::upgrade::apply_upgrade_server_override(cfg, "", &error));
    EXPECT_NE(error.find("http or https"), std::string::npos);
    EXPECT_EQ(cfg.upgrade.base_url, "http://old.example.test/updates/");
}
