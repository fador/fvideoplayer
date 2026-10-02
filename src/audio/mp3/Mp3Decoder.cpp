#include "audio/mp3/Mp3Decoder.hpp"

#include "audio/mp3/Mp3Tables.hpp"

#include <algorithm>
#include <cstring>

namespace player::audio::mp3 {
namespace {

constexpr int kHeaderSize = 4;
constexpr int kMaxFreeFormatFrameSize = 2304;
constexpr int kMaxFrameSyncMatches = 10;
constexpr int kMaxL3FramePayloadBytes = 2304;
constexpr int kMaxBitReservoirBytes = 511;
constexpr int kShortBlockType = 2;
constexpr int kStopBlockType = 3;
constexpr int kBitsDequantizerOut = -1;
constexpr int kMaxScf = 255 + kBitsDequantizerOut * 4 - 210;
constexpr int kMaxScfI = (kMaxScf + 3) & ~3;

struct BitReader {
    const uint8_t* buf = nullptr;
    int pos = 0;
    int limit = 0;
};

void bsInit(BitReader& bs, const uint8_t* data, int bytes) {
    bs.buf = data;
    bs.pos = 0;
    bs.limit = bytes * 8;
}

// Reads n bits; past the end it returns zeros, like the reference.
uint32_t getBits(BitReader& bs, int n) {
    uint32_t next;
    uint32_t cache = 0;
    const uint32_t s = static_cast<uint32_t>(bs.pos) & 7;
    int shl = n + static_cast<int>(s);
    const uint8_t* p = bs.buf + (bs.pos >> 3);
    bs.pos += n;
    if (bs.pos > bs.limit) {
        return 0;
    }
    next = *p++ & (255 >> s);
    while ((shl -= 8) > 0) {
        cache |= next << shl;
        next = *p++;
    }
    return cache | (next >> -shl);
}

bool hdrIsMono(const uint8_t* h) { return (h[3] & 0xC0) == 0xC0; }
bool hdrIsMsStereo(const uint8_t* h) { return (h[3] & 0xE0) == 0x60; }
bool hdrIsFreeFormat(const uint8_t* h) { return (h[2] & 0xF0) == 0; }
bool hdrIsCrc(const uint8_t* h) { return (h[1] & 1) == 0; }
bool hdrTestPadding(const uint8_t* h) { return (h[2] & 0x2) != 0; }
bool hdrTestMpeg1(const uint8_t* h) { return (h[1] & 0x8) != 0; }
bool hdrTestMpeg25(const uint8_t* h) { return (h[1] & 0x10) != 0; }
bool hdrTestIStereo(const uint8_t* h) { return (h[3] & 0x10) != 0; }
bool hdrTestMsStereo(const uint8_t* h) { return (h[3] & 0x20) != 0; }
bool hdrIsFrame576(const uint8_t* h) { return (h[1] & 14) == 2; }
bool hdrIsLayer1(const uint8_t* h) { return (h[1] & 6) == 6; }
unsigned hdrGetLayer(const uint8_t* h) { return (h[1] >> 1) & 3; }
unsigned hdrGetSampleRate(const uint8_t* h) { return (h[2] >> 2) & 3; }
unsigned hdrGetBitrate(const uint8_t* h) { return h[2] >> 4; }

unsigned hdrMySampleRate(const uint8_t* h) {
    return hdrGetSampleRate(h) + (((h[1] >> 3) & 1) + ((h[1] >> 4) & 1)) * 3;
}

bool hdrValid(const uint8_t* h) {
    return h[0] == 0xFF && ((h[1] & 0xF0) == 0xF0 || (h[1] & 0xFE) == 0xE2) &&
           hdrGetLayer(h) != 0 && hdrGetBitrate(h) != 15 && hdrGetSampleRate(h) != 3;
}

bool hdrCompare(const uint8_t* h1, const uint8_t* h2) {
    return hdrValid(h2) && ((h1[1] ^ h2[1]) & 0xFE) == 0 && ((h1[2] ^ h2[2]) & 0x0C) == 0 &&
           !(hdrIsFreeFormat(h1) ^ hdrIsFreeFormat(h2));
}

unsigned hdrBitrateKbps(const uint8_t* h) {
    const int mpeg1 = hdrTestMpeg1(h) ? 1 : 0;
    const int layer = static_cast<int>(hdrGetLayer(h)) - 1;
    return 2u * kBitrateKbps[mpeg1][layer][hdrGetBitrate(h)];
}

unsigned hdrSampleRateHz(const uint8_t* h) {
    return kSampleRates[hdrGetSampleRate(h)] >> static_cast<int>(!hdrTestMpeg1(h))
           >> static_cast<int>(!hdrTestMpeg25(h));
}

unsigned hdrFrameSamples(const uint8_t* h) {
    return hdrIsLayer1(h) ? 384u : (1152u >> static_cast<int>(hdrIsFrame576(h)));
}

int hdrFrameBytes(const uint8_t* h, int free_format_size) {
    const int frame_bytes =
        static_cast<int>(hdrFrameSamples(h) * hdrBitrateKbps(h) * 125 / hdrSampleRateHz(h));
    return frame_bytes ? (hdrIsLayer1(h) ? (frame_bytes & ~3) : frame_bytes) : free_format_size;
}

int hdrPadding(const uint8_t* h) {
    if (!hdrTestPadding(h)) {
        return 0;
    }
    return hdrIsLayer1(h) ? 4 : 1;
}

struct GranuleInfo {
    const uint8_t* sfbtab = nullptr;
    uint16_t part23_length = 0;
    uint16_t big_values = 0;
    uint16_t scalefac_compress = 0;
    uint8_t global_gain = 0;
    uint8_t block_type = 0;
    uint8_t mixed_block_flag = 0;
    uint8_t n_long_sfb = 0;
    uint8_t n_short_sfb = 0;
    uint8_t table_select[3] = {};
    uint8_t region_count[3] = {};
    uint8_t subblock_gain[3] = {};
    uint8_t preflag = 0;
    uint8_t scalefac_scale = 0;
    uint8_t count1_table = 0;
    uint8_t scfsi = 0;
};

struct Scratch {
    BitReader bs_frame;
    uint8_t maindata[kMaxBitReservoirBytes + kMaxL3FramePayloadBytes] = {};
    GranuleInfo gr_info[4];
    float grbuf[2][576] = {};
    float scf[40] = {};
    float syn[33][64] = {};
    uint8_t ist_pos[2][39] = {};
    float pcm[2 * 1152] = {};
};

int readSideInfo(BitReader& bs, GranuleInfo* gr, const uint8_t* hdr) {
    unsigned tables;
    unsigned scfsi = 0;
    int main_data_begin;
    int part_23_sum = 0;
    int sr_idx = static_cast<int>(hdrMySampleRate(hdr));
    sr_idx -= (sr_idx != 0);
    int gr_count = hdrIsMono(hdr) ? 1 : 2;

    if (hdrTestMpeg1(hdr)) {
        gr_count *= 2;
        main_data_begin = static_cast<int>(getBits(bs, 9));
        scfsi = getBits(bs, 7 + gr_count);
    } else {
        main_data_begin = static_cast<int>(getBits(bs, 8 + gr_count) >> gr_count);
    }

    do {
        if (hdrIsMono(hdr)) {
            scfsi <<= 4;
        }
        gr->part23_length = static_cast<uint16_t>(getBits(bs, 12));
        part_23_sum += gr->part23_length;
        gr->big_values = static_cast<uint16_t>(getBits(bs, 9));
        if (gr->big_values > 288) {
            return -1;
        }
        gr->global_gain = static_cast<uint8_t>(getBits(bs, 8));
        gr->scalefac_compress = static_cast<uint16_t>(getBits(bs, hdrTestMpeg1(hdr) ? 4 : 9));
        gr->sfbtab = kScfLong[sr_idx];
        gr->n_long_sfb = 22;
        gr->n_short_sfb = 0;
        if (getBits(bs, 1)) {
            gr->block_type = static_cast<uint8_t>(getBits(bs, 2));
            if (!gr->block_type) {
                return -1;
            }
            gr->mixed_block_flag = static_cast<uint8_t>(getBits(bs, 1));
            gr->region_count[0] = 7;
            gr->region_count[1] = 255;
            if (gr->block_type == kShortBlockType) {
                scfsi &= 0x0F0F;
                if (!gr->mixed_block_flag) {
                    gr->region_count[0] = 8;
                    gr->sfbtab = kScfShort[sr_idx];
                    gr->n_long_sfb = 0;
                    gr->n_short_sfb = 39;
                } else {
                    gr->sfbtab = kScfMixed[sr_idx];
                    gr->n_long_sfb = hdrTestMpeg1(hdr) ? 8 : 6;
                    gr->n_short_sfb = 30;
                }
            }
            tables = getBits(bs, 10);
            tables <<= 5;
            gr->subblock_gain[0] = static_cast<uint8_t>(getBits(bs, 3));
            gr->subblock_gain[1] = static_cast<uint8_t>(getBits(bs, 3));
            gr->subblock_gain[2] = static_cast<uint8_t>(getBits(bs, 3));
        } else {
            gr->block_type = 0;
            gr->mixed_block_flag = 0;
            tables = getBits(bs, 15);
            gr->region_count[0] = static_cast<uint8_t>(getBits(bs, 4));
            gr->region_count[1] = static_cast<uint8_t>(getBits(bs, 3));
            gr->region_count[2] = 255;
        }
        gr->table_select[0] = static_cast<uint8_t>(tables >> 10);
        gr->table_select[1] = static_cast<uint8_t>((tables >> 5) & 31);
        gr->table_select[2] = static_cast<uint8_t>(tables & 31);
        gr->preflag = hdrTestMpeg1(hdr) ? static_cast<uint8_t>(getBits(bs, 1))
                                        : (gr->scalefac_compress >= 500 ? 1 : 0);
        gr->scalefac_scale = static_cast<uint8_t>(getBits(bs, 1));
        gr->count1_table = static_cast<uint8_t>(getBits(bs, 1));
        gr->scfsi = static_cast<uint8_t>((scfsi >> 12) & 15);
        scfsi <<= 4;
        gr++;
    } while (--gr_count);

    if (part_23_sum + bs.pos > bs.limit + main_data_begin * 8) {
        return -1;
    }
    return main_data_begin;
}

void readScalefactors(uint8_t* scf, uint8_t* ist_pos, const uint8_t* scf_size,
                      const uint8_t* scf_count, BitReader& bitbuf, int scfsi) {
    for (int i = 0; i < 4 && scf_count[i]; i++, scfsi *= 2) {
        const int cnt = scf_count[i];
        if (scfsi & 8) {
            std::memcpy(scf, ist_pos, static_cast<size_t>(cnt));
        } else {
            const int bits = scf_size[i];
            if (!bits) {
                std::memset(scf, 0, static_cast<size_t>(cnt));
                std::memset(ist_pos, 0, static_cast<size_t>(cnt));
            } else {
                const int max_scf = (scfsi < 0) ? (1 << bits) - 1 : -1;
                for (int k = 0; k < cnt; k++) {
                    const int s = static_cast<int>(getBits(bitbuf, bits));
                    ist_pos[k] = static_cast<uint8_t>(s == max_scf ? -1 : s);
                    scf[k] = static_cast<uint8_t>(s);
                }
            }
        }
        ist_pos += cnt;
        scf += cnt;
    }
    scf[0] = scf[1] = scf[2] = 0;
}

float ldexpQ2(float y, int exp_q2) {
    int e;
    do {
        e = std::min(30 * 4, exp_q2);
        y *= kExpFrac[e & 3] * (1 << 30 >> (e >> 2));
    } while ((exp_q2 -= e) > 0);
    return y;
}

void decodeScalefactors(const uint8_t* hdr, uint8_t* ist_pos, BitReader& bs,
                        const GranuleInfo* gr, float* scf, int ch) {
    const int partition = (gr->n_short_sfb != 0 ? 1 : 0) + (gr->n_long_sfb == 0 ? 1 : 0);
    const uint8_t* scf_partition = kScfPartitions[partition];
    uint8_t scf_size[4];
    uint8_t iscf[40];
    const int scf_shift = gr->scalefac_scale + 1;
    int scfsi = gr->scfsi;

    if (hdrTestMpeg1(hdr)) {
        const int part = kScfcDecode[gr->scalefac_compress];
        scf_size[1] = scf_size[0] = static_cast<uint8_t>(part >> 2);
        scf_size[3] = scf_size[2] = static_cast<uint8_t>(part & 3);
    } else {
        int k;
        int modprod = 0;
        const int ist = hdrTestIStereo(hdr) && ch ? 1 : 0;
        int sfc = gr->scalefac_compress >> ist;
        for (k = ist * 3 * 4; sfc >= 0; sfc -= modprod, k += 4) {
            modprod = 1;
            for (int i = 3; i >= 0; i--) {
                scf_size[i] = static_cast<uint8_t>(sfc / modprod % kMod[k + i]);
                modprod *= kMod[k + i];
            }
        }
        scf_partition += k;
        scfsi = -16;
    }
    readScalefactors(iscf, ist_pos, scf_size, scf_partition, bs, scfsi);

    if (gr->n_short_sfb) {
        const int sh = 3 - scf_shift;
        for (int i = 0; i < gr->n_short_sfb; i += 3) {
            iscf[gr->n_long_sfb + i + 0] += gr->subblock_gain[0] << sh;
            iscf[gr->n_long_sfb + i + 1] += gr->subblock_gain[1] << sh;
            iscf[gr->n_long_sfb + i + 2] += gr->subblock_gain[2] << sh;
        }
    } else if (gr->preflag) {
        for (int i = 0; i < 10; i++) {
            iscf[11 + i] += kPreamp[i];
        }
    }

    const int gain_exp =
        gr->global_gain + kBitsDequantizerOut * 4 - 210 - (hdrIsMsStereo(hdr) ? 2 : 0);
    const float gain = ldexpQ2(static_cast<float>(1 << (kMaxScfI / 4)), kMaxScfI - gain_exp);
    for (int i = 0; i < gr->n_long_sfb + gr->n_short_sfb; i++) {
        scf[i] = ldexpQ2(gain, iscf[i] << scf_shift);
    }
}

float pow43(int x) {
    int sign;
    int mult = 256;

    if (x < 129) {
        return kPow43[16 + x];
    }
    if (x < 1024) {
        mult = 16;
        x <<= 3;
    }
    sign = 2 * x & 64;
    const float frac = static_cast<float>((x & 63) - sign) / ((x & ~63) + sign);
    return kPow43[16 + ((x + sign) >> 6)] * (1.f + frac * ((4.f / 3) + frac * (2.f / 9))) *
           static_cast<float>(mult);
}

void huffmanDecode(float* dst, BitReader& bs, const GranuleInfo* gr_info, const float* scf,
                   int layer3gr_limit) {
    float one = 0.0f;
    int big_val_cnt = gr_info->big_values;
    const uint8_t* sfb = gr_info->sfbtab;
    const uint8_t* bs_next_ptr = bs.buf + bs.pos / 8;
    uint32_t bs_cache = (((bs_next_ptr[0] * 256u + bs_next_ptr[1]) * 256u + bs_next_ptr[2]) * 256u +
                         bs_next_ptr[3])
                        << (bs.pos & 7);
    int bs_sh = (bs.pos & 7) - 8;
    bs_next_ptr += 4;

    const auto peekBits = [&](int n) -> uint32_t { return bs_cache >> (32 - n); };
    const auto flushBits = [&](int n) {
        bs_cache <<= n;
        bs_sh += n;
    };
    const auto checkBits = [&]() {
        while (bs_sh >= 0) {
            bs_cache |= static_cast<uint32_t>(*bs_next_ptr++) << bs_sh;
            bs_sh -= 8;
        }
    };
    const auto bsPos = [&]() {
        return static_cast<int>((bs_next_ptr - bs.buf) * 8 - 24 + bs_sh);
    };

    int ireg = 0;
    int np = 0;
    while (big_val_cnt > 0) {
        const int tab_num = gr_info->table_select[ireg];
        const int sfb_cnt = gr_info->region_count[ireg++];
        const int16_t* codebook = kHuffmanTabs + kHuffmanTabIndex[tab_num];
        const int linbits = kLinbits[tab_num];
        int sfb_left = sfb_cnt;
        if (linbits) {
            do {
                np = *sfb++ / 2;
                int pairs_to_decode = std::min(big_val_cnt, np);
                one = *scf++;
                do {
                    int w = 5;
                    int leaf = codebook[peekBits(w)];
                    while (leaf < 0) {
                        flushBits(w);
                        w = leaf & 7;
                        leaf = codebook[peekBits(w) - (leaf >> 3)];
                    }
                    flushBits(leaf >> 8);

                    for (int j = 0; j < 2; j++, dst++, leaf >>= 4) {
                        int lsb = leaf & 0x0F;
                        if (lsb == 15) {
                            lsb += static_cast<int>(peekBits(linbits));
                            flushBits(linbits);
                            checkBits();
                            *dst = one * pow43(lsb) *
                                   (static_cast<int32_t>(bs_cache) < 0 ? -1.f : 1.f);
                        } else {
                            *dst = kPow43[16 + lsb - 16 * static_cast<int>(bs_cache >> 31)] * one;
                        }
                        flushBits(lsb ? 1 : 0);
                    }
                    checkBits();
                } while (--pairs_to_decode);
            } while ((big_val_cnt -= np) > 0 && --sfb_left >= 0);
        } else {
            do {
                np = *sfb++ / 2;
                int pairs_to_decode = std::min(big_val_cnt, np);
                one = *scf++;
                do {
                    int w = 5;
                    int leaf = codebook[peekBits(w)];
                    while (leaf < 0) {
                        flushBits(w);
                        w = leaf & 7;
                        leaf = codebook[peekBits(w) - (leaf >> 3)];
                    }
                    flushBits(leaf >> 8);

                    for (int j = 0; j < 2; j++, dst++, leaf >>= 4) {
                        const int lsb = leaf & 0x0F;
                        *dst = kPow43[16 + lsb - 16 * static_cast<int>(bs_cache >> 31)] * one;
                        flushBits(lsb ? 1 : 0);
                    }
                    checkBits();
                } while (--pairs_to_decode);
            } while ((big_val_cnt -= np) > 0 && --sfb_left >= 0);
        }
    }

    np = 1 - big_val_cnt;
    for (;; dst += 4) {
        const uint8_t* codebook_count1 = gr_info->count1_table ? kHuffmanTab33 : kHuffmanTab32;
        int leaf = codebook_count1[peekBits(4)];
        if (!(leaf & 8)) {
            leaf = codebook_count1[(leaf >> 3) + ((bs_cache << 4) >> (32 - (leaf & 3)))];
        }
        flushBits(leaf & 7);
        if (bsPos() > layer3gr_limit) {
            break;
        }
        const auto reload = [&]() -> bool {
            if (--np == 0) {
                np = *sfb++ / 2;
                if (!np) {
                    return false;
                }
                one = *scf++;
            }
            return true;
        };
        const auto deq = [&](int s) {
            if (leaf & (128 >> s)) {
                dst[s] = static_cast<int32_t>(bs_cache) < 0 ? -one : one;
                flushBits(1);
            }
        };
        if (!reload()) {
            break;
        }
        deq(0);
        deq(1);
        if (!reload()) {
            break;
        }
        deq(2);
        deq(3);
        checkBits();
    }

    bs.pos = layer3gr_limit;
}

void midsideStereo(float* left, int n) {
    float* right = left + 576;
    for (int i = 0; i < n; i++) {
        const float a = left[i];
        const float b = right[i];
        left[i] = a + b;
        right[i] = a - b;
    }
}

void intensityStereoBand(float* left, int n, float kl, float kr) {
    for (int i = 0; i < n; i++) {
        left[i + 576] = left[i] * kr;
        left[i] = left[i] * kl;
    }
}

void stereoTopBand(const float* right, const uint8_t* sfb, int nbands, int max_band[3]) {
    max_band[0] = max_band[1] = max_band[2] = -1;

    for (int i = 0; i < nbands; i++) {
        for (int k = 0; k < sfb[i]; k += 2) {
            if (right[k] != 0 || right[k + 1] != 0) {
                max_band[i % 3] = i;
                break;
            }
        }
        right += sfb[i];
    }
}

void stereoProcess(float* left, const uint8_t* ist_pos, const uint8_t* sfb, const uint8_t* hdr,
                   int max_band[3], int mpeg2_sh) {
    const unsigned max_pos = hdrTestMpeg1(hdr) ? 7 : 64;

    for (unsigned i = 0; sfb[i]; i++) {
        const unsigned ipos = ist_pos[i];
        if (static_cast<int>(i) > max_band[i % 3] && ipos < max_pos) {
            float kl;
            float kr;
            const float s = hdrTestMsStereo(hdr) ? 1.41421356f : 1;
            if (hdrTestMpeg1(hdr)) {
                kl = kPan[2 * ipos];
                kr = kPan[2 * ipos + 1];
            } else {
                kl = 1;
                kr = ldexpQ2(1, (static_cast<int>(ipos) + 1) >> 1 << mpeg2_sh);
                if (ipos & 1) {
                    kl = kr;
                    kr = 1;
                }
            }
            intensityStereoBand(left, sfb[i], kl * s, kr * s);
        } else if (hdrTestMsStereo(hdr)) {
            midsideStereo(left, sfb[i]);
        }
        left += sfb[i];
    }
}

void intensityStereo(float* left, uint8_t* ist_pos, const GranuleInfo* gr, const uint8_t* hdr) {
    int max_band[3];
    const int n_sfb = gr->n_long_sfb + gr->n_short_sfb;
    const int max_blocks = gr->n_short_sfb ? 3 : 1;

    stereoTopBand(left + 576, gr->sfbtab, n_sfb, max_band);
    if (gr->n_long_sfb) {
        max_band[0] = max_band[1] = max_band[2] =
            std::max(std::max(max_band[0], max_band[1]), max_band[2]);
    }
    for (int i = 0; i < max_blocks; i++) {
        const int default_pos = hdrTestMpeg1(hdr) ? 3 : 0;
        const int itop = n_sfb - max_blocks + i;
        const int prev = itop - max_blocks;
        ist_pos[itop] = max_band[i] >= prev ? static_cast<uint8_t>(default_pos) : ist_pos[prev];
    }
    stereoProcess(left, ist_pos, gr->sfbtab, hdr, max_band, gr[1].scalefac_compress & 1);
}

void reorder(float* grbuf, float* scratch, const uint8_t* sfb) {
    float* src = grbuf;
    float* dst = scratch;

    for (int len = *sfb; len != 0; sfb += 3, src += 2 * len, len = *sfb) {
        for (int i = 0; i < len; i++, src++) {
            *dst++ = src[0 * len];
            *dst++ = src[1 * len];
            *dst++ = src[2 * len];
        }
    }
    std::memcpy(grbuf, scratch, static_cast<size_t>(dst - scratch) * sizeof(float));
}

void antialias(float* grbuf, int nbands) {
    for (; nbands > 0; nbands--, grbuf += 18) {
        for (int i = 0; i < 8; i++) {
            const float u = grbuf[18 + i];
            const float d = grbuf[17 - i];
            grbuf[18 + i] = u * kAliasReduction[0][i] - d * kAliasReduction[1][i];
            grbuf[17 - i] = u * kAliasReduction[1][i] + d * kAliasReduction[0][i];
        }
    }
}

void dct3_9(float* y) {
    float s0 = y[0];
    float s2 = y[2];
    float s4 = y[4];
    float s6 = y[6];
    const float s8 = y[8];
    float t0 = s0 + s6 * 0.5f;
    s0 -= s6;
    const float t4 = (s4 + s2) * 0.93969262f;
    const float t2 = (s8 + s2) * 0.76604444f;
    s6 = (s4 - s8) * 0.17364818f;
    s4 += s8 - s2;

    s2 = s0 - s4 * 0.5f;
    y[4] = s4 + s0;
    float s8_out = t0 - t2 + s6;
    float s0_out = t0 - t4 + t2;
    float s4_out = t0 + t4 - s6;

    float s1 = y[1];
    const float s3_in = y[3];
    float s5 = y[5];
    const float s7 = y[7];

    const float s3 = s3_in * 0.86602540f;
    t0 = (s5 + s1) * 0.98480775f;
    const float t4b = (s5 - s7) * 0.34202014f;
    const float t2b = (s1 + s7) * 0.64278761f;
    s1 = (s1 - s5 - s7) * 0.86602540f;

    s5 = t0 - s3 - t2b;
    const float s7_out = t4b - s3 - t0;
    const float s3_out = t4b + s3 - t2b;

    y[0] = s4_out - s7_out;
    y[1] = s2 + s1;
    y[2] = s0_out - s3_out;
    y[3] = s8_out + s5;
    y[5] = s8_out - s5;
    y[6] = s0_out + s3_out;
    y[7] = s2 - s1;
    y[8] = s4_out + s7_out;
}

void imdct36(float* grbuf, float* overlap, const float* window, int nbands) {
    for (int j = 0; j < nbands; j++, grbuf += 18, overlap += 9) {
        float co[9];
        float si[9];
        co[0] = -grbuf[0];
        si[0] = grbuf[17];
        for (int i = 0; i < 4; i++) {
            si[8 - 2 * i] = grbuf[4 * i + 1] - grbuf[4 * i + 2];
            co[1 + 2 * i] = grbuf[4 * i + 1] + grbuf[4 * i + 2];
            si[7 - 2 * i] = grbuf[4 * i + 4] - grbuf[4 * i + 3];
            co[2 + 2 * i] = -(grbuf[4 * i + 3] + grbuf[4 * i + 4]);
        }
        dct3_9(co);
        dct3_9(si);

        si[1] = -si[1];
        si[3] = -si[3];
        si[5] = -si[5];
        si[7] = -si[7];

        for (int i = 0; i < 9; i++) {
            const float ovl = overlap[i];
            const float sum = co[i] * kTwid9[9 + i] + si[i] * kTwid9[0 + i];
            overlap[i] = co[i] * kTwid9[0 + i] - si[i] * kTwid9[9 + i];
            grbuf[i] = ovl * window[0 + i] - sum * window[9 + i];
            grbuf[17 - i] = ovl * window[9 + i] + sum * window[0 + i];
        }
    }
}

void idct3(float x0, float x1, float x2, float* dst) {
    const float m1 = x1 * 0.86602540f;
    const float a1 = x0 - x2 * 0.5f;
    dst[1] = x0 + x2;
    dst[0] = a1 + m1;
    dst[2] = a1 - m1;
}

void imdct12(float* x, float* dst, float* overlap) {
    float co[3];
    float si[3];

    idct3(-x[0], x[6] + x[3], x[12] + x[9], co);
    idct3(x[15], x[12] - x[9], x[6] - x[3], si);
    si[1] = -si[1];

    for (int i = 0; i < 3; i++) {
        const float ovl = overlap[i];
        const float sum = co[i] * kTwid3[3 + i] + si[i] * kTwid3[0 + i];
        overlap[i] = co[i] * kTwid3[0 + i] - si[i] * kTwid3[3 + i];
        dst[i] = ovl * kTwid3[2 - i] - sum * kTwid3[5 - i];
        dst[5 - i] = ovl * kTwid3[5 - i] + sum * kTwid3[2 - i];
    }
}

void imdctShort(float* grbuf, float* overlap, int nbands) {
    for (; nbands > 0; nbands--, overlap += 9, grbuf += 18) {
        float tmp[18];
        std::memcpy(tmp, grbuf, sizeof(tmp));
        std::memcpy(grbuf, overlap, 6 * sizeof(float));
        imdct12(tmp, grbuf + 6, overlap + 6);
        imdct12(tmp + 1, grbuf + 12, overlap + 6);
        imdct12(tmp + 2, overlap, overlap + 6);
    }
}

void changeSign(float* grbuf) {
    grbuf += 18;
    for (int b = 0; b < 32; b += 2, grbuf += 36) {
        for (int i = 1; i < 18; i += 2) {
            grbuf[i] = -grbuf[i];
        }
    }
}

void imdctGranule(float* grbuf, float* overlap, unsigned block_type, unsigned n_long_bands) {
    if (n_long_bands) {
        imdct36(grbuf, overlap, kMdctWindow[0], n_long_bands);
        grbuf += 18 * n_long_bands;
        overlap += 9 * n_long_bands;
    }
    if (block_type == kShortBlockType) {
        imdctShort(grbuf, overlap, 32 - static_cast<int>(n_long_bands));
    } else {
        imdct36(grbuf, overlap, kMdctWindow[block_type == kStopBlockType ? 1 : 0],
                32 - static_cast<int>(n_long_bands));
    }
}

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
            const float t2 = (x1 - x2) * kSec[3 * i + 0];
            const float t3 = (x0 - x3) * kSec[3 * i + 1];
            x[0] = t0 + t1;
            x[8] = (t0 - t1) * kSec[3 * i + 2];
            x[16] = t3 + t2;
            x[24] = (t3 - t2) * kSec[3 * i + 2];
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
    const float* w = kSynthesisWindow;

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

        // The accumulation order matches the reference.
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

int matchFrame(const uint8_t* hdr, int mp3_bytes, int frame_bytes) {
    int i = 0;
    int nmatch;
    for (nmatch = 0; nmatch < kMaxFrameSyncMatches; nmatch++) {
        i += hdrFrameBytes(hdr + i, frame_bytes) + hdrPadding(hdr + i);
        if (i + kHeaderSize > mp3_bytes) {
            return nmatch > 0 ? 1 : 0;
        }
        if (!hdrCompare(hdr, hdr + i)) {
            return 0;
        }
    }
    return 1;
}

int findFrame(const uint8_t* mp3, int mp3_bytes, int* free_format_bytes, int* ptr_frame_bytes) {
    for (int i = 0; i < mp3_bytes - kHeaderSize; i++, mp3++) {
        if (hdrValid(mp3)) {
            int frame_bytes = hdrFrameBytes(mp3, *free_format_bytes);
            int frame_and_padding = frame_bytes + hdrPadding(mp3);

            for (int k = kHeaderSize;
                 !frame_bytes && k < kMaxFreeFormatFrameSize && i + 2 * k < mp3_bytes - kHeaderSize;
                 k++) {
                if (hdrCompare(mp3, mp3 + k)) {
                    const int fb = k - hdrPadding(mp3);
                    const int nextfb = fb + hdrPadding(mp3 + k);
                    if (i + k + nextfb + kHeaderSize > mp3_bytes ||
                        !hdrCompare(mp3, mp3 + k + nextfb)) {
                        continue;
                    }
                    frame_and_padding = k;
                    frame_bytes = fb;
                    *free_format_bytes = fb;
                }
            }
            if ((frame_bytes && i + frame_and_padding <= mp3_bytes &&
                 matchFrame(mp3, mp3_bytes - i, frame_bytes)) ||
                (!i && frame_and_padding == mp3_bytes)) {
                *ptr_frame_bytes = frame_and_padding;
                return i;
            }
            *free_format_bytes = 0;
        }
    }
    *ptr_frame_bytes = 0;
    return mp3_bytes;
}

void decodeGranule(const uint8_t* header, Scratch& s, GranuleInfo* gr_info, int nch,
                   float mdct_overlap[2][9 * 32]) {
    for (int ch = 0; ch < nch; ch++) {
        const int layer3gr_limit = s.bs_frame.pos + gr_info[ch].part23_length;
        decodeScalefactors(header, s.ist_pos[ch], s.bs_frame, gr_info + ch, s.scf, ch);
        huffmanDecode(s.grbuf[ch], s.bs_frame, gr_info + ch, s.scf, layer3gr_limit);
    }

    if (hdrTestIStereo(header)) {
        intensityStereo(s.grbuf[0], s.ist_pos[1], gr_info, header);
    } else if (hdrIsMsStereo(header)) {
        midsideStereo(s.grbuf[0], 576);
    }

    for (int ch = 0; ch < nch; ch++, gr_info++) {
        int aa_bands = 31;
        const int n_long_bands =
            (gr_info->mixed_block_flag ? 2 : 0) << static_cast<int>(hdrMySampleRate(header) == 2);

        if (gr_info->n_short_sfb) {
            aa_bands = n_long_bands - 1;
            reorder(s.grbuf[ch] + n_long_bands * 18, s.syn[0],
                    gr_info->sfbtab + gr_info->n_long_sfb);
        }

        antialias(s.grbuf[ch], aa_bands);
        imdctGranule(s.grbuf[ch], mdct_overlap[ch], gr_info->block_type,
                     static_cast<unsigned>(n_long_bands));
        changeSign(s.grbuf[ch]);
    }
}

}  // namespace

void Decoder::resetState() {
    header_[0] = 0;
    header_[1] = 0;
    header_[2] = 0;
    header_[3] = 0;
    free_format_bytes_ = 0;
    reservoir_ = 0;
    std::memset(reservoir_buf_, 0, sizeof(reservoir_buf_));
    std::memset(mdct_overlap_, 0, sizeof(mdct_overlap_));
    std::memset(qmf_state_, 0, sizeof(qmf_state_));
}

bool Decoder::configure(const uint8_t* config, size_t size, std::string& error) {
    if (config && size != 0) {
        error = "MP3 has no extradata";
        return false;
    }
    reset();
    configured_ = true;
    return true;
}

void Decoder::flush() {
    resetState();
}

void Decoder::reset() {
    resetState();
    configured_ = false;
    sample_rate_ = 0;
    channels_ = 0;
    pts_ = 0;
}

Status Decoder::decode(const uint8_t* data, size_t size, PcmBuffer& pcm, std::string& error) {
    pcm.clear();
    if (!configured_) {
        error = "MP3 decoder not configured";
        return Status::Invalid;
    }
    if (!data || size < kHeaderSize) {
        error = "short MP3 frame";
        return Status::Invalid;
    }

    const uint8_t* mp3 = data;
    const int mp3_bytes = static_cast<int>(size);
    int i = 0;
    int frame_size = 0;

    // Fast path: same header as the previous frame.
    if (mp3_bytes > 4 && header_[0] == 0xFF && hdrCompare(header_, mp3)) {
        frame_size = hdrFrameBytes(mp3, free_format_bytes_) + hdrPadding(mp3);
        if (frame_size != mp3_bytes &&
            (frame_size + kHeaderSize > mp3_bytes || !hdrCompare(mp3, mp3 + frame_size))) {
            frame_size = 0;
        }
    }
    if (!frame_size) {
        resetState();
        i = findFrame(mp3, mp3_bytes, &free_format_bytes_, &frame_size);
        if (!frame_size || i + frame_size > mp3_bytes) {
            error = "no MP3 frame found";
            return Status::Invalid;
        }
    }

    const uint8_t* hdr = mp3 + i;
    std::memcpy(header_, hdr, kHeaderSize);
    const int channels = hdrIsMono(hdr) ? 1 : 2;
    const int hz = static_cast<int>(hdrSampleRateHz(hdr));
    const int layer = 4 - static_cast<int>(hdrGetLayer(hdr));
    if (layer != 3) {
        error = "only MPEG Layer III (MP3) is supported";
        return Status::Unsupported;
    }

    BitReader bs_frame;
    bsInit(bs_frame, hdr + kHeaderSize, frame_size - kHeaderSize);
    if (hdrIsCrc(hdr)) {
        getBits(bs_frame, 16);
    }

    Scratch scratch;
    scratch.bs_frame = bs_frame;
    const int main_data_begin = readSideInfo(scratch.bs_frame, scratch.gr_info, hdr);
    if (main_data_begin < 0 || scratch.bs_frame.pos > scratch.bs_frame.limit) {
        header_[0] = 0;
        error = "invalid MP3 side info";
        return Status::Invalid;
    }

    // Bit reservoir: prepend the previously saved main data.
    const int frame_bytes = (scratch.bs_frame.limit - scratch.bs_frame.pos) / 8;
    const int bytes_have = std::min(reservoir_, main_data_begin);
    std::memcpy(scratch.maindata,
                reservoir_buf_ + std::max(0, reservoir_ - main_data_begin),
                static_cast<size_t>(std::min(reservoir_, main_data_begin)));
    std::memcpy(scratch.maindata + bytes_have, scratch.bs_frame.buf + scratch.bs_frame.pos / 8,
                static_cast<size_t>(frame_bytes));
    bsInit(scratch.bs_frame, scratch.maindata, bytes_have + frame_bytes);
    const bool success = reservoir_ >= main_data_begin;

    const int samples_per_channel = static_cast<int>(hdrFrameSamples(hdr));
    if (success) {
        for (int igr = 0; igr < (hdrTestMpeg1(hdr) ? 2 : 1); igr++) {
            std::memset(scratch.grbuf[0], 0, 576 * 2 * sizeof(float));
            decodeGranule(hdr, scratch, scratch.gr_info + igr * channels, channels, mdct_overlap_);
            synthGranule(qmf_state_, scratch.grbuf[0], 18, channels,
                         scratch.pcm + igr * 576 * channels, scratch.syn[0]);
        }
    }

    // Save the main data that was not consumed as the next frame's reservoir.
    {
        int pos = (scratch.bs_frame.pos + 7) / 8;
        int remains = scratch.bs_frame.limit / 8 - pos;
        if (remains > kMaxBitReservoirBytes) {
            pos += remains - kMaxBitReservoirBytes;
            remains = kMaxBitReservoirBytes;
        }
        if (remains > 0) {
            std::memmove(reservoir_buf_, scratch.maindata + pos, static_cast<size_t>(remains));
        }
        reservoir_ = remains;
    }

    sample_rate_ = static_cast<uint32_t>(hz);
    channels_ = static_cast<uint32_t>(channels);
    pcm.channels = channels_;
    pcm.sample_rate = sample_rate_;
    pcm.pts = pts_;
    const int total = success ? samples_per_channel * channels : 0;
    pcm.samples.resize(static_cast<size_t>(total));
    for (int k = 0; k < total; ++k) {
        pcm.samples[k] = scratch.pcm[k] * (1.0f / 32768.0f);
    }
    pts_ += samples_per_channel;
    return Status::Ok;
}

}  // namespace player::audio::mp3


