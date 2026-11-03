// addu94 — src/project/export_wav.cpp
//
// Offline export to WAV (see export_wav.hpp). Contains a private minimal
// RIFF/WAVE writer (IEEE float 32-bit, stereo, little-endian) so this
// module is independent of src/audio/wav_file.*; the integrator unifies
// the two later.
//
// File layout: "RIFF" <size> "WAVE" "fmt " 16 / 3 (IEEE float) / 2 ch /
// sample rate / byte rate / block align 8 / bits 32, "data" <size> with
// frame-interleaved L,R float samples.

#include "project/export_wav.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <utility>

namespace addu94::project {
namespace {

void put_u16(std::uint8_t* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v & 0xFFu);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
}

void put_u32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v & 0xFFu);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
}

/// Minimal WAV writer: 32-bit IEEE float, stereo, sample_rate Hz.
bool write_wav_f32(std::string const& path,
                   std::vector<float> const& left,
                   std::vector<float> const& right,
                   double sample_rate, std::string& error) {
    if (left.size() != right.size()) {
        error = "wav: left/right length mismatch (" + path + ")";
        return false;
    }
    std::uint64_t const frames = left.size();
    std::uint64_t const data_size = frames * 8u;  // 2 ch * 4 bytes
    if (data_size + 36u > 0xFFFFFFFFull) {
        error = "wav: too long for RIFF (" + path + ")";
        return false;
    }
    auto const rate = static_cast<std::uint32_t>(sample_rate + 0.5);
    std::uint32_t const byte_rate = rate * 8u;

    std::uint8_t hdr[44];
    std::memcpy(hdr, "RIFF", 4);
    put_u32(hdr + 4, static_cast<std::uint32_t>(data_size + 36u));
    std::memcpy(hdr + 8, "WAVE", 4);
    std::memcpy(hdr + 12, "fmt ", 4);
    put_u32(hdr + 16, 16u);          // fmt chunk size
    put_u16(hdr + 20, 3u);           // IEEE float
    put_u16(hdr + 22, 2u);           // channels
    put_u32(hdr + 24, rate);
    put_u32(hdr + 28, byte_rate);
    put_u16(hdr + 32, 8u);           // block align
    put_u16(hdr + 34, 32u);          // bits per sample
    std::memcpy(hdr + 36, "data", 4);
    put_u32(hdr + 40, static_cast<std::uint32_t>(data_size));

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        error = "wav: cannot open '" + path + "'";
        return false;
    }
    f.write(reinterpret_cast<char const*>(hdr), static_cast<std::streamsize>(sizeof hdr));
    // Interleave in fixed-size chunks: no huge temp buffer, no RT worries.
    std::vector<float> buf;
    std::size_t constexpr kChunk = 4096;
    buf.reserve(kChunk * 2u);
    for (std::uint64_t off = 0; off < frames; off += kChunk) {
        std::uint64_t const n =
            (frames - off < kChunk) ? frames - off : kChunk;
        buf.clear();
        for (std::uint64_t i = 0; i < n; ++i) {
            buf.push_back(left[static_cast<std::size_t>(off + i)]);
            buf.push_back(right[static_cast<std::size_t>(off + i)]);
        }
        f.write(reinterpret_cast<char const*>(buf.data()),
                static_cast<std::streamsize>(buf.size() * sizeof(float)));
    }
    f.flush();
    if (!f) {
        error = "wav: write failed '" + path + "'";
        return false;
    }
    return true;
}

}  // namespace

bool export_wav_stems(ExportSpec const& spec, std::string& error) {
    error.clear();
    if (spec.tracks.empty()) {
        error = "export: no tracks to render";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(spec.output_dir, ec);
    if (!std::filesystem::is_directory(spec.output_dir, ec)) {
        error = "export: cannot create output dir '" + spec.output_dir + "'";
        return false;
    }

    std::vector<audio::RenderResult> stems;
    stems.reserve(spec.tracks.size());
    double sr = 48000.0;
    for (std::size_t i = 0; i < spec.tracks.size(); ++i) {
        stems.push_back(
            audio::render_track(spec.tracks[i], spec.loop_count));
        sr = spec.tracks[i].sample_rate;
        std::string const path =
            spec.output_dir + "/track_" + std::to_string(i + 1) + ".wav";
        if (!write_wav_f32(path, stems.back().left, stems.back().right, sr,
                           error)) {
            return false;
        }
    }

    // Master = exact mix_down sum of the stems (pre-clip master bus).
    audio::RenderResult const master = audio::mix_down(stems);
    return write_wav_f32(spec.output_dir + "/master.wav", master.left,
                         master.right, sr, error);
}

}  // namespace addu94::project
