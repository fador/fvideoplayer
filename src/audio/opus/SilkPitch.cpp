#include "audio/opus/SilkPitch.hpp"

#include "audio/opus/SilkMath.hpp"
#include "audio/opus/SilkTables.hpp"

namespace player::audio::silk {

void decodePitch(int16_t lag_index, int8_t contour_index, int* pitch_lags, int fs_khz,
                 int nb_subfr) {
    // Lag codebook depends on the bandwidth and frame size.
    const int8_t* lag_cb;
    int cbk_size;
    if (fs_khz == 8) {
        if (nb_subfr == 4) {
            lag_cb = kSilkCbLagsStage2;
            cbk_size = 11;
        } else {
            lag_cb = kSilkCbLagsStage2_10ms;
            cbk_size = 3;
        }
    } else {
        if (nb_subfr == 4) {
            lag_cb = kSilkCbLagsStage3;
            cbk_size = 34;
        } else {
            lag_cb = kSilkCbLagsStage3_10ms;
            cbk_size = 12;
        }
    }

    constexpr int kMinLagMs = 2;
    constexpr int kMaxLagMs = 18;
    const int min_lag = kMinLagMs * fs_khz;
    const int max_lag = kMaxLagMs * fs_khz;
    const int lag = min_lag + lag_index;

    for (int k = 0; k < nb_subfr; ++k) {
        pitch_lags[k] = limit(lag + lag_cb[k * cbk_size + contour_index], min_lag, max_lag);
    }
}

}  // namespace player::audio::silk
