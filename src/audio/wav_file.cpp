// addu94 — src/audio/wav_file.cpp
//
// RIFF/WAVE parser + writer implementing wav_file.hpp.
//
// Parsing strategy: read the whole file into memory (control thread, bounded
// by the 512 MB cap), then walk chunks strictly. Every declared chunk size
// must fit inside the file — a chunk claiming more bytes than remain makes
// the file truncated and rejected. Unknown chunks are skipped (with their
// pad byte); 'fmt ' and 'data' may appear in any order (two-phase: locate,
// then decode). All multi-byte fields are little-endian per the RIFF spec;
// values are decoded byte-wise so endianness assumptions stay explicit.
//
// The writer emits the canonical Microsoft float layout: RIFF/WAVE,
// 16-byte 'fmt ' (format tag 3), 4-byte 'fact' (sample length), 'data'.

#include "audio/wav_file.hpp"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace addu94::audio {
namespace {

constexpr std::uint64_t kMaxWavBytes = std::uint64_t{512} << 20;  // 512 MB
constexpr std::uint64_t kMaxSamples = kMaxWavBytes / sizeof(float);
constexpr std::uint32_t kMaxSampleRate = 192000u;

[[nodiscard]] std::uint16_t rd_u16(std::uint8_t const* p) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(p[0]) |
        (static_cast<std::uint16_t>(p[1]) << 8));
}

[[nodiscard]] std::uint32_t rd_u32(std::uint8_t const* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

void wr_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
}

void wr_u16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
}

void wr_tag(std::vector<std::uint8_t>& out, char const (&tag)[5]) {
    for (std::size_t i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(tag[i]));
    }
}

[[nodiscard]] bool read_whole_file(std::string const& path,
                                   std::vector<std::uint8_t>& buf,
                                   std::string& error) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        error = "cannot open file: " + path;
        return false;
    }
    bool ok = false;
    if (std::fseek(f, 0, SEEK_END) != 0) {
        error = "cannot seek: " + path;
    } else {
        long const sz = std::ftell(f);
        if (sz < 0) {
            error = "cannot determine file size: " + path;
        } else if (static_cast<std::uint64_t>(sz) > kMaxWavBytes) {
            error = "file too large (cap 512 MB): " + path;
        } else if (std::fseek(f, 0, SEEK_SET) != 0) {
            error = "cannot rewind: " + path;
        } else {
            buf.resize(static_cast<std::size_t>(sz));
            std::size_t const got =
                sz == 0 ? 0 : std::fread(buf.data(), 1, buf.size(), f);
            if (got == buf.size()) {
                ok = true;
            } else {
                error = "short read: " + path;
            }
        }
    }
    if (std::fclose(f) != 0) {
        ok = false;
        if (error.empty()) error = "fclose failed: " + path;
    }
    return ok;
}

}  // namespace

std::optional<WavData> wav_load(std::string const& path, std::string& error) {
    error.clear();
    std::vector<std::uint8_t> buf;
    if (!read_whole_file(path, buf, error)) {
        return std::nullopt;
    }
    if (buf.size() < 12) {
        error = "file too small to be RIFF/WAVE: " + path;
        return std::nullopt;
    }
    if (std::memcmp(buf.data(), "RIFF", 4) != 0 ||
        std::memcmp(buf.data() + 8, "WAVE", 4) != 0) {
        error = "not a RIFF/WAVE file: " + path;
        return std::nullopt;
    }

    // RIFF size = bytes following the size field. It must be consistent:
    // a file whose RIFF box claims more bytes than it contains is
    // truncated/corrupt and rejected outright; trailing junk past a
    // consistent size is ignored.
    std::uint64_t const riff_size = rd_u32(buf.data() + 4);
    std::uint64_t const file_end = buf.size();
    if (riff_size < 4 || 8 + riff_size > file_end) {
        error = "RIFF size field inconsistent with file size: " + path;
        return std::nullopt;
    }
    std::uint64_t const end = 8 + riff_size;

    // Pass 1: walk chunks, locate 'fmt ' and 'data'.
    bool have_fmt = false;
    bool have_data = false;
    std::uint16_t fmt_tag = 0;
    std::uint16_t channels = 0;
    std::uint16_t bits = 0;
    std::uint32_t rate = 0;
    std::uint64_t data_off = 0;
    std::uint64_t data_size = 0;

    std::uint64_t pos = 12;
    while (pos + 8 <= end) {
        std::uint8_t const* hdr = buf.data() + pos;
        std::uint64_t const body = pos + 8;
        std::uint64_t const chunk_size = rd_u32(hdr + 4);
        if (body + chunk_size > end) {
            error = "truncated chunk at offset " + std::to_string(pos) +
                    ": " + path;
            return std::nullopt;
        }
        if (std::memcmp(hdr, "fmt ", 4) == 0) {
            if (chunk_size < 16) {
                error = "fmt chunk too small: " + path;
                return std::nullopt;
            }
            fmt_tag = rd_u16(buf.data() + body);
            channels = rd_u16(buf.data() + body + 2);
            rate = rd_u32(buf.data() + body + 4);
            bits = rd_u16(buf.data() + body + 14);
            have_fmt = true;
        } else if (std::memcmp(hdr, "data", 4) == 0) {
            data_off = body;
            data_size = chunk_size;
            have_data = true;
        }
        // Next chunk (payload + pad byte for odd sizes).
        pos = body + chunk_size + (chunk_size & 1u);
    }

    if (!have_fmt) {
        error = "missing fmt chunk: " + path;
        return std::nullopt;
    }
    if (!have_data) {
        error = "missing data chunk: " + path;
        return std::nullopt;
    }
    if (channels != 1 && channels != 2) {
        error = "unsupported channel count " + std::to_string(channels) +
                " (mono/stereo only): " + path;
        return std::nullopt;
    }
    if (rate < 1u || rate > kMaxSampleRate) {
        error = "unsupported sample rate " + std::to_string(rate) + ": " + path;
        return std::nullopt;
    }
    bool const is_pcm16 = fmt_tag == 1 && bits == 16;
    bool const is_float32 = fmt_tag == 3 && bits == 32;
    if (!is_pcm16 && !is_float32) {
        error = "unsupported format tag " + std::to_string(fmt_tag) + "/" +
                std::to_string(bits) + " bits (PCM16 or FLOAT32 only): " + path;
        return std::nullopt;
    }
    if (data_size == 0) {
        error = "empty data chunk: " + path;
        return std::nullopt;
    }

    std::uint64_t const bytes_per_frame =
        static_cast<std::uint64_t>(channels) * (bits / 8u);
    std::uint64_t const frames = data_size / bytes_per_frame;
    if (frames == 0 || frames > kMaxSamples / bytes_per_frame) {
        error = "sample data exceeds 512 MB cap: " + path;
        return std::nullopt;
    }

    WavData out;
    out.sample_rate = rate;
    out.channels = channels;
    out.samples.resize(static_cast<std::size_t>(frames) *
                       static_cast<std::size_t>(channels));

    std::uint8_t const* d = buf.data() + data_off;
    if (is_pcm16) {
        for (std::size_t i = 0; i < out.samples.size(); ++i) {
            std::int16_t const v = static_cast<std::int16_t>(
                rd_u16(d + i * 2));
            out.samples[i] = static_cast<float>(v) / 32768.f;
        }
    } else {
        for (std::size_t i = 0; i < out.samples.size(); ++i) {
            out.samples[i] = std::bit_cast<float>(
                rd_u32(d + i * 4));
        }
    }
    return out;
}

bool wav_save(std::string const& path, WavData const& data, std::string& error) {
    error.clear();
    if (data.channels != 1 && data.channels != 2) {
        error = "wav_save: channels must be 1 or 2";
        return false;
    }
    if (data.sample_rate < 1u || data.sample_rate > kMaxSampleRate) {
        error = "wav_save: unsupported sample rate";
        return false;
    }
    if (data.samples.empty() || data.samples.size() % data.channels != 0) {
        error = "wav_save: samples must be non-empty and a multiple of channels";
        return false;
    }
    std::uint64_t const frames =
        data.samples.size() / static_cast<std::uint64_t>(data.channels);
    if (frames > std::numeric_limits<std::uint32_t>::max()) {
        error = "wav_save: too many frames";
        return false;
    }
    std::uint64_t const data_bytes = data.samples.size() * sizeof(float);
    if (data_bytes > kMaxWavBytes) {
        error = "wav_save: sample data exceeds 512 MB cap";
        return false;
    }

    // Canonical float WAVE: fmt(16) + fact(4) + data.
    std::uint32_t const riff_size = 4 + (8 + 16) + (8 + 4) + 8 +
                                    static_cast<std::uint32_t>(data_bytes);
    std::uint32_t const byte_rate =
        data.sample_rate * static_cast<std::uint32_t>(data.channels) * 4u;
    std::uint16_t const block_align =
        static_cast<std::uint16_t>(data.channels * 4u);

    std::vector<std::uint8_t> hdr;
    hdr.reserve(44 + 24);
    wr_tag(hdr, "RIFF");
    wr_u32(hdr, riff_size);
    wr_tag(hdr, "WAVE");
    wr_tag(hdr, "fmt ");
    wr_u32(hdr, 16);
    wr_u16(hdr, 3);  // IEEE float
    wr_u16(hdr, data.channels);
    wr_u32(hdr, data.sample_rate);
    wr_u32(hdr, byte_rate);
    wr_u16(hdr, block_align);
    wr_u16(hdr, 32);  // bits per sample
    wr_tag(hdr, "fact");
    wr_u32(hdr, 4);  // fact chunk body size
    wr_u32(hdr, static_cast<std::uint32_t>(frames));
    wr_tag(hdr, "data");
    wr_u32(hdr, static_cast<std::uint32_t>(data_bytes));

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        error = "wav_save: cannot open " + path;
        return false;
    }
    bool ok = std::fwrite(hdr.data(), 1, hdr.size(), f) == hdr.size();
    // Stream samples through a small buffer instead of materialising a
    // second full-size byte image of the data.
    std::vector<std::uint8_t> chunk;
    chunk.reserve(4096 * 4);
    if (ok) {
        for (float v : data.samples) {
            std::uint32_t const bits32 = std::bit_cast<std::uint32_t>(v);
            chunk.push_back(static_cast<std::uint8_t>(bits32 & 0xFFu));
            chunk.push_back(static_cast<std::uint8_t>((bits32 >> 8) & 0xFFu));
            chunk.push_back(static_cast<std::uint8_t>((bits32 >> 16) & 0xFFu));
            chunk.push_back(static_cast<std::uint8_t>((bits32 >> 24) & 0xFFu));
            if (chunk.size() >= 4096 * 4) {
                ok = std::fwrite(chunk.data(), 1, chunk.size(), f) ==
                     chunk.size();
                chunk.clear();
                if (!ok) break;
            }
        }
        if (ok && !chunk.empty()) {
            ok = std::fwrite(chunk.data(), 1, chunk.size(), f) ==
                 chunk.size();
        }
    }
    if (std::fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        error = "wav_save: write failed: " + path;
    }
    return ok;
}

}  // namespace addu94::audio
