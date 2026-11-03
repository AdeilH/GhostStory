// addu94 — src/audio/wav_file.hpp
//
// Minimal RIFF/WAVE reader + writer, stdlib only (spec §4.1). Reads PCM 16-bit
// and 32-bit float, mono/stereo; writes canonical 32-bit float WAVE.
//
// Fuzz-target contract: wav_load NEVER throws on malformed input — any
// problem (bad magic, unknown codec, truncated chunk, absurd size) yields
// nullopt + a human-readable `error`. Hard cap: 512 MB of sample data.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace addu94::audio {

/// Decoded samples, interleaved (frame k occupies samples[k*channels .. ]).
struct WavData {
    std::uint32_t sample_rate{48000};
    std::uint16_t channels{1};
    std::vector<float> samples;  // interleaved
};

/// Parse a RIFF/WAVE file. Never throws on malformed input; returns
/// nullopt and fills `error` instead.
[[nodiscard]] std::optional<WavData> wav_load(std::string const& path,
                                              std::string& error);

/// Write a canonical 32-bit float WAVE (fmt + fact + data). Validates the
/// input the same way wav_load validates files.
[[nodiscard]] bool wav_save(std::string const& path, WavData const& data,
                            std::string& error);

}  // namespace addu94::audio
