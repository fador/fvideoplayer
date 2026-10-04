#include "audio/ac3/Ac3Decoder.hpp"

#include "audio/ac3/Ac3Tables.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace player::audio::ac3 {
namespace {

constexpr int kBlockSize = 256;
constexpr int kBlocks = 6;
constexpr int kSamplesPerFrame = kBlockSize * kBlocks;
constexpr int kCriticalBands = 50;
constexpr int kMaxCplBands = 18;
constexpr int kMaxCoeffs = 256;

// WAVE-order output slot for each AC-3 channel: 0 = L, 1 = C, 2 = R,
// 3 = Ls, 4 = Rs, 5 = LFE. The output layout is FL, FR, FC, LFE, BL, BR.
constexpr uint8_t kOutputMap[8][2][6] = {
    {{0, 1}, {0, 1, 2}},
    {{0}, {0, 1}},
    {{0, 1}, {0, 1, 2}},
    {{0, 2, 1}, {0, 2, 1, 3}},
    {{0, 1, 2}, {0, 1, 3, 2}},
    {{0, 2, 1, 3}, {0, 2, 1, 4, 3}},
    {{0, 1, 2, 3}, {0, 1, 3, 4, 2}},
    {{0, 2, 1, 3, 4}, {0, 2, 1, 4, 5, 3}},
};

// Default coupling band structure (A/52 E2.16); the decoder reads the actual
// bits for every block, so this only seeds block 0.
constexpr uint8_t kDefaultCplBandStruct[18] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 0,
                                               1, 1, 0, 1, 1, 1, 1, 1};

const uint8_t kCenterLevels[4] = {4, 5, 6, 5};
const uint8_t kSurroundLevels[4] = {4, 6, 7, 6};

// Modified Bessel function of the first kind, order zero, by series expansion.
double besselI0(double x) {
    double sum = 1.0;
    double term = 1.0;
    const double half = x * x / 4.0;
    for (int k = 1; k < 60; ++k) {
        term *= half / (static_cast<double>(k) * k);
        sum += term;
        if (term < 1e-16 * sum) {
            break;
        }
    }
    return sum;
}

// AC-3's 512-sample window is the symmetric KBD window of length 512; the
// first half is generated here and the overlap uses the mirrored second half.
void kbdWindow(float* window, double alpha, int n) {
    double temp[129] = {};
    const double alpha2 = 4.0 * (alpha * 3.14159265358979323846 / n) *
                          (alpha * 3.14159265358979323846 / n);
    double scale = 0.0;
    for (int i = 0; i <= n / 2; ++i) {
        temp[i] = besselI0(std::sqrt(i * (n - i) * alpha2));
        scale += temp[i] * (1.0 + ((i && i < n / 2) ? 1.0 : 0.0));
    }
    scale = 1.0 / (scale + 1.0);
    double sum = 0.0;
    for (int i = 0; i <= n / 2; ++i) {
        sum += temp[i];
        window[i] = static_cast<float>(std::sqrt(sum * scale));
    }
    for (int i = n / 2 + 1; i < n; ++i) {
        sum += temp[n - i];
        window[i] = static_cast<float>(std::sqrt(sum * scale));
    }
}

int symmetricDequant(int code, int levels) {
    return ((code - (levels >> 1)) * (1 << 24)) / levels;
}

struct GeneratedTables {
    int8_t ungroup7[128][3] = {};
    int bap1[32][3] = {};
    int bap2[128][3] = {};
    int bap4[128][2] = {};
    int bap3[8] = {};
    int bap5[16] = {};
    float window[256] = {};
    float scale[25] = {};
    float dynamicRange[256] = {};
    float heavyRange[256] = {};

    GeneratedTables() {
        for (int i = 0; i < 128; ++i) {
            ungroup7[i][0] = static_cast<int8_t>(i / 25);
            ungroup7[i][1] = static_cast<int8_t>((i % 25) / 5);
            ungroup7[i][2] = static_cast<int8_t>((i % 25) % 5);
        }
        for (int i = 0; i < 32; ++i) {
            const int values[3] = {i / 9, (i % 9) / 3, (i % 9) % 3};
            for (int k = 0; k < 3; ++k) {
                bap1[i][k] = symmetricDequant(values[k], 3);
            }
        }
        for (int i = 0; i < 128; ++i) {
            for (int k = 0; k < 3; ++k) {
                bap2[i][k] = symmetricDequant(ungroup7[i][k], 5);
            }
            bap4[i][0] = symmetricDequant(i / 11, 11);
            bap4[i][1] = symmetricDequant(i % 11, 11);
        }
        for (int i = 0; i < 8; ++i) {
            bap3[i] = i < 7 ? symmetricDequant(i, 7) : 0;
        }
        for (int i = 0; i < 16; ++i) {
            bap5[i] = i < 15 ? symmetricDequant(i, 15) : 0;
        }
        kbdWindow(window, 5.0, 256);
        for (int e = 0; e < 25; ++e) {
            scale[e] = std::ldexp(1.0f, -e);
        }
        for (int i = 0; i < 256; ++i) {
            const int v = (i >> 5) - ((i >> 7) << 3) - 5;
            dynamicRange[i] = std::pow(2.0f, static_cast<float>(v)) *
                              static_cast<float>((i & 0x1F) | 0x20);
            const int hv = (i >> 4) - ((i >> 7) << 4) - 4;
            heavyRange[i] = std::pow(2.0f, static_cast<float>(hv)) *
                            static_cast<float>((i & 0xF) | 0x10);
        }
    }
};

const GeneratedTables& tables() {
    static const GeneratedTables instance;
    return instance;
}

int clipUint6(int value) {
    return std::clamp(value, 0, 63);
}

// Complex value for the inverse FFT inside the transform.
struct Complex {
    double re = 0.0;
    double im = 0.0;
};

// Unnormalised inverse FFT (the AC-3 transform absorbs the 1/N).
void inverseFft(Complex* buf, int n) {
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(buf[i], buf[j]);
        }
    }
    constexpr double kPi = 3.14159265358979323846;
    for (int len = 2; len <= n; len <<= 1) {
        const double angle = 2.0 * kPi / len;
        const Complex w{std::cos(angle), std::sin(angle)};
        for (int i = 0; i < n; i += len) {
            Complex wn{1.0, 0.0};
            for (int j = 0; j < len / 2; ++j) {
                const Complex u = buf[i + j];
                const Complex v{buf[i + j + len / 2].re * wn.re - buf[i + j + len / 2].im * wn.im,
                                buf[i + j + len / 2].re * wn.im + buf[i + j + len / 2].im * wn.re};
                buf[i + j] = {u.re + v.re, u.im + v.im};
                buf[i + j + len / 2] = {u.re - v.re, u.im - v.im};
                wn = {wn.re * w.re - wn.im * w.im, wn.re * w.im + wn.im * w.re};
            }
        }
    }
}

// Overlap-add of the windowed block with the previous one. The transform
// equations run with the reference decoder's sign convention and without the
// specification's headroom factor, which its coefficient scaling absorbs.
void overlapAdd(const float* x, float* delay, float* out) {
    for (int n = 0; n < 256; ++n) {
        out[n] = x[n] + delay[n];
        delay[n] = x[256 + n];
    }
}

// 512-sample inverse MDCT (A/52 7.9.4.1): 256 coefficients in, 512 windowed
// samples out. Uses a 128-point complex inverse FFT.
void imdct512(const float* X, float* delay, const float* w, float* out) {
    constexpr double kPi = 3.14159265358979323846;
    constexpr int N = 512;
    Complex z[128];
    for (int k = 0; k < 128; ++k) {
        const double xc = -std::cos(2.0 * kPi * (8 * k + 1) / (8.0 * N));
        const double xs = -std::sin(2.0 * kPi * (8 * k + 1) / (8.0 * N));
        const double a = X[N / 2 - 2 * k - 1];
        const double b = X[2 * k];
        z[k].re = a * xc - b * xs;
        z[k].im = b * xc + a * xs;
    }
    inverseFft(z, 128);
    Complex y[128];
    for (int n = 0; n < 128; ++n) {
        const double xc = -std::cos(2.0 * kPi * (8 * n + 1) / (8.0 * N));
        const double xs = -std::sin(2.0 * kPi * (8 * n + 1) / (8.0 * N));
        y[n].re = z[n].re * xc - z[n].im * xs;
        y[n].im = z[n].im * xc + z[n].re * xs;
    }
    float x[512];
    for (int n = 0; n < 64; ++n) {
        x[2 * n] = static_cast<float>(-y[64 + n].im * w[2 * n]);
        x[2 * n + 1] = static_cast<float>(y[63 - n].re * w[2 * n + 1]);
        x[128 + 2 * n] = static_cast<float>(-y[n].re * w[128 + 2 * n]);
        x[128 + 2 * n + 1] = static_cast<float>(y[127 - n].im * w[128 + 2 * n + 1]);
        x[256 + 2 * n] = static_cast<float>(-y[64 + n].re * w[256 - 2 * n - 1]);
        x[256 + 2 * n + 1] = static_cast<float>(y[63 - n].im * w[256 - 2 * n - 2]);
        x[384 + 2 * n] = static_cast<float>(y[n].im * w[128 - 2 * n - 1]);
        x[384 + 2 * n + 1] = static_cast<float>(-y[127 - n].re * w[128 - 2 * n - 2]);
    }
    overlapAdd(x, delay, out);
}

// 256-sample inverse MDCT (A/52 7.9.4.2) for switched blocks: the even and odd
// coefficients are transformed separately with 64-point inverse FFTs.
void imdct256(const float* X, float* delay, const float* w, float* out) {
    constexpr double kPi = 3.14159265358979323846;
    constexpr int N = 512;
    float x1[128];
    float x2[128];
    for (int k = 0; k < 128; ++k) {
        x1[k] = X[2 * k];
        x2[k] = X[2 * k + 1];
    }
    Complex z1[64];
    Complex z2[64];
    for (int k = 0; k < 64; ++k) {
        const double xc = -std::cos(2.0 * kPi * (8 * k + 1) / (4.0 * N));
        const double xs = -std::sin(2.0 * kPi * (8 * k + 1) / (4.0 * N));
        const double a1 = x1[N / 4 - 2 * k - 1];
        const double b1 = x1[2 * k];
        z1[k].re = a1 * xc - b1 * xs;
        z1[k].im = b1 * xc + a1 * xs;
        const double a2 = x2[N / 4 - 2 * k - 1];
        const double b2 = x2[2 * k];
        z2[k].re = a2 * xc - b2 * xs;
        z2[k].im = b2 * xc + a2 * xs;
    }
    inverseFft(z1, 64);
    inverseFft(z2, 64);
    Complex y1[64];
    Complex y2[64];
    for (int n = 0; n < 64; ++n) {
        const double xc = -std::cos(2.0 * kPi * (8 * n + 1) / (4.0 * N));
        const double xs = -std::sin(2.0 * kPi * (8 * n + 1) / (4.0 * N));
        y1[n].re = z1[n].re * xc - z1[n].im * xs;
        y1[n].im = z1[n].im * xc + z1[n].re * xs;
        y2[n].re = z2[n].re * xc - z2[n].im * xs;
        y2[n].im = z2[n].im * xc + z2[n].re * xs;
    }
    float x[512];
    for (int n = 0; n < 64; ++n) {
        x[2 * n] = static_cast<float>(-y1[n].im * w[2 * n]);
        x[2 * n + 1] = static_cast<float>(y1[63 - n].re * w[2 * n + 1]);
        x[128 + 2 * n] = static_cast<float>(-y1[n].re * w[128 + 2 * n]);
        x[128 + 2 * n + 1] = static_cast<float>(y1[63 - n].im * w[128 + 2 * n + 1]);
        x[256 + 2 * n] = static_cast<float>(-y2[n].re * w[256 - 2 * n - 1]);
        x[256 + 2 * n + 1] = static_cast<float>(y2[63 - n].im * w[256 - 2 * n - 2]);
        x[384 + 2 * n] = static_cast<float>(y2[n].im * w[128 - 2 * n - 1]);
        x[384 + 2 * n + 1] = static_cast<float>(-y2[63 - n].re * w[128 - 2 * n - 2]);
    }
    overlapAdd(x, delay, out);
}

}  // namespace

bool Decoder::configure(const uint8_t* config, size_t size, std::string& error) {
    (void)config;
    (void)size;
    (void)error;
    configured_ = true;
    resetState();
    return true;
}

void Decoder::flush() {
    resetState();
}

void Decoder::reset() {
    configured_ = false;
    have_config_ = false;
    sample_rate_ = 0;
    channels_ = 0;
    resetState();
}

void Decoder::resetState() {
    std::memset(exp_strategy_, 0, sizeof(exp_strategy_));
    std::memset(dexps_, 0, sizeof(dexps_));
    std::memset(bap_, 0, sizeof(bap_));
    std::memset(psd_, 0, sizeof(psd_));
    std::memset(band_psd_, 0, sizeof(band_psd_));
    std::memset(mask_, 0, sizeof(mask_));
    std::memset(start_freq_, 0, sizeof(start_freq_));
    std::memset(end_freq_, 0, sizeof(end_freq_));
    std::memset(num_exp_groups_, 0, sizeof(num_exp_groups_));
    std::memset(snr_offset_, 0, sizeof(snr_offset_));
    std::memset(fast_gain_, 0, sizeof(fast_gain_));
    std::memset(dba_mode_, 0, sizeof(dba_mode_));
    std::memset(dba_nsegs_, 0, sizeof(dba_nsegs_));
    std::memset(dba_offsets_, 0, sizeof(dba_offsets_));
    std::memset(dba_lengths_, 0, sizeof(dba_lengths_));
    std::memset(dba_values_, 0, sizeof(dba_values_));
    std::memset(channel_in_cpl_, 0, sizeof(channel_in_cpl_));
    std::memset(cpl_in_use_, 0, sizeof(cpl_in_use_));
    std::memset(phase_flags_, 0, sizeof(phase_flags_));
    std::memset(cpl_coords_, 0, sizeof(cpl_coords_));
    std::memset(cpl_band_sizes_, 0, sizeof(cpl_band_sizes_));
    std::memset(coeffs_, 0, sizeof(coeffs_));
    std::memset(transform_coeffs_, 0, sizeof(transform_coeffs_));
    std::memset(delay_, 0, sizeof(delay_));
    std::memset(block_out_, 0, sizeof(block_out_));
    std::memset(block_switch_, 0, sizeof(block_switch_));
    std::memset(dither_flag_, 0, sizeof(dither_flag_));
    std::memset(rematrixing_flags_, 0, sizeof(rematrixing_flags_));
    for (bool& flag : first_cpl_coords_) {
        flag = true;
    }
    first_cpl_leak_ = false;
    phase_flags_in_use_ = false;
    num_cpl_bands_ = 0;
    num_rematrixing_bands_ = 0;
    dither_state_ = 0;
}

uint32_t Decoder::nextDither() {
    dither_state_ = dither_state_ * 1664525u + 1013904223u;
    return dither_state_;
}

bool Decoder::parseHeader(BitReader& reader, Header& header) {
    uint32_t value = 0;
    if (!reader.read(16, value) || value != 0x0B77) {
        return false;
    }
    if (!reader.read(16, value)) {  // CRC1
        return false;
    }
    if (!reader.read(2, value)) {
        return false;
    }
    header.sr_code = static_cast<int>(value);
    if (header.sr_code == 3) {
        return false;
    }
    if (!reader.read(6, value)) {
        return false;
    }
    const int frame_size_code = static_cast<int>(value);
    if (frame_size_code > 37) {
        return false;
    }
    header.bit_rate = kBitrates[frame_size_code >> 1] * 1000;
    if (!reader.read(5, value)) {
        return false;
    }
    header.bitstream_id = static_cast<int>(value);
    header.eac3 = header.bitstream_id > 10;
    if (header.eac3) {
        return true;  // the caller rejects E-AC-3
    }
    if (!reader.read(3, value)) {
        return false;
    }
    header.bitstream_mode = static_cast<int>(value);
    if (!reader.read(3, value)) {
        return false;
    }
    header.channel_mode = static_cast<int>(value);
    if (header.channel_mode == 2) {
        if (!reader.skip(2)) {  // dsurmod
            return false;
        }
    } else {
        if ((header.channel_mode & 1) && header.channel_mode != 1) {
            if (!reader.read(2, value)) {
                return false;
            }
            header.center_mix_level = kCenterLevels[value];
        }
        if (header.channel_mode & 4) {
            if (!reader.read(2, value)) {
                return false;
            }
            header.surround_mix_level = kSurroundLevels[value];
        }
    }
    if (!reader.read(1, value)) {
        return false;
    }
    header.lfe_on = value != 0;

    header.sr_shift = std::max(header.bitstream_id, 8) - 8;
    header.sample_rate = static_cast<uint32_t>(kSampleRates[header.sr_code] >> header.sr_shift);
    header.channels = kChannelsTab[header.channel_mode] + (header.lfe_on ? 1 : 0);
    header.frame_size = kFrameSize[frame_size_code][header.sr_code] * 2;

    // Volume control; dual mono carries two sets.
    const int sets = header.channel_mode ? 1 : 2;
    for (int i = 0; i < sets; ++i) {
        if (!reader.read(5, value)) {
            return false;
        }
        header.dialog_normalization[i] = -static_cast<int>(value);
        if (!reader.read(1, value)) {
            return false;
        }
        header.compression_exists[i] = value != 0;
        if (header.compression_exists[i]) {
            if (!reader.read(8, value)) {
                return false;
            }
            header.heavy_dynamic_range[i] = static_cast<int>(value);
        }
        if (!reader.read(1, value)) {
            return false;
        }
        if (value != 0 && !reader.skip(8)) {  // language code
            return false;
        }
        if (!reader.read(1, value)) {
            return false;
        }
        if (value != 0 && !reader.skip(7)) {  // audio production information
            return false;
        }
    }

    if (!reader.skip(2)) {  // copyright and original bitstream
        return false;
    }

    if (header.bitstream_id != 6) {
        if (!reader.read(1, value)) {
            return false;
        }
        if (value != 0 && !reader.skip(14)) {
            return false;
        }
        if (!reader.read(1, value)) {
            return false;
        }
        if (value != 0 && !reader.skip(14)) {
            return false;
        }
    } else {
        // Alternate bit stream syntax.
        if (!reader.read(1, value)) {
            return false;
        }
        if (value != 0) {
            uint32_t preferred = 0;
            uint32_t center_ltrt = 0;
            uint32_t surround_ltrt = 0;
            uint32_t center = 0;
            uint32_t surround = 0;
            if (!reader.read(2, preferred) || !reader.read(3, center_ltrt) ||
                !reader.read(3, surround_ltrt) || !reader.read(3, center) ||
                !reader.read(3, surround)) {
                return false;
            }
            header.center_mix_level = static_cast<int>(center);
            header.surround_mix_level = static_cast<int>(std::clamp(surround, 3u, 7u));
        }
        if (!reader.read(1, value)) {
            return false;
        }
        if (value != 0 && !reader.skip(14)) {  // 2 + 2 + 10 reserved bits
            return false;
        }
    }

    // Additional bitstream info.
    if (!reader.read(1, value)) {
        return false;
    }
    if (value != 0) {
        uint32_t count = 0;
        if (!reader.read(6, count)) {
            return false;
        }
        for (uint32_t i = 0; i <= count; ++i) {
            if (!reader.skip(8)) {
                return false;
            }
        }
    }
    return true;
}

Status Decoder::decode(const uint8_t* data, size_t size, PcmBuffer& pcm, std::string& error) {
    pcm.clear();
    if (!configured_) {
        error = "AC-3 decoder not configured";
        return Status::Invalid;
    }
    if (data == nullptr || size < 7) {
        error = "short AC-3 frame";
        return Status::Invalid;
    }
    BitReader reader(data, size);
    Header header;
    if (!parseHeader(reader, header)) {
        error = "bad AC-3 header";
        return Status::Invalid;
    }
    if (header.eac3) {
        error = "E-AC-3 is not supported";
        return Status::Unsupported;
    }
    if (header.frame_size > static_cast<int>(size)) {
        error = "incomplete AC-3 frame";
        return Status::Invalid;
    }

    const bool config_changed =
        !have_config_ || header.channel_mode != channel_mode_ || header.lfe_on != lfe_on_ ||
        header.sample_rate != sample_rate_ || header.sr_shift != sr_shift_;
    if (config_changed) {
        resetState();
        have_config_ = true;
    }

    channel_mode_ = header.channel_mode;
    lfe_on_ = header.lfe_on;
    bitstream_id_ = header.bitstream_id;
    bitstream_mode_ = header.bitstream_mode;
    sr_code_ = header.sr_code;
    sr_shift_ = header.sr_shift;
    frame_size_ = header.frame_size;
    sample_rate_ = header.sample_rate;
    channels_ = static_cast<uint32_t>(header.channels);
    fbw_channels_ = header.channels - (header.lfe_on ? 1 : 0);
    lfe_ch_ = fbw_channels_ + 1;

    for (int i = 0; i < (header.channel_mode ? 1 : 2); ++i) {
        int dialog = header.dialog_normalization[i];
        if (dialog == 0) {
            dialog = -31;
        }
        dynamic_range_[i] = 1.0f;
    }

    if (lfe_on_) {
        start_freq_[lfe_ch_] = 0;
        end_freq_[lfe_ch_] = 7;
        num_exp_groups_[lfe_ch_] = 2;
        channel_in_cpl_[lfe_ch_] = 0;
    }

    for (int blk = 0; blk < kBlocks; ++blk) {
        if (!decodeBlock(reader, blk)) {
            break;
        }
    }

    // Interleave into WAVE order.
    const uint8_t* map = kOutputMap[channel_mode_][lfe_on_ ? 1 : 0];
    pcm.channels = channels_;
    pcm.sample_rate = sample_rate_;
    pcm.samples.assign(static_cast<size_t>(kSamplesPerFrame) * channels_, 0.0f);
    for (int ch = 1; ch <= header.channels; ++ch) {
        const int slot = map[ch - 1];
        const float* source = block_out_[ch];
        for (int i = 0; i < kSamplesPerFrame; ++i) {
            pcm.samples[static_cast<size_t>(i) * channels_ + slot] = source[i];
        }
    }
    return Status::Ok;
}

bool Decoder::couplingStrategy(BitReader& reader, int blk) {
    (void)blk;
    uint32_t value = 0;
    if (!reader.read(1, value)) {
        return false;
    }
    cpl_in_use_[blk] = value != 0;
    if (!cpl_in_use_[blk]) {
        for (int ch = 1; ch <= fbw_channels_; ++ch) {
            channel_in_cpl_[ch] = 0;
            first_cpl_coords_[ch] = true;
        }
        first_cpl_leak_ = false;
        phase_flags_in_use_ = false;
        return true;
    }
    if (channel_mode_ < 2) {
        return false;  // coupling is not allowed in mono or dual mono
    }
    for (int ch = 1; ch <= fbw_channels_; ++ch) {
        if (!reader.read(1, value)) {
            return false;
        }
        channel_in_cpl_[ch] = value != 0;
    }
    if (channel_mode_ == 2) {
        if (!reader.read(1, value)) {
            return false;
        }
        phase_flags_in_use_ = value != 0;
    }
    uint32_t start = 0;
    uint32_t end = 0;
    if (!reader.read(4, start) || !reader.read(4, end)) {
        return false;
    }
    const int cpl_start_subband = static_cast<int>(start);
    const int cpl_end_subband = static_cast<int>(end) + 3;
    if (cpl_start_subband >= cpl_end_subband) {
        return false;
    }
    start_freq_[0] = cpl_start_subband * 12 + 37;
    end_freq_[0] = cpl_end_subband * 12 + 37;

    // Band structure: the bits are always present in AC-3.
    const int n_subbands = cpl_end_subband - cpl_start_subband;
    uint8_t band_struct[18];
    std::memcpy(band_struct, kDefaultCplBandStruct, sizeof(band_struct));
    uint8_t* p = band_struct + cpl_start_subband + 1;
    for (int subbnd = 0; subbnd < n_subbands - 1; ++subbnd) {
        if (!reader.read(1, value)) {
            return false;
        }
        p[subbnd] = static_cast<uint8_t>(value);
    }
    num_cpl_bands_ = n_subbands;
    int bnd = 0;
    cpl_band_sizes_[0] = 12;
    for (int subbnd = 1; subbnd < n_subbands; ++subbnd) {
        if (p[subbnd - 1] != 0) {
            --num_cpl_bands_;
            cpl_band_sizes_[bnd] = static_cast<uint8_t>(cpl_band_sizes_[bnd] + 12);
        } else {
            cpl_band_sizes_[++bnd] = 12;
        }
    }
    return true;
}

bool Decoder::couplingCoordinates(BitReader& reader, int blk) {
    uint32_t value = 0;
    bool coords_exist = false;
    for (int ch = 1; ch <= fbw_channels_; ++ch) {
        if (channel_in_cpl_[ch]) {
            if (!reader.read(1, value)) {
                return false;
            }
            if (value != 0) {
                first_cpl_coords_[ch] = false;
                coords_exist = true;
                uint32_t master = 0;
                if (!reader.read(2, master)) {
                    return false;
                }
                const int master_cpl_coord = 3 * static_cast<int>(master);
                for (int bnd = 0; bnd < num_cpl_bands_; ++bnd) {
                    uint32_t cpl_exp = 0;
                    uint32_t cpl_mant = 0;
                    if (!reader.read(4, cpl_exp) || !reader.read(4, cpl_mant)) {
                        return false;
                    }
                    int coord = 0;
                    if (cpl_exp == 15) {
                        coord = static_cast<int>(cpl_mant) << 22;
                    } else {
                        coord = (static_cast<int>(cpl_mant) + 16) << 21;
                    }
                    cpl_coords_[ch][bnd] = coord >> (static_cast<int>(cpl_exp) + master_cpl_coord);
                }
            } else if (blk == 0) {
                return false;
            }
        } else {
            first_cpl_coords_[ch] = true;
        }
    }
    if (channel_mode_ == 2 && coords_exist) {
        for (int bnd = 0; bnd < num_cpl_bands_; ++bnd) {
            if (phase_flags_in_use_) {
                if (!reader.read(1, value)) {
                    return false;
                }
                phase_flags_[bnd] = static_cast<int>(value);
            } else {
                phase_flags_[bnd] = 0;
            }
        }
    }
    return true;
}

bool Decoder::decodeExponents(BitReader& reader, int strategy, int groups, int absolute,
                              int8_t* exponents) {
    const GeneratedTables& gen = tables();
    int dexp[256];
    int count = 0;
    for (int grp = 0; grp < groups; ++grp) {
        uint32_t expacc = 0;
        if (!reader.read(7, expacc)) {
            return false;
        }
        if (expacc >= 125) {
            return false;
        }
        dexp[count++] = gen.ungroup7[expacc][0];
        dexp[count++] = gen.ungroup7[expacc][1];
        dexp[count++] = gen.ungroup7[expacc][2];
    }

    const int group_size = strategy + (strategy == 3 ? 1 : 0);
    int prevexp = absolute;
    int j = 0;
    for (int i = 0; i < groups * 3; ++i) {
        prevexp += dexp[i] - 2;
        if (prevexp > 24) {
            return false;
        }
        switch (group_size) {
            case 4:
                exponents[j++] = static_cast<int8_t>(prevexp);
                exponents[j++] = static_cast<int8_t>(prevexp);
                [[fallthrough]];
            case 2:
                exponents[j++] = static_cast<int8_t>(prevexp);
                [[fallthrough]];
            case 1:
                exponents[j++] = static_cast<int8_t>(prevexp);
                break;
            default:
                return false;
        }
    }
    return true;
}

bool Decoder::decodeBlock(BitReader& reader, int blk) {
    const GeneratedTables& gen = tables();
    const int fbw = fbw_channels_;
    const int mode = channel_mode_;
    uint32_t value = 0;

    // Block switch flags.
    bool different_transforms = false;
    for (int ch = 1; ch <= fbw; ++ch) {
        if (!reader.read(1, value)) {
            return false;
        }
        block_switch_[ch] = static_cast<int>(value);
        if (ch > 1 && block_switch_[ch] != block_switch_[1]) {
            different_transforms = true;
        }
    }
    (void)different_transforms;

    // Dither flags.
    for (int ch = 1; ch <= fbw; ++ch) {
        if (!reader.read(1, value)) {
            return false;
        }
        dither_flag_[ch] = static_cast<int>(value);
    }

    // Dynamic range.
    int i = mode ? 0 : 1;
    do {
        if (!reader.read(1, value)) {
            return false;
        }
        if (value != 0) {
            uint32_t range_bits = 0;
            if (!reader.read(8, range_bits)) {
                return false;
            }
            dynamic_range_[i] = gen.dynamicRange[range_bits];
        } else if (blk == 0) {
            dynamic_range_[i] = 1.0f;
        }
    } while (i--);

    // Coupling strategy.
    if (!reader.read(1, value)) {
        return false;
    }
    if (value != 0) {
        if (!couplingStrategy(reader, blk)) {
            return false;
        }
    } else {
        if (blk == 0) {
            return false;
        }
        cpl_in_use_[blk] = cpl_in_use_[blk - 1];
    }

    const int first = cpl_in_use_[blk] ? 0 : 1;

    if (cpl_in_use_[blk] && !couplingCoordinates(reader, blk)) {
        return false;
    }

    // Stereo rematrixing.
    if (mode == 2) {
        if (!reader.read(1, value)) {
            return false;
        }
        if (value != 0) {
            num_rematrixing_bands_ = 4;
            if (cpl_in_use_[blk] && start_freq_[0] <= 61) {
                num_rematrixing_bands_ -= 1 + (start_freq_[0] == 37 ? 1 : 0);
            }
            for (int bnd = 0; bnd < num_rematrixing_bands_; ++bnd) {
                if (!reader.read(1, value)) {
                    return false;
                }
                rematrixing_flags_[bnd] = static_cast<int>(value);
            }
        } else if (blk == 0) {
            num_rematrixing_bands_ = 0;
        }
    }

    // Exponent strategies.
    for (int ch = first; ch <= (lfe_on_ ? lfe_ch_ : fbw); ++ch) {
        if (!reader.read(ch == lfe_ch_ ? 1 : 2, value)) {
            return false;
        }
        exp_strategy_[blk][ch] = static_cast<int>(value);
    }

    // Channel bandwidth.
    for (int ch = 1; ch <= fbw; ++ch) {
        start_freq_[ch] = 0;
        if (exp_strategy_[blk][ch] != 0) {
            if (channel_in_cpl_[ch]) {
                end_freq_[ch] = start_freq_[0];
            } else {
                uint32_t code = 0;
                if (!reader.read(6, code)) {
                    return false;
                }
                if (code > 60) {
                    return false;
                }
                end_freq_[ch] = static_cast<int>(code) * 3 + 73;
            }
            const int group_size = 3 << (exp_strategy_[blk][ch] - 1);
            num_exp_groups_[ch] = (end_freq_[ch] + group_size - 4) / group_size;
        }
    }
    if (cpl_in_use_[blk] && exp_strategy_[blk][0] != 0) {
        num_exp_groups_[0] = (end_freq_[0] - start_freq_[0]) /
                             (3 << (exp_strategy_[blk][0] - 1));
    }

    // Exponents.
    for (int ch = first; ch <= (lfe_on_ ? lfe_ch_ : fbw); ++ch) {
        if (exp_strategy_[blk][ch] == 0) {
            continue;
        }
        uint32_t first_exp = 0;
        if (!reader.read(4, first_exp)) {
            return false;
        }
        dexps_[ch][0] = static_cast<int8_t>(first_exp << (ch == 0 ? 1 : 0));
        const int start = start_freq_[ch] + (ch == 0 ? 0 : 1);
        if (!decodeExponents(reader, exp_strategy_[blk][ch], num_exp_groups_[ch], dexps_[ch][0],
                             &dexps_[ch][start])) {
            return false;
        }
        if (ch != 0 && ch != lfe_ch_ && !reader.skip(2)) {  // gainrng
            return false;
        }
    }

    // Bit allocation information.
    if (!reader.read(1, value)) {
        return false;
    }
    if (value != 0) {
        uint32_t v = 0;
        if (!reader.read(2, v)) {
            return false;
        }
        slow_decay_ = kSlowDecay[v] >> sr_shift_;
        if (!reader.read(2, v)) {
            return false;
        }
        fast_decay_ = kFastDecay[v] >> sr_shift_;
        if (!reader.read(2, v)) {
            return false;
        }
        slow_gain_ = kSlowGain[v];
        if (!reader.read(2, v)) {
            return false;
        }
        db_per_bit_ = kDbPerBit[v];
        if (!reader.read(3, v)) {
            return false;
        }
        floor_ = kFloor[v];
    } else if (blk == 0) {
        return false;
    }

    // Signal-to-noise ratio offsets and fast gains.
    if (!reader.read(1, value)) {
        return false;
    }
    if (value != 0) {
        uint32_t csnr_bits = 0;
        if (!reader.read(6, csnr_bits)) {
            return false;
        }
        const int csnr = (static_cast<int>(csnr_bits) - 15) << 4;
        for (int ch = first; ch <= (lfe_on_ ? lfe_ch_ : fbw); ++ch) {
            uint32_t offset = 0;
            if (!reader.read(4, offset)) {
                return false;
            }
            snr_offset_[ch] = (csnr + static_cast<int>(offset)) << 2;
            uint32_t gain = 0;
            if (!reader.read(3, gain)) {
                return false;
            }
            fast_gain_[ch] = kFastGain[gain];
        }
    } else if (blk == 0) {
        return false;
    }

    // Coupling leak.
    if (cpl_in_use_[blk]) {
        if (first_cpl_leak_) {
            if (!reader.read(3, value)) {
                return false;
            }
            cpl_fast_leak_ = static_cast<int>(value);
            if (!reader.read(3, value)) {
                return false;
            }
            cpl_slow_leak_ = static_cast<int>(value);
        } else {
            if (!reader.read(1, value)) {
                return false;
            }
            if (value != 0) {
                uint32_t fast = 0;
                uint32_t slow = 0;
                if (!reader.read(3, fast) || !reader.read(3, slow)) {
                    return false;
                }
                cpl_fast_leak_ = static_cast<int>(fast);
                cpl_slow_leak_ = static_cast<int>(slow);
            } else if (blk == 0) {
                return false;
            }
        }
        first_cpl_leak_ = false;
    }

    // Delta bit allocation.
    if (!reader.read(1, value)) {
        return false;
    }
    if (value != 0) {
        for (int ch = first; ch <= fbw; ++ch) {
            if (!reader.read(2, value)) {
                return false;
            }
            dba_mode_[ch] = static_cast<int>(value);
            if (dba_mode_[ch] == 3) {
                return false;
            }
        }
        for (int ch = first; ch <= fbw; ++ch) {
            if (dba_mode_[ch] != 1) {
                continue;
            }
            uint32_t nsegs = 0;
            if (!reader.read(3, nsegs)) {
                return false;
            }
            dba_nsegs_[ch] = static_cast<int>(nsegs) + 1;
            for (int seg = 0; seg < dba_nsegs_[ch]; ++seg) {
                uint32_t offset = 0;
                uint32_t length = 0;
                uint32_t dba_value = 0;
                if (!reader.read(5, offset) || !reader.read(4, length) ||
                    !reader.read(3, dba_value)) {
                    return false;
                }
                dba_offsets_[ch][seg] = static_cast<uint8_t>(offset);
                dba_lengths_[ch][seg] = static_cast<uint8_t>(length);
                dba_values_[ch][seg] = static_cast<uint8_t>(dba_value);
            }
        }
    } else if (blk == 0) {
        for (int ch = 0; ch <= (lfe_on_ ? lfe_ch_ : fbw); ++ch) {
            dba_mode_[ch] = 2;  // none
        }
    }

    // Bit allocation.
    for (int ch = first; ch <= (lfe_on_ ? lfe_ch_ : fbw); ++ch) {
        bitAllocCalcPsd(ch);
        if (!bitAllocCalcMask(ch)) {
            return false;
        }
        bitAllocCalcBap(ch);
    }

    // Unused dummy data.
    if (!reader.read(1, value)) {
        return false;
    }
    if (value != 0) {
        uint32_t skip_length = 0;
        if (!reader.read(9, skip_length)) {
            return false;
        }
        if (!reader.skip(static_cast<size_t>(skip_length) * 8)) {
            return false;
        }
    }

    if (!decodeTransformCoeffs(reader, blk)) {
        return false;
    }
    if (mode == 2) {
        doRematrixing();
    }

    // Dynamic range scaling and the inverse transforms.
    for (int ch = 1; ch <= (lfe_on_ ? lfe_ch_ : fbw); ++ch) {
        const int audio_channel = (mode == 0 && ch <= 2) ? 2 - ch : 0;
        float gain = dynamic_range_[audio_channel] * (1.0f / 4194304.0f);
        for (int bin = 0; bin < kMaxCoeffs; ++bin) {
            transform_coeffs_[ch][bin] = coeffs_[ch][bin] * gain;
        }
    }
    for (int ch = 1; ch <= (lfe_on_ ? lfe_ch_ : fbw); ++ch) {
        doImdct(ch, &block_out_[ch][blk * kBlockSize]);
    }
    return true;
}

void Decoder::calcTransformCoeffsCpl() {
    int bin = start_freq_[0];
    for (int band = 0; band < num_cpl_bands_; ++band) {
        const int band_start = bin;
        const int band_end = bin + cpl_band_sizes_[band];
        for (int ch = 1; ch <= fbw_channels_; ++ch) {
            if (!channel_in_cpl_[ch]) {
                continue;
            }
            const float coord = static_cast<float>(cpl_coords_[ch][band]) * (1.0f / (1 << 23));
            for (bin = band_start; bin < band_end; ++bin) {
                coeffs_[ch][bin] = coeffs_[0][bin] * coord;
            }
            if (ch == 2 && phase_flags_[band] != 0) {
                for (bin = band_start; bin < band_end; ++bin) {
                    coeffs_[2][bin] = -coeffs_[2][bin];
                }
            }
        }
        bin = band_end;
    }
}

void Decoder::removeDithering() {
    for (int ch = 1; ch <= fbw_channels_; ++ch) {
        if (dither_flag_[ch] != 0 || !channel_in_cpl_[ch]) {
            continue;
        }
        for (int bin = start_freq_[0]; bin < end_freq_[0]; ++bin) {
            if (bap_[0][bin] == 0) {
                coeffs_[ch][bin] = 0.0f;
            }
        }
    }
}

bool Decoder::decodeTransformCoeffs(BitReader& reader, int blk) {
    (void)blk;
    const GeneratedTables& gen = tables();
    int b1_mant[2] = {};
    int b2_mant[2] = {};
    int b4_mant = 0;
    int b1 = 0;
    int b2 = 0;
    int b4 = 0;
    bool got_cplchan = false;

    // Decodes the mantissas of one channel. The coupling channel always
    // dithers its zero-bit mantissas.
    const auto decodeChannel = [&](int ch, bool force_dither) {
        const int start = start_freq_[ch];
        const int end = end_freq_[ch];
        const bool dither = force_dither || dither_flag_[ch] != 0;
        for (int freq = start; freq < end; ++freq) {
            const int bap = bap_[ch][freq];
            int mantissa = 0;
            switch (bap) {
                case 0:
                    mantissa = dither
                                   ? static_cast<int>(((nextDither() >> 8) * 181) >> 8) - 5931008
                                   : 0;
                    break;
                case 1:
                    if (b1 > 0) {
                        mantissa = b1_mant[--b1];
                    } else {
                        uint32_t bits = 0;
                        if (!reader.read(5, bits)) {
                            return false;
                        }
                        mantissa = gen.bap1[bits][0];
                        b1_mant[1] = gen.bap1[bits][1];
                        b1_mant[0] = gen.bap1[bits][2];
                        b1 = 2;
                    }
                    break;
                case 2:
                    if (b2 > 0) {
                        mantissa = b2_mant[--b2];
                    } else {
                        uint32_t bits = 0;
                        if (!reader.read(7, bits)) {
                            return false;
                        }
                        mantissa = gen.bap2[bits][0];
                        b2_mant[1] = gen.bap2[bits][1];
                        b2_mant[0] = gen.bap2[bits][2];
                        b2 = 2;
                    }
                    break;
                case 3: {
                    uint32_t bits = 0;
                    if (!reader.read(3, bits)) {
                        return false;
                    }
                    mantissa = gen.bap3[bits];
                    break;
                }
                case 4:
                    if (b4 > 0) {
                        b4 = 0;
                        mantissa = b4_mant;
                    } else {
                        uint32_t bits = 0;
                        if (!reader.read(7, bits)) {
                            return false;
                        }
                        mantissa = gen.bap4[bits][0];
                        b4_mant = gen.bap4[bits][1];
                        b4 = 1;
                    }
                    break;
                case 5: {
                    uint32_t bits = 0;
                    if (!reader.read(4, bits)) {
                        return false;
                    }
                    mantissa = gen.bap5[bits];
                    break;
                }
                default: {
                    const int bits = kQuantization[bap];
                    uint32_t raw = 0;
                    if (!reader.read(bits, raw)) {
                        return false;
                    }
                    const uint32_t sign = 1u << (bits - 1);
                    mantissa = static_cast<int>((raw ^ sign) - sign) << (24 - bits);
                    break;
                }
            }
            coeffs_[ch][freq] = static_cast<float>(mantissa) * gen.scale[dexps_[ch][freq]];
        }
        return true;
    };

    for (int ch = 1; ch <= (lfe_on_ ? lfe_ch_ : fbw_channels_); ++ch) {
        if (!decodeChannel(ch, false)) {
            return false;
        }
        // The coupling channel's mantissas follow the first coupled channel.
        if (channel_in_cpl_[ch] && !got_cplchan) {
            if (!decodeChannel(0, true)) {
                return false;
            }
            calcTransformCoeffsCpl();
            got_cplchan = true;
        }
        const int zero_from = channel_in_cpl_[ch] ? end_freq_[0] : end_freq_[ch];
        for (int bin = zero_from; bin < kMaxCoeffs; ++bin) {
            coeffs_[ch][bin] = 0.0f;
        }
    }
    if (got_cplchan) {
        for (int bin = end_freq_[0]; bin < kMaxCoeffs; ++bin) {
            coeffs_[0][bin] = 0.0f;
        }
    }
    removeDithering();
    return true;
}

void Decoder::doRematrixing() {
    const int end = std::min(end_freq_[1], end_freq_[2]);
    for (int bnd = 0; bnd < num_rematrixing_bands_; ++bnd) {
        if (rematrixing_flags_[bnd] == 0) {
            continue;
        }
        const int bnd_end = std::min(end, static_cast<int>(kRematrixBands[bnd + 1]));
        for (int i = kRematrixBands[bnd]; i < bnd_end; ++i) {
            const float tmp = coeffs_[1][i];
            coeffs_[1][i] += coeffs_[2][i];
            coeffs_[2][i] = tmp - coeffs_[2][i];
        }
    }
}

void Decoder::doImdct(int ch, float* out) {
    const GeneratedTables& gen = tables();
    if (block_switch_[ch]) {
        imdct256(transform_coeffs_[ch], delay_[ch], gen.window, out);
    } else {
        imdct512(transform_coeffs_[ch], delay_[ch], gen.window, out);
    }
}

void Decoder::bitAllocCalcPsd(int ch) {
    const int start = start_freq_[ch];
    const int end = end_freq_[ch];
    for (int bin = start; bin < end; ++bin) {
        psd_[ch][bin] = static_cast<int16_t>(3072 - (dexps_[ch][bin] << 7));
    }
    int bin = start;
    for (; bin < std::min(end, 28); ++bin) {
        band_psd_[ch][bin] = psd_[ch][bin];
    }
    if (bin >= end) {
        return;
    }
    int band = kBinToBand[bin];
    do {
        int v = psd_[ch][bin++];
        const int band_end = std::min(static_cast<int>(kBandStart[band + 1]), end);
        for (; bin < band_end; ++bin) {
            const int maximum = std::max(v, static_cast<int>(psd_[ch][bin]));
            const int adr =
                std::min(maximum - ((v + psd_[ch][bin] + 1) >> 1), 255);
            v = maximum + kLogAdd[adr];
        }
        band_psd_[ch][band++] = static_cast<int16_t>(v);
    } while (end > kBandStart[band]);
}

bool Decoder::bitAllocCalcMask(int ch) {
    const int start = start_freq_[ch];
    const int end = end_freq_[ch];
    if (end <= 0) {
        return false;
    }
    const int is_lfe = ch == lfe_ch_ ? 1 : 0;
    int16_t excite[kCriticalBands] = {};
    int lowcomp = 0;
    int fastleak = 0;
    int slowleak = 0;
    int begin = 0;

    const int band_start = kBinToBand[start];
    const int band_end = kBinToBand[end - 1] + 1;
    const auto calcLowcomp1 = [](int a, int b0, int b1, int c) {
        if ((b0 + 256) == b1) {
            return c;
        }
        if (b0 > b1) {
            return std::max(a - 64, 0);
        }
        return a;
    };
    const auto calcLowcomp = [&](int a, int b0, int b1, int band) {
        if (band < 7) {
            return calcLowcomp1(a, b0, b1, 384);
        }
        if (band < 20) {
            return calcLowcomp1(a, b0, b1, 320);
        }
        return std::max(a - 128, 0);
    };

    if (band_start == 0) {
        lowcomp = calcLowcomp1(0, band_psd_[ch][0], band_psd_[ch][1], 384);
        excite[0] = static_cast<int16_t>(band_psd_[ch][0] - fast_gain_[ch] - lowcomp);
        lowcomp = calcLowcomp1(lowcomp, band_psd_[ch][1], band_psd_[ch][2], 384);
        excite[1] = static_cast<int16_t>(band_psd_[ch][1] - fast_gain_[ch] - lowcomp);
        begin = 7;
        for (int band = 2; band < 7; ++band) {
            if (!(is_lfe && band == 6)) {
                lowcomp = calcLowcomp1(lowcomp, band_psd_[ch][band], band_psd_[ch][band + 1],
                                       384);
            }
            fastleak = band_psd_[ch][band] - fast_gain_[ch];
            slowleak = band_psd_[ch][band] - slow_gain_;
            excite[band] = static_cast<int16_t>(fastleak - lowcomp);
            if (!(is_lfe && band == 6) && band_psd_[ch][band] <= band_psd_[ch][band + 1]) {
                begin = band + 1;
                break;
            }
        }
        const int end1 = std::min(band_end, 22);
        for (int band = begin; band < end1; ++band) {
            if (!(is_lfe && band == 6)) {
                lowcomp = calcLowcomp(lowcomp, band_psd_[ch][band], band_psd_[ch][band + 1],
                                      band);
            }
            fastleak = std::max(fastleak - fast_decay_, static_cast<int>(band_psd_[ch][band]) - fast_gain_[ch]);
            slowleak = std::max(slowleak - slow_decay_, static_cast<int>(band_psd_[ch][band]) - slow_gain_);
            excite[band] = static_cast<int16_t>(std::max(fastleak - lowcomp, slowleak));
        }
        begin = 22;
    } else {
        // Coupling channel.
        begin = band_start;
        fastleak = (cpl_fast_leak_ << 8) + 768;
        slowleak = (cpl_slow_leak_ << 8) + 768;
    }

    for (int band = begin; band < band_end; ++band) {
        fastleak = std::max(fastleak - fast_decay_, static_cast<int>(band_psd_[ch][band]) - fast_gain_[ch]);
        slowleak = std::max(slowleak - slow_decay_, static_cast<int>(band_psd_[ch][band]) - slow_gain_);
        excite[band] = static_cast<int16_t>(std::max(fastleak, slowleak));
    }

    for (int band = band_start; band < band_end; ++band) {
        const int tmp = db_per_bit_ - band_psd_[ch][band];
        if (tmp > 0) {
            excite[band] = static_cast<int16_t>(excite[band] + (tmp >> 2));
        }
        mask_[ch][band] = static_cast<int16_t>(std::max(
            static_cast<int>(kHearingThreshold[band >> sr_shift_][sr_code_]),
            static_cast<int>(excite[band])));
    }

    if (dba_mode_[ch] == 0 || dba_mode_[ch] == 1) {
        if (dba_nsegs_[ch] > 8) {
            return false;
        }
        int band = band_start;
        for (int seg = 0; seg < dba_nsegs_[ch]; ++seg) {
            band += dba_offsets_[ch][seg];
            if (band >= kCriticalBands || dba_lengths_[ch][seg] > kCriticalBands - band) {
                return false;
            }
            const int dba_value = dba_values_[ch][seg];
            const int delta = dba_value >= 4 ? (dba_value - 3) * 128 : (dba_value - 4) * 128;
            for (int i = 0; i < dba_lengths_[ch][seg]; ++i) {
                mask_[ch][band] = static_cast<int16_t>(mask_[ch][band] + delta);
                ++band;
            }
        }
    }
    return true;
}

void Decoder::bitAllocCalcBap(int ch) {
    const int start = start_freq_[ch];
    const int end = end_freq_[ch];
    if (snr_offset_[ch] == -960) {
        std::memset(bap_[ch], 0, kMaxCoeffs);
        return;
    }
    int bin = start;
    for (; bin < std::min(end, 28); ++bin) {
        const int m = (std::max(static_cast<int>(mask_[ch][bin]) - snr_offset_[ch] - floor_, 0) &
                       0x1FE0) +
                      floor_;
        const int address = clipUint6((psd_[ch][bin] - m) >> 5);
        bap_[ch][bin] = kBapTab[address];
    }
    if (bin >= end) {
        return;
    }
    int band = kBinToBand[bin];
    int band_end = 0;
    do {
        const int m = (std::max(static_cast<int>(mask_[ch][band]) - snr_offset_[ch] - floor_, 0) &
                       0x1FE0) +
                      floor_;
        band_end = std::min(static_cast<int>(kBandStart[++band]), end);
        for (; bin < band_end; ++bin) {
            const int address = clipUint6((psd_[ch][bin] - m) >> 5);
            bap_[ch][bin] = kBapTab[address];
        }
    } while (end > band_end);
}

}  // namespace player::audio::ac3
