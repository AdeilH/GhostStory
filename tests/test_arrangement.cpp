// ghost_story — tests/test_arrangement.cpp
#include "test_framework.hpp"

#include "project/arrangement.hpp"

#include <memory>

using namespace ghost_story::project;

TEST(arrangement, song_length_is_max_block_end) {
    Timeline t;
    CHECK_EQ(song_length_bars(t), 0);
    t.push_back(Block{0, 0, 0, 4});
    t.push_back(Block{1, 0, 6, 8});
    CHECK_EQ(song_length_bars(t), 14);
}

TEST(arrangement, blocks_at_finds_overlapping) {
    Timeline t;
    t.push_back(Block{0, 0, 0, 4});
    t.push_back(Block{0, 1, 2, 4});  // overlaps bars 2-3
    t.push_back(Block{1, 0, 0, 4});  // other track
    auto at_bar3 = blocks_at(t, 0, 3);
    CHECK_EQ(at_bar3.size(), 2u);
    auto at_bar5 = blocks_at(t, 0, 5);
    CHECK_EQ(at_bar5.size(), 1u);  // only the 2..5 block
    CHECK_EQ(at_bar5[0].clip, 1);
}

TEST(arrangement, section_lookup_last_match_wins) {
    std::vector<SongSection> secs = {
        {"intro", 0, 4}, {"verse", 4, 8}, {"chorus", 8, 8}};
    CHECK_EQ(section_at(secs, 0)->name, "intro");
    CHECK_EQ(section_at(secs, 7)->name, "verse");
    CHECK_EQ(section_at(secs, 10)->name, "chorus");
    CHECK(section_at(secs, 100) == nullptr);
}

TEST_MAIN()
