// addu94 — src/project/document.hpp
//
// Project document (spec §2/§8): the song. Serializable to checksummed,
// versioned JSON; edits go through the journal (journal.hpp) so a crash
// loses at most the un-checkpointed tail.
#pragma once

#include "music/clip.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace addu94::project {

struct Track {
    std::string name{"Track"};
    // Mixer state
    float volume{0.8f};   // 0..1.5
    float pan{0.f};       // -1..1
    bool mute{false};
    bool solo{false};
    // Instrument selection ("synth" for M3; "sampler"/"drumkit" M6)
    std::string instrument{"synth"};
    // Clips on this track (pattern clips; arrangement slots reference them)
    std::vector<music::Clip> clips;
};

struct SongSection {
    std::string name{"Section"};   // intro/verse/chorus...
    int start_bar{0};
    int length_bars{4};
};

struct Project {
    int schema{1};
    std::string name{"Untitled"};
    double tempo_bpm{120.0};
    int meter_num{4};
    int meter_den{4};
    std::string key{"C"};
    std::string scale{"major"};
    std::vector<Track> tracks;
    std::vector<SongSection> sections;

    static Project make_default() {
        Project p;
        for (int i = 0; i < 4; ++i) {
            Track t;
            t.name = "Track " + std::to_string(i + 1);
            p.tracks.push_back(std::move(t));
        }
        return p;
    }
};

[[nodiscard]] std::string project_to_json(Project const& p);
[[nodiscard]] bool project_from_json(std::string const& json, Project& out,
                                     std::string& error);

}  // namespace addu94::project
