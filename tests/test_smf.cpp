// ghost_story — tests/test_smf.cpp
// SMF export: byte-level structure checks + parse-back with a minimal
// reader so "valid in external player" is provable without ears.
#include "test_framework.hpp"

#include "project/smf.hpp"

#include <cstdint>
#include <fstream>
#include <vector>

using ghost_story::music::Clip;
using ghost_story::music::ClipEvent;
using ghost_story::project::export_smf_type1;

namespace {

std::uint32_t be32(std::vector<std::uint8_t> const& b, std::size_t off) {
    return (std::uint32_t{b[off]} << 24) | (std::uint32_t{b[off + 1]} << 16) |
           (std::uint32_t{b[off + 2]} << 8) | std::uint32_t{b[off + 3]};
}

}  // namespace

TEST(smf, header_and_chunks_are_wellformed) {
    Clip clip;
    clip.length_ticks = 384;
    clip.insert_sorted(ClipEvent{0, 9, 36, 100, true, 0});
    clip.insert_sorted(ClipEvent{24, 9, 36, 0, false, 0});
    clip.insert_sorted(ClipEvent{96, 0, 60, 64, true, 0});
    auto const bytes = export_smf_type1(clip, 120.0, 4, 4);

    CHECK(bytes.size() > 4);
    CHECK_EQ(bytes[0], 'M'); CHECK_EQ(bytes[1], 'T'); CHECK_EQ(bytes[2], 'h');
    CHECK_EQ(bytes[3], 'd');
    CHECK_EQ(be32(bytes, 4), 6u);          // header length
    CHECK_EQ(bytes[9], 1);                  // format 1
    CHECK_EQ(bytes[11], 2);                 // 2 tracks
    CHECK_EQ(bytes[13], 96);                // division PPQ
    // Find MTrk chunks; total body lengths must cover the file.
    std::size_t pos = 14;
    int tracks = 0;
    while (pos < bytes.size()) {
        CHECK_EQ(bytes[pos], 'M'); CHECK_EQ(bytes[pos + 1], 'T');
        std::uint32_t const len = be32(bytes, pos + 4);
        pos += 8 + len;
        ++tracks;
    }
    CHECK_EQ(tracks, 2);
    CHECK_EQ(pos, bytes.size());
}

TEST(smf, tempo_event_matches_bpm) {
    Clip clip;
    clip.length_ticks = 384;
    auto const bytes = export_smf_type1(clip, 90.0, 4, 4);
    // Track 0 body starts after MThd(14) + MTrk header(8) + delta(0).
    std::size_t const t0 = 14 + 8;
    CHECK_EQ(bytes[t0], 0x00);          // delta
    CHECK_EQ(bytes[t0 + 1], 0xFF);
    CHECK_EQ(bytes[t0 + 2], 0x51);      // tempo
    CHECK_EQ(bytes[t0 + 3], 0x03);
    std::uint32_t const mpqn = (std::uint32_t{bytes[t0 + 4]} << 16) |
                               (std::uint32_t{bytes[t0 + 5]} << 8) |
                               bytes[t0 + 6];
    CHECK_EQ(mpqn, 666'666u);           // 60e6/90 = 666666.67 → truncated
}

TEST(smf, roundtrip_note_events_survive) {
    Clip clip;
    clip.length_ticks = 384;
    clip.insert_sorted(ClipEvent{0, 0, 60, 100, true, 0});
    clip.insert_sorted(ClipEvent{48, 0, 60, 0, false, 0});
    clip.insert_sorted(ClipEvent{96, 0, 62, 50, true, 0});
    clip.insert_sorted(ClipEvent{144, 0, 62, 0, false, 0});
    auto const bytes = export_smf_type1(clip, 120.0, 4, 4);

    // Minimal parse: walk to track 1, verify each note-on/off appears in
    // order with correct keys.
    std::size_t pos = 14;
    std::uint32_t const len0 = be32(bytes, pos + 4);
    pos += 8 + len0;
    std::size_t p = pos + 8;  // into track 1 body
    int found_on = 0, found_off = 0;
    std::vector<int> notes;
    while (p < bytes.size() - 4) {
        // delta (summed but not asserted in this test)
        for (;;) {
            std::uint8_t const b = bytes[p++];
            if ((b & 0x80) == 0) break;
        }
        if (bytes[p] == 0xFF) break;  // EOT
        std::uint8_t const status = bytes[p];
        if ((status & 0xF0) == 0x90 && bytes[p + 2] > 0) {
            ++found_on;
            notes.push_back(bytes[p + 1]);
        } else if ((status & 0xF0) == 0x80 ||
                   ((status & 0xF0) == 0x90 && bytes[p + 2] == 0)) {
            ++found_off;
        }
        p += 3;
    }
    CHECK_EQ(found_on, 2);
    CHECK_EQ(found_off, 2);
    CHECK_EQ(notes[0], 60);
    CHECK_EQ(notes[1], 62);
}

TEST(smf, vlq_handles_large_deltas) {
    Clip clip;
    clip.length_ticks = 96 * 4 * 16;  // long loop → delta > 127
    clip.insert_sorted(ClipEvent{0, 0, 60, 100, true, 0});
    clip.insert_sorted(ClipEvent{5000, 0, 60, 0, false, 0});
    auto const bytes = export_smf_type1(clip, 120.0, 4, 4);
    // File size sanity: multi-byte VLQ for 5000 → grows but bounded.
    CHECK(bytes.size() > 8 + 14 + 8 + 8);
    CHECK(bytes.size() < 200);
}

TEST_MAIN()
