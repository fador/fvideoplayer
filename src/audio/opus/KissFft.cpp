#include "audio/opus/KissFft.hpp"
#include "audio/opus/OpusTables.hpp"

#include <cmath>

namespace player::audio::opus {
namespace {

constexpr float kSqrtHalf = 0.7071067812f;

// kf_factor: factors n into a (radix, remaining length) sequence with radices
// at most 5, radix 4 first and the reversed order at the end.
bool factorize(int n, int* facbuf) {
    int p = 4;
    int stages = 0;
    const int original = n;
    do {
        while (n % p != 0) {
            switch (p) {
                case 4:
                    p = 2;
                    break;
                case 2:
                    p = 3;
                    break;
                default:
                    p += 2;
                    break;
            }
            if (p > 32000 || p * p > n) {
                p = n;
            }
        }
        n /= p;
        if (p > 5) {
            return false;
        }
        facbuf[2 * stages] = p;
        if (p == 2 && stages > 1) {
            facbuf[2 * stages] = 4;
            facbuf[2] = 2;
        }
        ++stages;
    } while (n > 1);
    n = original;
    for (int i = 0; i < stages / 2; ++i) {
        const int tmp = facbuf[2 * i];
        facbuf[2 * i] = facbuf[2 * (stages - i - 1)];
        facbuf[2 * (stages - i - 1)] = tmp;
    }
    for (int i = 0; i < stages; ++i) {
        n /= facbuf[2 * i];
        facbuf[2 * i + 1] = n;
    }
    return true;
}

// compute_bitrev_table: fills the bit-reversal permutation for the factors.
void computeBitrev(int fout, int16_t* f, int fstride, int in_stride, const int* factors) {
    const int p = factors[0];
    const int m = factors[1];
    if (m == 1) {
        for (int j = 0; j < p; ++j) {
            *f = static_cast<int16_t>(fout + j);
            f += fstride * in_stride;
        }
        return;
    }
    for (int j = 0; j < p; ++j) {
        computeBitrev(fout, f, fstride * p, in_stride, factors + 2);
        f += fstride * in_stride;
        fout += m;
    }
}

void addTo(Complex& res, const Complex& a) {
    res.r += a.r;
    res.i += a.i;
}

}  // namespace

void FftState::init(int nfft, const FftState* base) {
    nfft_ = nfft;
    if (base != nullptr) {
        // Smaller transform sharing the base twiddle table.
        twiddles_ = base->twiddles_;
        shift_ = 0;
        while (shift_ < 32 && (nfft << shift_) != base->nfft_) {
            ++shift_;
        }
    } else {
        constexpr double kPi = 3.14159265358979323846264338327;
        owned_twiddles_.resize(nfft);
        if (nfft == 480) {
            // Use the static mode's precomputed twiddles for the base size.
            for (int i = 0; i < nfft; ++i) {
                owned_twiddles_[i].r = kCeltFftTwiddles[2 * i];
                owned_twiddles_[i].i = kCeltFftTwiddles[2 * i + 1];
            }
        } else {
            for (int i = 0; i < nfft; ++i) {
                const double phase = (-2 * kPi / nfft) * i;
                owned_twiddles_[i].r = static_cast<float>(std::cos(phase));
                owned_twiddles_[i].i = static_cast<float>(std::sin(phase));
            }
        }
        twiddles_ = owned_twiddles_.data();
        shift_ = -1;
    }

    factorize(nfft_, factors_);
    bitrev_.resize(nfft_);
    computeBitrev(0, bitrev_.data(), 1, 1, factors_);
}

void FftState::butterfly2(Complex* out, int m, int count) const {
    if (m == 1) {
        for (int i = 0; i < count; ++i) {
            Complex* out2 = out + 1;
            const Complex t = *out2;
            out2->r = out->r - t.r;
            out2->i = out->i - t.i;
            addTo(*out, t);
            out += 2;
        }
        return;
    }
    for (int i = 0; i < count; ++i) {
        Complex* out2 = out + 4;
        Complex t = out2[0];
        out2[0].r = out[0].r - t.r;
        out2[0].i = out[0].i - t.i;
        addTo(out[0], t);

        t.r = (out2[1].r + out2[1].i) * kSqrtHalf;
        t.i = (out2[1].i - out2[1].r) * kSqrtHalf;
        out2[1].r = out[1].r - t.r;
        out2[1].i = out[1].i - t.i;
        addTo(out[1], t);

        t.r = out2[2].i;
        t.i = -out2[2].r;
        out2[2].r = out[2].r - t.r;
        out2[2].i = out[2].i - t.i;
        addTo(out[2], t);

        t.r = (out2[3].i - out2[3].r) * kSqrtHalf;
        t.i = -((out2[3].i + out2[3].r) * kSqrtHalf);
        out2[3].r = out[3].r - t.r;
        out2[3].i = out[3].i - t.i;
        addTo(out[3], t);
        out += 8;
    }
}

void FftState::butterfly4(Complex* out, int fstride, int m, int count, int mm) const {
    if (m == 1) {
        for (int i = 0; i < count; ++i) {
            Complex s0;
            Complex s1;
            s0.r = out[0].r - out[2].r;
            s0.i = out[0].i - out[2].i;
            addTo(out[0], out[2]);
            s1.r = out[1].r + out[3].r;
            s1.i = out[1].i + out[3].i;
            out[2].r = out[0].r - s1.r;
            out[2].i = out[0].i - s1.i;
            addTo(out[0], s1);
            s1.r = out[1].r - out[3].r;
            s1.i = out[1].i - out[3].i;
            out[1].r = s0.r + s1.i;
            out[1].i = s0.i - s1.r;
            out[3].r = s0.r - s1.i;
            out[3].i = s0.i + s1.r;
            out += 4;
        }
        return;
    }

    const int m2 = 2 * m;
    const int m3 = 3 * m;
    Complex* const begin = out;
    for (int i = 0; i < count; ++i) {
        out = begin + i * mm;
        const Complex* tw1 = twiddles_;
        const Complex* tw2 = twiddles_;
        const Complex* tw3 = twiddles_;
        for (int j = 0; j < m; ++j) {
            Complex s0;
            Complex s1;
            Complex s2;
            s0.r = out[m].r * tw1->r - out[m].i * tw1->i;
            s0.i = out[m].r * tw1->i + out[m].i * tw1->r;
            s1.r = out[m2].r * tw2->r - out[m2].i * tw2->i;
            s1.i = out[m2].r * tw2->i + out[m2].i * tw2->r;
            s2.r = out[m3].r * tw3->r - out[m3].i * tw3->i;
            s2.i = out[m3].r * tw3->i + out[m3].i * tw3->r;

            Complex s5;
            s5.r = out[0].r - s1.r;
            s5.i = out[0].i - s1.i;
            addTo(out[0], s1);
            Complex s3;
            s3.r = s0.r + s2.r;
            s3.i = s0.i + s2.i;
            Complex s4;
            s4.r = s0.r - s2.r;
            s4.i = s0.i - s2.i;
            out[m2].r = out[0].r - s3.r;
            out[m2].i = out[0].i - s3.i;
            tw1 += fstride;
            tw2 += fstride * 2;
            tw3 += fstride * 3;
            addTo(out[0], s3);

            out[m].r = s5.r + s4.i;
            out[m].i = s5.i - s4.r;
            out[m3].r = s5.r - s4.i;
            out[m3].i = s5.i + s4.r;
            ++out;
        }
    }
}

void FftState::butterfly3(Complex* out, int fstride, int m, int count, int mm) const {
    const int m2 = 2 * m;
    const Complex epi3 = twiddles_[fstride * m];
    Complex* const begin = out;
    for (int i = 0; i < count; ++i) {
        out = begin + i * mm;
        const Complex* tw1 = twiddles_;
        const Complex* tw2 = twiddles_;
        int k = m;
        do {
            Complex s1;
            Complex s2;
            s1.r = out[m].r * tw1->r - out[m].i * tw1->i;
            s1.i = out[m].r * tw1->i + out[m].i * tw1->r;
            s2.r = out[m2].r * tw2->r - out[m2].i * tw2->i;
            s2.i = out[m2].r * tw2->i + out[m2].i * tw2->r;
            Complex s3;
            s3.r = s1.r + s2.r;
            s3.i = s1.i + s2.i;
            Complex s0;
            s0.r = s1.r - s2.r;
            s0.i = s1.i - s2.i;
            tw1 += fstride;
            tw2 += fstride * 2;

            out[m].r = out[0].r - 0.5f * s3.r;
            out[m].i = out[0].i - 0.5f * s3.i;
            s0.r *= epi3.i;
            s0.i *= epi3.i;
            addTo(out[0], s3);
            out[m2].r = out[m].r + s0.i;
            out[m2].i = out[m].i - s0.r;
            out[m].r = out[m].r - s0.i;
            out[m].i = out[m].i + s0.r;
            ++out;
        } while (--k);
    }
}

void FftState::butterfly5(Complex* out, int fstride, int m, int count, int mm) const {
    const Complex ya = twiddles_[fstride * m];
    const Complex yb = twiddles_[fstride * 2 * m];
    const Complex* const tw = twiddles_;
    Complex* const begin = out;
    for (int i = 0; i < count; ++i) {
        Complex* f0 = begin + i * mm;
        Complex* f1 = f0 + m;
        Complex* f2 = f0 + 2 * m;
        Complex* f3 = f0 + 3 * m;
        Complex* f4 = f0 + 4 * m;
        for (int u = 0; u < m; ++u) {
            const Complex s0 = *f0;

            Complex s1;
            Complex s2;
            Complex s3;
            Complex s4;
            const Complex* t1 = tw + u * fstride;
            const Complex* t2 = tw + 2 * u * fstride;
            const Complex* t3 = tw + 3 * u * fstride;
            const Complex* t4 = tw + 4 * u * fstride;
            s1.r = f1->r * t1->r - f1->i * t1->i;
            s1.i = f1->r * t1->i + f1->i * t1->r;
            s2.r = f2->r * t2->r - f2->i * t2->i;
            s2.i = f2->r * t2->i + f2->i * t2->r;
            s3.r = f3->r * t3->r - f3->i * t3->i;
            s3.i = f3->r * t3->i + f3->i * t3->r;
            s4.r = f4->r * t4->r - f4->i * t4->i;
            s4.i = f4->r * t4->i + f4->i * t4->r;

            Complex s7;
            Complex s8;
            Complex s9;
            Complex s10;
            s7.r = s1.r + s4.r;
            s7.i = s1.i + s4.i;
            s10.r = s1.r - s4.r;
            s10.i = s1.i - s4.i;
            s8.r = s2.r + s3.r;
            s8.i = s2.i + s3.i;
            s9.r = s2.r - s3.r;
            s9.i = s2.i - s3.i;

            f0->r += s7.r + s8.r;
            f0->i += s7.i + s8.i;

            Complex s5;
            s5.r = s0.r + (s7.r * ya.r + s8.r * yb.r);
            s5.i = s0.i + (s7.i * ya.r + s8.i * yb.r);
            Complex s6;
            s6.r = s10.i * ya.i + s9.i * yb.i;
            s6.i = -(s10.r * ya.i + s9.r * yb.i);
            f1->r = s5.r - s6.r;
            f1->i = s5.i - s6.i;
            f4->r = s5.r + s6.r;
            f4->i = s5.i + s6.i;

            Complex s11;
            s11.r = s0.r + (s7.r * yb.r + s8.r * ya.r);
            s11.i = s0.i + (s7.i * yb.r + s8.i * ya.r);
            Complex s12;
            s12.r = s9.i * ya.i - s10.i * yb.i;
            s12.i = s10.r * yb.i - s9.r * ya.i;
            f2->r = s11.r + s12.r;
            f2->i = s11.i + s12.i;
            f3->r = s11.r - s12.r;
            f3->i = s11.i - s12.i;

            ++f0;
            ++f1;
            ++f2;
            ++f3;
            ++f4;
        }
    }
}

void FftState::transform(Complex* data) const {
    // The shared-twiddle states decimate the twiddle table.
    const int shift = shift_ > 0 ? shift_ : 0;

    int fstride[8];
    fstride[0] = 1;
    int l = 0;
    int m;
    do {
        const int p = factors_[2 * l];
        m = factors_[2 * l + 1];
        fstride[l + 1] = fstride[l] * p;
        ++l;
    } while (m != 1);

    m = factors_[2 * l - 1];
    for (int i = l - 1; i >= 0; --i) {
        const int m2 = i != 0 ? factors_[2 * i - 1] : 1;
        switch (factors_[2 * i]) {
            case 2:
                butterfly2(data, m, fstride[i]);
                break;
            case 4:
                butterfly4(data, fstride[i] << shift, m, fstride[i], m2);
                break;
            case 3:
                butterfly3(data, fstride[i] << shift, m, fstride[i], m2);
                break;
            case 5:
                butterfly5(data, fstride[i] << shift, m, fstride[i], m2);
                break;
        }
        m = m2;
    }
}

}  // namespace player::audio::opus
