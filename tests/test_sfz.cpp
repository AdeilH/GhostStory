// ghost_story — tests/test_sfz.cpp
//
// SFZ parser + sampler_load_sfz tests (spec §4.1). Covers: field parsing
// (key / lokey-hikey / pitch_keycenter / transpose / volume / tune), group
// inheritance, quoted sample paths, relative path resolution, robustness
// against garbage (unknown opcodes, malformed tokens, missing samples,
// unreadable file, ".." path traversal) and the audible end-to-end load:
// zero-crossing pitch discrimination between two zones and -6 dB volume
// folding to roughly half peak.

#include "test_framework.hpp"

#include "audio/sampler.hpp"
#include "audio/sfz.hpp"
#include "audio/wav_file.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using ghost_story::audio::SfzParseResult;
using ghost_story::audio::SfzRegion;
using ghost_story::audio::Sampler;
using ghost_story::audio::WavData;

namespace {

constexpr double kSr = 48000.0;
constexpr double kTwoPi = 6.283185307179586476925286766559;
std::string const kTmpDir = "/tmp/ghost_story_sfz_test";

// ---------------------------------------------------------------- helpers

bool ensure_tmp_dir() {
    std::error_code ec;
    std::filesystem::create_directories(kTmpDir, ec);
    return !ec;
}

bool write_text(std::string const& path, std::string const& text) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    bool ok = text.empty() ||
              std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}

/// 0.5 s sine at `hz`, saved as 32-bit float WAV.
bool write_sine_wav(std::string const& path, double hz, double amp = 0.9) {
    WavData w;
    w.sample_rate = static_cast<std::uint32_t>(kSr);
    w.channels = 1;
    std::size_t const frames = static_cast<std::size_t>(kSr * 0.5);
    w.samples.resize(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        double const t = static_cast<double>(i) / kSr;
        w.samples[i] = static_cast<float>(amp * std::sin(kTwoPi * hz * t));
    }
    std::string err;
    return ghost_story::audio::wav_save(path, w, err);
}

/// Render `seconds` of one note_on through chunked render() calls.
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
        std::uint32_t const n =
            (frames - off < 512) ? frames - off : 512;
        s.render(r.left.data() + off, r.right.data() + off, n);
    }
    for (float v : r.left) r.peak = std::max(r.peak, std::abs(v));
    return r;
}

/// Sign changes over the buffer: ~2 * freq * seconds for a decaying sine.
std::uint32_t zero_crossings(std::vector<float> const& buf) {
    std::uint32_t n = 0;
    for (std::size_t i = 1; i < buf.size(); ++i) {
        if ((buf[i - 1] <= 0.f) != (buf[i] <= 0.f)) ++n;
    }
    return n;
}

}  // namespace

// ------------------------------------------------------------------ parse

TEST(sfz, parses_fields_and_resolves_relative_paths) {
    CHECK(ensure_tmp_dir());
    std::string const sfz_path = kTmpDir + "/fields.sfz";
    CHECK(write_text(sfz_path,
                     "// comment line\n"
                     "<region> sample=sine440.wav key=60\n"
                     "<region> sample=\"sine 880.wav\" lokey=48 hikey=59\n"
                     "         pitch_keycenter=55 transpose=-2\n"
                     "         lovel=10 hivel=100 volume=-6.5 tune=25\n"));

    std::string err;
    std::optional<SfzParseResult> const res =
        ghost_story::audio::sfz_load(sfz_path, err);
    CHECK(err.empty());
    CHECK(res.has_value());
    if (!res.has_value()) return;
    CHECK_EQ(res->regions.size(), std::size_t{2});
    CHECK_EQ(res->skipped_lines, 0);
    CHECK_EQ(res->skipped_regions, 0);

    if (res->regions.size() != 2) return;
    SfzRegion const& a = res->regions[0];
    CHECK_EQ(a.sample_path, kTmpDir + "/sine440.wav");
    CHECK_EQ(a.key, 60);
    CHECK_EQ(a.lo_key, 60);
    CHECK_EQ(a.hi_key, 60);
    CHECK_EQ(a.pitch_keycenter, -1);
    CHECK_EQ(a.transpose, 0);
    CHECK_NEAR(a.volume_db, 0.0, 1e-6);
    CHECK_NEAR(a.tune_cents, 0.0, 1e-6);

    // Multi-line region, quoted path with a space, group-less inheritance.
    SfzRegion const& b = res->regions[1];
    CHECK_EQ(b.sample_path, kTmpDir + "/sine 880.wav");
    CHECK_EQ(b.key, -1);
    CHECK_EQ(b.lo_key, 48);
    CHECK_EQ(b.hi_key, 59);
    CHECK_EQ(b.lo_vel, 10);
    CHECK_EQ(b.hi_vel, 100);
    CHECK_EQ(b.pitch_keycenter, 55);
    CHECK_EQ(b.transpose, -2);
    CHECK_NEAR(b.volume_db, -6.5, 1e-5);
    CHECK_NEAR(b.tune_cents, 25.0, 1e-5);
}

TEST(sfz, group_defaults_inherited_and_reset) {
    CHECK(ensure_tmp_dir());
    std::string const sfz_path = kTmpDir + "/groups.sfz";
    CHECK(write_text(sfz_path,
                     "<group> volume=-3 transpose=5\n"
                     "<region> sample=sine440.wav key=60\n"
                     "<region> sample=sine880.wav key=62\n"
                     "<group> volume=-12\n"
                     "<region> sample=sine440.wav key=64\n"
                     "<region> sample=sine880.wav key=66 volume=-1\n"));

    std::string err;
    std::optional<SfzParseResult> const res =
        ghost_story::audio::sfz_load(sfz_path, err);
    CHECK(res.has_value());
    if (!res.has_value()) return;
    CHECK_EQ(res->regions.size(), std::size_t{4});
    if (res->regions.size() != 4) return;

    CHECK_NEAR(res->regions[0].volume_db, -3.0, 1e-5);
    CHECK_EQ(res->regions[0].transpose, 5);
    CHECK_NEAR(res->regions[1].volume_db, -3.0, 1e-5);
    CHECK_EQ(res->regions[1].transpose, 5);
    // Second <group> resets the defaults...
    CHECK_NEAR(res->regions[2].volume_db, -12.0, 1e-5);
    CHECK_EQ(res->regions[2].transpose, 0);
    // ...but region opcodes still override them.
    CHECK_NEAR(res->regions[3].volume_db, -1.0, 1e-5);
}

TEST(sfz, opcodes_before_any_header_are_group_defaults) {
    CHECK(ensure_tmp_dir());
    std::string const sfz_path = kTmpDir + "/prelude.sfz";
    CHECK(write_text(sfz_path,
                     "volume=-6\n"
                     "<region> sample=sine440.wav key=40\n"
                     "<region> sample=sine880.wav key=41\n"));
    std::string err;
    std::optional<SfzParseResult> const res =
        ghost_story::audio::sfz_load(sfz_path, err);
    CHECK(res.has_value());
    if (!res.has_value()) return;
    CHECK_EQ(res->regions.size(), std::size_t{2});
    for (SfzRegion const& r : res->regions) {
        CHECK_NEAR(r.volume_db, -6.0, 1e-5);
    }
}

TEST(sfz, clamps_and_swapped_ranges_are_normalized_or_skipped) {
    CHECK(ensure_tmp_dir());
    std::string const sfz_path = kTmpDir + "/clamps.sfz";
    CHECK(write_text(sfz_path,
                     "<region> sample=sine440.wav key=200 volume=99 tune=-500\n"
                     "<region> sample=sine440.wav lokey=90 hikey=10\n"));
    std::string err;
    std::optional<SfzParseResult> const res =
        ghost_story::audio::sfz_load(sfz_path, err);
    CHECK(res.has_value());
    if (!res.has_value()) return;
    // Region 1: clamped into range; region 2: inverted key range => skipped.
    CHECK_EQ(res->regions.size(), std::size_t{1});
    CHECK_EQ(res->skipped_regions, 1);
    if (res->regions.size() == 1) {
        CHECK_EQ(res->regions[0].key, 127);
        CHECK_NEAR(res->regions[0].volume_db, 12.0, 1e-5);
        CHECK_NEAR(res->regions[0].tune_cents, -100.0, 1e-5);
    }
}

TEST(sfz, garbage_tokens_and_unknown_opcodes_do_not_break_parsing) {
    CHECK(ensure_tmp_dir());
    std::string const sfz_path = kTmpDir + "/garbage.sfz";
    CHECK(write_text(sfz_path,
                     "this is not an opcode\n"
                     "lokey=48 noise_wobble=7\n"
                     "<region> sample=sine440.wav key=60 panopoly=42\n"
                     "<control>\n"
                     "set_cc12=100\n"
                     "<region> sample=sine880.wav key=62\n"
                     "<\n"
                     "=broken\n"));
    std::string err;
    std::optional<SfzParseResult> const res =
        ghost_story::audio::sfz_load(sfz_path, err);
    CHECK(res.has_value());
    CHECK(err.empty());
    if (!res.has_value()) return;
    CHECK_EQ(res->regions.size(), std::size_t{2});
    // Garbage tokens: "this", "is", "not", "an", "opcode", "=broken" —
    // ("noise_wobble=7" and "panopoly=42" are unknown opcodes = ignored;
    // "set_cc12=100" sits under an ignored <control> header; "<" parses as
    // an unknown header, also ignored rather than counted.)
    CHECK_EQ(res->skipped_lines, 6);
}

TEST(sfz, dotdot_escape_skips_region_at_parse_time) {
    CHECK(ensure_tmp_dir());
    std::string const sfz_path = kTmpDir + "/escape.sfz";
    CHECK(write_text(sfz_path,
                     "<region> sample=../../etc/passwd key=36\n"
                     "<region> sample=does_not_exist.wav key=37\n"
                     "<region> sample=sine440.wav key=38\n"));
    std::string err;
    std::optional<SfzParseResult> const res =
        ghost_story::audio::sfz_load(sfz_path, err);
    CHECK(res.has_value());  // file readable: parse succeeds
    if (!res.has_value()) return;
    // Parse is text-only: the missing-file region still parses (existence
    // is checked at sampler_load_sfz time); only the ".." escape is
    // rejected here.
    CHECK_EQ(res->regions.size(), std::size_t{2});
    CHECK_EQ(res->skipped_regions, 1);
    if (res->regions.size() == 2) {
        CHECK_EQ(res->regions[0].key, 37);
        CHECK_EQ(res->regions[1].key, 38);
    }
}

TEST(sfz, unreadable_file_is_clean_error) {
    std::string err;
    std::optional<SfzParseResult> const res = ghost_story::audio::sfz_load(
        "/tmp/ghost_story_sfz_test/no_such_file.sfz", err);
    CHECK(!res.has_value());
    CHECK(!err.empty());

    std::optional<SfzParseResult> const res2 =
        ghost_story::audio::sfz_load("/tmp/ghost_story_sfz_test", err);
    CHECK(!res2.has_value());
    CHECK(!err.empty());
}

// ---------------------------------------------------------------- loader

TEST(sfz, sampler_load_maps_notes_and_sounds_two_pitches) {
    CHECK(ensure_tmp_dir());
    CHECK(write_sine_wav(kTmpDir + "/sine440.wav", 440.0));
    CHECK(write_sine_wav(kTmpDir + "/sine 880.wav", 880.0));
    std::string const sfz_path = kTmpDir + "/load.sfz";
    CHECK(write_text(sfz_path,
                     "<region> sample=sine440.wav key=60\n"
                     "<region> sample=\"sine 880.wav\" lokey=62 hikey=70\n"));

    Sampler s;
    s.set_sample_rate(kSr);
    std::string err;
    CHECK(ghost_story::audio::sampler_load_sfz(s, sfz_path, err));
    CHECK(err.empty());
    CHECK(s.has_zone(60));  // from `key`
    CHECK(s.has_zone(62));  // from lokey (range center default = lo_key)
    CHECK(!s.has_zone(61));
    CHECK(!s.has_zone(63));

    s.note_on(0, 60, 1.0f);
    Rendered const a = render_for(s, 0.2);
    CHECK(a.peak > 0.05f);
    s.all_sound_off();
    s.note_on(0, 62, 1.0f);
    Rendered const b = render_for(s, 0.2);
    CHECK(b.peak > 0.05f);

    // Crude pitch check: 440 Hz vs 880 Hz => ~2x the zero crossings
    // (0.2 s * 2 * f, minus envelope edges). Tolerance is generous.
    std::uint32_t const zc_a = zero_crossings(a.left);
    std::uint32_t const zc_b = zero_crossings(b.left);
    CHECK(zc_a > 100);  // 440 Hz => ~176
    CHECK(zc_b > 200);  // 880 Hz => ~352
    CHECK(zc_b > zc_a * 3u / 2u);
}

TEST(sfz, transpose_and_pitch_keycenter_shift_the_zone_note) {
    CHECK(ensure_tmp_dir());
    CHECK(write_sine_wav(kTmpDir + "/sine440.wav", 440.0));
    std::string const sfz_path = kTmpDir + "/shift.sfz";
    CHECK(write_text(sfz_path,
                     "<region> sample=sine440.wav key=60 transpose=2\n"
                     "<region> sample=sine440.wav pitch_keycenter=48\n"));

    Sampler s;
    s.set_sample_rate(kSr);
    std::string err;
    CHECK(ghost_story::audio::sampler_load_sfz(s, sfz_path, err));
    CHECK(s.has_zone(62));  // key 60 + transpose 2
    CHECK(s.has_zone(48));  // pitch_keycenter wins over key/lokey
    CHECK(!s.has_zone(60));
}

TEST(sfz, volume_minus_6db_peaks_roughly_half) {
    CHECK(ensure_tmp_dir());
    // Amplitude 0.5 keeps tanh soft-clipping in its near-linear region.
    CHECK(write_sine_wav(kTmpDir + "/sine440.wav", 440.0, 0.5));
    std::string const sfz_path = kTmpDir + "/vol.sfz";
    CHECK(write_text(sfz_path,
                     "<group> volume=-6\n"
                     "<region> sample=sine440.wav key=40\n"));

    Sampler loud;
    loud.set_sample_rate(kSr);
    Sampler quiet;
    quiet.set_sample_rate(kSr);
    std::string err;

    // Reference: same WAV loaded directly at 0 dB.
    CHECK(loud.add_zone(40, kTmpDir + "/sine440.wav", err));
    CHECK(ghost_story::audio::sampler_load_sfz(quiet, sfz_path, err));

    loud.note_on(0, 40, 1.0f);
    Rendered const rl = render_for(loud, 0.1);
    quiet.note_on(0, 40, 1.0f);
    Rendered const rq = render_for(quiet, 0.1);
    CHECK(rl.peak > 0.05f);
    CHECK(rq.peak > 0.01f);
    // -6 dB ~ factor 0.501; tanh is near-linear here; 20% window.
    float const ratio = rq.peak / rl.peak;
    CHECK(ratio > 0.40f);
    CHECK(ratio < 0.60f);
}

TEST(sfz, first_matching_region_wins_for_duplicate_notes) {
    CHECK(ensure_tmp_dir());
    CHECK(write_sine_wav(kTmpDir + "/sine440.wav", 440.0));
    CHECK(write_sine_wav(kTmpDir + "/sine880.wav", 880.0));
    std::string const sfz_path = kTmpDir + "/dup.sfz";
    CHECK(write_text(sfz_path,
                     "<region> sample=sine440.wav key=60\n"
                     "<region> sample=sine880.wav key=60\n"
                     "<region> sample=sine880.wav key=61\n"));
    Sampler s;
    s.set_sample_rate(kSr);
    std::string err;
    CHECK(ghost_story::audio::sampler_load_sfz(s, sfz_path, err));
    CHECK(s.has_zone(60));
    CHECK(s.has_zone(61));
    // Zone 60 keeps the first region's content: 440 Hz.
    s.note_on(0, 60, 1.0f);
    std::uint32_t const zc = zero_crossings(render_for(s, 0.2).left);
    CHECK(zc > 100);
    CHECK(zc < 260);  // 440 Hz ballpark, clearly not 880 (~352)
}

TEST(sfz, malformed_sfz_skips_broken_regions_and_reports_clean_error) {
    CHECK(ensure_tmp_dir());
    CHECK(write_sine_wav(kTmpDir + "/sine440.wav", 440.0));
    std::string const sfz_path = kTmpDir + "/mixed.sfz";
    CHECK(write_text(sfz_path,
                     "garbage line here\n"
                     "<region> sample=missing.wav key=36\n"
                     "<region> sample=sine440.wav key=38\n"
                     "<region> key=40\n"
                     "<region> sample=../../secret.wav key=41\n"
                     "%%%\n"));

    Sampler s;
    s.set_sample_rate(kSr);
    std::string err;
    CHECK(ghost_story::audio::sampler_load_sfz(s, sfz_path, err));
    CHECK(err.empty());  // some regions loaded: not a failure
    CHECK(s.has_zone(38));
    CHECK(!s.has_zone(36));  // missing sample skipped
    CHECK(!s.has_zone(40));  // no sample opcode skipped
    CHECK(!s.has_zone(41));  // traversal path skipped

    // All regions broken => clean failure, no crash, error set.
    std::string const bad_path = kTmpDir + "/all_bad.sfz";
    CHECK(write_text(bad_path,
                     "<region> sample=missing.wav key=36\n"
                     "<region> sample=../nope.wav key=37\n"));
    Sampler s2;
    std::string err2;
    CHECK(!ghost_story::audio::sampler_load_sfz(s2, bad_path, err2));
    CHECK(!err2.empty());
}

TEST(sfz, unreadable_sfz_fails_sampler_load) {
    Sampler s;
    std::string err;
    CHECK(!ghost_story::audio::sampler_load_sfz(
        s, "/tmp/ghost_story_sfz_test/absent.sfz", err));
    CHECK(!err.empty());
}

TEST_MAIN()
