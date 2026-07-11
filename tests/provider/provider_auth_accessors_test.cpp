#include "provider/llm_provider.hpp"
#include "provider/openai_provider.hpp"

#include <gtest/gtest.h>

using namespace acecode;

TEST(ProviderAuthAccessors, OpenAiProviderExposesAndUpdatesApiKey) {
    OpenAiCompatProvider provider("https://models.example.com/v1/", "old-key", "test-model");
    EXPECT_EQ(provider.base_url(), "https://models.example.com/v1");
    EXPECT_EQ(provider.current_api_key(), "old-key");
    provider.update_api_key("new-key");
    EXPECT_EQ(provider.current_api_key(), "new-key");
}

TEST(ProviderAuthAccessors, AuthShapedErrorPredicate) {
    ProviderErrorInfo info;
    info.kind = ProviderErrorKind::Http;
    info.status_code = 400;
    EXPECT_TRUE(provider_error_is_auth_shaped(info));
    info.status_code = 401;
    EXPECT_TRUE(provider_error_is_auth_shaped(info));
    info.status_code = 403;
    EXPECT_FALSE(provider_error_is_auth_shaped(info));
    info.status_code = 400;
    info.kind = ProviderErrorKind::Timeout;
    EXPECT_FALSE(provider_error_is_auth_shaped(info));
}
