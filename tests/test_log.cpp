#include "utils/Log.hpp"

#include <gtest/gtest.h>

TEST(Log, StoresFormatsAndClears) {
    player::log::clear();
    const uint64_t before = player::log::version();

    player::log::add(player::log::Level::Info, "hello");
    player::log::addf(player::log::Level::Warning, "count=%d", 5);
    EXPECT_GT(player::log::version(), before);

    const std::vector<player::log::Entry> entries = player::log::snapshot();
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].text, "hello");
    EXPECT_EQ(entries[0].level, player::log::Level::Info);
    EXPECT_EQ(entries[1].text, "count=5");
    EXPECT_EQ(entries[1].level, player::log::Level::Warning);
    EXPECT_GE(entries[0].time_seconds, 0.0);

    player::log::clear();
    EXPECT_TRUE(player::log::snapshot().empty());
    EXPECT_GT(player::log::version(), before);
}

TEST(Log, KeepsBoundedHistory) {
    player::log::clear();
    for (int i = 0; i < 2500; ++i) {
        player::log::add(player::log::Level::Debug, "message");
    }
    const std::vector<player::log::Entry> entries = player::log::snapshot();
    EXPECT_LE(entries.size(), 2000u);
    player::log::clear();
}
