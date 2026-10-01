#pragma once

#include "audio/opus/RangeDecoder.hpp"

#include <cstdint>

namespace player::audio::silk {

// Decodes the excitation pulses of one SILK frame (silk_decode_pulses):
// rate level, shell coding, LSB extensions and signs. pulses receives
// frame_length values.
void decodePulses(opus::RangeDecoder& decoder, int16_t* pulses, int signal_type,
                  int quant_offset_type, int frame_length);

}  // namespace player::audio::silk
