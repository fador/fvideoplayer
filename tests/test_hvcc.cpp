#include "demux/HvcC.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using player::demux::HvccInfo;

void put16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void put32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void addArray(std::vector<uint8_t>& out, uint8_t nal_type,
              const std::vector<std::vector<uint8_t>>& nals) {
    out.push_back(static_cast<uint8_t>(0x80 | nal_type));  // array_completeness = 1
    put16(out, static_cast<uint16_t>(nals.size()));
    for (const std::vector<uint8_t>& nal : nals) {
        put16(out, static_cast<uint16_t>(nal.size()));
        out.insert(out.end(), nal.begin(), nal.end());
    }
}

std::vector<uint8_t> makeRecord(uint8_t profile_idc, uint8_t level_idc, uint8_t length_size_minus1,
                                uint8_t chroma_format_idc, uint8_t bit_depth_luma_minus8,
                                const std::vector<std::vector<uint8_t>>& vps,
                                const std::vector<std::vector<uint8_t>>& sps,
                                const std::vector<std::vector<uint8_t>>& pps) {
    std::vector<uint8_t> out;
    out.push_back(1);  // configurationVersion
    out.push_back(static_cast<uint8_t>(profile_idc & 0x1F));
    put32(out, 0x60000000);  // profile_compatibility_flags
    for (int i = 0; i < 6; ++i) {
        out.push_back(0);  // general_constraint_indicator_flags
    }
    out.push_back(level_idc);
    put16(out, 0xF000);  // min_spatial_segmentation_idc = 0
    out.push_back(0xFC);  // parallelismType = 0
    out.push_back(static_cast<uint8_t>(0xFC | chroma_format_idc));
    out.push_back(static_cast<uint8_t>(0xF8 | bit_depth_luma_minus8));
    out.push_back(0xF8);  // bit_depth_chroma_minus8 = 0
    put16(out, 0);        // avgFrameRate
    out.push_back(static_cast<uint8_t>((1 << 3) | (length_size_minus1 & 0x03)));
    const uint8_t num_arrays = static_cast<uint8_t>((vps.empty() ? 0 : 1) + (sps.empty() ? 0 : 1) +
                                                    (pps.empty() ? 0 : 1));
    out.push_back(num_arrays);
    if (!vps.empty()) {
        addArray(out, 32, vps);
    }
    if (!sps.empty()) {
        addArray(out, 33, sps);
    }
    if (!pps.empty()) {
        addArray(out, 34, pps);
    }
    return out;
}

}  // namespace

TEST(HvcC, ParsesValidRecord) {
    const std::vector<uint8_t> record =
        makeRecord(2, 153, 3, 1, 2, {{0x40, 0x01, 0x0C}}, {{0x42, 0x01, 0x02}}, {{0x44, 0x01, 0x03}});

    HvccInfo info;
    std::string error;
    ASSERT_TRUE(player::demux::parseHvcc(record.data(), record.size(), info, &error)) << error;

    EXPECT_TRUE(info.valid);
    EXPECT_EQ(info.configuration_version, 1);
    EXPECT_EQ(info.profile_idc, 2);
    EXPECT_FALSE(info.tier_flag);
    EXPECT_EQ(info.level_idc, 153);
    EXPECT_EQ(info.chroma_format_idc, 1);
    EXPECT_EQ(info.bit_depth_luma_minus8, 2);
    EXPECT_EQ(info.length_size, 4);
    ASSERT_EQ(info.nals.size(), 3u);
    EXPECT_EQ(info.nals[0].nal_type, 32u);
    EXPECT_EQ(info.nals[1].nal_type, 33u);
    EXPECT_EQ(info.nals[2].nal_type, 34u);
    EXPECT_EQ(info.nals[0].data, (std::vector<uint8_t>{0x40, 0x01, 0x0C}));
    EXPECT_EQ(info.nals[2].data, (std::vector<uint8_t>{0x44, 0x01, 0x03}));
}

TEST(HvcC, ParsesRecordWithoutParameterSets) {
    const std::vector<uint8_t> record = makeRecord(1, 120, 1, 1, 0, {}, {}, {});
    HvccInfo info;
    ASSERT_TRUE(player::demux::parseHvcc(record.data(), record.size(), info, nullptr));
    EXPECT_TRUE(info.valid);
    EXPECT_EQ(info.length_size, 2);
    EXPECT_TRUE(info.nals.empty());
}

TEST(HvcC, RejectsShortRecord) {
    const std::vector<uint8_t> record = {1, 2, 3, 4};
    HvccInfo info;
    std::string error;
    EXPECT_FALSE(player::demux::parseHvcc(record.data(), record.size(), info, &error));
    EXPECT_FALSE(error.empty());
}

TEST(HvcC, RejectsBadVersion) {
    std::vector<uint8_t> record =
        makeRecord(1, 120, 3, 1, 0, {{0x40, 0x01}}, {}, {});
    record[0] = 2;
    HvccInfo info;
    EXPECT_FALSE(player::demux::parseHvcc(record.data(), record.size(), info, nullptr));
}

TEST(HvcC, RejectsTruncatedNalData) {
    std::vector<uint8_t> record = makeRecord(1, 120, 3, 1, 0, {{0x40, 0x01, 0x02}}, {}, {});
    record.resize(record.size() - 2);
    HvccInfo info;
    std::string error;
    EXPECT_FALSE(player::demux::parseHvcc(record.data(), record.size(), info, &error));
    EXPECT_FALSE(error.empty());
}

TEST(HvcC, ProfileAndLevelNames) {
    HvccInfo info;
    info.profile_idc = 2;
    info.level_idc = 153;
    EXPECT_EQ(player::demux::hvccProfileName(info), "Main 10");
    EXPECT_EQ(player::demux::hvccLevelName(info), "L5.1");
    info.profile_idc = 1;
    info.level_idc = 120;
    EXPECT_EQ(player::demux::hvccProfileName(info), "Main");
    EXPECT_EQ(player::demux::hvccLevelName(info), "L4.0");
}
