#include "demux/Demuxer.hpp"
#include "demux/Mp4Demuxer.hpp"
#include "subtitle/Subtitle.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

constexpr uint32_t kFourcc(const char* tag) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(tag[0])) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(tag[1])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(tag[2])) << 8) |
           static_cast<uint32_t>(static_cast<uint8_t>(tag[3]));
}

void putU16(Bytes& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void putU32(Bytes& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void putU64(Bytes& out, uint64_t value) {
    putU32(out, static_cast<uint32_t>(value >> 32));
    putU32(out, static_cast<uint32_t>(value & 0xFFFFFFFFu));
}

void append(Bytes& out, const Bytes& data) {
    out.insert(out.end(), data.begin(), data.end());
}

Bytes box(uint32_t type, const Bytes& payload) {
    Bytes out;
    putU32(out, static_cast<uint32_t>(payload.size() + 8));
    putU32(out, type);
    append(out, payload);
    return out;
}

Bytes fullBox(uint32_t type, uint8_t version, uint32_t flags, const Bytes& payload) {
    Bytes body;
    body.push_back(version);
    body.push_back(static_cast<uint8_t>((flags >> 16) & 0xFF));
    body.push_back(static_cast<uint8_t>((flags >> 8) & 0xFF));
    body.push_back(static_cast<uint8_t>(flags & 0xFF));
    append(body, payload);
    return box(type, body);
}

Bytes makeHvcc() {
    Bytes out;
    out.push_back(1);        // configurationVersion
    out.push_back(2);        // Main 10
    putU32(out, 0x60000000);  // compatibility flags
    for (int i = 0; i < 6; ++i) {
        out.push_back(0);
    }
    out.push_back(120);      // level 4.0
    putU16(out, 0xF000);     // min_spatial_segmentation_idc
    out.push_back(0xFC);     // parallelismType
    out.push_back(0xFD);     // chroma_format_idc 1
    out.push_back(0xFA);     // bit_depth_luma_minus8 = 2
    out.push_back(0xF8);
    putU16(out, 0);          // avgFrameRate
    out.push_back(0x0F);     // temporalIdNested, lengthSizeMinusOne 3
    out.push_back(3);        // numOfArrays
    const uint8_t types[3] = {32, 33, 34};
    const Bytes nals[3] = {{0x40, 0x01, 0x0C}, {0x42, 0x01, 0x02}, {0x44, 0x01, 0x03}};
    for (int i = 0; i < 3; ++i) {
        out.push_back(static_cast<uint8_t>(0x80 | types[i]));
        putU16(out, 1);
        putU16(out, static_cast<uint16_t>(nals[i].size()));
        append(out, nals[i]);
    }
    return out;
}

Bytes visualSampleEntry() {
    Bytes payload;
    for (int i = 0; i < 6; ++i) {
        payload.push_back(0);
    }
    putU16(payload, 1);  // data_reference_index
    for (int i = 0; i < 16; ++i) {
        payload.push_back(0);
    }
    putU16(payload, 1280);
    putU16(payload, 720);
    putU32(payload, 0x00480000);
    putU32(payload, 0x00480000);
    putU32(payload, 0);
    putU16(payload, 1);  // frame count
    for (int i = 0; i < 32; ++i) {
        payload.push_back(0);  // compressorname
    }
    putU16(payload, 0x0018);
    putU16(payload, 0xFFFF);
    append(payload, box(kFourcc("hvcC"), makeHvcc()));
    return box(kFourcc("hvc1"), payload);
}

// AudioSampleEntry fixed fields, shared by every audio entry below.
void putAudioFields(Bytes& payload, uint16_t channels, uint32_t sample_rate) {
    for (int i = 0; i < 6; ++i) {
        payload.push_back(0);
    }
    putU16(payload, 1);  // data_reference_index
    putU16(payload, 0);  // version
    putU16(payload, 0);  // revision
    putU32(payload, 0);  // vendor
    putU16(payload, channels);
    putU16(payload, 16);  // sample size
    putU16(payload, 0);
    putU16(payload, 0);
    putU32(payload, sample_rate << 16);
}

// MPEG-4 descriptor. Only the leading length bytes carry the continuation
// bit; the last one terminates the value.
Bytes descriptor(uint8_t tag, const Bytes& payload) {
    Bytes out;
    out.push_back(tag);
    out.push_back(static_cast<uint8_t>(0x80 | ((payload.size() >> 16) & 0x7F)));
    out.push_back(static_cast<uint8_t>(0x80 | ((payload.size() >> 8) & 0x7F)));
    out.push_back(static_cast<uint8_t>(payload.size() & 0x7F));
    append(out, payload);
    return out;
}

// esds carrying an AAC AudioSpecificConfig, the way ffmpeg writes it.
Bytes esdsBox(const Bytes& asc) {
    Bytes config;
    config.push_back(0x40);  // object type: MPEG-4 audio
    config.push_back(0x15);  // stream type audio
    config.push_back(0);
    config.push_back(0);
    config.push_back(0);
    putU32(config, 128000);  // max bitrate
    putU32(config, 128000);  // average bitrate
    append(config, descriptor(0x05, asc));
    const Bytes decoder_config = descriptor(0x04, config);

    Bytes es;
    putU16(es, 2);  // ES_ID
    es.push_back(0);  // flags
    append(es, decoder_config);
    es.push_back(0x06);  // SLConfigDescriptor
    es.push_back(0x02);
    return fullBox(kFourcc("esds"), 0, 0, descriptor(0x03, es));
}

Bytes mp4aEntry(uint16_t channels, uint32_t sample_rate, const Bytes& asc) {
    Bytes payload;
    putAudioFields(payload, channels, sample_rate);
    append(payload, esdsBox(asc));
    return box(kFourcc("mp4a"), payload);
}

// dOps holds the OpusHead fields big endian and carries no magic.
Bytes opusEntry(uint16_t channels, uint16_t pre_skip, uint32_t input_rate) {
    Bytes dops;
    dops.push_back(0);  // version
    dops.push_back(static_cast<uint8_t>(channels));
    putU16(dops, pre_skip);
    putU32(dops, input_rate);
    putU16(dops, 0);  // output gain
    dops.push_back(0);  // mapping family

    Bytes payload;
    putAudioFields(payload, channels, 48000);
    append(payload, box(kFourcc("dOps"), dops));
    return box(kFourcc("Opus"), payload);
}

// tx3g timed text entry. The text itself lives in the samples, not here.
Bytes tx3gEntry() {
    Bytes payload;
    putU32(payload, 0);       // displayFlags
    payload.push_back(1);     // horizontal justification: centered
    payload.push_back(0xFF);  // vertical justification: bottom
    for (int i = 0; i < 4; ++i) {
        payload.push_back(0);  // background colour
    }
    for (int i = 0; i < 8; ++i) {
        payload.push_back(0);  // default text box
    }
    for (int i = 0; i < 8; ++i) {
        payload.push_back(0);  // reserved
    }
    putU16(payload, 1);  // font id
    payload.push_back(0);  // face style
    payload.push_back(0);  // reserved
    for (int i = 0; i < 4; ++i) {
        payload.push_back(0xFF);  // text colour
    }
    return box(kFourcc("tx3g"), payload);
}

uint16_t isoLanguageCode(char a, char b, char c) {
    return static_cast<uint16_t>((static_cast<uint16_t>(a - 0x60) << 10) |
                                 (static_cast<uint16_t>(b - 0x60) << 5) |
                                 static_cast<uint16_t>(c - 0x60));
}

Bytes tkhd(uint32_t track_id, uint32_t duration, uint32_t width, uint32_t height) {
    Bytes payload;
    putU32(payload, 0);          // creation
    putU32(payload, 0);          // modification
    putU32(payload, track_id);
    putU32(payload, 0);          // reserved
    putU32(payload, duration);
    for (int i = 0; i < 8; ++i) {
        payload.push_back(0);  // reserved
    }
    putU16(payload, 0);  // layer
    putU16(payload, 0);  // alternate_group
    putU16(payload, 0);  // volume
    putU16(payload, 0);  // reserved
    const uint32_t matrix[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};
    for (uint32_t value : matrix) {
        putU32(payload, value);
    }
    putU32(payload, width << 16);
    putU32(payload, height << 16);
    return fullBox(kFourcc("tkhd"), 0, 0x000007, payload);
}

Bytes mdhdLanguage(uint32_t timescale, uint32_t duration, uint16_t language) {
    Bytes payload;
    putU32(payload, 0);
    putU32(payload, 0);
    putU32(payload, timescale);
    putU32(payload, duration);
    putU16(payload, language);
    putU16(payload, 0);
    return fullBox(kFourcc("mdhd"), 0, 0, payload);
}

Bytes mdhd(uint32_t timescale, uint32_t duration) {
    return mdhdLanguage(timescale, duration, 0x55C4);  // "und"
}

Bytes hdlr(const char* handler) {
    Bytes payload;
    putU32(payload, 0);  // pre_defined
    putU32(payload, kFourcc(handler));
    for (int i = 0; i < 12; ++i) {
        payload.push_back(0);
    }
    payload.push_back(0);
    return fullBox(kFourcc("hdlr"), 0, 0, payload);
}

struct Mp4Spec {
    std::vector<uint32_t> sample_sizes;
    std::vector<size_t> keyframes;  // 0-based
    std::vector<int64_t> ctts;      // empty for no ctts box
    uint32_t timescale = 30000;
    uint32_t frame_delta = 1000;
    bool with_audio = false;
    bool with_opus = false;
    bool with_subtitles = false;
};

// AudioSpecificConfig for AAC-LC: 48 kHz, mono, no extension.
const Bytes& aacConfig() {
    static const Bytes config = {0x11, 0x88, 0x56, 0xE5, 0x00};
    return config;
}

// One tx3g sample: a two byte length followed by the text.
Bytes textSample(const std::string& text) {
    Bytes sample;
    putU16(sample, static_cast<uint16_t>(text.size()));
    sample.insert(sample.end(), text.begin(), text.end());
    return sample;
}

void appendPackets(Bytes& out, const std::vector<Bytes>& packets) {
    for (const Bytes& packet : packets) {
        append(out, packet);
    }
}

std::vector<uint32_t> sizesOf(const std::vector<Bytes>& packets) {
    std::vector<uint32_t> sizes;
    sizes.reserve(packets.size());
    for (const Bytes& packet : packets) {
        sizes.push_back(static_cast<uint32_t>(packet.size()));
    }
    return sizes;
}

Bytes videoTrack(const Mp4Spec& spec, uint64_t first_sample_offset, uint32_t duration) {
    Bytes stbl_children;

    Bytes stsd_payload;
    putU32(stsd_payload, 1);  // entry count
    append(stsd_payload, visualSampleEntry());
    append(stbl_children, fullBox(kFourcc("stsd"), 0, 0, stsd_payload));

    Bytes stts_payload;
    putU32(stts_payload, 1);
    putU32(stts_payload, static_cast<uint32_t>(spec.sample_sizes.size()));
    putU32(stts_payload, spec.frame_delta);
    append(stbl_children, fullBox(kFourcc("stts"), 0, 0, stts_payload));

    if (!spec.ctts.empty()) {
        Bytes ctts_payload;
        putU32(ctts_payload, static_cast<uint32_t>(spec.ctts.size()));
        for (int64_t offset : spec.ctts) {
            putU32(ctts_payload, 1);
            putU32(ctts_payload, static_cast<uint32_t>(offset));
        }
        append(stbl_children, fullBox(kFourcc("ctts"), 0, 0, ctts_payload));
    }

    Bytes stss_payload;
    putU32(stss_payload, static_cast<uint32_t>(spec.keyframes.size()));
    for (size_t index : spec.keyframes) {
        putU32(stss_payload, static_cast<uint32_t>(index + 1));
    }
    append(stbl_children, fullBox(kFourcc("stss"), 0, 0, stss_payload));

    Bytes stsz_payload;
    putU32(stsz_payload, 0);
    putU32(stsz_payload, static_cast<uint32_t>(spec.sample_sizes.size()));
    for (uint32_t size : spec.sample_sizes) {
        putU32(stsz_payload, size);
    }
    append(stbl_children, fullBox(kFourcc("stsz"), 0, 0, stsz_payload));

    Bytes stsc_payload;
    putU32(stsc_payload, 1);
    putU32(stsc_payload, 1);
    putU32(stsc_payload, static_cast<uint32_t>(spec.sample_sizes.size()));
    putU32(stsc_payload, 1);
    append(stbl_children, fullBox(kFourcc("stsc"), 0, 0, stsc_payload));

    Bytes stco_payload;
    putU32(stco_payload, 1);
    putU32(stco_payload, static_cast<uint32_t>(first_sample_offset));
    append(stbl_children, fullBox(kFourcc("stco"), 0, 0, stco_payload));

    const Bytes minf = box(kFourcc("minf"), box(kFourcc("stbl"), stbl_children));

    Bytes mdia_children;
    append(mdia_children, mdhd(spec.timescale, duration));
    append(mdia_children, hdlr("vide"));
    append(mdia_children, minf);

    Bytes trak_children;
    append(trak_children, tkhd(1, duration, 1280, 720));
    append(trak_children, box(kFourcc("mdia"), mdia_children));
    return box(kFourcc("trak"), trak_children);
}

// One media track stored as a single chunk: stts, stsz, stsc and stco cover it.
Bytes mediaTrack(uint32_t track_id, const char* handler, uint32_t timescale, uint32_t duration,
                 const Bytes& sample_entry, const std::vector<uint32_t>& sample_sizes,
                 uint32_t sample_delta, uint64_t first_sample_offset, uint16_t language) {
    Bytes stbl_children;
    Bytes stsd_payload;
    putU32(stsd_payload, 1);
    append(stsd_payload, sample_entry);
    append(stbl_children, fullBox(kFourcc("stsd"), 0, 0, stsd_payload));

    Bytes stts_payload;
    putU32(stts_payload, 1);
    putU32(stts_payload, static_cast<uint32_t>(sample_sizes.size()));
    putU32(stts_payload, sample_delta);
    append(stbl_children, fullBox(kFourcc("stts"), 0, 0, stts_payload));

    Bytes stsz_payload;
    putU32(stsz_payload, 0);
    putU32(stsz_payload, static_cast<uint32_t>(sample_sizes.size()));
    for (uint32_t size : sample_sizes) {
        putU32(stsz_payload, size);
    }
    append(stbl_children, fullBox(kFourcc("stsz"), 0, 0, stsz_payload));

    Bytes stsc_payload;
    putU32(stsc_payload, 1);
    putU32(stsc_payload, 1);
    putU32(stsc_payload, static_cast<uint32_t>(sample_sizes.size()));
    putU32(stsc_payload, 1);
    append(stbl_children, fullBox(kFourcc("stsc"), 0, 0, stsc_payload));

    Bytes stco_payload;
    putU32(stco_payload, 1);
    putU32(stco_payload, static_cast<uint32_t>(first_sample_offset));
    append(stbl_children, fullBox(kFourcc("stco"), 0, 0, stco_payload));

    const Bytes minf = box(kFourcc("minf"), box(kFourcc("stbl"), stbl_children));
    Bytes mdia_children;
    append(mdia_children, mdhdLanguage(timescale, duration, language));
    append(mdia_children, hdlr(handler));
    append(mdia_children, minf);

    Bytes trak_children;
    append(trak_children, tkhd(track_id, duration, 0, 0));
    append(trak_children, box(kFourcc("mdia"), mdia_children));
    return box(kFourcc("trak"), trak_children);
}

Bytes mvhd(uint32_t timescale, uint32_t duration) {
    Bytes payload;
    putU32(payload, 0);  // creation
    putU32(payload, 0);  // modification
    putU32(payload, timescale);
    putU32(payload, duration);
    putU32(payload, 0x00010000);  // rate
    putU16(payload, 0x0100);      // volume
    putU16(payload, 0);           // reserved
    for (int i = 0; i < 8; ++i) {
        payload.push_back(0);
    }
    const uint32_t matrix[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};
    for (uint32_t value : matrix) {
        putU32(payload, value);
    }
    for (int i = 0; i < 6; ++i) {
        putU32(payload, 0);
    }
    putU32(payload, 3);  // next_track_id
    return fullBox(kFourcc("mvhd"), 0, 0, payload);
}

Bytes buildMp4(const Mp4Spec& spec) {
    const uint32_t duration =
        static_cast<uint32_t>(spec.sample_sizes.size()) * spec.frame_delta;

    Bytes sample_data;
    for (size_t i = 0; i < spec.sample_sizes.size(); ++i) {
        const uint32_t size = spec.sample_sizes[i];
        Bytes sample(size, 0);
        const uint32_t nal_size = size >= 4 ? size - 4 : 0;
        sample[0] = static_cast<uint8_t>(nal_size >> 24);
        sample[1] = static_cast<uint8_t>((nal_size >> 16) & 0xFF);
        sample[2] = static_cast<uint8_t>((nal_size >> 8) & 0xFF);
        sample[3] = static_cast<uint8_t>(nal_size & 0xFF);
        sample[4] = 0x02;  // NAL header
        if (size > 5) {
            sample[5] = 0x01;
        }
        for (size_t k = 6; k < size; ++k) {
            sample[k] = static_cast<uint8_t>(0xAA + i);
        }
        append(sample_data, sample);
    }

    Bytes ftyp_payload;
    putU32(ftyp_payload, kFourcc("isom"));
    putU32(ftyp_payload, 0);
    putU32(ftyp_payload, kFourcc("isom"));
    putU32(ftyp_payload, kFourcc("iso2"));
    const Bytes ftyp = box(kFourcc("ftyp"), ftyp_payload);

    // Audio and subtitle payloads follow the video samples in the same mdat,
    // so their chunk offsets have to account for everything before them.
    const std::vector<Bytes> audio_packets = {Bytes(4, 0xA1), Bytes(4, 0xA2), Bytes(4, 0xA3)};
    const std::vector<Bytes> opus_packets = {Bytes(3, 0xB1), Bytes(3, 0xB2)};
    const std::vector<Bytes> subtitle_packets = {textSample("First cue"), textSample("Second cue")};

    uint64_t cursor = ftyp.size() + 8 + sample_data.size();
    uint64_t audio_offset = cursor;
    uint64_t opus_offset = cursor;
    uint64_t subtitle_offset = cursor;
    if (spec.with_audio) {
        for (const Bytes& packet : audio_packets) {
            cursor += packet.size();
        }
    }
    if (spec.with_opus) {
        opus_offset = cursor;
        for (const Bytes& packet : opus_packets) {
            cursor += packet.size();
        }
    }
    if (spec.with_subtitles) {
        subtitle_offset = cursor;
        for (const Bytes& packet : subtitle_packets) {
            cursor += packet.size();
        }
    }
    if (spec.with_audio) {
        appendPackets(sample_data, audio_packets);
    }
    if (spec.with_opus) {
        appendPackets(sample_data, opus_packets);
    }
    if (spec.with_subtitles) {
        appendPackets(sample_data, subtitle_packets);
    }

    const Bytes mdat = box(kFourcc("mdat"), sample_data);
    const uint64_t first_sample_offset = ftyp.size() + 8;

    Bytes moov_children;
    append(moov_children, mvhd(1000, duration / 30 + 1));
    append(moov_children, videoTrack(spec, first_sample_offset, duration));
    if (spec.with_audio) {
        append(moov_children,
               mediaTrack(2, "soun", 48000, 48000, mp4aEntry(1, 48000, aacConfig()),
                          sizesOf(audio_packets), 1024, audio_offset,
                          isoLanguageCode('e', 'n', 'g')));
    }
    if (spec.with_opus) {
        append(moov_children,
               mediaTrack(3, "soun", 48000, 48000, opusEntry(1, 312, 48000), sizesOf(opus_packets),
                          960, opus_offset, isoLanguageCode('j', 'p', 'n')));
    }
    if (spec.with_subtitles) {
        append(moov_children,
               mediaTrack(4, "sbtl", 1000, 3000, tx3gEntry(), sizesOf(subtitle_packets), 1500,
                          subtitle_offset, isoLanguageCode('e', 'n', 'g')));
    }
    const Bytes moov = box(kFourcc("moov"), moov_children);

    Bytes file;
    append(file, ftyp);
    append(file, mdat);
    append(file, moov);
    return file;
}

std::filesystem::path writeMp4(const Bytes& data, const std::string& name) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream file(path, std::ios::binary, std::ios::trunc);
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return path;
}

Mp4Spec simpleSpec() {
    Mp4Spec spec;
    spec.sample_sizes = {7, 6, 6, 6};
    spec.keyframes = {0, 2};
    spec.with_audio = true;
    return spec;
}

}  // namespace

TEST(Mp4Demuxer, ParsesVideoAndAudioTrack) {
    const std::filesystem::path path = writeMp4(buildMp4(simpleSpec()), "hevc_player_mp4_basic.mp4");
    {
        player::demux::Mp4Demuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;

        EXPECT_EQ(demuxer.video().codec, player::demux::Codec::Hevc);
        EXPECT_EQ(demuxer.video().width, 1280u);
        EXPECT_EQ(demuxer.video().height, 720u);
        EXPECT_EQ(demuxer.video().timescale, 30000u);
        EXPECT_FALSE(demuxer.video().format.annexb);
        EXPECT_EQ(demuxer.video().format.nal_length_size, 4);
        EXPECT_FALSE(demuxer.video().hvcc.empty());
        EXPECT_DOUBLE_EQ(demuxer.video().fps, 30.0);
        EXPECT_NEAR(demuxer.duration(), 4000.0 / 30000.0, 1e-9);

        EXPECT_TRUE(demuxer.audio().present);
        EXPECT_EQ(demuxer.audio().codec_fourcc, kFourcc("mp4a"));
        EXPECT_EQ(demuxer.audio().channels, 1u);
        EXPECT_EQ(demuxer.audio().sample_rate, 48000u);
        EXPECT_EQ(demuxer.audio().language, "eng");
    }
    std::filesystem::remove(path);
}

TEST(Mp4Demuxer, ReadsSamplesWithPtsAndKeyframes) {
    const std::filesystem::path path = writeMp4(buildMp4(simpleSpec()), "hevc_player_mp4_samples.mp4");
    {
        player::demux::Mp4Demuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;

        player::demux::Sample sample;
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_EQ(sample.pts, 0);
        EXPECT_TRUE(sample.keyframe);
        EXPECT_EQ(sample.data.size(), 7u);
        EXPECT_EQ(sample.data[3], 3u);
        EXPECT_EQ(sample.data[4], 0x02u);

        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_EQ(sample.pts, 1000);
        EXPECT_FALSE(sample.keyframe);

        while (demuxer.readSample(sample)) {
        }
        EXPECT_FALSE(demuxer.readSample(sample));
    }
    std::filesystem::remove(path);
}

TEST(Mp4Demuxer, SeeksToPreviousKeyframe) {
    const std::filesystem::path path = writeMp4(buildMp4(simpleSpec()), "hevc_player_mp4_seek.mp4");
    {
        player::demux::Mp4Demuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;

        ASSERT_TRUE(demuxer.seek(0.08));  // 80 ms -> previous keyframe at 66 ms
        player::demux::Sample sample;
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_EQ(sample.pts, 2000);
        EXPECT_TRUE(sample.keyframe);

        ASSERT_TRUE(demuxer.seek(0.0));
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_EQ(sample.pts, 0);
    }
    std::filesystem::remove(path);
}

TEST(Mp4Demuxer, AppliesCompositionOffsets) {
    Mp4Spec spec;
    spec.sample_sizes = {6, 6, 6};
    spec.keyframes = {0};
    spec.ctts = {2000, 0, 1000};

    const std::filesystem::path path = writeMp4(buildMp4(spec), "hevc_player_mp4_ctts.mp4");
    {
        player::demux::Mp4Demuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;

        player::demux::Sample sample;
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_EQ(sample.dts, 0);
        EXPECT_EQ(sample.pts, 2000);
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_EQ(sample.pts, 1000);
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_EQ(sample.pts, 3000);
    }
    std::filesystem::remove(path);
}

TEST(Mp4Demuxer, ReadsAudioSamplesWithEsdsConfig) {
    const std::filesystem::path path =
        writeMp4(buildMp4(simpleSpec()), "hevc_player_mp4_audio.mp4");
    {
        player::demux::Mp4Demuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;

        const std::vector<player::demux::AudioTrackInfo> tracks = demuxer.audioTracks();
        ASSERT_EQ(tracks.size(), 1u);
        EXPECT_EQ(tracks[0].codec, "mp4a");
        EXPECT_EQ(tracks[0].codec_fourcc, kFourcc("mp4a"));
        EXPECT_EQ(tracks[0].channels, 1u);
        EXPECT_EQ(tracks[0].sample_rate, 48000u);
        EXPECT_TRUE(tracks[0].default_track);
        // The decoder takes the AudioSpecificConfig out of the esds descriptor.
        EXPECT_EQ(tracks[0].extradata, aacConfig());

        player::demux::Sample sample;
        ASSERT_TRUE(demuxer.readAudioSample(sample));
        EXPECT_EQ(sample.pts, 0);
        EXPECT_EQ(sample.track, player::demux::TrackType::Audio);
        ASSERT_EQ(sample.data.size(), 4u);
        EXPECT_EQ(sample.data[0], 0xA1u);
        ASSERT_TRUE(demuxer.readAudioSample(sample));
        EXPECT_EQ(sample.pts, 21);  // 1024 samples at 48 kHz
        EXPECT_EQ(sample.data[0], 0xA2u);
        ASSERT_TRUE(demuxer.readAudioSample(sample));
        EXPECT_EQ(sample.pts, 42);
        EXPECT_EQ(sample.data[0], 0xA3u);
        EXPECT_FALSE(demuxer.readAudioSample(sample));
    }
    std::filesystem::remove(path);
}

TEST(Mp4Demuxer, BuildsOpusHeadFromDops) {
    Mp4Spec spec;
    spec.sample_sizes = {6, 6};
    spec.keyframes = {0};
    spec.with_audio = true;
    spec.with_opus = true;
    const std::filesystem::path path = writeMp4(buildMp4(spec), "hevc_player_mp4_opus.mp4");
    {
        player::demux::Mp4Demuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;

        const std::vector<player::demux::AudioTrackInfo> tracks = demuxer.audioTracks();
        ASSERT_EQ(tracks.size(), 2u);
        EXPECT_EQ(tracks[0].language, "eng");
        EXPECT_EQ(tracks[1].language, "jpn");
        EXPECT_EQ(tracks[1].codec, "Opus");
        EXPECT_EQ(tracks[1].codec_fourcc, kFourcc("Opus"));
        EXPECT_FALSE(tracks[1].default_track);

        // dOps is big endian while OpusHead is little endian, so the fields
        // have to be swapped. Reading them verbatim turned a pre-skip of 312
        // into 14337 and dropped the first quarter second of audio.
        const std::vector<uint8_t>& head = tracks[1].extradata;
        ASSERT_EQ(head.size(), 19u);
        EXPECT_EQ(std::string(head.begin(), head.begin() + 8), "OpusHead");
        EXPECT_EQ(head[8], 0u);      // version
        EXPECT_EQ(head[9], 1u);      // channels
        EXPECT_EQ(head[10], 0x38u);  // pre-skip 312
        EXPECT_EQ(head[11], 0x01u);
        EXPECT_EQ(head[12], 0x80u);  // input sample rate 48000
        EXPECT_EQ(head[13], 0xBBu);
        EXPECT_EQ(head[14], 0x00u);
        EXPECT_EQ(head[15], 0x00u);

        ASSERT_TRUE(demuxer.selectAudioTrack(1));
        player::demux::Sample sample;
        ASSERT_TRUE(demuxer.readAudioSample(sample));
        EXPECT_EQ(sample.data.size(), 3u);
        EXPECT_EQ(sample.data[0], 0xB1u);
        EXPECT_EQ(sample.pts, 0);
        EXPECT_FALSE(demuxer.selectAudioTrack(2));
    }
    std::filesystem::remove(path);
}

TEST(Mp4Demuxer, ReadsTimedTextSubtitleTrack) {
    Mp4Spec spec;
    spec.sample_sizes = {6, 6};
    spec.keyframes = {0};
    spec.with_subtitles = true;
    const std::filesystem::path path = writeMp4(buildMp4(spec), "hevc_player_mp4_subs.mp4");
    {
        player::demux::Mp4Demuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;

        const std::vector<player::demux::SubtitleTrackInfo> tracks = demuxer.subtitleTracks();
        ASSERT_EQ(tracks.size(), 1u);
        EXPECT_EQ(tracks[0].codec, "tx3g");
        EXPECT_EQ(tracks[0].language, "eng");
        EXPECT_FALSE(tracks[0].bitmap);

        std::vector<player::subtitle::SubtitleBlock> blocks;
        ASSERT_TRUE(demuxer.readSubtitleTrack(0, blocks, error)) << error;
        ASSERT_EQ(blocks.size(), 2u);
        EXPECT_EQ(blocks[0].pts_ms, 0);
        EXPECT_EQ(blocks[0].duration_ms, 1500);
        EXPECT_EQ(blocks[1].pts_ms, 1500);

        std::vector<player::subtitle::Cue> cues;
        ASSERT_TRUE(
            player::subtitle::parseEmbeddedTrack(tracks[0].codec, "", blocks, cues, &error))
            << error;
        ASSERT_EQ(cues.size(), 2u);
        EXPECT_EQ(cues[0].text, "First cue");
        EXPECT_DOUBLE_EQ(cues[0].start, 0.0);
        EXPECT_DOUBLE_EQ(cues[0].end, 1.5);
        EXPECT_EQ(cues[1].text, "Second cue");
        EXPECT_DOUBLE_EQ(cues[1].start, 1.5);
        EXPECT_DOUBLE_EQ(cues[1].end, 3.0);

        EXPECT_FALSE(demuxer.readSubtitleTrack(1, blocks, error));
    }
    std::filesystem::remove(path);
}

TEST(Mp4Demuxer, SeekRepositionsAudioCursor) {
    const std::filesystem::path path =
        writeMp4(buildMp4(simpleSpec()), "hevc_player_mp4_seek_audio.mp4");
    {
        player::demux::Mp4Demuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;

        player::demux::Sample sample;
        while (demuxer.readAudioSample(sample)) {
        }
        ASSERT_FALSE(demuxer.readAudioSample(sample));

        // A backward seek has to rewind the audio cursor too, otherwise the
        // player runs dry and stays silent until the track is picked again.
        ASSERT_TRUE(demuxer.seek(0.0));
        ASSERT_TRUE(demuxer.readAudioSample(sample));
        EXPECT_EQ(sample.pts, 0);
        while (demuxer.readAudioSample(sample)) {
        }

        // 30 ms falls inside the second packet, which starts at 21 ms.
        ASSERT_TRUE(demuxer.seek(0.03));
        ASSERT_TRUE(demuxer.readAudioSample(sample));
        EXPECT_EQ(sample.pts, 21);
    }
    std::filesystem::remove(path);
}

TEST(Mp4Demuxer, FactoryDetectsMp4) {
    const std::filesystem::path path = writeMp4(buildMp4(simpleSpec()), "hevc_player_mp4_factory.mp4");
    {
        std::string error;
        std::unique_ptr<player::demux::IDemuxer> demuxer = player::demux::createDemuxer(path, error);
        ASSERT_TRUE(demuxer) << error;
        EXPECT_EQ(demuxer->video().codec, player::demux::Codec::Hevc);
        EXPECT_EQ(demuxer->video().width, 1280u);
    }
    std::filesystem::remove(path);
}
