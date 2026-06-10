#include <gtest/gtest.h>

#include "config/config.hpp"
#include "provider/provider_factory.hpp"

#include <httplib.h>

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

using acecode::ModelProfile;
using acecode::create_provider_from_entry;

namespace {

using namespace std::chrono_literals;

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
        return "http://127.0.0.1:" + std::to_string(port);
    }
};

acecode::ChatMessage user_message() {
    acecode::ChatMessage msg;
    msg.role = "user";
    msg.content = "hi";
    return msg;
}

ModelProfile openai_profile(const std::string& base_url) {
    ModelProfile profile;
    profile.name = "gateway";
    profile.provider = "openai";
    profile.base_url = base_url;
    profile.api_key = "sk-test";
    profile.model = "test-model";
    return profile;
}

} // namespace

TEST(ProviderFactory, EmptyProviderReturnsNull) {
    ModelProfile profile;
    profile.name = "";
    profile.provider = "";
    profile.model = "";

    EXPECT_EQ(create_provider_from_entry(profile), nullptr);
}

TEST(ProviderFactory, OpenAiProfileRequestHeadersReachProvider) {
    std::mutex mu;
    std::string seen_header;

    LocalHttpServer server([&](httplib::Server& s) {
        s.Post("/chat/completions", [&](const httplib::Request& req, httplib::Response& res) {
            {
                std::lock_guard<std::mutex> lk(mu);
                seen_header = req.get_header_value("X-Factory");
            }
            res.set_content(R"({"choices":[{"message":{"content":"ok"},"finish_reason":"stop"}]})",
                            "application/json");
        });
    });

    auto profile = openai_profile(server.base_url());
    profile.request_headers = {{"X-Factory", "profile"}};
    auto provider = create_provider_from_entry(profile);
    ASSERT_TRUE(provider);

    auto response = provider->chat({user_message()}, {});
    EXPECT_EQ(response.content, "ok");

    std::lock_guard<std::mutex> lk(mu);
    EXPECT_EQ(seen_header, "profile");
}

TEST(ProviderFactory, OpenAiProfileInheritsGlobalRequestHeadersWhenEntryOmitsThem) {
    std::mutex mu;
    std::string seen_header;

    LocalHttpServer server([&](httplib::Server& s) {
        s.Post("/chat/completions", [&](const httplib::Request& req, httplib::Response& res) {
            {
                std::lock_guard<std::mutex> lk(mu);
                seen_header = req.get_header_value("X-Global");
            }
            res.set_content(R"({"choices":[{"message":{"content":"ok"},"finish_reason":"stop"}]})",
                            "application/json");
        });
    });

    acecode::AppConfig cfg;
    cfg.openai.request_headers = {{"X-Global", "fallback"}};
    auto profile = openai_profile(server.base_url());
    auto provider = create_provider_from_entry(profile, &cfg);
    ASSERT_TRUE(provider);

    auto response = provider->chat({user_message()}, {});
    EXPECT_EQ(response.content, "ok");

    std::lock_guard<std::mutex> lk(mu);
    EXPECT_EQ(seen_header, "fallback");
}

TEST(ProviderFactory, UnknownProviderReturnsNull) {
    ModelProfile profile;
    profile.name = "anthropic";
    profile.provider = "anthropic";
    profile.model = "claude";

    EXPECT_EQ(create_provider_from_entry(profile), nullptr);
}
