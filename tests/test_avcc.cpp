#include "demux/Avcc.hpp"

#include <cstdint>

#include <gtest/gtest.h>

namespace {

// Real avcC records produced by libx264 through ffmpeg.

// Baseline 8-bit 4:2:0 (level 1.3).
const uint8_t kBaselineAvcc[] = {
    0x01, 0x42, 0xc0, 0x0d, 0xff, 0xe1, 0x00, 0x16, 0x67, 0x42, 0xc0, 0x0d, 0xda,
    0x05, 0x07, 0xec, 0x04, 0x40, 0x00, 0x00, 0x03, 0x00, 0x40, 0x00, 0x00, 0x0c,
    0x83, 0xc5, 0x0a, 0xa8, 0x01, 0x00, 0x04, 0x68, 0xce, 0x0f, 0xc8};

// High 10 profile, 10-bit 4:2:0 (level 1.0); its SPS contains emulation
// prevention bytes.
const uint8_t kHigh10Avcc[] = {
    0x01, 0x6e, 0x00, 0x0a, 0xff, 0xe1, 0x00, 0x19, 0x67, 0x6e, 0x00, 0x0a, 0xa6, 0xcd, 0x94, 0x42,
    0x6c, 0x04, 0x40, 0x00, 0x00, 0x03, 0x00, 0x40, 0x00, 0x00, 0x03, 0x02, 0x83, 0xc4, 0x89, 0x65,
    0x80, 0x01, 0x00, 0x06, 0x68, 0xeb, 0xe3, 0xcb, 0x22, 0xc0, 0xfd, 0xfa, 0xfa, 0x00};

}  // namespace

TEST(Avcc, ParsesBaselineSpsFields) {
    player::demux::AvccInfo info;
    ASSERT_TRUE(player::demux::parseAvcc(kBaselineAvcc, sizeof(kBaselineAvcc), info, nullptr));
    ASSERT_EQ(info.sps.size(), 1u);
    ASSERT_EQ(info.pps.size(), 1u);
    EXPECT_EQ(info.length_size, 4);
    EXPECT_EQ(info.profile_idc, 66);
    EXPECT_EQ(info.level_idc, 13);
    EXPECT_EQ(info.chroma_format_idc, 1);
    EXPECT_EQ(info.bit_depth_luma, 8);
    EXPECT_EQ(player::demux::avcProfileName(info.profile_idc), "Baseline");
    EXPECT_EQ(player::demux::avcLevelName(info.level_idc), "L1.3");
}

TEST(Avcc, ParsesHigh10SpsFields) {
    player::demux::AvccInfo info;
    ASSERT_TRUE(player::demux::parseAvcc(kHigh10Avcc, sizeof(kHigh10Avcc), info, nullptr));
    ASSERT_EQ(info.sps.size(), 1u);
    EXPECT_EQ(info.length_size, 4);
    EXPECT_EQ(info.profile_idc, 110);
    EXPECT_EQ(info.level_idc, 10);
    EXPECT_EQ(info.chroma_format_idc, 1);
    EXPECT_EQ(info.bit_depth_luma, 10);
    EXPECT_EQ(player::demux::avcProfileName(info.profile_idc), "High 10");
    EXPECT_EQ(player::demux::avcLevelName(info.level_idc), "L1.0");
}

TEST(Avcc, RejectsTruncatedRecord) {
    player::demux::AvccInfo info;
    EXPECT_FALSE(player::demux::parseAvcc(kBaselineAvcc, 4, info, nullptr));
    EXPECT_FALSE(player::demux::parseAvcc(kBaselineAvcc, 7, info, nullptr));
}

TEST(Avcc, KeepsDefaultsWhenSpsIsTruncated) {
    // One SPS entry of two bytes and no PPS: the record parses, but the SPS
    // header cannot, leaving the informational fields at their defaults.
    const uint8_t record[] = {0x01, 0x42, 0x00, 0x1e, 0xff, 0xe1, 0x00, 0x02,
                              0x67, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    player::demux::AvccInfo info;
    ASSERT_TRUE(player::demux::parseAvcc(record, sizeof(record), info, nullptr));
    EXPECT_EQ(info.chroma_format_idc, 1);
    EXPECT_EQ(info.bit_depth_luma, 8);
}
