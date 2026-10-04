#pragma once

#include "filters/Filter.hpp"

#include <memory>
#include <string>

namespace player::filters {

// Parses an ffmpeg-style filter description. The whole graph is a comma
// separated list of filters, each "name" or "name=key=value:key=value"; a
// filter's positional arguments ("scale=640:360") are positional, named
// arguments ("eq=brightness=0.1") may be interleaved.
//
//   "scale=1280:720,flip=h,eq=contrast=1.2"
//   "volume=1.5,resample=48000,equalizer=bass=6:treble=-3"
//
// An empty spec yields an empty (but valid) chain; a parse error returns null
// and fills `error`.
std::shared_ptr<VideoFilterChain> parseVideoFilterSpec(const std::string& spec, std::string& error);
std::shared_ptr<AudioFilterChain> parseAudioFilterSpec(const std::string& spec, std::string& error);

// Builds one filter from a single description, e.g. "scale=640:-2".
std::unique_ptr<IVideoFilter> createVideoFilter(const std::string& description, std::string& error);
std::unique_ptr<IAudioFilter> createAudioFilter(const std::string& description, std::string& error);

}  // namespace player::filters
