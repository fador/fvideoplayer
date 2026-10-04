#pragma once

#include "decode/Frame.hpp"
#include "decode/KvzdecDecoder.hpp"  // DecodedFrame
#include "decode/mpeg12/Mpeg12BitReader.hpp"
#include "decode/mpeg12/Mpeg12Idct.hpp"
#include "decode/mpeg12/Mpeg12Motion.hpp"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace player::decode::mpeg12 {

// Which of the two standards the stream turned out to be. The picture coding
// extension only exists in MPEG-2, so it is detected while parsing.
enum class Standard {
    Mpeg1,
    Mpeg2,
};

// From-scratch MPEG-1 (ISO/IEC 11172-2) and MPEG-2 (ISO/IEC 13818-2) video
// decoder.
//
// Feed start-code prefixed elementary stream bytes through pushSample(); the
// decoder buffers whatever it needs, decodes each picture as soon as it is
// complete, and hands out Frames in display order through pullFrames().
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
    double fps() const;
    Standard standard() const { return standard_; }
    bool progressiveSequence() const { return progressive_sequence_; }
    bool haveSequence() const { return have_sequence_; }

    // True when the stream uses interlaced coding (frame or field pictures).
    bool interlaced() const { return !progressive_sequence_; }

    // True when the last slice hit a damaged or truncated bitstream.
    bool damaged() const { return damaged_; }

    // Pictures whose slices did not parse cleanly since the last reset().
    uint64_t damagedPictures() const { return damaged_pictures_; }

    // Where the most recent damaged slice gave up. Diagnostics only: a damaged
    // stream is much easier to chase with the macroblock position and the bit
    // offset inside the slice than with a bare flag.
    int damageMbX() const { return damage_mb_x_; }
    int damageMbY() const { return damage_mb_y_; }
    size_t damageBit() const { return damage_bit_; }
    const char* damageReason() const { return damage_reason_; }

    uint64_t picturesDecoded() const { return pictures_decoded_; }
    uint64_t picturesDropped() const { return pictures_dropped_; }

    // Set when a stream used a feature this decoder does not implement. The
    // picture is skipped rather than decoded wrongly.
    const std::string& unsupported() const { return unsupported_; }

private:
    enum class PictureType { I = 1, P = 2, B = 3 };
    enum class Direction { Forward = 0, Backward = 1 };
    enum class PictureStructure { TopField = 1, BottomField = 2, Frame = 3 };
    enum class MotionType { Field = 1, Frame = 2, DualPrime = 3 };

    struct Sequence {
        uint32_t horizontal_size = 0;
        uint32_t vertical_size = 0;
        uint32_t aspect_code = 1;
        uint32_t frame_rate_code = 4;
        uint32_t bit_rate = 0;
        uint32_t vbv_buffer_size = 0;
        bool constrained = false;
        uint8_t intra_matrix[64] = {};
        uint8_t non_intra_matrix[64] = {};
        // Sequence extension.
        bool progressive_sequence = true;
        int chroma_format = 1;  // 1 = 4:2:0
        int frame_rate_extension_n = 0;
        int frame_rate_extension_d = 0;
    };

    struct PictureHeader {
        PictureType type = PictureType::I;
        int temporal_reference = 0;
        bool full_pel_forward = false;
        bool full_pel_backward = false;
        int forward_f_code = 0;
        int backward_f_code = 0;
        // Picture coding extension.
        bool has_coding_extension = false;
        int f_code[2][2] = {{1, 1}, {1, 1}};
        int intra_dc_precision = 0;
        PictureStructure structure = PictureStructure::Frame;
        bool top_field_first = false;
        bool frame_pred_frame_dct = true;
        bool concealment_motion_vectors = false;
        bool q_scale_type = false;
        bool intra_vlc_format = false;
        bool alternate_scan = false;
        bool repeat_first_field = false;
        bool progressive_frame = true;
    };

    struct MacroblockFlags {
        bool intra = true;
        bool motion_forward = false;
        bool motion_backward = false;
        bool has_pattern = false;
        bool quant_type = false;
        bool zero_mv = false;
        int coded_block_pattern = 0;
        MotionType motion_type = MotionType::Frame;
        bool field_motion = false;  // motion_type was explicitly coded
        bool interlaced_dct = false;
    };

    struct MotionVector {
        int x = 0;
        int y = 0;
        int field_select = 0;
    };

    // Picture lifecycle.
    void beginPicture(int64_t pts);
    void endPicture();
    bool parseSequenceHeader(BitReader& reader);
    bool parseSequenceExtension(BitReader& reader);
    bool parsePictureHeader(BitReader& reader, int64_t pts);
    bool parsePictureCodingExtension(BitReader& reader);
    void decodeSlice(const uint8_t* data, size_t size, int slice_start_code);
    void decodeSliceBody(BitReader& reader);
    void decodeMacroblock(BitReader& reader);
    void skipMacroblock();

    // Blocks.
    void decodeBlockIntra(BitReader& reader, int block_index, int component, int quant_scale);
    void decodeBlockInter(BitReader& reader, int block_index, int quant_scale);
    void storeBlock(const Block& block, int plane, int block_index, bool intra);
    PlaneGrid blockGrid(int plane, int block_index) const;
    void blockPosition(int plane, int block_index, int& x, int& y) const;

    // Motion compensation.
    void readMotionVectors(BitReader& reader, const MacroblockFlags& flags,
                           MotionVector mv[2][2]);
    void motionCompensate(const MacroblockFlags& flags, const MotionVector mv[2][2]);
    void predictDirection(const Frame& reference, const MacroblockFlags& flags,
                          const MotionVector mv[2]);
    void savePrediction();
    void averageWithPrediction();

    // Runs once the first slice of a picture is seen, by which point the
    // picture coding extension has told us the picture structure.
    void startPicture();

    void emit(Frame& frame);
    // Records where a slice gave up, for diagnostics.
    void markDamaged(const char* reason);
    void resetPictureState();
    int quantScale() const;
    int dcPredictorBase() const { return 128 << header_.intra_dc_precision; }
    int fieldParity() const {
        return header_.structure == PictureStructure::TopField ? 0 : 1;
    }
    bool fieldPicture() const { return header_.structure != PictureStructure::Frame; }
    // Coefficient scan order, chosen by alternate_scan at picture start.
    const int* scan() const { return scan_; }

    // Start-code scanning.
    struct StartCode {
        size_t header = 0;
        size_t body = 0;
        size_t code = 0;
    };
    bool findStartCode(size_t from, StartCode& out) const;
    void drain(bool at_end);

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    int mb_width_ = 0;
    int mb_height_ = 0;
    bool have_sequence_ = false;
    bool damaged_ = false;
    Standard standard_ = Standard::Mpeg1;
    bool progressive_sequence_ = true;
    std::string unsupported_;

    Sequence sequence_{};
    PictureHeader header_{};

    Frame reference_[2];
    Frame current_;
    std::deque<Frame> pending_b_;
    std::vector<DecodedFrame> ready_;
    std::vector<uint8_t> pending_;
    // Byte offset in pending_ of each pushed sample, with its presentation
    // time, so a picture can be given the time of the sample that carried it.
    std::deque<std::pair<size_t, int64_t>> pts_marks_;
    size_t cursor_ = 0;
    bool in_picture_ = false;
    bool picture_started_ = false;
    bool picture_damaged_ = false;
    bool waiting_for_reference_ = false;
    bool picture_unsupported_ = false;
    bool mb_interlaced_dct_ = false;
    int64_t picture_pts_ = 0;
    int64_t output_order_ = 0;

    // Field picture accumulation: a frame coded as two field pictures is
    // decoded into one full-height buffer and emitted once both fields are in.
    bool field_pending_ = false;
    int field_temporal_reference_ = -1;
    int fields_present_ = 0;

    // Slice and macroblock state.
    int mb_x_ = 0;
    int mb_y_ = 0;
    int quant_scale_code_ = 16;
    const int* scan_ = nullptr;
    int dc_pred_[3] = {};
    int last_mv_[2][2][2] = {};
    bool last_intra_ = true;
    bool last_motion_forward_ = false;
    bool last_motion_backward_ = false;
    uint8_t prediction_[3][256] = {};
    uint64_t pictures_decoded_ = 0;
    uint64_t damaged_pictures_ = 0;
    const BitReader* active_reader_ = nullptr;
    int damage_mb_x_ = -1;
    int damage_mb_y_ = -1;
    size_t damage_bit_ = 0;
    const char* damage_reason_ = "";
    uint64_t pictures_dropped_ = 0;
};

}  // namespace player::decode::mpeg12
