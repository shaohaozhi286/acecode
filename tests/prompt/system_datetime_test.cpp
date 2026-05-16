#include <gtest/gtest.h>

#include "prompt/system_datetime.hpp"

#include <ctime>

TEST(SystemDatetimeTest, FormatsUtcOffsets) {
    EXPECT_EQ(acecode::format_utc_offset(0), "UTC+00:00");
    EXPECT_EQ(acecode::format_utc_offset(480), "UTC+08:00");
    EXPECT_EQ(acecode::format_utc_offset(-330), "UTC-05:30");
}

TEST(SystemDatetimeTest, FormatsPromptDatetimeWithWeekday) {
    std::tm local{};
    local.tm_year = 2026 - 1900;
    local.tm_mon = 5 - 1;
    local.tm_mday = 15;
    local.tm_hour = 19;
    local.tm_min = 32;
    local.tm_sec = 10;
    local.tm_wday = 5;

    EXPECT_EQ(
        acecode::format_prompt_datetime(local, 480),
        "2026-05-15 19:32:10 UTC+08:00 (Friday)");
}

TEST(SystemDatetimeTest, CurrentPromptDatetimeContainsRequiredShape) {
    const std::string value = acecode::current_prompt_datetime();

    ASSERT_GE(value.size(), 35u);
    EXPECT_EQ(value[4], '-');
    EXPECT_EQ(value[7], '-');
    EXPECT_EQ(value[10], ' ');
    EXPECT_EQ(value[13], ':');
    EXPECT_EQ(value[16], ':');
    EXPECT_NE(value.find(" UTC"), std::string::npos);
    EXPECT_NE(value.find("("), std::string::npos);
    EXPECT_NE(value.find(")"), std::string::npos);

    for (unsigned char c : value) {
        EXPECT_LT(c, 128);
    }
}
