#pragma once

#include <cstdint>
#include <vector>

namespace player::audio::opus {

// Complex sample used by the FFT.
struct Complex {
    float r;
    float i;
};

// Radix-2/3/4/5 FFT as used by the CELT MDCT (kiss_fft from libopus). A state
// can share the twiddle table of a base state twice its size, which is how the
// mode builds the smaller transforms from the largest one.
class FftState {
public:
    void init(int nfft, const FftState* base = nullptr);

    // In-place transform of data already arranged in bit-reversed order
    // (opus_fft_impl).
    void transform(Complex* data) const;

    int size() const { return nfft_; }
    const int16_t* bitrev() const { return bitrev_.data(); }

private:
    void butterfly2(Complex* out, int m, int count) const;
    void butterfly3(Complex* out, int fstride, int m, int count, int mm) const;
    void butterfly4(Complex* out, int fstride, int m, int count, int mm) const;
    void butterfly5(Complex* out, int fstride, int m, int count, int mm) const;

    int nfft_ = 0;
    int shift_ = -1;  // twiddle decimation relative to the base state
    int factors_[16] = {};
    std::vector<Complex> owned_twiddles_;
    const Complex* twiddles_ = nullptr;
    std::vector<int16_t> bitrev_;
};

}  // namespace player::audio::opus
