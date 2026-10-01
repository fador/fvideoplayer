#include "playback/PlaybackEngine.hpp"

#include "demux/HvcC.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>

namespace player::playback {
namespace {

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

bool PlaybackEngine::open(const std::filesystem::path& path, std::string& error) {
    close();

    std::string local_error;
    std::unique_ptr<demux::IDemuxer> demuxer = demux::createDemuxer(path, local_error);
    if (!demuxer) {
        last_error_ = local_error;
        error = local_error;
        return false;
    }
    video_info_ = demuxer->video();
    audio_info_ = demuxer->audio();
    duration_ = demuxer->duration();

    if (!decoder_.open(&local_error)) {
        last_error_ = local_error;
        error = local_error;
        return false;
    }
    if (!video_info_.hvcc.empty() &&
        !decoder_.pushParameterSets(video_info_.hvcc, &local_error)) {
        last_error_ = local_error;
        error = local_error;
        decoder_.close();
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
    info.codec = "HEVC";
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
    if (audio_info_.present) {
        audio::AudioFormat format;
        format.codec_fourcc = audio_info_.codec_fourcc;
        format.channels = audio_info_.channels;
        format.sample_rate = audio_info_.sample_rate;
        audio_sink_.configure(format);
    }

    quit_ = false;
    eos_ = false;
    state_ = State::Paused;
    clock_.setPosition(0.0);
    current_.reset();
    current_serial_ = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        seek_pending_ = false;
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
    current_.reset();
    eos_ = false;
    state_ = State::Empty;
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
    cv_.notify_all();
}

void PlaybackEngine::pause() {
    if (!hasFile()) {
        return;
    }
    clock_.pause();
    state_ = State::Paused;
    stats_.setPlaying(false);
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
    if (state_.load() == State::Playing && eos_.load() && queue_.empty()) {
        clock_.pause();
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
        cv_.wait(lock, [this] {
            return quit_.load() || seek_pending_ ||
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
        lock.unlock();

        demux::Sample sample;
        if (!demuxer_->readSample(sample)) {
            std::vector<decode::DecodedFrame> frames;
            decoder_.flush(frames);
            for (decode::DecodedFrame& frame : frames) {
                stats_.onFrameDecoded(frame, 0.0);
                if (!queue_.push(std::move(frame), [this] { return quit_.load(); })) {
                    break;
                }
            }
            eos_ = true;
            stats_.setEos(true);
            stats_.setQueue(queue_.size(), queue_.capacity(), bufferedSeconds());
            continue;
        }

        stats_.onBytesDemuxed(sample.data.size());
        const auto start = std::chrono::steady_clock::now();
        const bool ok =
            decoder_.pushSample(sample.data.data(), sample.data.size(), sample.pts,
                                video_info_.format.annexb, video_info_.format.nal_length_size);
        std::vector<decode::DecodedFrame> frames;
        if (ok) {
            decoder_.pullFrames(frames);
        }
        const double elapsed_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        const double per_frame_ms =
            frames.empty() ? elapsed_ms : elapsed_ms / static_cast<double>(frames.size());
        for (decode::DecodedFrame& frame : frames) {
            frame.sample_bytes = sample.data.size();
            stats_.onFrameDecoded(frame, per_frame_ms);
            if (!queue_.push(std::move(frame), [this] { return quit_.load(); })) {
                break;
            }
        }

        // Dummy audio path: consume packets if a demuxer provides them.
        demux::Sample audio_sample;
        int guard = 0;
        while (guard++ < 32 && demuxer_->readAudioSample(audio_sample)) {
            audio_sink_.push(audio_sample);
        }

        stats_.setQueue(queue_.size(), queue_.capacity(), bufferedSeconds());
    }
}

void PlaybackEngine::performSeek(double seconds) {
    const auto start = std::chrono::steady_clock::now();
    queue_.clear();
    decoder_.close();
    std::string error;
    decoder_.open(&error);
    if (!video_info_.hvcc.empty()) {
        decoder_.pushParameterSets(video_info_.hvcc, nullptr);
    }
    demuxer_->seek(seconds);
    audio_sink_.reset();
    eos_ = false;
    stats_.setEos(false);
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    stats_.onSeek(elapsed_ms);
}

}  // namespace player::playback
