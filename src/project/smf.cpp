// ghost_story — src/project/smf.cpp
#include "project/smf.hpp"

#include "music/transport.hpp"  // kPPQ

#include <cstdio>

namespace ghost_story::project {

namespace {

void push_be32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

void push_be16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

void push_vlq(std::vector<std::uint8_t>& out, std::uint32_t v) {
    std::uint8_t buf[5];
    int n = 0;
    buf[n++] = static_cast<std::uint8_t>(v & 0x7F);
    v >>= 7;
    while (v > 0) {
        buf[n++] = static_cast<std::uint8_t>((v & 0x7F) | 0x80);
        v >>= 7;
    }
    while (n > 0) out.push_back(buf[--n]);
}

std::vector<std::uint8_t> make_chunk(char const* id,
                                     std::vector<std::uint8_t> const& body) {
    std::vector<std::uint8_t> out;
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(id[i]));
    push_be32(out, static_cast<std::uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

}  // namespace

std::vector<std::uint8_t> export_smf_type1(music::Clip const& clip,
                                           double tempo_bpm, int meter_num,
                                           int meter_den) {
    // Header chunk: type 1, one clip → 2 tracks (tempo + clip).
    std::vector<std::uint8_t> header;
    push_be16(header, 1);  // format
    push_be16(header, 2);  // ntrks
    push_be16(header, static_cast<std::uint16_t>(music::kPPQ));  // division

    // Track 0: tempo + time signature + EOT.
    std::vector<std::uint8_t> t0;
    push_vlq(t0, 0);
    t0.push_back(0xFF); t0.push_back(0x51); t0.push_back(0x03);
    std::uint32_t const mpqn =
        static_cast<std::uint32_t>(60'000'000.0 / (tempo_bpm > 0 ? tempo_bpm : 120));
    t0.push_back(static_cast<std::uint8_t>(mpqn >> 16));
    t0.push_back(static_cast<std::uint8_t>(mpqn >> 8));
    t0.push_back(static_cast<std::uint8_t>(mpqn));
    push_vlq(t0, 0);
    t0.push_back(0xFF); t0.push_back(0x58); t0.push_back(0x04);
    int den_pow = 0;
    for (int d = meter_den; d > 1; d >>= 1) ++den_pow;
    t0.push_back(static_cast<std::uint8_t>(meter_num));
    t0.push_back(static_cast<std::uint8_t>(den_pow));
    t0.push_back(24);  // clocks per metronome
    t0.push_back(8);   // 32nds per quarter
    push_vlq(t0, 0);
    t0.push_back(0xFF); t0.push_back(0x2F); t0.push_back(0x00);
    std::vector<std::uint8_t> bytes = make_chunk("MThd", header);
    auto tr0 = make_chunk("MTrk", t0);
    bytes.insert(bytes.end(), tr0.begin(), tr0.end());

    // Track 1: clip events. Running delta times; channel events in order.
    // Placement: events shift by the clip's start_tick so the exported
    // MIDI matches where the clip sits on the timeline.
    std::vector<std::uint8_t> t1;
    std::uint32_t last_tick = 0;
    auto const start = clip.start_tick > 0
                           ? static_cast<std::uint32_t>(clip.start_tick)
                           : 0u;
    if (start > 0) {
        push_vlq(t1, start);  // leading delta to the clip's first position
        last_tick = start;
    }
    for (auto const& e : clip.events) {
        if (e.tick < 0) continue;
        auto const tick = start + static_cast<std::uint32_t>(e.tick);
        push_vlq(t1, tick - last_tick);
        last_tick = tick;
        std::uint8_t const status =
            static_cast<std::uint8_t>((e.on ? 0x90 : 0x80) | (e.channel & 0x0F));
        t1.push_back(status);
        t1.push_back(e.note & 0x7F);
        t1.push_back(e.velocity & 0x7F);
    }
    // Pad to loop end then EOT.
    if (clip.length_ticks > 0 &&
        last_tick < start + static_cast<std::uint32_t>(clip.length_ticks)) {
        push_vlq(t1, start + static_cast<std::uint32_t>(clip.length_ticks) -
                         last_tick);
    } else {
        push_vlq(t1, 0);
    }
    t1.push_back(0xFF); t1.push_back(0x2F); t1.push_back(0x00);
    auto tr1 = make_chunk("MTrk", t1);
    bytes.insert(bytes.end(), tr1.begin(), tr1.end());
    return bytes;
}

bool write_smf_file(std::string const& path,
                    std::vector<std::uint8_t> const& bytes) {
    std::string const tmp = path + ".tmp";
    {
        std::FILE* f = std::fopen(tmp.c_str(), "wb");
        if (f == nullptr) return false;
        std::size_t const n = std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
        if (n != bytes.size()) {
            std::remove(tmp.c_str());
            return false;
        }
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace ghost_story::project
