#include "config/config.hpp"
#include "upgrade/manifest.hpp"
#include "upgrade/upgrade.hpp"
#include "utils/sha256.hpp"

#include <gtest/gtest.h>
#include <httplib.h>

#include <chrono>
#include <functional>
#include <sstream>
#include <string>
#include <thread>

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
    int code = acecode::upgrade::run_upgrade_command(
        upgrade_config_for(server), "acecode-test", "0.1.2", out, err);

    EXPECT_NE(code, 0);
    EXPECT_NE(err.str().find("package request returned HTTP 500"), std::string::npos);
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
    int code = acecode::upgrade::run_upgrade_command(
        upgrade_config_for(server), "acecode-test", "0.1.2", out, err);

    EXPECT_NE(code, 0);
    EXPECT_NE(err.str().find("checksum mismatch"), std::string::npos);
}
