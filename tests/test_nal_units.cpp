#include "decode/NalUnits.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using player::decode::NalView;

std::vector<uint8_t> withLengthPrefixes(const std::vector<std::vector<uint8_t>>& nals,
                                        uint8_t length_size) {
    std::vector<uint8_t> out;
    for (const std::vector<uint8_t>& nal : nals) {
        for (int i = length_size - 1; i >= 0; --i) {
            out.push_back(static_cast<uint8_t>((nal.size() >> (8 * i)) & 0xFF));
        }
        out.insert(out.end(), nal.begin(), nal.end());
    }
    return out;
}

}  // namespace

TEST(NalUnits, SplitsFourByteLengths) {
    const std::vector<std::vector<uint8_t>> input = {{0x26, 0x01, 0xAA}, {0x02, 0x01}};
    const std::vector<uint8_t> data = withLengthPrefixes(input, 4);

    std::vector<NalView> nals;
    ASSERT_TRUE(player::decode::splitLengthPrefixed(data.data(), data.size(), 4, nals));
    ASSERT_EQ(nals.size(), 2u);
    EXPECT_EQ(nals[0].size, 3u);
    EXPECT_EQ(nals[0].data[0], 0x26u);
    EXPECT_EQ(nals[1].size, 2u);
    EXPECT_EQ(nals[1].data[0], 0x02u);
}

TEST(NalUnits, SplitsTwoByteLengths) {
    const std::vector<std::vector<uint8_t>> input = {{0x40, 0x01}, {0x42, 0x01, 0x02}};
    const std::vector<uint8_t> data = withLengthPrefixes(input, 2);

    std::vector<NalView> nals;
    ASSERT_TRUE(player::decode::splitLengthPrefixed(data.data(), data.size(), 2, nals));
    ASSERT_EQ(nals.size(), 2u);
    EXPECT_EQ(nals[0].size, 2u);
    EXPECT_EQ(nals[1].size, 3u);
}

TEST(NalUnits, SkipsEmptyNals) {
    std::vector<uint8_t> data = {0, 0, 0, 0, 0, 0, 0, 2, 0x26, 0x01};
    std::vector<NalView> nals;
    ASSERT_TRUE(player::decode::splitLengthPrefixed(data.data(), data.size(), 4, nals));
    ASSERT_EQ(nals.size(), 1u);
    EXPECT_EQ(nals[0].size, 2u);
}

TEST(NalUnits, RejectsTruncatedNal) {
    const std::vector<uint8_t> data = {0, 0, 0, 5, 0x26, 0x01};
    std::vector<NalView> nals;
    EXPECT_FALSE(player::decode::splitLengthPrefixed(data.data(), data.size(), 4, nals));
}

TEST(NalUnits, RejectsTruncatedLengthField) {
    const std::vector<uint8_t> data = {0, 0, 0};
    std::vector<NalView> nals;
    EXPECT_FALSE(player::decode::splitLengthPrefixed(data.data(), data.size(), 4, nals));
}

TEST(NalUnits, RejectsInvalidLengthSize) {
    const std::vector<uint8_t> data = {1, 2, 3, 4};
    std::vector<NalView> nals;
    EXPECT_FALSE(player::decode::splitLengthPrefixed(data.data(), data.size(), 0, nals));
    EXPECT_FALSE(player::decode::splitLengthPrefixed(data.data(), data.size(), 5, nals));
}
