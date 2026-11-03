// addu94 — tests/test_sampler.cpp
//
// WAV file, Sampler and DrumKit tests (spec §4.1/§4.3). The chunked render
// helper mirrors tests/test_synth.cpp's render_for, including the tail
// partial-chunk fix: the last iteration renders `frames - off` frames, not
// a full 512.

#include "test_framework.hpp"

#include "audio/drum_kit.hpp"
#include "audio/sampler.hpp"
#include "audio/wav_file.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

using addu94::audio::DrumKit;
using addu94::audio::Sampler;
using addu94::audio::SamplerParams;
using addu94::audio::WavData;

namespace {

constexpr double kSr = 48000.0;
constexpr double kTwoPi = 6.283185307179586476925286766559;
std::string const kTmpDir = "/tmp/addu94_sampler_test";

// ---------------------------------------------------------------- helpers

struct Rendered {
    std::vector<float> left, right;
    float peak{0.f};
};

template <typename Source>
Rendered render_for(Source& s, double seconds) {
    Rendered r;
    auto const frames = static_cast<std::uint32_t>(kSr * seconds);
    r.left.assign(frames, 0.f);
    r.right.assign(frames, 0.f);
    for (std::uint32_t off = 0; off < frames; off += 512) {
        // Tail partial chunk: never render past the end of the buffer.
        std::uint32_t const n =
            (frames - off < 512) ? frames - off : 512;
        s.render(r.left.data() + off, r.right.data() + off, n);
    }
    for (float v : r.left) r.peak = std::max(r.peak, std::abs(v));
    return r;
}

double energy(Rendered const& r) {
    double e = 0.0;
    for (float v : r.left) e += static_cast<double>(v) * static_cast<double>(v);
    return e;
}

bool write_bytes(std::string const& path,
                 std::vector<std::uint8_t> const& bytes) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    bool ok = true;
    if (!bytes.empty()) {
        ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    }
    return std::fclose(f) == 0 && ok;
}

void push_tag(std::vector<std::uint8_t>& v, char const (&tag)[5]) {
    for (std::size_t i = 0; i < 4; ++i) {
        v.push_back(static_cast<std::uint8_t>(tag[i]));
    }
}

void push_u16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>(x & 0xFFu));
    v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFFu));
}

void push_u32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(static_cast<std::uint8_t>(x & 0xFFu));
    v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFFu));
    v.push_back(static_cast<std::uint8_t>((x >> 16) & 0xFFu));
    v.push_back(static_cast<std::uint8_t>((x >> 24) & 0xFFu));
}

/// Hand-built little-endian WAV byte image (for malformed-input tests).
std::vector<std::uint8_t> make_wav_bytes(std::uint16_t fmt_tag,
                                         std::uint16_t channels,
                                         std::uint32_t rate,
                                         std::uint16_t bits,
                                         std::vector<std::uint8_t> payload) {
    std::uint32_t const data_size = static_cast<std::uint32_t>(payload.size());
    std::vector<std::uint8_t> b;
    push_tag(b, "RIFF");
    push_u32(b, 4 + 8 + 16 + 8 + data_size);
    push_tag(b, "WAVE");
    push_tag(b, "fmt ");
    push_u32(b, 16);
    push_u16(b, fmt_tag);
    push_u16(b, channels);
    push_u32(b, rate);
    std::uint32_t const bytes_per_frame =
        static_cast<std::uint32_t>(channels) * (static_cast<std::uint32_t>(bits) / 8u);
    push_u32(b, rate * bytes_per_frame);
    push_u16(b, static_cast<std::uint16_t>(bytes_per_frame));
    push_u16(b, bits);
    push_tag(b, "data");
    push_u32(b, data_size);
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}

/// 0.5 s sine at `hz`, saved as 32-bit float WAV.
bool write_sine_wav(std::string const& path, double hz, int channels,
                    double amp = 0.9) {
    WavData w;
    w.sample_rate = static_cast<std::uint32_t>(kSr);
    w.channels = static_cast<std::uint16_t>(channels);
    std::size_t const frames = static_cast<std::size_t>(kSr * 0.5);
    w.samples.resize(frames * static_cast<std::size_t>(channels));
    for (std::size_t i = 0; i < frames; ++i) {
        double const t = static_cast<double>(i) / kSr;
        float const v =
            static_cast<float>(amp * std::sin(kTwoPi * hz * t));
        w.samples[i * static_cast<std::size_t>(channels)] = v;
        if (channels == 2) {
            float const r = static_cast<float>(
                amp * std::sin(kTwoPi * (hz * 2.0) * t));
            w.samples[i * 2 + 1] = r;
        }
    }
    std::string err;
    return addu94::audio::wav_save(path, w, err);
}

/// Frames with |x| > threshold, from the first note_on until silence.
std::uint32_t audible_frames(std::vector<float> const& buf, float threshold) {
    std::uint32_t n = 0;
    for (float v : buf) {
        if (std::abs(v) > threshold) ++n;
    }
    return n;
}

bool ensure_tmp_dir() {
    std::error_code ec;
    std::filesystem::create_directories(kTmpDir, ec);
    return !ec;
}

bool bit_equal(float a, float b) {
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

}  // namespace

// ------------------------------------------------------------------- WAV

TEST(wav, roundtrip_mono_bit_exact) {
    CHECK(ensure_tmp_dir());
    WavData w;
    w.sample_rate = 48000;
    w.channels = 1;
    w.samples.resize(1001);
    for (std::size_t i = 0; i < w.samples.size(); ++i) {
        w.samples[i] = static_cast<float>(
            0.7 * std::sin(0.031 * static_cast<double>(i)));
    }
    // Awkward bit patterns that must survive verbatim (finite values only:
    // NaN would not compare equal to itself after the round-trip).
    w.samples[0] = 0.f;
    w.samples[1] = -0.f;
    w.samples[2] = std::numeric_limits<float>::denorm_min();
    w.samples[3] = std::numeric_limits<float>::lowest() / 2.f;
    w.samples[4] = std::numeric_limits<float>::epsilon();

    std::string err;
    std::string const path = kTmpDir + "/rt_mono.wav";
    CHECK(addu94::audio::wav_save(path, w, err));
    CHECK(err.empty());
    std::optional<WavData> const back = addu94::audio::wav_load(path, err);
    CHECK(back.has_value());
    if (back.has_value()) {
        CHECK_EQ(back->sample_rate, 48000u);
        CHECK_EQ(back->channels, std::uint16_t{1});
        CHECK_EQ(back->samples.size(), w.samples.size());
        for (std::size_t i = 0; i < w.samples.size(); ++i) {
            if (!bit_equal(back->samples[i], w.samples[i])) {
                CHECK_EQ(back->samples[i], w.samples[i]);  // report mismatch
                break;
            }
        }
    }
}

TEST(wav, roundtrip_stereo_bit_exact) {
    CHECK(ensure_tmp_dir());
    WavData w;
    w.sample_rate = 44100;
    w.channels = 2;
    w.samples.resize(2048);
    for (std::size_t i = 0; i < w.samples.size(); i += 2) {
        w.samples[i] = static_cast<float>(std::sin(0.01 * static_cast<double>(i)));
        w.samples[i + 1] = static_cast<float>(std::cos(0.017 * static_cast<double>(i)));
    }
    std::string err;
    std::string const path = kTmpDir + "/rt_stereo.wav";
    CHECK(addu94::audio::wav_save(path, w, err));
    std::optional<WavData> const back = addu94::audio::wav_load(path, err);
    CHECK(back.has_value());
    if (back.has_value()) {
        CHECK_EQ(back->sample_rate, 44100u);
        CHECK_EQ(back->channels, std::uint16_t{2});
        CHECK_EQ(back->samples.size(), w.samples.size());
        for (std::size_t i = 0; i < w.samples.size(); ++i) {
            if (!bit_equal(back->samples[i], w.samples[i])) {
                CHECK_EQ(back->samples[i], w.samples[i]);
                break;
            }
        }
    }
}

TEST(wav, loads_hand_built_pcm16) {
    CHECK(ensure_tmp_dir());
    // 4 stereo frames of PCM16 with known values.
    std::vector<std::uint8_t> payload;
    std::int16_t const raw[8] = {0, 16383, -16384, -1, 32767, -32768, 7, -7};
    for (std::int16_t v : raw) {
        push_u16(payload, static_cast<std::uint16_t>(v));
    }
    std::string const path = kTmpDir + "/pcm16.wav";
    CHECK(write_bytes(path, make_wav_bytes(1, 2, 48000, 16, payload)));

    std::string err;
    std::optional<WavData> const w = addu94::audio::wav_load(path, err);
    CHECK(w.has_value());
    if (w.has_value()) {
        CHECK_EQ(w->sample_rate, 48000u);
        CHECK_EQ(w->channels, std::uint16_t{2});
        CHECK_EQ(w->samples.size(), std::size_t{8});
        CHECK_NEAR(w->samples[0], 0.0, 1e-9);
        CHECK_NEAR(w->samples[1], 16383.0 / 32768.0, 1e-9);
        CHECK_NEAR(w->samples[2], -0.5, 1e-9);
        CHECK_NEAR(w->samples[3], -1.0 / 32768.0, 1e-9);
        CHECK_NEAR(w->samples[4], 32767.0 / 32768.0, 1e-9);
        CHECK_NEAR(w->samples[5], -1.0, 1e-9);
    }
}

TEST(wav, malformed_input_rejected_without_crash) {
    CHECK(ensure_tmp_dir());
    std::string err;
    auto const expect_rejected = [&](std::string const& path) {
        std::optional<WavData> const w = addu94::audio::wav_load(path, err);
        if (w.has_value()) {
            CHECK(!"malformed WAV was accepted");
        } else {
            CHECK(!err.empty());
        }
    };

    // 1. empty file
    std::string const empty = kTmpDir + "/bad_empty.wav";
    CHECK(write_bytes(empty, {}));
    expect_rejected(empty);

    // 2. "RIFF" only
    std::string const riff = kTmpDir + "/bad_riff.wav";
    CHECK(write_bytes(riff, {'R', 'I', 'F', 'F'}));
    expect_rejected(riff);

    // 3. valid header, data chunk claims 0xFFFFFFF0 bytes but 4 present
    std::vector<std::uint8_t> trunc = make_wav_bytes(3, 1, 48000, 32, {1, 2, 3, 4});
    std::size_t const data_off = trunc.size() - 4;
    trunc[data_off - 4] = 0xF0;  // data size low byte: 0xFFFFFFF0
    trunc[data_off - 3] = 0xFF;
    trunc[data_off - 2] = 0xFF;
    trunc[data_off - 1] = 0xFF;
    std::string const trpath = kTmpDir + "/bad_truncated.wav";
    CHECK(write_bytes(trpath, trunc));
    expect_rejected(trpath);

    // 4. absurd RIFF size (0xFFFFFFFF) on a tiny file
    std::vector<std::uint8_t> absurd = make_wav_bytes(3, 1, 48000, 32, {0});
    absurd[4] = 0xFF;
    absurd[5] = 0xFF;
    absurd[6] = 0xFF;
    absurd[7] = 0xFF;
    std::string const apath = kTmpDir + "/bad_absurd.wav";
    CHECK(write_bytes(apath, absurd));
    expect_rejected(apath);

    // 5. unknown codec (tag 7, u-law)
    std::string const codec = kTmpDir + "/bad_codec.wav";
    CHECK(write_bytes(codec, make_wav_bytes(7, 1, 48000, 8, {0})));
    expect_rejected(codec);

    // 6. three channels (mono/stereo only)
    std::string const chans = kTmpDir + "/bad_channels.wav";
    CHECK(write_bytes(chans, make_wav_bytes(3, 3, 48000, 32, {0, 0, 0})));
    expect_rejected(chans);

    // 7. missing fmt chunk
    std::vector<std::uint8_t> nofmt;
    push_tag(nofmt, "RIFF");
    push_u32(nofmt, 4 + 8 + 4);
    push_tag(nofmt, "WAVE");
    push_tag(nofmt, "data");
    push_u32(nofmt, 4);
    push_u32(nofmt, 0);
    std::string const nf = kTmpDir + "/bad_nofmt.wav";
    CHECK(write_bytes(nf, nofmt));
    expect_rejected(nf);

    // 8. missing data chunk
    std::string const nd = kTmpDir + "/bad_nodata.wav";
    CHECK(write_bytes(nd, make_wav_bytes(3, 1, 48000, 32, {})));
    // make_wav_bytes with empty payload yields a zero-size data chunk,
    // which is also rejected:
    expect_rejected(nd);

    // 9. header only (RIFF+WAVE, no chunks)
    std::vector<std::uint8_t> hdr;
    push_tag(hdr, "RIFF");
    push_u32(hdr, 4);
    push_tag(hdr, "WAVE");
    std::string const ho = kTmpDir + "/bad_header_only.wav";
    CHECK(write_bytes(ho, hdr));
    expect_rejected(ho);

    // 10. nonexistent file
    expect_rejected(kTmpDir + "/does_not_exist.wav");
}

TEST(wav, save_validates_input) {
    std::string err;
    WavData bad;
    bad.channels = 3;
    CHECK(!addu94::audio::wav_save(kTmpDir + "/never.wav", bad, err));
    CHECK(!err.empty());

    WavData uneven;
    uneven.channels = 2;
    uneven.samples = {1.f, 2.f, 3.f};  // not a multiple of 2
    CHECK(!addu94::audio::wav_save(kTmpDir + "/never.wav", uneven, err));
    CHECK(!err.empty());
}

// --------------------------------------------------------------- Sampler

TEST(sampler, renders_loaded_sample_audibly) {
    CHECK(ensure_tmp_dir());
    std::string const path = kTmpDir + "/sine440.wav";
    CHECK(write_sine_wav(path, 440.0, 1));

    Sampler s;
    s.set_sample_rate(kSr);
    std::string err;
    CHECK(s.add_zone(36, path, err));
    CHECK(s.has_zone(36));
    CHECK(!s.has_zone(37));

    s.note_on(9, 36, 1.0f);
    CHECK_EQ(s.active_voices(), 1);
    Rendered r = render_for(s, 0.5);
    CHECK(r.peak > 0.05f);
    CHECK(r.peak <= 1.0f);
    for (float v : r.left) {
        CHECK(!std::isnan(v));
        CHECK(!std::isinf(v));
    }
    // One-shot: sample ends on its own; after 0.6 s everything is silent.
    Rendered r2 = render_for(s, 0.2);
    CHECK_EQ(r2.peak, 0.f);
    CHECK_EQ(s.active_voices(), 0);
}

TEST(sampler, all_sound_off_is_exact_zero) {
    CHECK(ensure_tmp_dir());
    std::string const path = kTmpDir + "/sine440b.wav";
    CHECK(write_sine_wav(path, 440.0, 1));

    Sampler s;
    s.set_sample_rate(kSr);
    std::string err;
    CHECK(s.add_zone(36, path, err));
    for (int i = 0; i < 10; ++i) s.note_on(9, 36 + (i % 3), 0.8f);
    Rendered r = render_for(s, 0.1);
    CHECK(r.peak > 0.01f);
    s.all_sound_off();
    std::vector<float> buf(4800, 1.f);
    s.render(buf.data(), buf.data(), 4800);
    for (float v : buf) CHECK_EQ(v, 0.f);
    CHECK_EQ(s.active_voices(), 0);
}

TEST(sampler, tuning_plus12_halves_duration) {
    CHECK(ensure_tmp_dir());
    std::string const path = kTmpDir + "/sine440c.wav";
    CHECK(write_sine_wav(path, 440.0, 1));

    Sampler s;
    s.set_sample_rate(kSr);
    std::string err;
    CHECK(s.add_zone(36, path, err));

    s.note_on(9, 36, 1.0f);
    std::uint32_t const natural = audible_frames(render_for(s, 0.6).left, 0.001f);

    SamplerParams p;
    p.pad_tuning_semitones[0] = 12;  // +12 => playback rate 2.0
    s.set_params(p);
    s.note_on(9, 36, 1.0f);
    std::uint32_t const tuned = audible_frames(render_for(s, 0.6).left, 0.001f);

    // natural ~ 24000 frames, tuned ~ 12000 frames (± envelope + interp).
    CHECK_NEAR(static_cast<double>(natural), 24000.0, 500.0);
    CHECK_NEAR(static_cast<double>(tuned),
               static_cast<double>(natural) / 2.0, 600.0);
}

TEST(sampler, choke_group_silences_previous_voice) {
    CHECK(ensure_tmp_dir());
    std::string const p1 = kTmpDir + "/sine440d.wav";
    std::string const p2 = kTmpDir + "/sine660.wav";
    CHECK(write_sine_wav(p1, 440.0, 1));
    CHECK(write_sine_wav(p2, 660.0, 1));

    SamplerParams choked;
    choked.pad_choke_group[0] = 1;
    choked.pad_choke_group[1] = 1;

    Sampler a;  // with choke
    a.set_sample_rate(kSr);
    std::string err;
    CHECK(a.add_zone(36, p1, err));
    CHECK(a.add_zone(37, p2, err));
    a.set_params(choked);

    Sampler b;  // identical, no choke
    b.set_sample_rate(kSr);
    CHECK(b.add_zone(36, p1, err));
    CHECK(b.add_zone(37, p2, err));

    a.note_on(9, 36, 1.0f);
    b.note_on(9, 36, 1.0f);
    Rendered ra = render_for(a, 0.1);
    Rendered rb = render_for(b, 0.1);
    CHECK(ra.peak > 0.05f);
    CHECK(bit_equal(ra.peak, rb.peak));  // identical until the second hit

    a.note_on(9, 37, 1.0f);  // same choke group: voice 36 fades over 2 ms
    b.note_on(9, 37, 1.0f);
    Rendered settle = render_for(a, 0.01);  // 10 ms > 2 ms fade
    CHECK_EQ(a.active_voices(), 1);         // only the new voice remains
    CHECK_EQ(b.active_voices(), 2);         // control: both still sound

    Rendered ra2 = render_for(a, 0.35);
    Rendered rb2 = render_for(b, 0.35);
    CHECK(energy(ra2) < energy(rb2) * 0.8);  // choked mix is quieter
    CHECK(ra2.peak > 0.05f);                 // new voice still audible
    for (float v : ra2.left) {
        CHECK(!std::isnan(v));
    }
}

TEST(sampler, mono_sample_feeds_both_channels) {
    CHECK(ensure_tmp_dir());
    std::string const path = kTmpDir + "/sine440e.wav";
    CHECK(write_sine_wav(path, 440.0, 1));
    Sampler s;
    s.set_sample_rate(kSr);
    std::string err;
    CHECK(s.add_zone(40, path, err));
    s.note_on(9, 40, 1.0f);
    Rendered r = render_for(s, 0.1);
    CHECK(r.peak > 0.05f);
    for (std::size_t i = 0; i < r.left.size(); ++i) {
        if (!bit_equal(r.left[i], r.right[i])) {
            CHECK_EQ(r.right[i], r.left[i]);
            break;
        }
    }
}

TEST(sampler, stereo_sample_splits_channels) {
    CHECK(ensure_tmp_dir());
    std::string const path = kTmpDir + "/sine_stereo.wav";
    CHECK(write_sine_wav(path, 440.0, 2));  // L=440 Hz, R=880 Hz
    Sampler s;
    s.set_sample_rate(kSr);
    std::string err;
    CHECK(s.add_zone(41, path, err));
    s.note_on(9, 41, 1.0f);
    Rendered r = render_for(s, 0.1);
    CHECK(r.peak > 0.05f);
    // L=440 Hz vs R=880 Hz: both channels audible, waveforms differ.
    float pl = 0.f;
    float pr = 0.f;
    bool differ = false;
    for (std::size_t i = 0; i < r.left.size(); ++i) {
        pl = std::max(pl, std::abs(r.left[i]));
        pr = std::max(pr, std::abs(r.right[i]));
        if (!bit_equal(r.left[i], r.right[i])) differ = true;
    }
    CHECK(pl > 0.05f);
    CHECK(pr > 0.05f);
    CHECK(differ);
}

TEST(sampler, note_off_is_noop_for_one_shots) {
    CHECK(ensure_tmp_dir());
    std::string const path = kTmpDir + "/sine440f.wav";
    CHECK(write_sine_wav(path, 440.0, 1));
    Sampler s;
    s.set_sample_rate(kSr);
    std::string err;
    CHECK(s.add_zone(36, path, err));
    s.note_on(9, 36, 1.0f);
    s.note_off(9, 36);
    CHECK_EQ(s.active_voices(), 1);
    Rendered r = render_for(s, 0.05);
    CHECK(r.peak > 0.05f);
}

TEST(sampler, stress_100_note_ons_bounded_and_clean) {
    CHECK(ensure_tmp_dir());
    std::string const path = kTmpDir + "/sine440g.wav";
    CHECK(write_sine_wav(path, 440.0, 1));
    Sampler s;
    s.set_sample_rate(kSr);
    std::string err;
    CHECK(s.add_zone(36, path, err));
    CHECK(s.add_zone(37, path, err));

    for (int i = 0; i < 100; ++i) {
        s.note_on(9, 36 + (i % 16), 0.3f + 0.01f * static_cast<float>(i % 8));
    }
    CHECK(s.active_voices() <= addu94::audio::kSamplerMaxVoices);
    Rendered r = render_for(s, 1.0);
    for (std::size_t i = 0; i < r.left.size(); ++i) {
        CHECK(!std::isnan(r.left[i]));
        CHECK(!std::isinf(r.left[i]));
        CHECK(!std::isnan(r.right[i]));
        CHECK(!std::isinf(r.right[i]));
        CHECK(r.left[i] <= 1.0f);
        CHECK(r.left[i] >= -1.0f);
        if (i > 60000) break;  // spot-check the dense polyphony window
    }
}

// --------------------------------------------------------------- DrumKit

TEST(drum_kit, fallback_kit_is_audible_and_distinct_per_pad) {
    DrumKit dk;
    std::string err;
    CHECK(dk.init(kSr, err));
    CHECK(err.empty());

    float peaks[8] = {};
    for (int i = 0; i < 8; ++i) {
        int const note = addu94::audio::kPadBaseNote + i;
        CHECK(dk.sampler().has_zone(note));
        dk.note_on(note, 1.0f);
        Rendered r = render_for(dk, 0.4);
        CHECK(r.peak > 0.05f);
        peaks[static_cast<std::size_t>(i)] = r.peak;
        dk.sampler().all_sound_off();
        render_for(dk, 0.01);  // flush (overwrite semantics)
    }
    for (int i = 0; i < 8; ++i) {
        for (int j = i + 1; j < 8; ++j) {
            if (std::abs(peaks[static_cast<std::size_t>(i)] -
                         peaks[static_cast<std::size_t>(j)]) < 1e-3f) {
                CHECK(!"fallback pad peaks not distinct");
            }
        }
    }
}

TEST(drum_kit, render_is_deterministic) {
    DrumKit a;
    DrumKit b;
    std::string err;
    CHECK(a.init(kSr, err));
    CHECK(b.init(kSr, err));

    for (int i = 0; i < 8; ++i) {
        a.note_on(addu94::audio::kPadBaseNote + i,
                  0.4f + 0.06f * static_cast<float>(i));
        b.note_on(addu94::audio::kPadBaseNote + i,
                  0.4f + 0.06f * static_cast<float>(i));
    }
    Rendered ra = render_for(a, 0.6);
    Rendered rb = render_for(b, 0.6);
    CHECK_EQ(ra.left.size(), rb.left.size());
    for (std::size_t i = 0; i < ra.left.size(); ++i) {
        if (!bit_equal(ra.left[i], rb.left[i]) ||
            !bit_equal(ra.right[i], rb.right[i])) {
            CHECK(!"drum kit renders differ between identical runs");
            break;
        }
    }
}

TEST(drum_kit, pad_range_and_shifted_notes_sound) {
    DrumKit dk;
    std::string err;
    CHECK(dk.init(kSr, err));
    // Bank A: 36..43
    for (int note = 36; note < 44; ++note) {
        dk.note_on(note, 0.9f);
        Rendered r = render_for(dk, 0.1);
        CHECK(r.peak > 0.05f);
        dk.sampler().all_sound_off();
    }
    // Shifted bank: 44..51 = same pads +12 (still audible, faster playback).
    for (int note = 44; note < 52; ++note) {
        dk.note_on(note, 0.9f);
        Rendered r = render_for(dk, 0.1);
        CHECK(r.peak > 0.01f);
        dk.sampler().all_sound_off();
    }
}

TEST(drum_kit, load_override_replaces_pad) {
    CHECK(ensure_tmp_dir());
    std::string const path = kTmpDir + "/override.wav";
    // Amplitude 0.5: peak after tanh (~0.46) differs from every fallback's
    // normalized peak (0.5..0.9 -> tanh 0.46..0.72) — in particular from
    // the kick (0.9), so "content changed" is observable via the peak.
    CHECK(write_sine_wav(path, 220.0, 1, 0.5));

    DrumKit dk;
    std::string err;
    CHECK(dk.init(kSr, err));
    float const fallback_peak = [&] {
        dk.note_on(36, 1.0f);
        Rendered r = render_for(dk, 0.2);
        dk.sampler().all_sound_off();
        render_for(dk, 0.01);
        return r.peak;
    }();

    CHECK(dk.load_override(36, path, err));
    dk.note_on(36, 1.0f);
    Rendered r = render_for(dk, 0.2);
    CHECK(r.peak > 0.05f);
    CHECK(std::abs(r.peak - fallback_peak) > 1e-3f);  // content changed

    CHECK(!dk.load_override(36, kTmpDir + "/bad_empty.wav", err));
}

TEST(drum_kit, init_rejects_bad_rate) {
    DrumKit dk;
    std::string err;
    CHECK(!dk.init(0.0, err));
    CHECK(!err.empty());
    CHECK(!dk.init(500000.0, err));
    CHECK(!err.empty());
}

TEST_MAIN()
