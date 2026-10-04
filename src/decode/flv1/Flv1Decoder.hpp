#pragma once

#include "decode/Frame.hpp"
#include "decode/KvzdecDecoder.hpp"  // DecodedFrame
#include "decode/mpeg12/Mpeg12BitReader.hpp"
#include "decode/mpeg12/Mpeg12Idct.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace player::decode::flv1 {

// From-scratch Sorenson Spark (FLV1) video decoder: H.263 baseline with the FLV
// picture header and the FLV escape form of the run-level codes.
//
// FLV stores one complete picture per tag, and FLV1 is I/P only with no
// reordering, so pushSample() decodes the picture immediately and pullFrames()
// hands out the result. P pictures predict from the previous picture.
class Decoder {
public:
    Decoder();
    ~Decoder();

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    void reset();
    void pushSample(const uint8_t* data, size_t size, int64_t pts);
    int pullFrames(std::vector<DecodedFrame>& out);
    int flush(std::vector<DecodedFrame>& out);

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    bool damaged() const { return damaged_; }
    uint64_t picturesDecoded() const { return pictures_decoded_; }
    const std::string& unsupported() const { return unsupported_; }

private:
    struct PictureHeader {
        bool intra = true;
        uint32_t width = 0;
        uint32_t height = 0;
        int quantiser = 0;
        int timestamp = 0;
    };

    bool parseHeader(mpeg12::BitReader& reader, PictureHeader& header);
    bool decodePicture(const uint8_t* data, size_t size, int64_t pts);
    bool decodeSlice(mpeg12::BitReader& reader);
    bool parseGobHeader(mpeg12::BitReader& reader);
    bool decodeMacroblock(mpeg12::BitReader& reader, int mb_x, int mb_y);
    bool decodeBlock(mpeg12::BitReader& reader, mpeg12::Block& block, bool coded, bool intra);
    void decodeDquant(mpeg12::BitReader& reader);
    int decodeMotion(mpeg12::BitReader& reader, int predictor);
    int predictMotion(int mb_x, int mb_y, int block, int& pred_x, int& pred_y);
    void updateMotionVal(int mb_x, int mb_y, bool intra, const int mv[4][2], bool use_4mv);
    void clearMotionGrid();
    void motionCompensate(int mb_x, int mb_y, const int mv[4][2], bool use_4mv);
    void reconstructIntra(mpeg12::Block blocks[6], uint8_t* dest_y, uint8_t* dest_cb,
                          uint8_t* dest_cr, int stride_y, int stride_c);
    void reconstructInter(mpeg12::Block blocks[6], int cbp, uint8_t* dest_y, uint8_t* dest_cb,
                          uint8_t* dest_cr, int stride_y, int stride_c);
    void emitFrame(int64_t pts, const PictureHeader& header);

    Frame current_;
    Frame reference_;
    bool have_reference_ = false;
    bool picture_intra_ = true;
    int format_ = 0;
    int width_ = 0;  // display size from the picture header
    int height_ = 0;
    int mb_width_ = 0;
    int mb_height_ = 0;
    int b8_stride_ = 0;
    int mb_x_ = 0;
    int mb_y_ = 0;
    int qscale_ = 1;
    bool first_slice_line_ = true;
    int resync_mb_x_ = 0;
    int resync_mb_y_ = 0;
    std::vector<int16_t> mv_x_;
    std::vector<int16_t> mv_y_;
    std::vector<DecodedFrame> ready_;
    uint64_t pictures_decoded_ = 0;
    bool damaged_ = false;
    std::string unsupported_;
};

}  // namespace player::decode::flv1
