#include "decode/mpeg12/Mpeg12Decoder.hpp"

#include "decode/mpeg12/Mpeg12Motion.hpp"
#include "decode/mpeg12/Mpeg12Tables.hpp"
#include "decode/mpeg12/Mpeg12Vlc.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace player::decode::mpeg12 {
namespace {

constexpr uint8_t kStartSequence = 0xB3;
constexpr uint8_t kStartExtension = 0xB5;
constexpr uint8_t kStartGop = 0xB8;
constexpr uint8_t kStartPicture = 0x00;
constexpr uint8_t kStartSequenceEnd = 0xB7;
constexpr uint8_t kStartUserData = 0xB2;

constexpr uint8_t kExtensionSequence = 1;
constexpr uint8_t kExtensionSequenceDisplay = 2;
constexpr uint8_t kExtensionQuantMatrix = 3;
constexpr uint8_t kExtensionPictureCoding = 8;

constexpr uint8_t kStartSliceFirst = 0x01;
constexpr uint8_t kStartSliceLast = 0xAF;

bool isSliceStart(size_t code) {
    return code >= kStartSliceFirst && code <= kStartSliceLast;
}

// Block origin inside a macroblock, in luma samples, for frame DCT.
constexpr int kBlockX[6] = {0, 8, 0, 8, 0, 0};
constexpr int kBlockY[6] = {0, 0, 8, 8, 0, 0};

}  // namespace

Decoder::Decoder() {
    std::memcpy(sequence_.intra_matrix, kDefaultIntraMatrix, sizeof(sequence_.intra_matrix));
    std::memcpy(sequence_.non_intra_matrix, kDefaultNonIntraMatrix,
                sizeof(sequence_.non_intra_matrix));
    scan_ = kZigZag;
}

Decoder::~Decoder() = default;

double Decoder::fps() const {
    const FrameRate& rate = kFrameRate[sequence_.frame_rate_code & 0x0F];
    if (rate.denominator == 0) {
        return 0.0;
    }
    double value = static_cast<double>(rate.numerator) / static_cast<double>(rate.denominator);
    // The sequence extension may scale the nominal frame rate further.
    value *= static_cast<double>(sequence_.frame_rate_extension_n + 1) /
             static_cast<double>(sequence_.frame_rate_extension_d + 1);
    return value;
}

int Decoder::quantScale() const {
    if (header_.q_scale_type) {
        return kNonLinearQScale[quant_scale_code_ & 0x1F];
    }
    return quant_scale_code_ * 2;
}

void Decoder::reset() {
    width_ = 0;
    height_ = 0;
    mb_width_ = 0;
    mb_height_ = 0;
    have_sequence_ = false;
    damaged_ = false;
    standard_ = Standard::Mpeg1;
    progressive_sequence_ = true;
    unsupported_.clear();
    sequence_ = Sequence{};
    std::memcpy(sequence_.intra_matrix, kDefaultIntraMatrix, sizeof(sequence_.intra_matrix));
    std::memcpy(sequence_.non_intra_matrix, kDefaultNonIntraMatrix,
                sizeof(sequence_.non_intra_matrix));
    header_ = PictureHeader{};
    reference_[0].release();
    reference_[1].release();
    current_.release();
    pending_b_.clear();
    ready_.clear();
    pending_.clear();
    cursor_ = 0;
    in_picture_ = false;
    picture_damaged_ = false;
    picture_unsupported_ = false;
    waiting_for_reference_ = false;
    picture_pts_ = 0;
    output_order_ = 0;
    field_pending_ = false;
    field_temporal_reference_ = -1;
    fields_present_ = 0;
    scan_ = kZigZag;
    resetPictureState();
    pictures_decoded_ = 0;
    pictures_dropped_ = 0;
    damaged_pictures_ = 0;
}

void Decoder::resetPictureState() {
    mb_x_ = 0;
    mb_y_ = 0;
    quant_scale_code_ = 16;
    last_intra_ = true;
    last_motion_forward_ = false;
    last_motion_backward_ = false;
    for (int c = 0; c < 3; ++c) {
        dc_pred_[c] = dcPredictorBase();
    }
    for (int d = 0; d < 2; ++d) {
        for (int j = 0; j < 2; ++j) {
            for (int k = 0; k < 2; ++k) {
                last_mv_[d][j][k] = 0;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// start code scanning
// ---------------------------------------------------------------------------

bool Decoder::findStartCode(size_t from, StartCode& out) const {
    for (size_t i = from; i + 3 < pending_.size(); ++i) {
        if (pending_[i] != 0 || pending_[i + 1] != 0 || pending_[i + 2] != 1) {
            continue;
        }
        out.header = i;
        out.body = i + 4;
        out.code = pending_[i + 3];
        return true;
    }
    return false;
}

void Decoder::pushSample(const uint8_t* data, size_t size, int64_t pts) {
    if (data == nullptr || size == 0) {
        return;
    }
    pending_.insert(pending_.end(), data, data + size);
    (void)pts;
    drain(false);
}

void Decoder::drain(bool at_end) {
    const auto compact = [this] {
        if (cursor_ == 0) {
            return;
        }
        size_t keep = 0;
        if (pending_.size() >= 2 && pending_[pending_.size() - 2] == 0 &&
            pending_[pending_.size() - 1] == 0) {
            keep = 2;
        } else if (!pending_.empty() && pending_.back() == 0) {
            keep = 1;
        }
        const size_t drop = std::min(cursor_, pending_.size() - keep);
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<ptrdiff_t>(drop));
        cursor_ = 0;
    };

    StartCode sc;
    if (!findStartCode(cursor_, sc)) {
        compact();
        return;
    }

    for (;;) {
        const size_t available = pending_.size() - sc.body;

        if (isSliceStart(sc.code)) {
            StartCode next;
            const bool have_next = findStartCode(sc.body, next);
            if (!have_next && !at_end) {
                cursor_ = sc.header;
                compact();
                return;
            }
            if (in_picture_) {
                decodeSlice(pending_.data() + sc.body, available, static_cast<int>(sc.code));
            }
            if (!have_next) {
                cursor_ = pending_.size();
                compact();
                return;
            }
            sc = next;
            continue;
        }

        if (in_picture_ && sc.code != kStartExtension) {
            endPicture();
        }

        if (sc.code == kStartSequenceEnd) {
            cursor_ = sc.body;
            compact();
            return;
        }

        StartCode next;
        const bool have_next = findStartCode(sc.body, next);
        if (!have_next && !at_end) {
            cursor_ = sc.header;
            compact();
            return;
        }

        BitReader reader(pending_.data() + sc.body, available);
        switch (sc.code) {
            case kStartSequence:
                parseSequenceHeader(reader);
                break;
            case kStartExtension: {
                const uint32_t id = reader.read(4);
                if (id == kExtensionSequence) {
                    parseSequenceExtension(reader);
                } else if (id == kExtensionPictureCoding) {
                    // The picture coding extension belongs to the picture that
                    // has already been started, so it must not end it.
                    parsePictureCodingExtension(reader);
                }
                // Sequence display and quantisation matrix extensions carry
                // nothing the decoder needs.
                break;
            }
            case kStartGop:
                reader.skip(25);
                break;
            case kStartPicture:
                in_picture_ = parsePictureHeader(reader, picture_pts_);
                break;
            default:
                break;
        }
        if (!have_next) {
            cursor_ = pending_.size();
            compact();
            return;
        }
        sc = next;
    }
}

int Decoder::pullFrames(std::vector<DecodedFrame>& out) {
    const int count = static_cast<int>(ready_.size());
    out.insert(out.end(), std::make_move_iterator(ready_.begin()),
               std::make_move_iterator(ready_.end()));
    ready_.clear();
    return count;
}

int Decoder::flush(std::vector<DecodedFrame>& out) {
    drain(true);
    if (in_picture_) {
        endPicture();
    }
    for (Frame& frame : pending_b_) {
        emit(frame);
    }
    pending_b_.clear();
    // Both anchors are still held: the older one never reached its second
    // successor, and the newer one is still needed as a reference by the B
    // pictures that follow it, which will never arrive now.
    emit(reference_[0]);
    reference_[0].release();
    if (reference_[1].valid()) {
        emit(reference_[1]);
        reference_[1].release();
    }
    return pullFrames(out);
}

void Decoder::markDamaged(const char* reason) {
    // Keep the first failure: the outer checks run after the inner ones and
    // would otherwise overwrite the specific reason with a generic one.
    if (!picture_damaged_) {
        damage_mb_x_ = mb_x_;
        damage_mb_y_ = mb_y_;
        damage_bit_ = active_reader_ != nullptr ? active_reader_->bitPosition() : 0;
        damage_reason_ = reason;
    }
    picture_damaged_ = true;
}

void Decoder::emit(Frame& frame) {
    if (!frame.valid()) {
        return;
    }
    DecodedFrame decoded;
    decoded.image = std::move(frame);
    decoded.pts = decoded.image.pts;
    decoded.poc = static_cast<int32_t>(output_order_++);
    decoded.image.poc = decoded.poc;
    decoded.slice_type = decoded.image.coding_type;
    decoded.qp = decoded.image.quantiser * 2;
    ready_.push_back(std::move(decoded));
    ++pictures_decoded_;
}

// ---------------------------------------------------------------------------
// headers
// ---------------------------------------------------------------------------

bool Decoder::parseSequenceHeader(BitReader& reader) {
    const uint32_t horizontal = reader.read(12);
    const uint32_t vertical = reader.read(12);
    const uint32_t aspect = reader.read(4);
    const uint32_t frame_rate = reader.read(4);
    sequence_.bit_rate = reader.read(18);
    reader.skip(1);  // marker_bit
    sequence_.vbv_buffer_size = reader.read(10) << 10;
    reader.skip(1);  // constrained_parameters_flag is not the same field any more
    const bool load_intra = reader.read(1) != 0;
    const bool load_non_intra = reader.read(1) != 0;

    std::memcpy(sequence_.intra_matrix, kDefaultIntraMatrix, sizeof(sequence_.intra_matrix));
    std::memcpy(sequence_.non_intra_matrix, kDefaultNonIntraMatrix,
                sizeof(sequence_.non_intra_matrix));
    if (load_intra) {
        for (int i = 0; i < 64; ++i) {
            sequence_.intra_matrix[i] = static_cast<uint8_t>(reader.read(8));
        }
    }
    if (load_non_intra) {
        for (int i = 0; i < 64; ++i) {
            sequence_.non_intra_matrix[i] = static_cast<uint8_t>(reader.read(8));
        }
    }

    if (horizontal == 0 || vertical == 0) {
        return false;
    }

    const bool changed = horizontal != width_ || vertical != height_;
    width_ = horizontal;
    height_ = vertical;
    mb_width_ = static_cast<int>((width_ + 15) / 16);
    mb_height_ = static_cast<int>((height_ + 15) / 16);
    sequence_.horizontal_size = horizontal;
    sequence_.vertical_size = vertical;
    sequence_.aspect_code = aspect;
    sequence_.frame_rate_code = frame_rate;
    have_sequence_ = true;

    if (changed) {
        reference_[0].release();
        reference_[1].release();
        current_.release();
        pending_b_.clear();
        for (DecodedFrame& frame : ready_) {
            frame.image.release();
        }
        ready_.clear();
        output_order_ = 0;
        field_pending_ = false;
        fields_present_ = 0;
    }
    return !reader.error();
}

bool Decoder::parseSequenceExtension(BitReader& reader) {
    standard_ = Standard::Mpeg2;
    reader.skip(8);  // profile_and_level_indication
    sequence_.progressive_sequence = reader.read(1) != 0;
    sequence_.chroma_format = static_cast<int>(reader.read(2));
    const uint32_t horizontal_extension = reader.read(2);
    const uint32_t vertical_extension = reader.read(2);
    reader.skip(12);  // bit_rate_extension
    reader.skip(1);   // marker_bit
    reader.skip(8);   // vbv_buffer_size_extension
    reader.skip(1);   // low_delay
    sequence_.frame_rate_extension_n = static_cast<int>(reader.read(2));
    sequence_.frame_rate_extension_d = static_cast<int>(reader.read(5));
    progressive_sequence_ = sequence_.progressive_sequence;

    width_ = sequence_.horizontal_size | (horizontal_extension << 12);
    height_ = sequence_.vertical_size | (vertical_extension << 12);
    mb_width_ = static_cast<int>((width_ + 15) / 16);
    mb_height_ = static_cast<int>((height_ + 15) / 16);

    if (sequence_.chroma_format != 1) {
        // 4:2:2 and 4:4:4 would need 8 more blocks per macroblock and a second
        // chroma plane kind; refuse rather than decode wrongly.
        unsupported_ = "MPEG-2 chroma_format other than 4:2:0 is not supported";
    }
    return !reader.error();
}

bool Decoder::parsePictureHeader(BitReader& reader, int64_t pts) {
    header_ = PictureHeader{};
    header_.temporal_reference = static_cast<int>(reader.read(10));
    const uint32_t coding_type = reader.read(3);
    reader.skip(16);  // vbv_delay
    // The forward and backward vectors come straight after vbv_delay. Reading
    // extra_bit_picture first shifts both by one bit, and a wrong forward_f_code
    // then reads the wrong number of motion vector extension bits, which drifts
    // the whole slice. Only P and B pictures carry them, so only those broke.
    if (coding_type == 2) {
        header_.full_pel_forward = reader.read(1) != 0;
        header_.forward_f_code = static_cast<int>(reader.read(3));
    } else if (coding_type == 3) {
        header_.full_pel_forward = reader.read(1) != 0;
        header_.forward_f_code = static_cast<int>(reader.read(3));
        header_.full_pel_backward = reader.read(1) != 0;
        header_.backward_f_code = static_cast<int>(reader.read(3));
    }
    // extra_bit_picture and its loop follow, but they only pad the header out
    // to the next start code; the slice is parsed from its own reader.
    if (coding_type < 1 || coding_type > 3) {
        return false;
    }
    header_.type = static_cast<PictureType>(coding_type);
    if (header_.forward_f_code == 0) {
        header_.forward_f_code = 1;
    }
    if (header_.backward_f_code == 0) {
        header_.backward_f_code = 1;
    }
    header_.f_code[static_cast<int>(Direction::Forward)][0] = header_.forward_f_code;
    header_.f_code[static_cast<int>(Direction::Forward)][1] = header_.forward_f_code;
    header_.f_code[static_cast<int>(Direction::Backward)][0] = header_.backward_f_code;
    header_.f_code[static_cast<int>(Direction::Backward)][1] = header_.backward_f_code;

    picture_pts_ = pts;
    picture_damaged_ = false;
    picture_unsupported_ = false;
    picture_started_ = false;
    scan_ = kZigZag;
    resetPictureState();

    const bool have_newest = reference_[1].valid();
    const bool have_previous = reference_[0].valid();
    waiting_for_reference_ =
        (header_.type == PictureType::P && !have_newest) ||
        (header_.type == PictureType::B && !(have_newest && have_previous));
    if (waiting_for_reference_) {
        ++pictures_dropped_;
        return false;
    }
    return !reader.error();
}

void Decoder::startPicture() {
    picture_started_ = true;

    const bool field = fieldPicture();
    const bool second_field = field && field_pending_ &&
                              field_temporal_reference_ == header_.temporal_reference;
    if (second_field) {
        // Decode into the frame the first field is already in.
        picture_pts_ = current_.pts;
        return;
    }

    if (field_pending_) {
        // Only one field of the previous frame ever arrived.
        endPicture();
    }
    beginPicture(picture_pts_);
    if (field) {
        field_pending_ = true;
        field_temporal_reference_ = header_.temporal_reference;
        fields_present_ = 0;
    }
}

bool Decoder::parsePictureCodingExtension(BitReader& reader) {
    standard_ = Standard::Mpeg2;
    header_.has_coding_extension = true;
    for (int direction = 0; direction < 2; ++direction) {
        for (int component = 0; component < 2; ++component) {
            int value = static_cast<int>(reader.read(4));
            if (value == 0) {
                value = 1;
            }
            header_.f_code[direction][component] = value;
        }
    }
    // full_pel does not exist in MPEG-2.
    header_.full_pel_forward = false;
    header_.full_pel_backward = false;

    header_.intra_dc_precision = static_cast<int>(reader.read(2));
    header_.structure = static_cast<PictureStructure>(reader.read(2));
    header_.top_field_first = reader.read(1) != 0;
    header_.frame_pred_frame_dct = reader.read(1) != 0;
    header_.concealment_motion_vectors = reader.read(1) != 0;
    header_.q_scale_type = reader.read(1) != 0;
    header_.intra_vlc_format = reader.read(1) != 0;
    header_.alternate_scan = reader.read(1) != 0;
    header_.repeat_first_field = reader.read(1) != 0;
    reader.skip(1);  // chroma_420_type
    header_.progressive_frame = reader.read(1) != 0;
    reader.skip(1);  // composite_display_flag

    scan_ = header_.alternate_scan ? kAlternateScan : kZigZag;
    return !reader.error();
}

void Decoder::beginPicture(int64_t pts) {
    current_.release();
    if (!current_.alloc(static_cast<int>(width_), static_cast<int>(height_), ChromaFormat::C420)) {
        waiting_for_reference_ = true;
        ++pictures_dropped_;
        return;
    }
    current_.fillNeutral();
    current_.pts = pts;
    if (fieldPicture()) {
        fields_present_ = 1;
    }
}

void Decoder::endPicture() {
    in_picture_ = false;
    if (!picture_started_ || waiting_for_reference_ || !current_.valid()) {
        picture_started_ = false;
        current_.release();
        field_pending_ = false;
        fields_present_ = 0;
        return;
    }
    picture_started_ = false;
    if (fieldPicture() && field_pending_) {
        ++fields_present_;
        if (fields_present_ < 2) {
            // Wait for the other field before displaying the frame.
            return;
        }
        field_pending_ = false;
        fields_present_ = 0;
    }
    field_pending_ = false;
    field_temporal_reference_ = -1;
    damaged_ = picture_damaged_;
    if (damaged_) {
        ++damaged_pictures_;
    }
    if (picture_unsupported_) {
        current_.release();
        return;
    }

    // Record the coding type and quantiser on the frame itself: this picture
    // may not be emitted until two more anchors have been decoded, and by then
    // header_ describes a different picture.
    switch (header_.type) {
        case PictureType::I:
            current_.coding_type = SliceType::I;
            break;
        case PictureType::P:
            current_.coding_type = SliceType::P;
            break;
        case PictureType::B:
            current_.coding_type = SliceType::B;
            break;
    }
    current_.quantiser = quant_scale_code_;

    if (header_.type == PictureType::B) {
        pending_b_.push_back(std::move(current_));
        return;
    }

    // A new anchor arrived, so the B pictures that display between the two held
    // anchors are complete. The older anchor goes out first, then those B
    // pictures. The newer anchor stays held: the B pictures that display after
    // it are still to come and need it as their backward reference, so it must
    // not be moved into the output queue yet.
    emit(reference_[0]);
    for (Frame& frame : pending_b_) {
        emit(frame);
    }
    pending_b_.clear();
    reference_[0] = std::move(reference_[1]);
    reference_[1] = std::move(current_);
}

// ---------------------------------------------------------------------------
// slices and macroblocks
// ---------------------------------------------------------------------------

void Decoder::decodeSlice(const uint8_t* data, size_t size, int slice_start_code) {
    if (waiting_for_reference_ || !in_picture_) {
        return;
    }
    if (!picture_started_) {
        startPicture();
    }
    if (!current_.valid() || picture_unsupported_) {
        return;
    }
    BitReader reader(data, size);
    active_reader_ = &reader;

    // The vertical position is the low seven bits of the slice start code, so
    // code 0x01 is the first macroblock row. Very tall MPEG-2 pictures extend
    // it with three more bits.
    int slice_vertical = slice_start_code - kStartSliceFirst;
    if (standard_ == Standard::Mpeg2 && mb_height_ > 2800 / 16) {
        slice_vertical += static_cast<int>(reader.read(3)) << 7;
    }

    quant_scale_code_ = static_cast<int>(reader.read(5));
    // The slice header ends with skip_1stop_8data_bits. A conformant encoder
    // writes quantiser_scale_code followed by a single zero bit, so the
    // macroblock data starts six bits into the payload.
    if (!reader.skip1Stop8DataBits()) {
        markDamaged("slice header");
    }

    mb_x_ = 0;
    mb_y_ = slice_vertical;

    for (int c = 0; c < 3; ++c) {
        dc_pred_[c] = dcPredictorBase();
    }
    for (int d = 0; d < 2; ++d) {
        for (int j = 0; j < 2; ++j) {
            for (int k = 0; k < 2; ++k) {
                last_mv_[d][j][k] = 0;
            }
        }
    }
    last_intra_ = true;
    last_motion_forward_ = false;
    last_motion_backward_ = false;

    decodeSliceBody(reader);
    if (reader.error()) {
        markDamaged("slice body read past end");
    }
    active_reader_ = nullptr;
}

void Decoder::decodeSliceBody(BitReader& reader) {
    int skip_run = -1;

    for (;;) {
        if (mb_y_ >= mb_height_) {
            return;
        }

        if (skip_run == -1) {
            skip_run = 0;
            for (;;) {
                const int symbol = readVlc(reader, kMbAddrIncr, kMbAddrIncrCount, 11);
                if (symbol < 0) {
                    markDamaged("macroblock address increment");
                    return;
                }
                if (symbol == kMbAddrIncrEnd) {
                    return;
                }
                if (symbol == kMbAddrIncrEscape) {
                    skip_run += 33;
                    continue;
                }
                if (symbol == kMbAddrIncrStuffing) {
                    continue;
                }
                skip_run += symbol;
                break;
            }
            if (skip_run != 0 && header_.type == PictureType::I) {
                markDamaged("skipped macroblock in an I picture");
                return;
            }
        }

        if ((skip_run--) != 0) {
            skipMacroblock();
        } else {
            decodeMacroblock(reader);
        }

        ++mb_x_;
        if (mb_x_ >= mb_width_) {
            mb_x_ = 0;
            ++mb_y_;
            if (mb_y_ >= mb_height_) {
                return;
            }
        }
        if (reader.error()) {
            markDamaged("slice loop read past end");
            return;
        }
    }
}

void Decoder::skipMacroblock() {
    MacroblockFlags flags;
    MotionVector mv[2][2];

    if (header_.type == PictureType::B) {
        if (last_intra_) {
            markDamaged("skipped B macroblock after an intra one");
        }
        flags.intra = false;
        flags.motion_forward = last_motion_forward_;
        flags.motion_backward = last_motion_backward_;
        for (int d = 0; d < 2; ++d) {
            for (int half = 0; half < 2; ++half) {
                mv[d][half].x = last_mv_[d][half][0];
                mv[d][half].y = last_mv_[d][half][1];
            }
        }
        motionCompensate(flags, mv);
    } else {
        flags.intra = false;
        flags.motion_forward = true;
        flags.motion_backward = false;
        for (int k = 0; k < 2; ++k) {
            last_mv_[static_cast<int>(Direction::Forward)][0][k] = 0;
            last_mv_[static_cast<int>(Direction::Forward)][1][k] = 0;
        }
        motionCompensate(flags, mv);
        last_intra_ = false;
        last_motion_forward_ = true;
        last_motion_backward_ = false;
    }

    for (int c = 0; c < 3; ++c) {
        dc_pred_[c] = dcPredictorBase();
    }
}

void Decoder::decodeMacroblock(BitReader& reader) {
    MacroblockFlags flags;
    MotionVector mv[2][2];

    switch (header_.type) {
        case PictureType::I: {
            bool quant_type = false;
            const int type = readMbTypeI(reader, quant_type);
            if (type < 0) {
                markDamaged("macroblock_type (I)");
                return;
            }
            flags.intra = true;
            flags.quant_type = quant_type;
            break;
        }
        case PictureType::P: {
            const int type = readMbTypeP(reader);
            if (type < 0) {
                markDamaged("macroblock_type (P)");
                return;
            }
            const MbTypeP& entry = kMbTypeP[type];
            flags.intra = entry.intra != 0;
            flags.motion_forward = entry.motion_forward != 0;
            flags.has_pattern = entry.pattern != 0;
            flags.quant_type = entry.quant_type != 0;
            flags.zero_mv = type == 1 || type == 5;
            break;
        }
        case PictureType::B: {
            const int type = readMbTypeB(reader);
            if (type < 0) {
                markDamaged("macroblock_type (B)");
                return;
            }
            const MbTypeB& entry = kMbTypeB[type];
            flags.intra = entry.intra != 0;
            flags.motion_forward = entry.motion_forward != 0;
            flags.motion_backward = entry.motion_backward != 0;
            flags.has_pattern = entry.pattern != 0;
            flags.quant_type = entry.quant_type != 0;
            break;
        }
    }

    // The DCT type is only coded for frame pictures that do not force frame
    // DCT, and it comes before the quantiser scale.
    const bool dct_type_coded =
        standard_ == Standard::Mpeg2 && header_.structure == PictureStructure::Frame &&
        !header_.frame_pred_frame_dct;
    mb_interlaced_dct_ = false;

    if (flags.intra) {
        if (dct_type_coded) {
            mb_interlaced_dct_ = reader.read(1) != 0;
        }
    } else if (flags.zero_mv) {
        if (dct_type_coded) {
            mb_interlaced_dct_ = reader.read(1) != 0;
        }
        flags.motion_type = MotionType::Frame;
    } else {
        if (header_.structure == PictureStructure::Frame && header_.frame_pred_frame_dct) {
            flags.motion_type = MotionType::Frame;
        } else {
            flags.field_motion = true;
            const int type = static_cast<int>(reader.read(2));
            if (type < static_cast<int>(MotionType::Field) ||
                type > static_cast<int>(MotionType::DualPrime)) {
                markDamaged("motion_type");
                return;
            }
            flags.motion_type = static_cast<MotionType>(type);
        }
        // With explicit vectors the DCT type is only present when the
        // macroblock also codes a pattern.
        if (dct_type_coded && flags.has_pattern) {
            mb_interlaced_dct_ = reader.read(1) != 0;
        }
    }

    if (flags.motion_type == MotionType::DualPrime) {
        // Dual prime needs its own prediction path; refuse rather than decode
        // the picture with the wrong motion vectors.
        picture_unsupported_ = true;
        unsupported_ = "MPEG-2 dual prime prediction is not supported";
        return;
    }

    if (flags.quant_type) {
        quant_scale_code_ = static_cast<int>(reader.read(5));
        if (quant_scale_code_ == 0) {
            quant_scale_code_ = 1;
        }
    }

    // The syntax reads the motion vectors before the coded block pattern.
    // Reading them the other way round misaligns every inter macroblock, which
    // is why intra pictures decoded and predicted ones did not.
    if (flags.intra) {
        // Intra macroblocks have no coded vectors; the concealment vectors, if
        // present, are parsed and discarded.
        if (header_.concealment_motion_vectors) {
            if (header_.structure != PictureStructure::Frame) {
                reader.skip(1);  // field_select
            }
            last_mv_[0][0][0] = readMotionVector(reader, last_mv_[0][0][0], header_.f_code[0][0]);
            last_mv_[0][1][0] = last_mv_[0][0][0];
            last_mv_[0][0][1] = readMotionVector(reader, last_mv_[0][0][1], header_.f_code[0][1]);
            last_mv_[0][1][1] = last_mv_[0][0][1];
            if (reader.read(1) != 1) {  // marker_bit
                markDamaged("concealment marker bit");
            }
        } else {
            for (int d = 0; d < 2; ++d) {
                for (int j = 0; j < 2; ++j) {
                    for (int k = 0; k < 2; ++k) {
                        last_mv_[d][j][k] = 0;
                    }
                }
            }
        }
    } else {
        readMotionVectors(reader, flags, mv);
    }

    if (flags.has_pattern) {
        flags.coded_block_pattern = readCodedBlockPattern(reader);
        if (flags.coded_block_pattern < 0) {
            markDamaged("coded_block_pattern");
            return;
        }
    }

    if (flags.intra) {
        for (int i = 0; i < 4; ++i) {
            decodeBlockIntra(reader, i, 0, quantScale());
        }
        decodeBlockIntra(reader, 4, 1, quantScale());
        decodeBlockIntra(reader, 5, 2, quantScale());
        last_intra_ = true;
        last_motion_forward_ = false;
        last_motion_backward_ = false;
        if (reader.error()) {
            markDamaged("intra block read past end");
        }
        return;
    }

    motionCompensate(flags, mv);

    Block blocks[6];
    for (int i = 0; i < 6; ++i) {
        clearBlock(blocks[i]);
    }
    const int cbp = flags.coded_block_pattern;
    for (int i = 0; i < 4; ++i) {
        if (cbp & (0x20 >> i)) {
            decodeBlockInter(reader, i, quantScale());
        }
    }
    for (int i = 4; i < 6; ++i) {
        if (cbp & (0x02 >> (i - 4))) {
            decodeBlockInter(reader, i, quantScale());
        }
    }

    for (int c = 0; c < 3; ++c) {
        dc_pred_[c] = dcPredictorBase();
    }
    last_intra_ = false;
    last_motion_forward_ = flags.motion_forward;
    last_motion_backward_ = flags.motion_backward;
    if (reader.error()) {
        markDamaged("inter block read past end");
    }
}

// ---------------------------------------------------------------------------
// blocks
// ---------------------------------------------------------------------------

PlaneGrid Decoder::blockGrid(int plane, int block_index) const {
    if (fieldPicture()) {
        // A field picture writes to its own parity of the frame.
        return fieldGrid(current_, plane, fieldParity());
    }
    if (mb_interlaced_dct_ && plane == 0) {
        // Field DCT: the two luma halves of the macroblock go to the two
        // parities of the same eight-line span.
        return fieldGrid(current_, plane, block_index >= 2 ? 1 : 0);
    }
    return frameGrid(current_, plane);
}

void Decoder::blockPosition(int plane, int block_index, int& x, int& y) const {
    if (plane != 0) {
        // The chroma blocks are always frame DCT in 4:2:0.
        x = mb_x_ * 8;
        y = mb_y_ * 8;
        return;
    }
    if (fieldPicture()) {
        x = mb_x_ * 16 + (block_index & 1) * 8;
        y = mb_y_ * 16 + (block_index >> 1) * 8;
        return;
    }
    if (mb_interlaced_dct_) {
        x = mb_x_ * 16 + (block_index & 1) * 8;
        y = mb_y_ * 8;
        return;
    }
    x = mb_x_ * 16 + kBlockX[block_index];
    y = mb_y_ * 16 + kBlockY[block_index];
}

void Decoder::storeBlock(const Block& block, int plane, int block_index, bool intra) {
    if (!current_.valid() || current_.plane(plane) == nullptr) {
        return;
    }
    const PlaneGrid grid = blockGrid(plane, block_index);
    int x = 0;
    int y = 0;
    blockPosition(plane, block_index, x, y);
    if (x < 0 || y < 0 || x + 8 > grid.width || y + 8 > grid.height) {
        return;
    }
    uint8_t* dst = grid.data + static_cast<size_t>(y) * grid.stride + x;
    if (intra) {
        idctPutBlock(block, dst, grid.stride);
    } else {
        idctAddBlock(block, dst, grid.stride);
    }
}

void Decoder::decodeBlockIntra(BitReader& reader, int block_index, int component,
                               int quant_scale) {
    (void)block_index;
    Block block;
    clearBlock(block);

    const uint8_t* matrix = sequence_.intra_matrix;
    dc_pred_[component] += readIntraDcDifference(reader, component);
    if (standard_ == Standard::Mpeg2) {
        // The predictor already carries the precision shift, so the level is
        // scaled back by 3 - intra_dc_precision.
        block.coef[0] =
            static_cast<int16_t>(dc_pred_[component] * (1 << (3 - header_.intra_dc_precision)));
    } else {
        block.coef[0] = static_cast<int16_t>(dc_pred_[component] * matrix[0]);
    }

    const int* scan_order = scan();
    const AcTable table = (standard_ == Standard::Mpeg2 && header_.intra_vlc_format)
                              ? AcTable::Mpeg2
                              : AcTable::Mpeg1;
    int mismatch = standard_ == Standard::Mpeg2 ? (block.coef[0] ^ 1) : 0;
    int index = 0;
    for (;;) {
        int level = 0;
        if (!readAcCoefficient(reader, table, index, level)) {
            break;
        }
        const int position = scan_order[index];
        const int value =
            inverseQuantize(level, quant_scale, matrix[position], true,
                            standard_ != Standard::Mpeg2);
        block.coef[position] = static_cast<int16_t>(value);
        if (standard_ == Standard::Mpeg2) {
            mismatch ^= value;
        }
    }
    if (standard_ == Standard::Mpeg2) {
        // Mismatch control: an odd sum has to be corrected.
        block.coef[63] ^= mismatch & 1;
    }

    storeBlock(block, block_index < 4 ? 0 : (block_index == 4 ? 1 : 2), block_index, true);
}

void Decoder::decodeBlockInter(BitReader& reader, int block_index, int quant_scale) {
    Block block;
    clearBlock(block);

    const uint8_t* matrix = sequence_.non_intra_matrix;
    const bool mpeg2 = standard_ == Standard::Mpeg2;
    const int* scan_order = scan();
    int mismatch = 1;
    int index = -1;
    bool more = true;

    if (reader.peek(1) != 0) {
        // A DC magnitude of one is signalled with a '1' bit and the sign.
        reader.skip(1);
        const int sign = reader.read(1) != 0 ? -1 : 1;
        const int value = inverseQuantize(sign, quant_scale, matrix[0], false, !mpeg2);
        block.coef[0] = static_cast<int16_t>(value);
        mismatch ^= value;
        index = 0;
    } else {
        int level = 0;
        more = readAcCoefficient(reader, AcTable::Mpeg1, index, level);
        if (more && index >= 0) {
            const int position = scan_order[index];
            const int value = inverseQuantize(level, quant_scale, matrix[position], false, !mpeg2);
            block.coef[position] = static_cast<int16_t>(value);
            mismatch ^= value;
        }
    }

    while (more) {
        int level = 0;
        more = readAcCoefficient(reader, AcTable::Mpeg1, index, level);
        if (more) {
            const int position = scan_order[index];
            const int value = inverseQuantize(level, quant_scale, matrix[position], false, !mpeg2);
            block.coef[position] = static_cast<int16_t>(value);
            mismatch ^= value;
        }
    }
    if (mpeg2) {
        block.coef[63] ^= mismatch & 1;
    }

    storeBlock(block, block_index < 4 ? 0 : (block_index == 4 ? 1 : 2), block_index, false);
}

// ---------------------------------------------------------------------------
// motion compensation
// ---------------------------------------------------------------------------

void Decoder::predictDirection(const Frame& reference, const MacroblockFlags& flags,
                               const MotionVector mv[2]) {
    if (!reference.valid()) {
        return;
    }
    const bool field_picture = fieldPicture();
    const int luma_x = mb_x_ * 16;
    const int chroma_x = mb_x_ * 8;

    if (flags.motion_type == MotionType::Frame && !field_picture) {
        PlaneGrid dst = frameGrid(current_, 0);
        PlaneGrid src = frameGrid(reference, 0);
        predictBlock(dst, src, luma_x, mb_y_ * 16, 16, 16, mv[0].x, mv[0].y);
        for (int plane = 1; plane < 3; ++plane) {
            PlaneGrid dst_c = frameGrid(current_, plane);
            PlaneGrid src_c = frameGrid(reference, plane);
            predictBlock(dst_c, src_c, chroma_x, mb_y_ * 8, 8, 8, mv[0].x / 2, mv[0].y / 2);
        }
        return;
    }

    if (flags.motion_type == MotionType::Frame) {
        // 16x8 in a field picture: two halves of the field.
        for (int half = 0; half < 2; ++half) {
            PlaneGrid dst = fieldGrid(current_, 0, fieldParity());
            PlaneGrid src = fieldGrid(reference, 0, mv[half].field_select);
            predictBlock(dst, src, luma_x, mb_y_ * 16 + half * 8, 16, 8, mv[half].x,
                         mv[half].y);
            for (int plane = 1; plane < 3; ++plane) {
                PlaneGrid dst_c = fieldGrid(current_, plane, fieldParity());
                PlaneGrid src_c = fieldGrid(reference, plane, mv[half].field_select);
                predictBlock(dst_c, src_c, chroma_x, mb_y_ * 8 + half * 4, 8, 4, mv[half].x / 2,
                             mv[half].y / 2);
            }
        }
        return;
    }

    if (field_picture) {
        // 16x16 from one field of the reference.
        PlaneGrid dst = fieldGrid(current_, 0, fieldParity());
        PlaneGrid src = fieldGrid(reference, 0, mv[0].field_select);
        predictBlock(dst, src, luma_x, mb_y_ * 16, 16, 16, mv[0].x, mv[0].y);
        for (int plane = 1; plane < 3; ++plane) {
            PlaneGrid dst_c = fieldGrid(current_, plane, fieldParity());
            PlaneGrid src_c = fieldGrid(reference, plane, mv[0].field_select);
            predictBlock(dst_c, src_c, chroma_x, mb_y_ * 8, 8, 8, mv[0].x / 2, mv[0].y / 2);
        }
        return;
    }

    // Field prediction in a frame picture: each 16x8 half comes from its own
    // reference field and lands on the matching parity of the frame.
    for (int half = 0; half < 2; ++half) {
        PlaneGrid dst = fieldGrid(current_, 0, half);
        PlaneGrid src = fieldGrid(reference, 0, mv[half].field_select);
        predictBlock(dst, src, luma_x, mb_y_ * 8, 16, 8, mv[half].x, mv[half].y);
        for (int plane = 1; plane < 3; ++plane) {
            PlaneGrid dst_c = fieldGrid(current_, plane, half);
            PlaneGrid src_c = fieldGrid(reference, plane, mv[half].field_select);
            predictBlock(dst_c, src_c, chroma_x, mb_y_ * 4, 8, 4, mv[half].x / 2, mv[half].y / 2);
        }
    }
}

void Decoder::savePrediction() {
    if (!current_.valid()) {
        return;
    }
    // Stash the macroblock of the destination grid into the scratch planes.
    const bool field_picture = fieldPicture();
    for (int plane = 0; plane < 3; ++plane) {
        const int width = plane == 0 ? 16 : 8;
        const int height = plane == 0 ? 16 : 8;
        PlaneGrid grid = plane == 0 ? (field_picture ? fieldGrid(current_, 0, fieldParity())
                                                      : frameGrid(current_, 0))
                                    : (field_picture ? fieldGrid(current_, plane, fieldParity())
                                                     : frameGrid(current_, plane));
        const int x = plane == 0 ? mb_x_ * 16 : mb_x_ * 8;
        const int y = plane == 0 ? mb_y_ * 16 : mb_y_ * 8;
        PlaneGrid scratch;
        scratch.data = prediction_[plane];
        scratch.stride = width;
        scratch.width = width;
        scratch.height = height;
        copyBlock(scratch, 0, 0, grid, x, y, width, height);
    }
}

void Decoder::averageWithPrediction() {
    if (!current_.valid()) {
        return;
    }
    const bool field_picture = fieldPicture();
    for (int plane = 0; plane < 3; ++plane) {
        const int width = plane == 0 ? 16 : 8;
        const int height = plane == 0 ? 16 : 8;
        PlaneGrid grid = plane == 0 ? (field_picture ? fieldGrid(current_, 0, fieldParity())
                                                      : frameGrid(current_, 0))
                                    : (field_picture ? fieldGrid(current_, plane, fieldParity())
                                                     : frameGrid(current_, plane));
        const int x = plane == 0 ? mb_x_ * 16 : mb_x_ * 8;
        const int y = plane == 0 ? mb_y_ * 16 : mb_y_ * 8;
        PlaneGrid scratch;
        scratch.data = prediction_[plane];
        scratch.stride = width;
        scratch.width = width;
        scratch.height = height;
        averageBlock(grid, x, y, scratch, 0, 0, width, height);
    }
}

void Decoder::readMotionVectors(BitReader& reader, const MacroblockFlags& flags,
                                MotionVector mv[2][2]) {
    if (flags.zero_mv) {
        for (int k = 0; k < 2; ++k) {
            last_mv_[0][0][k] = 0;
            last_mv_[0][1][k] = 0;
        }
        return;
    }
    if (!flags.motion_forward && !flags.motion_backward) {
        // Neither direction is coded, so the macroblock repeats the previous
        // macroblock's prediction.
        for (int d = 0; d < 2; ++d) {
            for (int half = 0; half < 2; ++half) {
                mv[d][half].x = last_mv_[d][half][0];
                mv[d][half].y = last_mv_[d][half][1];
            }
        }
        return;
    }

    const bool field_picture = header_.structure != PictureStructure::Frame;
    for (int direction = 0; direction < 2; ++direction) {
        const bool forward = direction == static_cast<int>(Direction::Forward);
        if (forward ? !flags.motion_forward : !flags.motion_backward) {
            continue;
        }
        const int fcode_x = header_.f_code[direction][0];
        const int fcode_y = header_.f_code[direction][1];
        if (flags.motion_type == MotionType::Frame && !field_picture) {
            // 16x16 frame prediction: one vector, shared by both halves.
            const int x = readMotionVector(reader, last_mv_[direction][0][0], fcode_x);
            const int y = readMotionVector(reader, last_mv_[direction][0][1], fcode_y);
            for (int half = 0; half < 2; ++half) {
                last_mv_[direction][half][0] = x;
                last_mv_[direction][half][1] = y;
                mv[direction][half].x = x;
                mv[direction][half].y = y;
            }
        } else if (flags.motion_type == MotionType::Frame) {
            // 16x8 in a field picture: a vector per half, each with its own
            // reference field.
            for (int half = 0; half < 2; ++half) {
                mv[direction][half].field_select = static_cast<int>(reader.read(1));
                mv[direction][half].x =
                    readMotionVector(reader, last_mv_[direction][half][0], fcode_x);
                mv[direction][half].y =
                    readMotionVector(reader, last_mv_[direction][half][1], fcode_y);
                last_mv_[direction][half][0] = mv[direction][half].x;
                last_mv_[direction][half][1] = mv[direction][half].y;
            }
        } else if (!field_picture) {
            // Field prediction in a frame picture: the vertical component is
            // coded in field lines, so its predictor is halved.
            for (int half = 0; half < 2; ++half) {
                mv[direction][half].field_select = static_cast<int>(reader.read(1));
                const int predicted_y = last_mv_[direction][half][1] >> 1;
                const int y = readMotionVector(reader, predicted_y, fcode_y);
                mv[direction][half].x =
                    readMotionVector(reader, last_mv_[direction][half][0], fcode_x);
                mv[direction][half].y = y;
                last_mv_[direction][half][0] = mv[direction][half].x;
                last_mv_[direction][half][1] = 2 * y;
            }
        } else {
            // Field prediction in a field picture: one vector in field units,
            // shared by both halves.
            mv[direction][0].field_select = static_cast<int>(reader.read(1));
            mv[direction][0].x = readMotionVector(reader, last_mv_[direction][0][0], fcode_x);
            mv[direction][0].y = readMotionVector(reader, last_mv_[direction][0][1], fcode_y);
            for (int half = 0; half < 2; ++half) {
                mv[direction][half].field_select = mv[direction][0].field_select;
                mv[direction][half].x = mv[direction][0].x;
                mv[direction][half].y = mv[direction][0].y;
                last_mv_[direction][half][0] = mv[direction][0].x;
                last_mv_[direction][half][1] = mv[direction][0].y;
            }
        }
        // MPEG-1 picture_full_pel doubles the reconstructed vector.
        const bool full_pel = forward ? header_.full_pel_forward : header_.full_pel_backward;
        if (full_pel) {
            for (int half = 0; half < 2; ++half) {
                mv[direction][half].x *= 2;
                mv[direction][half].y *= 2;
            }
        }
    }
}

void Decoder::motionCompensate(const MacroblockFlags& flags, const MotionVector mv[2][2]) {
    if (flags.intra || !current_.valid()) {
        return;
    }

    const bool bidirectional = header_.type == PictureType::B;
    const Frame& forward = reference_[bidirectional ? 0 : 1];
    const Frame& backward = reference_[1];
    const bool can_average =
        flags.motion_forward && flags.motion_backward && forward.valid() && backward.valid();

    if (flags.motion_forward) {
        predictDirection(forward, flags, mv[0]);
        if (can_average) {
            savePrediction();
        }
    }
    if (flags.motion_backward) {
        predictDirection(backward, flags, mv[1]);
        if (can_average) {
            averageWithPrediction();
        }
    }
}

}  // namespace player::decode::mpeg12
