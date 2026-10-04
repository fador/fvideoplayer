#include "decode/flv1/Flv1Decoder.hpp"

#include "decode/flv1/Flv1Tables.hpp"
#include "decode/mpeg12/Mpeg12Vlc.hpp"

#include <algorithm>
#include <cstring>

namespace player::decode::flv1 {
namespace {

using mpeg12::BitReader;
using mpeg12::Block;
using mpeg12::clearBlock;
using mpeg12::idctAddBlock;
using mpeg12::idctPutBlock;
using mpeg12::kZigZag;
using mpeg12::readVlc;

int midPred(int a, int b, int c) {
    return std::max(std::min(a, b), std::min(std::max(a, b), c));
}

int clampQscale(int value) {
    return std::clamp(value, 1, 31);
}

int roundChroma(int value) {
    return kChromaRoundTable[value & 0xF] + (value >> 3);
}

// H.263 inverse quantisation. The DC coefficient has a fixed quantiser of 8;
// the AC coefficients use 2*q and (q-1)|1.
void unquantizeIntra(Block& block, int qscale) {
    block.coef[0] = static_cast<int16_t>(block.coef[0] * 8);
    const int qmul = qscale << 1;
    const int qadd = (qscale - 1) | 1;
    for (int i = 1; i < 64; ++i) {
        const int level = block.coef[i];
        if (level != 0) {
            block.coef[i] =
                static_cast<int16_t>(level < 0 ? level * qmul - qadd : level * qmul + qadd);
        }
    }
}

void unquantizeInter(Block& block, int qscale) {
    const int qmul = qscale << 1;
    const int qadd = (qscale - 1) | 1;
    for (int i = 0; i < 64; ++i) {
        const int level = block.coef[i];
        if (level != 0) {
            block.coef[i] =
                static_cast<int16_t>(level < 0 ? level * qmul - qadd : level * qmul + qadd);
        }
    }
}

// Edge replication, matching the reference decoder's emulated edge.
int edgeSample(const uint8_t* plane, int stride, int width, int height, int x, int y) {
    x = std::clamp(x, 0, width - 1);
    y = std::clamp(y, 0, height - 1);
    return plane[static_cast<size_t>(y) * stride + x];
}

// Half-pel bilinear copy: (a+b+1)/2 along one axis, (a+b+c+d+2)/4 along both.
void copyBlock(const uint8_t* src, int src_stride, int src_width, int src_height, int src_x,
               int src_y, int frac_x, int frac_y, uint8_t* dst, int dst_stride, int width,
               int height) {
    for (int y = 0; y < height; ++y) {
        uint8_t* out = dst + static_cast<size_t>(y) * dst_stride;
        for (int x = 0; x < width; ++x) {
            const int a = edgeSample(src, src_stride, src_width, src_height, src_x + x, src_y + y);
            if (frac_x && frac_y) {
                const int b =
                    edgeSample(src, src_stride, src_width, src_height, src_x + x + 1, src_y + y);
                const int c =
                    edgeSample(src, src_stride, src_width, src_height, src_x + x, src_y + y + 1);
                const int d =
                    edgeSample(src, src_stride, src_width, src_height, src_x + x + 1, src_y + y + 1);
                out[x] = static_cast<uint8_t>((a + b + c + d + 2) >> 2);
            } else if (frac_x) {
                const int b =
                    edgeSample(src, src_stride, src_width, src_height, src_x + x + 1, src_y + y);
                out[x] = static_cast<uint8_t>((a + b + 1) >> 1);
            } else if (frac_y) {
                const int c =
                    edgeSample(src, src_stride, src_width, src_height, src_x + x, src_y + y + 1);
                out[x] = static_cast<uint8_t>((a + c + 1) >> 1);
            } else {
                out[x] = static_cast<uint8_t>(a);
            }
        }
    }
}

void copyPlane(const Frame& src, int plane, Frame& dst) {
    const size_t row_bytes = static_cast<size_t>(src.planeWidth(plane)) * src.pixelSize();
    for (int y = 0; y < src.planeHeight(plane); ++y) {
        std::memcpy(dst.row(plane, y), src.row(plane, y), row_bytes);
    }
}

}  // namespace

Decoder::Decoder() = default;

Decoder::~Decoder() = default;

void Decoder::reset() {
    current_ = Frame();
    reference_ = Frame();
    have_reference_ = false;
    picture_intra_ = true;
    format_ = 0;
    width_ = height_ = 0;
    mb_width_ = mb_height_ = 0;
    b8_stride_ = 0;
    mb_x_ = mb_y_ = 0;
    qscale_ = 1;
    first_slice_line_ = true;
    resync_mb_x_ = resync_mb_y_ = 0;
    mv_x_.clear();
    mv_y_.clear();
    ready_.clear();
    pictures_decoded_ = 0;
    damaged_ = false;
    unsupported_.clear();
}

void Decoder::pushSample(const uint8_t* data, size_t size, int64_t pts) {
    if (data == nullptr || size == 0) {
        return;
    }
    (void)decodePicture(data, size, pts);
}

int Decoder::pullFrames(std::vector<DecodedFrame>& out) {
    const int count = static_cast<int>(ready_.size());
    for (DecodedFrame& frame : ready_) {
        out.push_back(std::move(frame));
    }
    ready_.clear();
    return count;
}

int Decoder::flush(std::vector<DecodedFrame>& out) {
    (void)out;
    return 0;
}

bool Decoder::parseHeader(BitReader& reader, PictureHeader& header) {
    if (reader.read(17) != 1) {
        unsupported_ = "bad FLV1 picture start code";
        return false;
    }
    format_ = static_cast<int>(reader.read(5));
    if (format_ != 0 && format_ != 1) {
        unsupported_ = "unsupported FLV1 escape format";
        return false;
    }
    header.timestamp = static_cast<int>(reader.read(8));
    const uint32_t size_code = reader.read(3);
    switch (size_code) {
        case 0:
            header.width = reader.read(8);
            header.height = reader.read(8);
            break;
        case 1:
            header.width = reader.read(16);
            header.height = reader.read(16);
            break;
        case 2:
            header.width = 352;
            header.height = 288;
            break;
        case 3:
            header.width = 176;
            header.height = 144;
            break;
        case 4:
            header.width = 128;
            header.height = 96;
            break;
        case 5:
            header.width = 320;
            header.height = 240;
            break;
        case 6:
            header.width = 160;
            header.height = 120;
            break;
        default:
            unsupported_ = "reserved FLV1 picture size code";
            return false;
    }
    header.intra = reader.read(2) == 0;
    reader.skip(1);  // deblocking flag; FLV1 does not use the H.263 loop filter
    header.quantiser = static_cast<int>(reader.read(5));
    // PEI: a one bit followed by an eight-bit PSPARE byte, repeated.
    while (reader.bitsLeft() >= 1 && reader.read(1) != 0) {
        if (reader.bitsLeft() < 8) {
            break;
        }
        reader.skip(8);
    }
    if (reader.error() || header.width == 0 || header.height == 0 || header.quantiser == 0) {
        return false;
    }
    return true;
}

bool Decoder::decodePicture(const uint8_t* data, size_t size, int64_t pts) {
    BitReader reader(data, size);
    PictureHeader header;
    if (!parseHeader(reader, header)) {
        damaged_ = true;
        return false;
    }
    // A droppable P picture with no reference is skipped, not an error.
    if (!header.intra && !have_reference_) {
        return false;
    }
    if (static_cast<int>(header.width) != width_ ||
        static_cast<int>(header.height) != height_) {
        have_reference_ = false;
        if (!header.intra) {
            return false;
        }
    }
    width_ = static_cast<int>(header.width);
    height_ = static_cast<int>(header.height);
    mb_width_ = (width_ + 15) / 16;
    mb_height_ = (height_ + 15) / 16;
    b8_stride_ = mb_width_ * 2 + 1;
    const int coded_width = mb_width_ * 16;
    const int coded_height = mb_height_ * 16;
    if (!current_.alloc(coded_width, coded_height, ChromaFormat::C420, 8)) {
        damaged_ = true;
        return false;
    }
    current_.crop_right_ = coded_width - width_;
    current_.crop_bottom_ = coded_height - height_;
    current_.pts = pts;
    picture_intra_ = header.intra;
    qscale_ = clampQscale(header.quantiser);
    clearMotionGrid();
    mb_x_ = 0;
    mb_y_ = 0;
    for (;;) {
        if (!decodeSlice(reader)) {
            damaged_ = true;
            break;
        }
        if (mb_y_ >= mb_height_) {
            break;
        }
        if (!parseGobHeader(reader)) {
            damaged_ = true;
            break;
        }
    }
    if (reader.error()) {
        damaged_ = true;
    }
    emitFrame(pts, header);
    reference_ = std::move(current_);
    have_reference_ = true;
    ++pictures_decoded_;
    return true;
}

bool Decoder::decodeSlice(BitReader& reader) {
    first_slice_line_ = true;
    resync_mb_x_ = mb_x_;
    resync_mb_y_ = mb_y_;
    while (mb_y_ < mb_height_) {
        if (resync_mb_x_ == mb_x_ && resync_mb_y_ + 1 == mb_y_) {
            first_slice_line_ = false;
        }
        if (!decodeMacroblock(reader, mb_x_, mb_y_)) {
            return false;
        }
        // A zero window means the GOB start code (16+ zero bits) follows.
        const size_t left = reader.bitsLeft();
        uint32_t window = 0;
        if (left >= 16) {
            window = reader.peek(16);
        } else if (left > 0) {
            window = reader.peek(static_cast<unsigned>(left)) << (16 - left);
        }
        if (++mb_x_ >= mb_width_) {
            mb_x_ = 0;
            ++mb_y_;
        }
        if (window == 0) {
            return true;
        }
    }
    return true;
}

bool Decoder::parseGobHeader(BitReader& reader) {
    if (reader.bitsLeft() < 17 || reader.peek(16) != 0) {
        return false;
    }
    reader.skip(16);
    bool found = false;
    while (reader.bitsLeft() > 13) {
        if (reader.read(1) != 0) {
            found = true;
            break;
        }
    }
    if (!found) {
        return false;
    }
    const int gob_number = static_cast<int>(reader.read(5));
    reader.skip(2);  // GFID
    const int quantiser = static_cast<int>(reader.read(5));
    if (quantiser == 0) {
        return false;
    }
    // Pictures up to 400 lines use one macroblock row per GOB.
    mb_x_ = 0;
    mb_y_ = gob_number;
    if (mb_y_ >= mb_height_) {
        return false;
    }
    qscale_ = quantiser;
    return true;
}

bool Decoder::decodeMacroblock(BitReader& reader, int mb_x, int mb_y) {
    Block blocks[6];
    for (Block& block : blocks) {
        clearBlock(block);
    }
    bool intra = false;
    int cbp = 0;
    int mv[4][2] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
    bool use_4mv = false;

    if (picture_intra_) {
        int cbpc = 0;
        do {
            cbpc = readVlc(reader, kIntraMcbpc, 9, 9);
        } while (cbpc == 8);
        if (cbpc < 0) {
            return false;
        }
        const bool dquant = (cbpc & 4) != 0;
        const int cbpy = readVlc(reader, kCbpy, 16, 6);
        if (cbpy < 0) {
            return false;
        }
        cbp = (cbpc & 3) | (cbpy << 2);
        if (dquant) {
            decodeDquant(reader);
        }
        intra = true;
    } else {
        if (reader.read(1) != 0) {
            // Not coded: the macroblock is the reference at the same position.
            motionCompensate(mb_x, mb_y, mv, false);
            updateMotionVal(mb_x, mb_y, false, mv, false);
            return true;
        }
        int cbpc = 0;
        do {
            cbpc = readVlc(reader, kInterMcbpc, 25, 9);
        } while (cbpc == 20);
        if (cbpc < 0) {
            return false;
        }
        const bool dquant = (cbpc & 8) != 0;
        intra = (cbpc & 4) != 0;
        const int cbpy = readVlc(reader, kCbpy, 16, 6);
        if (cbpy < 0) {
            return false;
        }
        if (intra) {
            cbp = (cbpc & 3) | (cbpy << 2);
            if (dquant) {
                decodeDquant(reader);
            }
        } else {
            cbp = (cbpc & 3) | ((cbpy ^ 0xF) << 2);
            if (dquant) {
                decodeDquant(reader);
            }
            if ((cbpc & 16) == 0) {
                int pred_x = 0;
                int pred_y = 0;
                (void)predictMotion(mb_x, mb_y, 0, pred_x, pred_y);
                mv[0][0] = decodeMotion(reader, pred_x);
                mv[0][1] = decodeMotion(reader, pred_y);
                for (int i = 1; i < 4; ++i) {
                    mv[i][0] = mv[0][0];
                    mv[i][1] = mv[0][1];
                }
            } else {
                use_4mv = true;
                for (int i = 0; i < 4; ++i) {
                    int pred_x = 0;
                    int pred_y = 0;
                    const int slot = predictMotion(mb_x, mb_y, i, pred_x, pred_y);
                    mv[i][0] = decodeMotion(reader, pred_x);
                    mv[i][1] = decodeMotion(reader, pred_y);
                    mv_x_[static_cast<size_t>(slot)] = static_cast<int16_t>(mv[i][0]);
                    mv_y_[static_cast<size_t>(slot)] = static_cast<int16_t>(mv[i][1]);
                }
            }
        }
    }

    int cbp_bits = cbp;
    for (int i = 0; i < 6; ++i) {
        const bool coded = (cbp_bits & 32) != 0;
        cbp_bits <<= 1;
        if (!decodeBlock(reader, blocks[i], coded, intra)) {
            return false;
        }
    }

    uint8_t* dest_y = current_.row(0, mb_y * 16) + mb_x * 16;
    uint8_t* dest_cb = current_.row(1, mb_y * 8) + mb_x * 8;
    uint8_t* dest_cr = current_.row(2, mb_y * 8) + mb_x * 8;
    const int stride_y = current_.planeStride(0);
    const int stride_c = current_.planeStride(1);
    if (intra) {
        reconstructIntra(blocks, dest_y, dest_cb, dest_cr, stride_y, stride_c);
    } else {
        motionCompensate(mb_x, mb_y, mv, use_4mv);
        reconstructInter(blocks, cbp, dest_y, dest_cb, dest_cr, stride_y, stride_c);
    }
    updateMotionVal(mb_x, mb_y, intra, mv, use_4mv);
    return true;
}

bool Decoder::decodeBlock(BitReader& reader, Block& block, bool coded, bool intra) {
    // The scan index the first run is added to: 0 after the intra DC, -1
    // before the first coefficient of an inter block, as the reference
    // decoder's offset by one does.
    int index = intra ? 0 : -1;
    if (intra) {
        int dc = static_cast<int>(reader.read(8));
        if (dc == 255) {
            dc = 128;
        }
        block.coef[0] = static_cast<int16_t>(dc);
    }
    if (!coded) {
        return true;
    }
    for (;;) {
        const int symbol = readVlc(reader, kRunLevel, 103, 12);
        if (symbol < 0) {
            return false;
        }
        int run = 0;
        int level = 0;
        bool last = false;
        if (symbol == kRunLevelEscape) {
            if (format_ == 1) {
                const bool wide = reader.read(1) != 0;
                run = static_cast<int>(reader.read(7)) + 1;
                level = wide ? reader.readSigned(11) : reader.readSigned(7);
            } else {
                run = static_cast<int>(reader.read(7)) + 1;
                level = static_cast<int8_t>(reader.read(8));
                if (level == -128) {
                    level = static_cast<int>(reader.read(5)) | (reader.readSigned(6) << 5);
                }
            }
        } else {
            run = kRunLevelRun[symbol] + 1;
            level = kRunLevelLevel[symbol];
            if (reader.read(1) != 0) {
                level = -level;
            }
            last = symbol >= kRunLevelLast;
        }
        index += run;
        if (index >= 64) {
            // A run that passes the end of the block ends it; the level lands
            // on the coefficient the run wrapped to.
            index = index - run + ((run - 1) & 63) + 1;
            if (index < 64) {
                block.coef[kZigZag[index]] = static_cast<int16_t>(level);
                break;
            }
            return false;
        }
        block.coef[kZigZag[index]] = static_cast<int16_t>(level);
        if (last) {
            break;
        }
    }
    return true;
}

void Decoder::decodeDquant(BitReader& reader) {
    static const int kQuantTab[4] = {-1, -2, 1, 2};
    qscale_ = clampQscale(qscale_ + kQuantTab[reader.read(2)]);
}

int Decoder::decodeMotion(BitReader& reader, int predictor) {
    const int code = readVlc(reader, kMotionVlc, 33, 12);
    if (code < 0) {
        reader.setError();
        return predictor;
    }
    if (code == 0) {
        return predictor;
    }
    const bool negative = reader.read(1) != 0;
    int value = negative ? -code : code;
    value += predictor;
    // f_code is always 1 for FLV1: the vector decodes modulo 64.
    value &= 0x3F;
    if (value >= 32) {
        value -= 64;
    }
    return value;
}

int Decoder::predictMotion(int mb_x, int mb_y, int block, int& pred_x, int& pred_y) {
    static const int kOff[4] = {2, 1, 1, -1};
    const int base = 2 + 2 * mb_x + 2 * mb_y * b8_stride_;
    const int index = base + (block == 0 ? 0
                                         : block == 1 ? 1
                                                      : block == 2 ? b8_stride_ : b8_stride_ + 1);
    const int a = index - 1;
    const int b = index - b8_stride_;
    const int c = index + kOff[block] - b8_stride_;
    if (first_slice_line_ && block < 3) {
        if (block == 0) {
            if (mb_x == resync_mb_x_) {
                pred_x = 0;
                pred_y = 0;
            } else {
                pred_x = mv_x_[static_cast<size_t>(a)];
                pred_y = mv_y_[static_cast<size_t>(a)];
            }
        } else if (block == 1) {
            pred_x = mv_x_[static_cast<size_t>(a)];
            pred_y = mv_y_[static_cast<size_t>(a)];
        } else {
            int ax = mv_x_[static_cast<size_t>(a)];
            int ay = mv_y_[static_cast<size_t>(a)];
            if (mb_x == resync_mb_x_) {
                ax = 0;
                ay = 0;
            }
            pred_x = midPred(ax, mv_x_[static_cast<size_t>(b)], mv_x_[static_cast<size_t>(c)]);
            pred_y = midPred(ay, mv_y_[static_cast<size_t>(b)], mv_y_[static_cast<size_t>(c)]);
        }
    } else {
        pred_x = midPred(mv_x_[static_cast<size_t>(a)], mv_x_[static_cast<size_t>(b)],
                         mv_x_[static_cast<size_t>(c)]);
        pred_y = midPred(mv_y_[static_cast<size_t>(a)], mv_y_[static_cast<size_t>(b)],
                         mv_y_[static_cast<size_t>(c)]);
    }
    return index;
}

void Decoder::updateMotionVal(int mb_x, int mb_y, bool intra, const int mv[4][2], bool use_4mv) {
    if (use_4mv) {
        return;  // written while the vectors were parsed
    }
    const int base = 2 + 2 * mb_x + 2 * mb_y * b8_stride_;
    const int index[4] = {base, base + 1, base + b8_stride_, base + b8_stride_ + 1};
    const int mx = intra ? 0 : mv[0][0];
    const int my = intra ? 0 : mv[0][1];
    for (int i = 0; i < 4; ++i) {
        mv_x_[static_cast<size_t>(index[i])] = static_cast<int16_t>(mx);
        mv_y_[static_cast<size_t>(index[i])] = static_cast<int16_t>(my);
    }
}

void Decoder::clearMotionGrid() {
    const size_t count = static_cast<size_t>(b8_stride_) * (2 * mb_height_) + 4;
    mv_x_.assign(count, 0);
    mv_y_.assign(count, 0);
}

void Decoder::motionCompensate(int mb_x, int mb_y, const int mv[4][2], bool use_4mv) {
    if (!have_reference_) {
        return;
    }
    const uint8_t* plane_y = reference_.plane(0);
    const uint8_t* plane_cb = reference_.plane(1);
    const uint8_t* plane_cr = reference_.plane(2);
    const int ref_w = reference_.width_;
    const int ref_h = reference_.height_;
    const int ref_cw = reference_.width_c_;
    const int ref_ch = reference_.height_c_;
    const int stride_y = reference_.planeStride(0);
    const int stride_c = reference_.planeStride(1);
    uint8_t* dest_y = current_.row(0, mb_y * 16) + mb_x * 16;
    uint8_t* dest_cb = current_.row(1, mb_y * 8) + mb_x * 8;
    uint8_t* dest_cr = current_.row(2, mb_y * 8) + mb_x * 8;
    const int dstride_y = current_.planeStride(0);
    const int dstride_c = current_.planeStride(1);

    if (!use_4mv) {
        copyBlock(plane_y, stride_y, ref_w, ref_h, mb_x * 16 + (mv[0][0] >> 1),
                  mb_y * 16 + (mv[0][1] >> 1), mv[0][0] & 1, mv[0][1] & 1, dest_y, dstride_y, 16,
                  16);
        const int mx = mv[0][0];
        const int my = mv[0][1];
        const int uvx = (mb_x * 16 + (mx >> 1)) >> 1;
        const int uvy = (mb_y * 16 + (my >> 1)) >> 1;
        const int frac_x = ((mx & 1) | ((mx & 2) >> 1)) != 0;
        const int frac_y = ((my & 1) | ((my & 2) >> 1)) != 0;
        copyBlock(plane_cb, stride_c, ref_cw, ref_ch, uvx, uvy, frac_x, frac_y, dest_cb,
                  dstride_c, 8, 8);
        copyBlock(plane_cr, stride_c, ref_cw, ref_ch, uvx, uvy, frac_x, frac_y, dest_cr,
                  dstride_c, 8, 8);
    } else {
        for (int i = 0; i < 4; ++i) {
            uint8_t* block = dest_y + (i & 1) * 8 + (i >> 1) * 8 * dstride_y;
            copyBlock(plane_y, stride_y, ref_w, ref_h,
                      mb_x * 16 + (i & 1) * 8 + (mv[i][0] >> 1),
                      mb_y * 16 + (i >> 1) * 8 + (mv[i][1] >> 1), mv[i][0] & 1, mv[i][1] & 1, block,
                      dstride_y, 8, 8);
        }
        int sum_x = mv[0][0] + mv[1][0] + mv[2][0] + mv[3][0];
        int sum_y = mv[0][1] + mv[1][1] + mv[2][1] + mv[3][1];
        sum_x = roundChroma(sum_x);
        sum_y = roundChroma(sum_y);
        const int uvx = mb_x * 8 + (sum_x >> 1);
        const int uvy = mb_y * 8 + (sum_y >> 1);
        copyBlock(plane_cb, stride_c, ref_cw, ref_ch, uvx, uvy, sum_x & 1, sum_y & 1, dest_cb,
                  dstride_c, 8, 8);
        copyBlock(plane_cr, stride_c, ref_cw, ref_ch, uvx, uvy, sum_x & 1, sum_y & 1, dest_cr,
                  dstride_c, 8, 8);
    }
}

void Decoder::reconstructIntra(Block blocks[6], uint8_t* dest_y, uint8_t* dest_cb, uint8_t* dest_cr,
                               int stride_y, int stride_c) {
    for (int i = 0; i < 4; ++i) {
        unquantizeIntra(blocks[i], qscale_);
        idctPutBlock(blocks[i], dest_y + (i & 1) * 8 + (i >> 1) * 8 * stride_y, stride_y);
    }
    unquantizeIntra(blocks[4], qscale_);
    idctPutBlock(blocks[4], dest_cb, stride_c);
    unquantizeIntra(blocks[5], qscale_);
    idctPutBlock(blocks[5], dest_cr, stride_c);
}

void Decoder::reconstructInter(Block blocks[6], int cbp, uint8_t* dest_y, uint8_t* dest_cb,
                               uint8_t* dest_cr, int stride_y, int stride_c) {
    int bits = cbp;
    for (int i = 0; i < 6; ++i) {
        const bool coded = (bits & 32) != 0;
        bits <<= 1;
        if (!coded) {
            continue;
        }
        unquantizeInter(blocks[i], qscale_);
        if (i < 4) {
            idctAddBlock(blocks[i], dest_y + (i & 1) * 8 + (i >> 1) * 8 * stride_y, stride_y);
        } else if (i == 4) {
            idctAddBlock(blocks[i], dest_cb, stride_c);
        } else {
            idctAddBlock(blocks[i], dest_cr, stride_c);
        }
    }
}

void Decoder::emitFrame(int64_t pts, const PictureHeader& header) {
    DecodedFrame frame;
    if (!frame.image.alloc(current_.width_, current_.height_, ChromaFormat::C420, 8)) {
        return;
    }
    frame.image.crop_right_ = current_.crop_right_;
    frame.image.crop_bottom_ = current_.crop_bottom_;
    for (int plane = 0; plane < 3; ++plane) {
        copyPlane(current_, plane, frame.image);
    }
    frame.pts = pts;
    frame.poc = header.timestamp;
    frame.qp = header.quantiser;
    frame.slice_type = header.intra ? SliceType::I : SliceType::P;
    ready_.push_back(std::move(frame));
}

}  // namespace player::decode::flv1
