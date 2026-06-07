#include <gtest/gtest.h>

#include "provider/provider_factory.hpp"

using acecode::ModelProfile;
using acecode::create_provider_from_entry;

TEST(ProviderFactory, EmptyProviderReturnsNull) {
    ModelProfile profile;
    profile.name = "";
    profile.provider = "";
    profile.model = "";

    EXPECT_EQ(create_provider_from_entry(profile), nullptr);
}

TEST(ProviderFactory, UnknownProviderReturnsNull) {
    ModelProfile profile;
    profile.name = "anthropic";
    profile.provider = "anthropic";
    profile.model = "claude";

    EXPECT_EQ(create_provider_from_entry(profile), nullptr);
}
