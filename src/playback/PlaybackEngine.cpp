#include "playback/PlaybackEngine.hpp"

#include "demux/HvcC.hpp"
#include "filters/FilterSpec.hpp"
#include "utils/Log.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>

namespace player::playback {
namespace {

// 'mp4a': AAC audio in MP4/Matroska/AVI containers. 'Opus': Opus audio.
constexpr uint32_t kMp4aFourcc = 0x6D703461;
constexpr uint32_t kAc3Fourcc = 0x61632D33;  // 'ac-3'
constexpr uint32_t kOpusFourcc = 0x4F707573;
constexpr uint32_t kMp2Fourcc = 0x6D703220;  // 'mp2 '
constexpr uint32_t kMp3Fourcc = 0x6D703320;  // 'mp3 '
constexpr uint32_t kFlacFourcc = 0x664C6143;

std::string containerName(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    if (!ext.empty() && ext[0] == '.') {
        ext.erase(0, 1);
    }
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return ext.empty() ? "RAW" : ext;
}

}  // namespace

PlaybackEngine::PlaybackEngine() = default;

PlaybackEngine::~PlaybackEngine() {
    close();
}

const char* PlaybackEngine::videoCodecName(demux::Codec codec) {
    switch (codec) {
        case demux::Codec::Hevc:
            return "HEVC";
        case demux::Codec::Mpeg1:
            return "MPEG-1";
        case demux::Codec::Mpeg2:
            return "MPEG-2";
        case demux::Codec::Flv1:
            return "FLV1";
        default:
            return "audio only";
    }
}

bool PlaybackEngine::configureVideoDecoder(std::string& error) {
    switch (video_info_.codec) {
        case demux::Codec::Hevc:
            video_codec_ = VideoCodec::Hevc;
            if (!decoder_.open(&error)) {
                video_codec_ = VideoCodec::None;
                return false;
            }
            if (!video_info_.hvcc.empty() && !decoder_.pushParameterSets(video_info_.hvcc, &error)) {
                decoder_.close();
                video_codec_ = VideoCodec::None;
                return false;
            }
            break;
        case demux::Codec::Mpeg1:
        case demux::Codec::Mpeg2:
            video_codec_ = VideoCodec::Mpeg1;
            mpeg12_decoder_.reset();
            break;
        case demux::Codec::Flv1:
            video_codec_ = VideoCodec::Flv1;
            flv1_decoder_.reset();
            break;
        default:
            // Audio-only files carry no video track, which is not an error.
            video_codec_ = VideoCodec::None;
            break;
    }
    if (video_codec_ == VideoCodec::None) {
        log::add(log::Level::Info, "No video track");
        return true;
    }
    log::addf(log::Level::Info, "Video decoder: %s %ux%u @ %.3f fps", videoCodecName(video_info_.codec),
              video_info_.width, video_info_.height, video_info_.fps);
    return true;
}

void PlaybackEngine::resetVideoDecoder() {
    switch (video_codec_) {
        case VideoCodec::Hevc:
            decoder_.close();
            if (!decoder_.open(nullptr)) {
                video_codec_ = VideoCodec::None;
                return;
            }
            if (!video_info_.hvcc.empty()) {
                decoder_.pushParameterSets(video_info_.hvcc, nullptr);
            }
            break;
        case VideoCodec::Mpeg1:
            // The reference pictures have to go, so the decoder is rebuilt.
            mpeg12_decoder_.reset();
            break;
        case VideoCodec::Flv1:
            flv1_decoder_.reset();
            break;
        case VideoCodec::None:
            break;
    }
}

bool PlaybackEngine::pushVideoSample(const demux::Sample& sample) {
    switch (video_codec_) {
        case VideoCodec::Hevc:
            return decoder_.pushSample(sample.data.data(), sample.data.size(), sample.pts,
                                       video_info_.format.annexb,
                                       video_info_.format.nal_length_size);
        case VideoCodec::Mpeg1:
            mpeg12_decoder_.pushSample(sample.data.data(), sample.data.size(), sample.pts);
            return true;
        case VideoCodec::Flv1:
            flv1_decoder_.pushSample(sample.data.data(), sample.data.size(), sample.pts);
            return true;
        case VideoCodec::None:
            return false;
    }
    return false;
}

int PlaybackEngine::pullVideoFrames(std::vector<decode::DecodedFrame>& out) {
    switch (video_codec_) {
        case VideoCodec::Hevc:
            return decoder_.pullFrames(out);
        case VideoCodec::Mpeg1:
            return mpeg12_decoder_.pullFrames(out);
        case VideoCodec::Flv1:
            return flv1_decoder_.pullFrames(out);
        case VideoCodec::None:
            return 0;
    }
    return 0;
}

int PlaybackEngine::flushVideoDecoder(std::vector<decode::DecodedFrame>& out) {
    switch (video_codec_) {
        case VideoCodec::Hevc:
            return decoder_.flush(out);
        case VideoCodec::Mpeg1:
            return mpeg12_decoder_.flush(out);
        case VideoCodec::Flv1:
            return flv1_decoder_.flush(out);
        case VideoCodec::None:
            return 0;
    }
    return 0;
}

bool PlaybackEngine::setVideoFilters(const std::string& spec, std::string& error) {
    error.clear();
    std::shared_ptr<filters::VideoFilterChain> chain =
        filters::parseVideoFilterSpec(spec, error);
    if (!chain) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(filter_mutex_);
        video_filter_spec_ = spec;
        video_filters_ = std::move(chain);
    }
    video_filter_warned_ = false;
    if (spec.empty()) {
        log::add(log::Level::Info, "Video filters cleared");
    } else {
        log::addf(log::Level::Info, "Video filters: %s", spec.c_str());
    }
    return true;
}

bool PlaybackEngine::setAudioFilters(const std::string& spec, std::string& error) {
    error.clear();
    std::shared_ptr<filters::AudioFilterChain> chain =
        filters::parseAudioFilterSpec(spec, error);
    if (!chain) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(filter_mutex_);
        audio_filter_spec_ = spec;
        audio_filters_ = std::move(chain);
    }
    audio_filter_warned_ = false;
    // Existing buffered PCM came out of the old graph, and a format change
    // (resampler or remix) must not mix into the sink.
    {
        std::lock_guard<std::mutex> audio_lock(audio_mutex_);
        audio_buffer_.clear();
        sink().flush();
    }
    if (spec.empty()) {
        log::add(log::Level::Info, "Audio filters cleared");
    } else {
        log::addf(log::Level::Info, "Audio filters: %s", spec.c_str());
    }
    return true;
}

std::string PlaybackEngine::videoFilterSpec() const {
    std::lock_guard<std::mutex> lock(filter_mutex_);
    return video_filter_spec_;
}

std::string PlaybackEngine::audioFilterSpec() const {
    std::lock_guard<std::mutex> lock(filter_mutex_);
    return audio_filter_spec_;
}

std::vector<std::string> PlaybackEngine::videoFilterNames() const {
    std::shared_ptr<filters::VideoFilterChain> chain;
    {
        std::lock_guard<std::mutex> lock(filter_mutex_);
        chain = video_filters_;
    }
    return chain ? chain->names() : std::vector<std::string>{};
}

std::vector<std::string> PlaybackEngine::audioFilterNames() const {
    std::shared_ptr<filters::AudioFilterChain> chain;
    {
        std::lock_guard<std::mutex> lock(filter_mutex_);
        chain = audio_filters_;
    }
    return chain ? chain->names() : std::vector<std::string>{};
}

void PlaybackEngine::rebuildFilters() {
    std::string video_error;
    std::string audio_error;
    {
        std::lock_guard<std::mutex> lock(filter_mutex_);
        std::shared_ptr<filters::VideoFilterChain> video =
            filters::parseVideoFilterSpec(video_filter_spec_, video_error);
        std::shared_ptr<filters::AudioFilterChain> audio =
            filters::parseAudioFilterSpec(audio_filter_spec_, audio_error);
        if (video) {
            video_filters_ = std::move(video);
        }
        if (audio) {
            audio_filters_ = std::move(audio);
        }
    }
    if (!video_error.empty()) {
        log::addf(log::Level::Warning, "Video filter graph rejected: %s", video_error.c_str());
    }
    if (!audio_error.empty()) {
        log::addf(log::Level::Warning, "Audio filter graph rejected: %s", audio_error.c_str());
    }
    video_filter_warned_ = false;
    audio_filter_warned_ = false;
}

bool PlaybackEngine::applyVideoFilters(decode::DecodedFrame& frame) {
    std::shared_ptr<filters::VideoFilterChain> chain;
    {
        std::lock_guard<std::mutex> lock(filter_mutex_);
        chain = video_filters_;
    }
    if (!chain || chain->empty() || !frame.image.valid()) {
        return true;
    }
    std::string error;
    if (!chain->process(frame.image, error)) {
        if (!video_filter_warned_) {
            log::addf(log::Level::Warning, "Video filter failed: %s", error.c_str());
            video_filter_warned_ = true;
        }
        return false;
    }
    return true;
}

bool PlaybackEngine::applyAudioFilters(audio::PcmBuffer& chunk) {
    std::shared_ptr<filters::AudioFilterChain> chain;
    {
        std::lock_guard<std::mutex> lock(filter_mutex_);
        chain = audio_filters_;
    }
    if (!chain || chain->empty()) {
        return true;
    }
    std::string error;
    if (!chain->process(chunk, error)) {
        if (!audio_filter_warned_) {
            log::addf(log::Level::Warning, "Audio filter failed: %s", error.c_str());
            audio_filter_warned_ = true;
        }
        return false;
    }
    if (chunk.sample_rate != audio_out_rate_ || chunk.channels != audio_out_channels_) {
        audio_out_rate_ = chunk.sample_rate;
        audio_out_channels_ = chunk.channels;
        audio::AudioFormat format;
        format.codec_fourcc = audio_out_fourcc_;
        format.channels = chunk.channels;
        format.sample_rate = chunk.sample_rate;
        sink().configure(format);
        log::addf(log::Level::Info, "Audio filter output: %u Hz, %u channel(s)",
                  chunk.sample_rate, chunk.channels);
    }
    return true;
}

void PlaybackEngine::resetFilterState() {
    std::shared_ptr<filters::VideoFilterChain> video;
    std::shared_ptr<filters::AudioFilterChain> audio;
    {
        std::lock_guard<std::mutex> lock(filter_mutex_);
        video = video_filters_;
        audio = audio_filters_;
    }
    if (video) {
        video->reset();
    }
    if (audio) {
        audio->reset();
    }
}

bool PlaybackEngine::open(const std::filesystem::path& path, std::string& error) {
    close();

    std::string local_error;
    std::unique_ptr<demux::IDemuxer> demuxer = demux::createDemuxer(path, local_error);
    if (!demuxer) {
        last_error_ = local_error;
        error = local_error;
        log::addf(log::Level::Error, "Demuxer error: %s", local_error.c_str());
        return false;
    }
    video_info_ = demuxer->video();
    audio_info_ = demuxer->audio();
    audio_track_infos_ = demuxer->audioTracks();
    subtitle_track_infos_ = demuxer->subtitleTracks();
    duration_ = demuxer->duration();

    if (!configureVideoDecoder(local_error)) {
        last_error_ = local_error;
        error = local_error;
        log::addf(log::Level::Error, "Decoder open failed: %s", local_error.c_str());
        return false;
    }
    demuxer_ = std::move(demuxer);

    // Decode-ahead buffer: about two seconds of frames, bounded by memory.
    const double fps = video_info_.fps > 1.0 ? video_info_.fps : 30.0;
    const size_t frames_for_two_seconds = static_cast<size_t>(std::ceil(fps * 2.0));
    const uint64_t pixels = (video_info_.width && video_info_.height)
                                ? static_cast<uint64_t>(video_info_.width) * video_info_.height
                                : 1920ull * 1080ull;
    const uint64_t picture_bytes = pixels * 3;  // 4:2:0 16-bit worst case
    constexpr uint64_t kBudgetBytes = 768ull * 1024ull * 1024ull;
    const size_t budget_frames = picture_bytes ? static_cast<size_t>(kBudgetBytes / picture_bytes) : 120;
    size_t capacity = std::min(frames_for_two_seconds, budget_frames);
    capacity = std::max<size_t>(8, std::min<size_t>(capacity, 240));
    queue_.setCapacity(capacity);

    stats_.reset();
    stats::StreamInfo info;
    info.container = containerName(path);
    info.codec = videoCodecName(video_info_.codec);
    info.width = video_info_.width;
    info.height = video_info_.height;
    info.fps = video_info_.fps;
    info.duration = duration_;
    if (!video_info_.hvcc.empty()) {
        demux::HvccInfo hvcc;
        if (demux::parseHvcc(video_info_.hvcc.data(), video_info_.hvcc.size(), hvcc, nullptr)) {
            info.profile = demux::hvccProfileName(hvcc);
            info.level = demux::hvccLevelName(hvcc);
            info.bitdepth = 8 + hvcc.bit_depth_luma_minus8;
            info.chroma_format = hvcc.chroma_format_idc;
            stream_bitdepth_ = info.bitdepth;
        }
    }
    stats_.setStreamInfo(info);
    audio_warned_ = false;
    audio_late_drops_ = 0;
    audio_eos_ = false;
    audio_buffer_.clear();
    rebuildFilters();
    configureAudioDecoder(audio_info_);

    quit_ = false;
    eos_ = false;
    state_ = State::Paused;
    clock_.setPosition(0.0);
    current_.reset();
    current_serial_ = 0;
    selected_audio_ = 0;
    audio_enabled_ = !audio_track_infos_.empty();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        seek_pending_ = false;
        audio_pending_ = false;
    }
    log::addf(log::Level::Info, "%s: %ux%u, %.3f fps, %.2f s, buffer %zu frames",
              info.container.c_str(), video_info_.width, video_info_.height, video_info_.fps,
              duration_, queue_.capacity());
    if (audio_info_.present) {
        log::addf(log::Level::Info, "Audio: fourcc 0x%08X, %u channels, %u Hz (%s)",
                  audio_info_.codec_fourcc, audio_info_.channels, audio_info_.sample_rate,
                  audioCodecName());
    }
    thread_ = std::thread([this] { pipelineLoop(); });
    return true;
}

void PlaybackEngine::close() {
    if (thread_.joinable()) {
        quit_ = true;
        queue_.abort();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            seek_pending_ = false;
        }
        cv_.notify_all();
        thread_.join();
    }
    queue_.reset();
    queue_.setCapacity(16);
    decoder_.close();
    demuxer_.reset();
    resetFilterState();
    current_.reset();
    if (audio_info_.present && audio_decoding_.load() && sink().framesConsumed() > 0) {
        log::addf(log::Level::Info, "Audio: %llu frames played, %llu dropped, %llu late, %llu stale, %llu underruns",
                  static_cast<unsigned long long>(sink().framesConsumed()),
                  static_cast<unsigned long long>(sink().framesDropped()),
                  static_cast<unsigned long long>(audio_buffer_.droppedChunks()),
                  static_cast<unsigned long long>(audio_late_drops_),
                  static_cast<unsigned long long>(sink().underruns()));
    }
    sink().reset();
    audio_buffer_.clear();
    resetAudioDecoder();
    audio_decoding_ = false;
    audio_warned_ = false;
    audio_track_infos_.clear();
    subtitle_track_infos_.clear();
    audio_enabled_ = false;
    selected_audio_ = 0;
    eos_ = false;
    state_ = State::Empty;
}

void PlaybackEngine::selectAudioTrack(size_t index) {
    if (!hasFile()) {
        return;
    }
    if (index != kNoAudioTrack && index >= audio_track_infos_.size()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        audio_pending_ = true;
        audio_target_ = index;
    }
    cv_.notify_all();
}

bool PlaybackEngine::loadSubtitleTrack(size_t index,
                                       std::vector<subtitle::SubtitleBlock>& blocks,
                                       std::string& error) {
    if (!demuxer_) {
        error = "no file open";
        return false;
    }
    return demuxer_->readSubtitleTrack(index, blocks, error);
}

void PlaybackEngine::play() {
    if (!hasFile()) {
        return;
    }
    if (state_.load() == State::Ended) {
        seek(0.0);
    }
    clock_.start(clock_.position());
    state_ = State::Playing;
    stats_.setPlaying(true);
    sink().resume();
    cv_.notify_all();
}

void PlaybackEngine::pause() {
    if (!hasFile()) {
        return;
    }
    clock_.pause();
    state_ = State::Paused;
    stats_.setPlaying(false);
    sink().pause();
}

void PlaybackEngine::togglePlayPause() {
    if (state_.load() == State::Playing) {
        pause();
    } else {
        play();
    }
}

void PlaybackEngine::stop() {
    if (!hasFile()) {
        return;
    }
    pause();
    seek(0.0);
}

void PlaybackEngine::seek(double seconds) {
    if (!hasFile()) {
        return;
    }
    if (seconds < 0.0) {
        seconds = 0.0;
    }
    if (duration_ > 0.0 && seconds > duration_) {
        seconds = duration_;
    }
    clock_.setPosition(seconds);
    current_.reset();
    queue_.clear();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        seek_target_ = seconds;
        seek_pending_ = true;
    }
    if (state_.load() == State::Ended) {
        state_ = State::Paused;
    }
    cv_.notify_all();
}

bool PlaybackEngine::update() {
    bool changed = false;
    bool popped_any = false;
    if (state_.load() == State::Playing) {
        const int64_t due_ms = static_cast<int64_t>(std::llround(clock_.position() * 1000.0));
        decode::DecodedFrame frame;
        int popped = 0;
        while (queue_.tryPopDue(due_ms, frame)) {
            popped_any = true;
            const bool dropped = popped > 0;
            current_ = std::make_unique<decode::DecodedFrame>(std::move(frame));
            ++current_serial_;
            stats_.onFrameDisplayed(dropped);
            changed = true;
            ++popped;
        }
    }
    // Feed audio from the UI thread as well: the pipeline thread can be busy
    // decoding video or reading from disk, and the sink must not run dry.
    feedAudio();
    if (state_.load() == State::Playing && eos_.load() && queue_.empty() &&
        (!audio_enabled_.load() ||
         (audio_eos_.load() && audio_buffer_.empty() && sink().queuedMs() < kAudioLateMs))) {
        clock_.pause();
        // The stream is over: stop the audio device too, or it keeps pulling
        // from a buffer that will never be filled again.
        sink().pause();
        state_ = State::Ended;
        stats_.setPlaying(false);
    }
    if (popped_any) {
        // Wake the pipeline: it may be waiting for queue space that the
        // frames consumed above have freed.
        cv_.notify_all();
    }
    return changed;
}

bool PlaybackEngine::buffering() const {
    return state_.load() == State::Playing && current_ == nullptr && !eos_.load();
}

void PlaybackEngine::pipelineLoop() {
    const auto bufferedSeconds = [this] {
        const double fps = video_info_.fps > 1.0 ? video_info_.fps : 30.0;
        return static_cast<double>(queue_.size()) / fps;
    };

    for (;;) {
        std::unique_lock<std::mutex> lock(mutex_);
        // The timeout keeps audio fed even while the video queue is full and
        // there is nothing else to do.
        cv_.wait_for(lock, std::chrono::milliseconds(10), [this] {
            return quit_.load() || seek_pending_ || audio_pending_ ||
                   (!eos_.load() && queue_.size() < queue_.capacity());
        });
        if (quit_.load()) {
            break;
        }
        if (seek_pending_) {
            const double target = seek_target_;
            seek_pending_ = false;
            lock.unlock();
            performSeek(target);
            continue;
        }
        if (audio_pending_) {
            const size_t target = audio_target_;
            audio_pending_ = false;
            lock.unlock();
            if (target == kNoAudioTrack) {
                audio_enabled_ = false;
                {
                    std::lock_guard<std::mutex> audio_lock(audio_mutex_);
                    audio_buffer_.clear();
                    sink().reset();
                }
                log::add(log::Level::Info, "Audio disabled");
            } else if (demuxer_->selectAudioTrack(target)) {
                {
                    std::lock_guard<std::mutex> audio_lock(audio_mutex_);
                    audio_buffer_.clear();
                    sink().reset();
                }
                selected_audio_ = target;
                audio_enabled_ = true;
                audio_eos_ = false;
                audio_warned_ = false;
                configureAudioDecoder(audio_track_infos_[target]);
                log::addf(log::Level::Info, "Audio track %zu selected", target);
            } else {
                log::addf(log::Level::Warning, "Failed to select audio track %zu", target);
            }
            continue;
        }
        const bool video_ready = !eos_.load() && queue_.size() < queue_.capacity();
        lock.unlock();

        if (video_ready) {
            demux::Sample sample;
            if (!demuxer_->readSample(sample)) {
                std::vector<decode::DecodedFrame> frames;
                flushVideoDecoder(frames);
                for (decode::DecodedFrame& frame : frames) {
                    if (!applyVideoFilters(frame)) {
                        continue;
                    }
                    stats_.onFrameDecoded(frame, 0.0);
                    if (!queue_.push(std::move(frame), [this] { return quit_.load(); })) {
                        break;
                    }
                }
                eos_ = true;
                stats_.setEos(true);
                log::addf(log::Level::Info, "End of stream (%lld frames decoded)",
                          static_cast<long long>(stats_.snapshot().frames_decoded));
            } else {
                stats_.onBytesDemuxed(sample.data.size());
                const auto start = std::chrono::steady_clock::now();
                const bool ok = pushVideoSample(sample);
                std::vector<decode::DecodedFrame> frames;
                if (ok) {
                    pullVideoFrames(frames);
                } else {
                    log::addf(log::Level::Error, "Decoder rejected a sample (pts %lld)",
                              static_cast<long long>(sample.pts));
                }
                const double elapsed_ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                              start)
                        .count();
                const double per_frame_ms =
                    frames.empty() ? elapsed_ms : elapsed_ms / static_cast<double>(frames.size());
                for (decode::DecodedFrame& frame : frames) {
                    if (!applyVideoFilters(frame)) {
                        continue;
                    }
                    frame.sample_bytes = sample.data.size();
                    stats_.onFrameDecoded(frame, per_frame_ms);
                    if (!queue_.push(std::move(frame), [this] { return quit_.load(); })) {
                        break;
                    }
                }
            }
        }

        pumpAudio();
        feedAudio();
        stats_.setQueue(queue_.size(), queue_.capacity(), bufferedSeconds());
        if (audio_enabled_.load()) {
            stats_.setAudio(audio_buffer_.durationMs(), sink().queuedMs(),
                            audio_buffer_.endPtsMs() - positionMs(),
                            static_cast<int64_t>(sink().underruns()));
        } else {
            stats_.setAudio(0, 0, 0, 0);
        }
    }
}

void PlaybackEngine::performSeek(double seconds) {
    const auto start = std::chrono::steady_clock::now();
    queue_.clear();
    resetVideoDecoder();
    resetFilterState();
    demuxer_->seek(seconds);
    resetAudioState();
    eos_ = false;
    stats_.setEos(false);
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    stats_.onSeek(elapsed_ms);
    log::addf(log::Level::Debug, "Seek to %.3f s (%.1f ms)", seconds, elapsed_ms);
}

void PlaybackEngine::configureAudioDecoder(const demux::AudioTrackInfo& track) {
    audio_codec_ = AudioCodec::None;
    audio_decoding_ = false;
    audio_frame_ms_ = 21;
    audio_out_rate_ = 0;
    audio_out_channels_ = 0;
    audio_out_fourcc_ = 0;
    if (!track.present) {
        return;
    }

    std::string error;
    uint32_t rate = track.sample_rate;
    uint32_t channels = track.channels;
    if (track.codec_fourcc == kMp4aFourcc) {
        if (!aac_decoder_.configure(track.extradata.data(), track.extradata.size(), error)) {
            log::addf(log::Level::Warning, "AAC decoder rejected the track config: %s",
                      error.c_str());
            return;
        }
        audio_codec_ = AudioCodec::Aac;
        rate = aac_decoder_.config().sample_rate;
        channels = aac_decoder_.config().channel_count;
        audio_frame_ms_ = rate > 0 ? static_cast<int>(1024ull * 1000 / rate) : 21;
    } else if (track.codec_fourcc == kAc3Fourcc) {
        // AC-3 has no extradata; the frame headers state the format.
        if (!ac3_decoder_.configure(nullptr, 0, error)) {
            log::addf(log::Level::Warning, "AC-3 decoder rejected the track config: %s",
                      error.c_str());
            return;
        }
        audio_codec_ = AudioCodec::Ac3;
        rate = track.sample_rate;
        channels = track.channels;
        audio_frame_ms_ = rate > 0 ? static_cast<int>(1536ull * 1000 / rate) : 32;
    } else if (track.codec_fourcc == kOpusFourcc) {
        if (!opus_decoder_.configure(track.extradata.data(), track.extradata.size(), error)) {
            log::addf(log::Level::Warning, "Opus decoder rejected the track config: %s",
                      error.c_str());
            return;
        }
        // Opus always outputs 48 kHz regardless of the original input rate.
        audio_codec_ = AudioCodec::Opus;
        rate = opus_decoder_.sampleRate();
        channels = opus_decoder_.channels();
        audio_frame_ms_ = 20;
    } else if (track.codec_fourcc == kMp2Fourcc) {
        // Layer II carries no extradata, so there is nothing to configure it
        // with; the frames themselves state the rate and channel count.
        if (!mp2_decoder_.configure(nullptr, 0, error)) {
            log::addf(log::Level::Warning, "MP2 decoder rejected the track config: %s",
                      error.c_str());
            return;
        }
        audio_codec_ = AudioCodec::Mp2;
        rate = track.sample_rate;
        channels = track.channels;
        audio_frame_ms_ = rate > 0 ? static_cast<int>(1152ull * 1000 / rate) : 26;
    } else if (track.codec_fourcc == kMp3Fourcc) {
        if (!mp3_decoder_.configure(track.extradata.data(), track.extradata.size(), error)) {
            log::addf(log::Level::Warning, "MP3 decoder rejected the track config: %s",
                      error.c_str());
            return;
        }
        audio_codec_ = AudioCodec::Mp3;
        rate = track.sample_rate;
        channels = track.channels;
        audio_frame_ms_ = rate > 0 ? static_cast<int>(1152ull * 1000 / rate) : 26;
    } else if (track.codec_fourcc == kFlacFourcc) {
        // Native FLAC carries its own STREAMINFO, so the demuxer hands the
        // first metadata block over as extradata.
        if (!flac_decoder_.configure(track.extradata.data(), track.extradata.size(), error)) {
            log::addf(log::Level::Warning, "FLAC decoder rejected the track config: %s",
                      error.c_str());
            return;
        }
        audio_codec_ = AudioCodec::Flac;
        rate = flac_decoder_.sampleRate();
        channels = flac_decoder_.channels();
        audio_frame_ms_ = rate > 0 ? static_cast<int>(4096ull * 1000 / rate) : 93;
    } else {
        log::addf(log::Level::Warning, "No audio decoder for codec '%s'", track.codec.c_str());
        return;
    }

    audio_decoding_ = true;
    audio_out_rate_ = rate;
    audio_out_channels_ = channels;
    audio_out_fourcc_ = track.codec_fourcc;
    audio::AudioFormat format;
    format.codec_fourcc = track.codec_fourcc;
    format.channels = channels;
    format.sample_rate = rate;
    sink().configure(format);
    resetFilterState();
}

const char* PlaybackEngine::audioCodecName() const {
    switch (audio_codec_) {
        case AudioCodec::Aac:
            return "AAC";
        case AudioCodec::Ac3:
            return "AC-3";
        case AudioCodec::Opus:
            return "Opus";
        case AudioCodec::Mp2:
            return "MP2";
        case AudioCodec::Mp3:
            return "MP3";
        case AudioCodec::Flac:
            return "FLAC";
        case AudioCodec::None:
            break;
    }
    return "no decoder";
}

// Decodes one access unit into audio_pcm_; reports a failure once per track.
void PlaybackEngine::decodeAudioSample(const demux::Sample& sample) {
    std::string error;
    bool ok = false;
    bool unsupported = false;
    switch (audio_codec_) {
        case AudioCodec::Aac: {
            const audio::aac::Status status =
                aac_decoder_.decode(sample.data.data(), sample.data.size(), audio_pcm_, error);
            ok = status == audio::aac::Status::Ok;
            unsupported = status == audio::aac::Status::Unsupported;
            break;
        }
        case AudioCodec::Ac3: {
            const audio::ac3::Status status =
                ac3_decoder_.decode(sample.data.data(), sample.data.size(), audio_pcm_, error);
            ok = status == audio::ac3::Status::Ok;
            unsupported = status == audio::ac3::Status::Unsupported;
            break;
        }
        case AudioCodec::Opus: {
            const audio::opus::Status status =
                opus_decoder_.decode(sample.data.data(), sample.data.size(), audio_pcm_, error);
            ok = status == audio::opus::Status::Ok;
            unsupported = status == audio::opus::Status::Unsupported;
            break;
        }
        case AudioCodec::Mp2: {
            const audio::mp2::Status status =
                mp2_decoder_.decode(sample.data.data(), sample.data.size(), audio_pcm_, error);
            ok = status == audio::mp2::Status::Ok;
            unsupported = status == audio::mp2::Status::Unsupported;
            break;
        }
        case AudioCodec::Mp3: {
            const audio::mp3::Status status =
                mp3_decoder_.decode(sample.data.data(), sample.data.size(), audio_pcm_, error);
            ok = status == audio::mp3::Status::Ok;
            unsupported = status == audio::mp3::Status::Unsupported;
            break;
        }
        case AudioCodec::Flac: {
            const audio::flac::Status status =
                flac_decoder_.decode(sample.data.data(), sample.data.size(), audio_pcm_, error);
            ok = status == audio::flac::Status::Ok;
            unsupported = status == audio::flac::Status::Unsupported;
            break;
        }
        case AudioCodec::None:
            return;
    }
    if (!ok && !audio_warned_) {
        log::addf(log::Level::Warning, "Audio decode (%s): %s",
                  unsupported ? "unsupported" : "invalid", error.c_str());
        audio_warned_ = true;
    }
}

void PlaybackEngine::flushAudioDecoder() {
    aac_decoder_.flush();
    ac3_decoder_.flush();
    opus_decoder_.flush();
    mp2_decoder_.flush();
    mp3_decoder_.flush();
    flac_decoder_.flush();
}

void PlaybackEngine::resetAudioDecoder() {
    aac_decoder_.reset();
    ac3_decoder_.reset();
    opus_decoder_.reset();
    mp2_decoder_.reset();
    mp3_decoder_.reset();
    flac_decoder_.reset();
    audio_codec_ = AudioCodec::None;
}

// Decodes audio ahead of the media clock into the audio buffer.
void PlaybackEngine::pumpAudio() {
    if (!audio_enabled_.load()) {
        return;
    }
    if (!audio_decoding_.load()) {
        audio_eos_ = true;  // nothing to decode for this track
        return;
    }
    const int64_t position = positionMs();
    demux::Sample sample;
    int guard = 0;
    while (guard++ < 64 && audio_buffer_.durationMs() < kAudioBufferMs - audio_frame_ms_) {
        if (!demuxer_->readAudioSample(sample)) {
            audio_eos_ = true;
            break;
        }
        decodeAudioSample(sample);
        if (audio_pcm_.samples.empty()) {
            continue;  // decoder rejected or does not implement this packet
        }
        if (!applyAudioFilters(audio_pcm_)) {
            continue;  // filter failed; the frame is dropped and reported once
        }
        const int64_t duration_ms =
            audio_pcm_.sample_rate > 0
                ? static_cast<int64_t>(audio_pcm_.frames()) * 1000 / audio_pcm_.sample_rate
                : audio_frame_ms_;
        if (sample.pts + duration_ms < position - kAudioLateMs) {
            ++audio_late_drops_;
            continue;  // already late: drop
        }
        audio_pcm_.pts = sample.pts;
        audio_buffer_.push(std::move(audio_pcm_));
    }
}

// Moves buffered PCM into the sink, keeping it around kAudioSinkLeadMs ahead
// of the media clock so audio stays locked to the video. Called from both the
// pipeline and the UI thread.
void PlaybackEngine::feedAudio() {
    std::lock_guard<std::mutex> lock(audio_mutex_);
    if (!audio_enabled_.load()) {
        return;
    }
    const int64_t position = positionMs();

    audio::PcmBuffer chunk;
    while (!audio_buffer_.empty() &&
           audio_buffer_.frontEndPtsMs() < position - kAudioLateMs) {
        audio_buffer_.pop(chunk);  // past its presentation time: drop
    }

    const audio::Mute mute = muted_.load() ? audio::Mute::On : audio::Mute::Off;
    const float gain = audio::volumeGain(volume_.load(), mute);
    while (!audio_buffer_.empty() && sink().queuedMs() < kAudioSinkLeadMs) {
        if (audio_buffer_.frontPtsMs() > position + kAudioSinkLeadMs) {
            break;  // wait until the chunk's time comes
        }
        if (!audio_buffer_.pop(chunk)) {
            break;
        }
        if (gain != 1.0f) {
            audio::applyGain(chunk.samples.data(), chunk.samples.size(), gain);
        }
        sink().push(chunk.samples.data(), chunk.frames());
    }
}

void PlaybackEngine::resetAudioState() {
    std::lock_guard<std::mutex> lock(audio_mutex_);
    audio_buffer_.clear();
    flushAudioDecoder();
    sink().flush();
    audio_eos_ = false;
}

}  // namespace player::playback



