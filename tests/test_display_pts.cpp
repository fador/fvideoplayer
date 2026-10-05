#include "decode/DisplayPts.hpp"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace {

using player::decode::DisplayPts;

// The demuxer hands samples over in decode order (I, P, B, B, ...), so the
// pushed timestamps jump around; pictures come out in display order.
TEST(DisplayPts, ReordersDecodeOrderTimestamps) {
    DisplayPts pts;
    const std::vector<int64_t> pushed = {0, 167, 83, 42, 125, 334, 250, 209, 292};
    for (int64_t value : pushed) {
        pts.push(value);
    }
    const std::vector<int64_t> expected = {0, 42, 83, 125, 167, 209, 250, 292, 334};
    for (int64_t value : expected) {
        EXPECT_EQ(pts.next(), value);
    }
}

TEST(DisplayPts, FirstPushedIsSmallest) {
    DisplayPts pts;
    pts.push(0);
    pts.push(83);
    pts.push(42);
    EXPECT_EQ(pts.next(), 0);
    EXPECT_EQ(pts.next(), 42);
    EXPECT_EQ(pts.next(), 83);
}

TEST(DisplayPts, ExtraPicturesReuseLastTimestamp) {
    DisplayPts pts;
    pts.push(100);
    pts.push(50);
    EXPECT_EQ(pts.next(), 50);
    EXPECT_EQ(pts.next(), 100);
    EXPECT_EQ(pts.next(), 100);
    EXPECT_EQ(pts.next(), 100);
}

TEST(DisplayPts, KeepsDuplicateTimestamps) {
    DisplayPts pts;
    pts.push(40);
    pts.push(40);
    pts.push(0);
    EXPECT_EQ(pts.next(), 0);
    EXPECT_EQ(pts.next(), 40);
    EXPECT_EQ(pts.next(), 40);
}

TEST(DisplayPts, ResetClearsState) {
    DisplayPts pts;
    pts.push(500);
    pts.push(100);
    EXPECT_EQ(pts.next(), 100);
    pts.reset();
    EXPECT_EQ(pts.next(), 0);
    pts.push(200);
    EXPECT_EQ(pts.next(), 200);
}

}  // namespace
