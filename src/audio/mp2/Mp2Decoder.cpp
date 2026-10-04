#include "audio/mp2/Mp2Decoder.hpp"

#include "audio/BitReader.hpp"
// Layer II shares the polyphase synthesis filterbank and its window with
// Layer III, so the tables live with the Layer III decoder.
#include "audio/mp3/Mp3Tables.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace player::audio::mp2 {
namespace {

constexpr int kSubbands = 32;
constexpr int kSamplesPerSubband = 36;  // one Layer II frame, three groups of 12

struct Header {
    bool valid = false;
    int version = 1;  // 1 = MPEG-1, 2 = MPEG-2, 25 = MPEG-2.5
    uint32_t bitrate = 0;
    uint32_t sample_rate = 0;
    int channels = 2;
    bool joint_stereo = false;
    int mode_ext = 0;
    bool protected_bit = false;
    int frame_bytes = 0;
};

const uint32_t kBitrateMpeg1[15] = {0,   32,  48,  56,  64,  80,  96, 112,
                                    128, 160, 192, 224, 256, 320, 384};
const uint32_t kBitrateMpeg2[15] = {0,  8,  16, 24, 32,  40,  48,  56,
                                    64, 80, 96, 112, 128, 144, 160};
const uint32_t kSampleRates[3][3] = {{44100, 48000, 32000},
                                     {22050, 24000, 16000},
                                     {11025, 12000, 8000}};

Header parseHeader(const uint8_t* h) {
    Header out;
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) {
        return out;
    }
    const int version_bits = (h[1] >> 3) & 3;
    const int layer_bits = (h[1] >> 1) & 3;
    if (version_bits == 1 || layer_bits != 2) {
        return out;  // reserved version, or not Layer II
    }
    const int bitrate_index = (h[2] >> 4) & 0x0F;
    const int rate_index = (h[2] >> 2) & 3;
    if (bitrate_index == 0 || bitrate_index == 15 || rate_index == 3) {
        return out;
    }
    const int padding = (h[2] >> 1) & 1;
    const int mode = (h[3] >> 6) & 3;

    out.version = version_bits == 3 ? 1 : (version_bits == 2 ? 2 : 25);
    out.bitrate = (version_bits == 3 ? kBitrateMpeg1 : kBitrateMpeg2)[bitrate_index] * 1000u;
    out.sample_rate = kSampleRates[version_bits == 3 ? 0 : (version_bits == 2 ? 1 : 2)][rate_index];
    out.channels = mode == 3 ? 1 : 2;
    out.joint_stereo = mode == 1;
    out.mode_ext = (h[3] >> 4) & 3;
    out.protected_bit = (h[1] & 1) == 0;
    // 1152 samples per frame for MPEG-1, 576 for MPEG-2 and 2.5.
    const int samples = version_bits == 3 ? 1152 : 576;
    out.frame_bytes = samples / 8 * static_cast<int>(out.bitrate) /
                          static_cast<int>(out.sample_rate) +
                      padding;
    out.valid = out.frame_bytes > 4;
    return out;
}

// ISO/IEC 11172-3 table 3-B.1: 2^(1 - index/3).
struct ScaleFactors {
    float value[64];
    ScaleFactors() {
        for (int i = 0; i < 64; ++i) {
            value[i] = std::pow(2.0f, 1.0f - static_cast<float>(i) / 3.0f);
        }
    }
};
const ScaleFactors& scaleFactors() {
    static const ScaleFactors table;
    return table;
}

// 1 / (2^n - 1): the Layer II dequantisation divides the centred code by the
// number of levels minus one, which is what brings a full-scale signal to
// roughly +/-1 before the synthesis filterbank.
struct Inverses {
    float value[18];
    Inverses() {
        for (int i = 0; i < 18; ++i) {
            value[i] = 1.0f / static_cast<float>((1 << i) - 1);
        }
    }
};
const Inverses& inverses() {
    static const Inverses table;
    return table;
}

// The three dequantisation tables. Index 0 and 1 share one, 2 and 3 another,
// 4 a third; the table is chosen from the per-channel bitrate.
// Flat allocation table 1 (also used for index 1): a row per subband,
// 1 << width entries long. Entry 0 is the width, the rest map an
// allocation code to a quantisation index.
const uint8_t kAllocTable1[] = {
    4, 0, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
    4, 0, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
    4, 0, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
    4, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 16,
    4, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 16,
    4, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 16,
    4, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 16,
    4, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 16,
    4, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 16,
    4, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 16,
    3, 0, 1, 2, 3, 4, 5, 16, 3, 0, 1, 2, 3, 4, 5, 16,
    3, 0, 1, 2, 3, 4, 5, 16, 3, 0, 1, 2, 3, 4, 5, 16,
    3, 0, 1, 2, 3, 4, 5, 16, 3, 0, 1, 2, 3, 4, 5, 16,
    3, 0, 1, 2, 3, 4, 5, 16, 3, 0, 1, 2, 3, 4, 5, 16,
    3, 0, 1, 2, 3, 4, 5, 16, 3, 0, 1, 2, 3, 4, 5, 16,
    2, 0, 1, 16, 2, 0, 1, 16, 2, 0, 1, 16, 2, 0, 1, 16,
    2, 0, 1, 16, 2, 0, 1, 16, 2, 0, 1, 16,
};

// Flat allocation table 3 (also used for index 3).
const uint8_t kAllocTable3[] = {
    4, 0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    4, 0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    3, 0, 1, 3, 4, 5, 6, 7, 3, 0, 1, 3, 4, 5, 6, 7,
    3, 0, 1, 3, 4, 5, 6, 7, 3, 0, 1, 3, 4, 5, 6, 7,
    3, 0, 1, 3, 4, 5, 6, 7, 3, 0, 1, 3, 4, 5, 6, 7,
    3, 0, 1, 3, 4, 5, 6, 7, 3, 0, 1, 3, 4, 5, 6, 7,
    3, 0, 1, 3, 4, 5, 6, 7, 3, 0, 1, 3, 4, 5, 6, 7,
};

// Flat allocation table 4.
const uint8_t kAllocTable4[] = {
    4, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
    4, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
    4, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
    4, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
    3, 0, 1, 3, 4, 5, 6, 7, 3, 0, 1, 3, 4, 5, 6, 7,
    3, 0, 1, 3, 4, 5, 6, 7, 3, 0, 1, 3, 4, 5, 6, 7,
    3, 0, 1, 3, 4, 5, 6, 7, 3, 0, 1, 3, 4, 5, 6, 7,
    3, 0, 1, 3, 4, 5, 6, 7, 2, 0, 1, 3, 2, 0, 1, 3,
    2, 0, 1, 3, 2, 0, 1, 3, 2, 0, 1, 3, 2, 0, 1, 3,
    2, 0, 1, 3, 2, 0, 1, 3, 2, 0, 1, 3, 2, 0, 1, 3,
    2, 0, 1, 3, 2, 0, 1, 3, 2, 0, 1, 3, 2, 0, 1, 3,
    2, 0, 1, 3, 2, 0, 1, 3, 2, 0, 1, 3, 2, 0, 1, 3,
    2, 0, 1, 3,
};

const uint8_t* const kAllocTables[5] = {kAllocTable1, kAllocTable1, kAllocTable3,
                                        kAllocTable3, kAllocTable4};
const uint8_t kSblimitTable[5] = {27, 30, 8, 12, 30};
// Number of quantisation steps for each allocation code, and the bit width.
const int kQuantSteps[17] = {3,     5,     7,     9,     15,    31,   63,   127,  255,
                             511,   1023,  2047,  4095,  8191,  16383, 32767, 65535};
// Negative when three samples share one code (the width is the magnitude).
const int kQuantBits[17] = {-5, -7, 3, -10, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

// Chooses the allocation table from the per-channel bitrate.
int selectTable(uint32_t bitrate_bps, int channels, uint32_t sample_rate, bool lsf) {
    const int per_channel = static_cast<int>(bitrate_bps / 1000u) / channels;
    if (lsf) {
        return 4;
    }
    if ((sample_rate == 48000 && per_channel >= 56) ||
        (per_channel >= 56 && per_channel <= 80)) {
        return 0;
    }
    if (sample_rate != 48000 && per_channel >= 96) {
        return 1;
    }
    if (sample_rate != 32000 && per_channel <= 48) {
        return 2;
    }
    return 3;
}

// ---------------------------------------------------------------------------
// polyphase synthesis, shared with Layer III
// ---------------------------------------------------------------------------

void dctII(float* grbuf, int n) {
    for (int k = 0; k < n; k++) {
        float t[4][8];
        float* x;
        float* y = grbuf + k;
        int i;

        for (x = t[0], i = 0; i < 8; i++, x++) {
            const float x0 = y[i * 18];
            const float x1 = y[(15 - i) * 18];
            const float x2 = y[(16 + i) * 18];
            const float x3 = y[(31 - i) * 18];
            const float t0 = x0 + x3;
            const float t1 = x1 + x2;
            const float t2 = (x1 - x2) * mp3::kSec[3 * i + 0];
            const float t3 = (x0 - x3) * mp3::kSec[3 * i + 1];
            x[0] = t0 + t1;
            x[8] = (t0 - t1) * mp3::kSec[3 * i + 2];
            x[16] = t3 + t2;
            x[24] = (t3 - t2) * mp3::kSec[3 * i + 2];
        }
        for (x = t[0], i = 0; i < 4; i++, x += 8) {
            float x0 = x[0];
            float x1 = x[1];
            float x2 = x[2];
            float x3 = x[3];
            float x4 = x[4];
            float x5 = x[5];
            float x6 = x[6];
            float x7 = x[7];
            float xt;
            xt = x0 - x7;
            x0 += x7;
            x7 = x1 - x6;
            x1 += x6;
            x6 = x2 - x5;
            x2 += x5;
            x5 = x3 - x4;
            x3 += x4;
            x4 = x0 - x3;
            x0 += x3;
            x3 = x1 - x2;
            x1 += x2;
            x[0] = x0 + x1;
            x[4] = (x0 - x1) * 0.70710677f;
            x5 = x5 + x6;
            x6 = (x6 + x7) * 0.70710677f;
            x7 = x7 + xt;
            x3 = (x3 + x4) * 0.70710677f;
            x5 -= x7 * 0.198912367f;  // rotate by PI/8
            x7 += x5 * 0.382683432f;
            x5 -= x7 * 0.198912367f;
            x0 = xt - x6;
            xt += x6;
            x[1] = (xt + x7) * 0.50979561f;
            x[2] = (x4 + x3) * 0.54119611f;
            x[3] = (x0 - x5) * 0.60134488f;
            x[5] = (x0 + x5) * 0.89997619f;
            x[6] = (x4 - x3) * 1.30656302f;
            x[7] = (xt - x7) * 2.56291556f;
        }
        for (int i = 0; i < 7; i++, y += 4 * 18) {
            y[0 * 18] = t[0][i];
            y[1 * 18] = t[2][i] + t[3][i] + t[3][i + 1];
            y[2 * 18] = t[1][i] + t[1][i + 1];
            y[3 * 18] = t[2][i + 1] + t[3][i] + t[3][i + 1];
        }
        y[0 * 18] = t[0][7];
        y[1 * 18] = t[2][7] + t[3][7];
        y[2 * 18] = t[1][7];
        y[3 * 18] = t[3][7];
    }
}

void synthPair(float* pcm, int nch, const float* z) {
    float a = (z[14 * 64] - z[0]) * 29;
    a += (z[1 * 64] + z[13 * 64]) * 213;
    a += (z[12 * 64] - z[2 * 64]) * 459;
    a += (z[3 * 64] + z[11 * 64]) * 2037;
    a += (z[10 * 64] - z[4 * 64]) * 5153;
    a += (z[5 * 64] + z[9 * 64]) * 6574;
    a += (z[8 * 64] - z[6 * 64]) * 37489;
    a += z[7 * 64] * 75038;
    pcm[0] = a;

    z += 2;
    a = z[14 * 64] * 104;
    a += z[12 * 64] * 1567;
    a += z[10 * 64] * 9727;
    a += z[8 * 64] * 64019;
    a += z[6 * 64] * -9975;
    a += z[4 * 64] * -45;
    a += z[2 * 64] * 146;
    a += z[0 * 64] * -5;
    pcm[16 * nch] = a;
}

void synth(float* xl, float* dstl, int nch, float* lins) {
    float* xr = xl + 576 * (nch - 1);
    float* dstr = dstl + (nch - 1);
    float* zlin = lins + 15 * 64;
    const float* w = mp3::kSynthesisWindow;

    zlin[4 * 15] = xl[18 * 16];
    zlin[4 * 15 + 1] = xr[18 * 16];
    zlin[4 * 15 + 2] = xl[0];
    zlin[4 * 15 + 3] = xr[0];

    zlin[4 * 31] = xl[1 + 18 * 16];
    zlin[4 * 31 + 1] = xr[1 + 18 * 16];
    zlin[4 * 31 + 2] = xl[1];
    zlin[4 * 31 + 3] = xr[1];

    synthPair(dstr, nch, lins + 4 * 15 + 1);
    synthPair(dstr + 32 * nch, nch, lins + 4 * 15 + 64 + 1);
    synthPair(dstl, nch, lins + 4 * 15);
    synthPair(dstl + 32 * nch, nch, lins + 4 * 15 + 64);

    for (int i = 14; i >= 0; i--) {
        float a[4];
        float b[4];
        const auto load = [&](int k, float& w0, float& w1, float*& vz, float*& vy) {
            w0 = *w++;
            w1 = *w++;
            vz = &zlin[4 * i - k * 64];
            vy = &zlin[4 * i - (15 - k) * 64];
        };

        zlin[4 * i] = xl[18 * (31 - i)];
        zlin[4 * i + 1] = xr[18 * (31 - i)];
        zlin[4 * i + 2] = xl[1 + 18 * (31 - i)];
        zlin[4 * i + 3] = xr[1 + 18 * (31 - i)];
        zlin[4 * (i + 16)] = xl[1 + 18 * (1 + i)];
        zlin[4 * (i + 16) + 1] = xr[1 + 18 * (1 + i)];
        zlin[4 * (i - 16) + 2] = xl[18 * (1 + i)];
        zlin[4 * (i - 16) + 3] = xr[18 * (1 + i)];

        for (int k = 0; k < 8; k++) {
            float w0;
            float w1;
            float* vz;
            float* vy;
            load(k, w0, w1, vz, vy);
            if (k == 0) {
                for (int j = 0; j < 4; j++) {
                    b[j] = vz[j] * w1 + vy[j] * w0;
                    a[j] = vz[j] * w0 - vy[j] * w1;
                }
            } else if ((k & 1) == 0) {
                for (int j = 0; j < 4; j++) {
                    b[j] += vz[j] * w1 + vy[j] * w0;
                    a[j] += vz[j] * w0 - vy[j] * w1;
                }
            } else {
                for (int j = 0; j < 4; j++) {
                    b[j] += vz[j] * w1 + vy[j] * w0;
                    a[j] += vy[j] * w1 - vz[j] * w0;
                }
            }
        }

        dstr[(15 - i) * nch] = a[1];
        dstr[(17 + i) * nch] = b[1];
        dstl[(15 - i) * nch] = a[0];
        dstl[(17 + i) * nch] = b[0];
        dstr[(47 - i) * nch] = a[3];
        dstr[(49 + i) * nch] = b[3];
        dstl[(47 - i) * nch] = a[2];
        dstl[(49 + i) * nch] = b[2];
    }
}

// Transforms one granule of 18 subband samples per subband into 576 samples.
void synthGranule(float* qmf_state, float* grbuf, int nbands, int nch, float* pcm, float* lins) {
    for (int i = 0; i < nch; i++) {
        dctII(grbuf + 576 * i, nbands);
    }

    std::memcpy(lins, qmf_state, sizeof(float) * 15 * 64);

    for (int i = 0; i < nbands; i += 2) {
        synth(grbuf + i, pcm + 32 * nch * i, nch, lins + i * 64);
    }
    if (nch == 1) {
        for (int i = 0; i < 15 * 64; i += 2) {
            qmf_state[i] = lins[nbands * 64 + i];
        }
    } else {
        std::memcpy(qmf_state, lins + nbands * 64, sizeof(float) * 15 * 64);
    }
}

}  // namespace

bool Decoder::configure(const uint8_t* config, size_t size, std::string& error) {
    if (config != nullptr || size != 0) {
        error = "MP2 has no extradata";
        return false;
    }
    configured_ = true;
    resetState();
    return true;
}

void Decoder::resetState() {
    std::memset(qmf_state_, 0, sizeof(qmf_state_));
    sample_rate_ = 0;
    channels_ = 0;
}

void Decoder::flush() { std::memset(qmf_state_, 0, sizeof(qmf_state_)); }

void Decoder::reset() { resetState(); }

Status Decoder::decode(const uint8_t* data, size_t size, PcmBuffer& pcm, std::string& error) {
    if (!configured_) {
        error = "MP2 decoder is not configured";
        return Status::Invalid;
    }
    if (data == nullptr || size < 4) {
        error = "truncated MP2 frame";
        return Status::Invalid;
    }
    const Header header = parseHeader(data);
    if (!header.valid) {
        error = "not an MPEG audio Layer II frame";
        return Status::Unsupported;
    }
    if (static_cast<int>(size) < header.frame_bytes) {
        error = "truncated MP2 frame";
        return Status::Invalid;
    }
    if (sample_rate_ != header.sample_rate || channels_ != static_cast<uint32_t>(header.channels)) {
        sample_rate_ = header.sample_rate;
        channels_ = static_cast<uint32_t>(header.channels);
        flush();
    }

    BitReader reader(data + 4, static_cast<size_t>(header.frame_bytes - 4));
    // A read past the end leaves the position alone, so the frame simply stops
    // producing data; note it so the caller can treat the frame as damaged.
    bool truncated = false;
    const auto take = [&](unsigned count) -> uint32_t {
        uint32_t value = 0;
        if (!reader.read(count, value)) {
            truncated = true;
        }
        return value;
    };
    const int channels = header.channels;
    const bool lsf = header.version != 1;
    // A protected frame carries a 16-bit CRC straight after the header, before
    // the allocation data.
    if (header.protected_bit) {
        take(16);
    }

    const int table = selectTable(header.bitrate, channels, header.sample_rate, lsf);
    const int sblimit = kSblimitTable[table];
    const uint8_t* alloc_table = kAllocTables[table];

    int bound = sblimit;
    if (header.joint_stereo) {
        bound = (header.mode_ext + 1) * 4;
        if (bound > sblimit) {
            bound = sblimit;
        }
    }

    int bit_alloc[2][kSubbands] = {};
    int scale_code[2][kSubbands] = {};
    int scale_factors[2][kSubbands][3] = {};

    // Bit allocation. Above `bound` the two channels share one field.
    int j = 0;
    for (int i = 0; i < bound; ++i) {
        const int width = alloc_table[j];
        for (int ch = 0; ch < channels; ++ch) {
            bit_alloc[ch][i] = static_cast<int>(take(static_cast<unsigned>(width)));
        }
        j += 1 << width;
    }
    for (int i = bound; i < sblimit; ++i) {
        const int width = alloc_table[j];
        const int v = static_cast<int>(take(static_cast<unsigned>(width)));
        bit_alloc[0][i] = v;
        bit_alloc[1][i] = v;
        j += 1 << width;
    }

    // Scalefactor selection information: two bits per coded subband.
    for (int i = 0; i < sblimit; ++i) {
        for (int ch = 0; ch < channels; ++ch) {
            if (bit_alloc[ch][i] != 0) {
                scale_code[ch][i] = static_cast<int>(take(2));
            }
        }
    }

    // Scalefactors.
    for (int i = 0; i < sblimit; ++i) {
        for (int ch = 0; ch < channels; ++ch) {
            if (bit_alloc[ch][i] == 0) {
                continue;
            }
            int* sf = scale_factors[ch][i];
            switch (scale_code[ch][i]) {
                case 2:
                    sf[0] = sf[1] = sf[2] = static_cast<int>(take(6));
                    break;
                case 1:
                    sf[0] = sf[1] = static_cast<int>(take(6));
                    sf[2] = static_cast<int>(take(6));
                    break;
                case 3:
                    sf[0] = static_cast<int>(take(6));
                    sf[1] = sf[2] = static_cast<int>(take(6));
                    break;
                default:
                    sf[0] = static_cast<int>(take(6));
                    sf[1] = static_cast<int>(take(6));
                    sf[2] = static_cast<int>(take(6));
                    break;
            }
        }
    }

    if (truncated) {
        error = "MP2 frame ended inside its headers";
        return Status::Invalid;
    }

    const ScaleFactors& scf_table = scaleFactors();
    const Inverses& inv = inverses();
    float subband[2][kSamplesPerSubband][kSubbands] = {};

    // Samples: three passes of 12 samples, three samples per code.
    for (int k = 0; k < 3; ++k) {
        for (int l = 0; l < 12; l += 3) {
            j = 0;
            for (int i = 0; i < bound; ++i) {
                const int width = alloc_table[j];
                for (int ch = 0; ch < channels; ++ch) {
                    const int b = bit_alloc[ch][i];
                    if (b == 0) {
                        continue;  // the subband advance happens once, below
                    }
                    const int qindex = alloc_table[j + b];
                    const int bits = kQuantBits[qindex];
                    const float scale = scf_table.value[scale_factors[ch][i][k]];
                    if (bits < 0) {
                        const int v = static_cast<int>(take(static_cast<unsigned>(-bits)));
                        const int steps = kQuantSteps[qindex];
                        const float step_scale = scale * inv.value[qindex == 0 ? 2 : 4];
                        int remainder = v;
                        for (int m = 0; m < 3; ++m) {
                            const int code = remainder % steps;
                            remainder /= steps;
                            subband[ch][k * 12 + l + m][i] =
                                (static_cast<float>(code) - static_cast<float>(steps >> 1)) *
                                step_scale;
                        }
                    } else {
                        const float step_scale = scale * inv.value[bits];
                        for (int m = 0; m < 3; ++m) {
                            const int code = static_cast<int>(take(static_cast<unsigned>(bits)));
                            subband[ch][k * 12 + l + m][i] =
                                (static_cast<float>(code) -
                                 static_cast<float>(1 << (bits - 1))) *
                                step_scale;
                        }
                    }
                }
                j += 1 << width;
            }
            // Joint stereo above `bound`: one code drives both channels.
            for (int i = bound; i < sblimit; ++i) {
                const int width = alloc_table[j];
                const int b = bit_alloc[0][i];
                if (b == 0) {
                    j += 1 << width;
                    continue;
                }
                const int qindex = alloc_table[j + b];
                const int bits = kQuantBits[qindex];
                const float scale0 = scf_table.value[scale_factors[0][i][k]];
                const float scale1 = scf_table.value[scale_factors[1][i][k]];
                if (bits < 0) {
                    const int v = static_cast<int>(take(static_cast<unsigned>(-bits)));
                    const int steps = kQuantSteps[qindex];
                    const float f = inv.value[qindex == 0 ? 2 : 4];
                    int remainder = v;
                    for (int m = 0; m < 3; ++m) {
                        const int code = remainder % steps;
                        remainder /= steps;
                        const float centered =
                            static_cast<float>(code) - static_cast<float>(steps >> 1);
                        subband[0][k * 12 + l + m][i] = centered * scale0 * f;
                        subband[1][k * 12 + l + m][i] = centered * scale1 * f;
                    }
                } else {
                    const float f = inv.value[bits];
                    for (int m = 0; m < 3; ++m) {
                        const int code = static_cast<int>(take(static_cast<unsigned>(bits)));
                        const float centered =
                            static_cast<float>(code) - static_cast<float>(1 << (bits - 1));
                        subband[0][k * 12 + l + m][i] = centered * scale0 * f;
                        subband[1][k * 12 + l + m][i] = centered * scale1 * f;
                    }
                }
                j += 1 << width;
            }
        }
    }

    // Synthesis: two granules of 18 subband samples each. The granule length
    // is what the synthesis takes, not the subband count.
    constexpr int kGranuleSamples = 18;
    float granule[2][576];
    float lins[15 * 64 + 32 * 64];
    const int samples_per_granule = kGranuleSamples * 32;
    pcm.samples.assign(static_cast<size_t>(2) * samples_per_granule * static_cast<size_t>(channels),
                       0.0f);
    for (int half = 0; half < 2; ++half) {
        for (int ch = 0; ch < channels; ++ch) {
            for (int sb = 0; sb < kSubbands; ++sb) {
                for (int s = 0; s < kGranuleSamples; ++s) {
                    granule[ch][sb * kGranuleSamples + s] = subband[ch][half * 18 + s][sb];
                }
            }
        }
        synthGranule(qmf_state_, granule[0], kGranuleSamples, channels,
                     pcm.samples.data() + static_cast<size_t>(half) * samples_per_granule * channels,
                     lins);
    }
    pcm.sample_rate = sample_rate_;
    pcm.channels = channels_;
    return Status::Ok;
}

}  // namespace player::audio::mp2
