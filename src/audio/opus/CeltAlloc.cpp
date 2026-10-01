#include "audio/opus/CeltAlloc.hpp"

#include <algorithm>

namespace player::audio::opus {
namespace {

constexpr int kAllocSteps = 6;
constexpr int kFineOffset = 21;
constexpr int kAllocVectors = 11;

// The reference uses unsigned division for non-negative quantities.
int32_t udiv(int32_t value, int32_t divisor) {
    return static_cast<int32_t>(static_cast<uint32_t>(value) / static_cast<uint32_t>(divisor));
}

//    Splits the total bit budget between the bands: a bisection over the
//    allocation vectors, backward skip decisions (a reserved bit per skipped
//    band), the intensity/dual-stereo parameters and finally the fine energy
//    versus PVQ split per band (RFC 6716 4.3.3).
int interpBits2Pulses(int start, int end, int skip_start, const int* bits1, const int* bits2,
                      const int* thresh, const int* cap, int32_t total, int32_t& balance_out,
                      int skip_rsv, int& intensity, int intensity_rsv, int& dual_stereo,
                      int dual_stereo_rsv, int* pulses, int* ebits, int* fine_priority,
                      int channels, int lm, RangeDecoder& decoder) {
    const int alloc_floor = channels << kCeltBitsRes;
    const int stereo = channels > 1 ? 1 : 0;
    const int log_m = lm << kCeltBitsRes;

    // Bisection over the interpolation factor between two allocation vectors.
    int lo = 0;
    int hi = 1 << kAllocSteps;
    for (int i = 0; i < kAllocSteps; ++i) {
        const int mid = (lo + hi) >> 1;
        int32_t psum = 0;
        int done = 0;
        for (int j = end; j-- > start;) {
            const int tmp = bits1[j] + (mid * bits2[j] >> kAllocSteps);
            if (tmp >= thresh[j] || done) {
                done = 1;
                psum += std::min(tmp, cap[j]);
            } else if (tmp >= alloc_floor) {
                psum += alloc_floor;
            }
        }
        if (psum > total) {
            hi = mid;
        } else {
            lo = mid;
        }
    }

    int32_t psum = 0;
    int done = 0;
    for (int j = end; j-- > start;) {
        int tmp = bits1[j] + (lo * bits2[j] >> kAllocSteps);
        if (tmp < thresh[j] && !done) {
            tmp = tmp >= alloc_floor ? alloc_floor : 0;
        } else {
            done = 1;
        }
        tmp = std::min(tmp, cap[j]);
        pulses[j] = tmp;
        psum += tmp;
    }

    // Decide which bands to skip, working backwards from the end.
    int coded_bands = end;
    for (;; --coded_bands) {
        const int j = coded_bands - 1;
        if (j <= skip_start) {
            total += skip_rsv;  // give the reserved bit back
            break;
        }
        int32_t left = total - psum;
        const int percoeff = udiv(left, kCeltEBands[coded_bands] - kCeltEBands[start]);
        left -= (kCeltEBands[coded_bands] - kCeltEBands[start]) * percoeff;
        const int rem =
            std::max(static_cast<int>(left - (kCeltEBands[j] - kCeltEBands[start])), 0);
        const int band_width = kCeltEBands[coded_bands] - kCeltEBands[j];
        int band_bits = pulses[j] + percoeff * band_width + rem;
        if (band_bits >= std::max(thresh[j], alloc_floor + (1 << kCeltBitsRes))) {
            if (decoder.decodeBitLogp(1)) {
                break;
            }
            psum += 1 << kCeltBitsRes;
            band_bits -= 1 << kCeltBitsRes;
        }
        psum -= pulses[j] + intensity_rsv;
        if (intensity_rsv > 0) {
            intensity_rsv = kCeltLog2Frac[j - start];
        }
        psum += intensity_rsv;
        if (band_bits >= alloc_floor) {
            psum += alloc_floor;
            pulses[j] = alloc_floor;
        } else {
            pulses[j] = 0;
        }
    }

    // Intensity and dual stereo parameters.
    if (intensity_rsv > 0) {
        intensity =
            start + static_cast<int>(decoder.decodeUint(static_cast<uint32_t>(coded_bands + 1 - start)));
    } else {
        intensity = 0;
    }
    if (intensity <= start) {
        total += dual_stereo_rsv;
        dual_stereo_rsv = 0;
    }
    if (dual_stereo_rsv > 0) {
        dual_stereo = decoder.decodeBitLogp(1) ? 1 : 0;
    } else {
        dual_stereo = 0;
    }

    // Distribute the remaining bits over the coded bands.
    int32_t left = total - psum;
    int32_t percoeff = udiv(left, kCeltEBands[coded_bands] - kCeltEBands[start]);
    left -= (kCeltEBands[coded_bands] - kCeltEBands[start]) * percoeff;
    for (int j = start; j < coded_bands; ++j) {
        pulses[j] += static_cast<int>(percoeff * (kCeltEBands[j + 1] - kCeltEBands[j]));
    }
    for (int j = start; j < coded_bands; ++j) {
        const int tmp = std::min(static_cast<int>(left), kCeltEBands[j + 1] - kCeltEBands[j]);
        pulses[j] += tmp;
        left -= tmp;
    }

    // Split each band between fine energy and PVQ.
    int32_t balance = 0;
    int j = start;
    for (; j < coded_bands; ++j) {
        const int n0 = kCeltEBands[j + 1] - kCeltEBands[j];
        const int n = n0 << lm;
        const int32_t bit = pulses[j] + balance;
        int32_t excess = 0;
        if (n > 1) {
            excess = std::max(bit - cap[j], static_cast<int32_t>(0));
            pulses[j] = static_cast<int>(bit - excess);
            const int den = channels * n +
                            ((channels == 2 && n > 2 && dual_stereo == 0 && j < intensity) ? 1 : 0);
            const int nclogn = den * (kCeltLogN[j] + log_m);
            int offset = (nclogn >> 1) - den * kFineOffset;
            if (n == 2) {
                offset += den << kCeltBitsRes >> 2;
            }
            if (pulses[j] + offset < den * 2 << kCeltBitsRes) {
                offset += nclogn >> 2;
            } else if (pulses[j] + offset < den * 3 << kCeltBitsRes) {
                offset += nclogn >> 3;
            }
            ebits[j] = std::max(0, pulses[j] + offset + (den << (kCeltBitsRes - 1)));
            ebits[j] = udiv(ebits[j], den) >> kCeltBitsRes;
            if (channels * ebits[j] > (pulses[j] >> kCeltBitsRes)) {
                ebits[j] = pulses[j] >> stereo >> kCeltBitsRes;
            }
            ebits[j] = std::min(ebits[j], kCeltMaxFineBits);
            fine_priority[j] = ebits[j] * (den << kCeltBitsRes) >= pulses[j] + offset ? 1 : 0;
            pulses[j] -= channels * ebits[j] << kCeltBitsRes;
        } else {
            excess = std::max(static_cast<int32_t>(0), bit - (channels << kCeltBitsRes));
            pulses[j] = static_cast<int>(bit - excess);
            ebits[j] = 0;
            fine_priority[j] = 1;
        }
        if (excess > 0) {
            const int extra_fine =
                std::min(static_cast<int>(excess >> (stereo + kCeltBitsRes)),
                         kCeltMaxFineBits - ebits[j]);
            ebits[j] += extra_fine;
            const int extra_bits = extra_fine * channels << kCeltBitsRes;
            fine_priority[j] = extra_bits >= excess - balance ? 1 : 0;
            excess -= extra_bits;
        }
        balance = excess;
    }
    for (; j < end; ++j) {
        ebits[j] = pulses[j] >> stereo >> kCeltBitsRes;
        pulses[j] = 0;
        fine_priority[j] = ebits[j] < 1 ? 1 : 0;
    }

    balance_out = balance;
    return coded_bands;
}

}  // namespace

void initCaps(int* cap, int lm, int channels) {
    for (int i = 0; i < kCeltBands; ++i) {
        const int n = (kCeltEBands[i + 1] - kCeltEBands[i]) << lm;
        cap[i] = (kCeltCacheCaps[kCeltBands * (2 * lm + channels - 1) + i] + 64) * channels * n >> 2;
    }
}

void computeAllocation(RangeDecoder& decoder, int start, int end, const int* offsets,
                       const int* cap, int alloc_trim, int32_t total, int channels, int lm,
                       Allocation& out) {
    total = std::max<int32_t>(total, 0);
    const int len = kCeltBands;
    int skip_start = start;
    int skip_rsv = total >= 1 << kCeltBitsRes ? 1 << kCeltBitsRes : 0;
    total -= skip_rsv;
    int intensity_rsv = 0;
    int dual_stereo_rsv = 0;
    if (channels == 2) {
        intensity_rsv = kCeltLog2Frac[end - start];
        if (intensity_rsv > total) {
            intensity_rsv = 0;
        } else {
            total -= intensity_rsv;
            dual_stereo_rsv = total >= 1 << kCeltBitsRes ? 1 << kCeltBitsRes : 0;
            total -= dual_stereo_rsv;
        }
    }

    int bits1[kCeltBands] = {};
    int bits2[kCeltBands] = {};
    int thresh[kCeltBands] = {};
    int trim_offset[kCeltBands] = {};
    for (int j = start; j < end; ++j) {
        thresh[j] = std::max(channels << kCeltBitsRes,
                             (3 * (kCeltEBands[j + 1] - kCeltEBands[j]) << lm << kCeltBitsRes) >> 4);
        trim_offset[j] = channels * (kCeltEBands[j + 1] - kCeltEBands[j]) * (alloc_trim - 5 - lm) *
                         (end - j - 1) * (1 << (lm + kCeltBitsRes)) >> 6;
        if ((kCeltEBands[j + 1] - kCeltEBands[j]) << lm == 1) {
            trim_offset[j] -= channels << kCeltBitsRes;
        }
    }

    // Pick the two allocation vectors to interpolate between.
    int lo = 1;
    int hi = kAllocVectors - 1;
    do {
        int done = 0;
        int32_t psum = 0;
        const int mid = (lo + hi) >> 1;
        for (int j = end; j-- > start;) {
            const int n = kCeltEBands[j + 1] - kCeltEBands[j];
            int bitsj = channels * n * kCeltBandAllocation[mid * len + j] << lm >> 2;
            if (bitsj > 0) {
                bitsj = std::max(0, bitsj + trim_offset[j]);
            }
            bitsj += offsets[j];
            if (bitsj >= thresh[j] || done) {
                done = 1;
                psum += std::min(bitsj, cap[j]);
            } else if (bitsj >= channels << kCeltBitsRes) {
                psum += channels << kCeltBitsRes;
            }
        }
        if (psum > total) {
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    } while (lo <= hi);
    hi = lo--;

    for (int j = start; j < end; ++j) {
        const int n = kCeltEBands[j + 1] - kCeltEBands[j];
        int bits1j = channels * n * kCeltBandAllocation[lo * len + j] << lm >> 2;
        int bits2j = hi >= kAllocVectors
                         ? cap[j]
                         : channels * n * kCeltBandAllocation[hi * len + j] << lm >> 2;
        if (bits1j > 0) {
            bits1j = std::max(0, bits1j + trim_offset[j]);
        }
        if (bits2j > 0) {
            bits2j = std::max(0, bits2j + trim_offset[j]);
        }
        if (lo > 0) {
            bits1j += offsets[j];
        }
        bits2j += offsets[j];
        if (offsets[j] > 0) {
            skip_start = j;
        }
        bits2j = std::max(0, bits2j - bits1j);
        bits1[j] = bits1j;
        bits2[j] = bits2j;
    }

    out.coded_bands = interpBits2Pulses(start, end, skip_start, bits1, bits2, thresh, cap, total,
                                        out.balance, skip_rsv, out.intensity, intensity_rsv,
                                        out.dual_stereo, dual_stereo_rsv, out.pulses, out.fine_quant,
                                        out.fine_priority, channels, lm, decoder);
}

}  // namespace player::audio::opus
