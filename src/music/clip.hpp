// addu94 — src/music/clip.hpp
//
// Pattern clips: loopable MIDI regions recorded from the MPK Mini (spec
// §2.3). Events stored in 96-PPQ ticks. Takes: each punch-in/overdub pass
// is a take; undo per take removes the last take's events (spec §2.5).
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace addu94::music {

struct ClipEvent {
    std::int64_t tick{0};   // 0..length_ticks
    std::uint8_t channel{0};
    std::uint8_t note{0};
    std::uint8_t velocity{0};
    bool on{true};          // NoteOn else NoteOff
    int take{0};            // take index that recorded this event
};

struct Clip {
    std::int64_t length_ticks{96 * 4 * 4};  // 4 beats × 4 bars default
    std::vector<ClipEvent> events;
    int next_take{0};

    /// Insert keeping tick order (stable).
    void insert_sorted(ClipEvent ev) {
        auto at = events.begin();
        while (at != events.end() && at->tick <= ev.tick) ++at;
        events.insert(at, ev);
    }

    /// Undo the last take: drop all its events (spec: undo per take).
    /// Returns false when nothing to undo.
    bool undo_last_take() {
        if (next_take == 0) return false;
        int const last = next_take - 1;
        events.erase(std::remove_if(events.begin(), events.end(),
                                    [last](ClipEvent const& e) {
                                        return e.take == last;
                                    }),
                     events.end());
        --next_take;
        return true;
    }

    /// All events for `take` recorded at `tick` (overdub view).
    [[nodiscard]] std::vector<ClipEvent> events_in_range(
        std::int64_t from_tick, std::int64_t to_tick) const {
        std::vector<ClipEvent> out;
        for (auto const& e : events) {
            if (e.tick >= from_tick && e.tick < to_tick) out.push_back(e);
        }
        return out;
    }

    void clear() {
        events.clear();
        next_take = 0;
    }
};

}  // namespace addu94::music
