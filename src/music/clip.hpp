// ghost_story — src/music/clip.hpp
//
// Pattern clips: loopable MIDI regions recorded from the MPK Mini (spec
// §2.3). Events stored in 96-PPQ ticks. Takes: each punch-in/overdub pass
// is a take; undo per take removes the last take's events (spec §2.5),
// and undo/redo work as stacks (user report: "BANK B doesn't work at all"
// — B2/Redo emitted its action but nothing restored an undone take, so
// undo was destructive with no way back).
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace ghost_story::music {

struct ClipEvent {
    std::int64_t tick{0};   // 0..length_ticks
    std::uint8_t channel{0};
    std::uint8_t note{0};
    std::uint8_t velocity{0};
    bool on{true};          // NoteOn else NoteOff
    int take{0};            // take index that recorded this event
};

/// One recorded take's visual span on the timeline (absolute ticks, the
/// same space as start_tick). Events stay clip-local and wrap into the
/// clip's loop window — boxes are VIEW metadata only: each take draws its
/// own block ([rec 1] [rec 2] both visible), playback still loops the one
/// union window. Overlapping takes merge into one box (an overdub pass
/// over an existing take extends it instead of stacking a twin block).
struct TakeBox {
    int take{0};            // earliest take recorded into this box
    std::int64_t start_tick{0};
    std::int64_t length_ticks{0};
};

struct Clip {
    std::int64_t length_ticks{96 * 4 * 4};  // 4 beats × 4 bars default
    // Timeline placement: the clip's local tick 0 sits at this transport
    // tick (drag the block in the arrangement). Placement is NOT take
    // content — clear()/undo/redo leave it alone.
    std::int64_t start_tick{0};
    std::vector<ClipEvent> events;
    std::vector<TakeBox> boxes;  // visual per-take spans (see TakeBox)
    int next_take{0};

    /// Insert keeping tick order (stable).
    void insert_sorted(ClipEvent ev) {
        auto at = events.begin();
        while (at != events.end() && at->tick <= ev.tick) ++at;
        events.insert(at, ev);
    }

    /// Undo the last take: drop all its events (spec: undo per take).
    /// Returns false when nothing to undo. The take is stashed so
    /// redo_last_take() can restore it (multi-level: undo/undo/redo/redo
    /// walks back and forth through the take history). The take's visual
    /// box rides the stash too, so undo/redo agree with what's on the
    /// canvas.
    bool undo_last_take() {
        if (next_take == 0) return false;
        int const last = next_take - 1;
        RedoEntry r;
        r.take = last;
        for (auto const& e : events) {
            if (e.take == last) r.events.push_back(e);
        }
        events.erase(std::remove_if(events.begin(), events.end(),
                                    [last](ClipEvent const& e) {
                                        return e.take == last;
                                    }),
                     events.end());
        // Drop this take's box unless an earlier take merged into it (the
        // box carries the earliest take — that take's span must survive).
        for (std::size_t i = 0; i < boxes.size(); ++i) {
            if (boxes[i].take == last) {
                r.box = boxes[i];
                r.had_box = true;
                boxes.erase(boxes.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
        --next_take;
        redo_.push_back(std::move(r));
        return true;
    }

    /// Restore the most recently undone take. Returns false when the redo
    /// stack is empty (or after new recording invalidated it — see
    /// clear_redo, called from Recorder::begin_take).
    bool redo_last_take() {
        if (redo_.empty()) return false;
        RedoEntry r = std::move(redo_.back());
        redo_.pop_back();
        for (auto const& e : r.events) {
            insert_sorted(e);
        }
        if (r.had_box) boxes.push_back(r.box);
        next_take = r.take + 1;  // takes undo in LIFO order, so this is
                                 // always the next index
        return true;
    }

    /// New recording invalidates redo: the stashed take index is about to
    /// be reused by the fresh take, so restoring it would interleave two
    /// takes under one number.
    void clear_redo() { redo_.clear(); }
    [[nodiscard]] bool has_redo() const noexcept { return !redo_.empty(); }

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
        boxes.clear();
        next_take = 0;
        redo_.clear();
    }

    /// Record (or grow) the visual span of the take at [start, start+len).
    /// Overlaps merge into the existing box (an overdub pass extends it);
    /// a span in fresh territory opens its own box. Boxes cover their
    /// whole union span, so the merged box keeps the EARLIEST take index
    /// (undo of the newer take must not erase the older take's visual).
    void place_take_span(int take, std::int64_t start,
                         std::int64_t length) {
        if (length <= 0) return;
        std::int64_t const end = start + length;
        for (TakeBox& b : boxes) {
            std::int64_t const b_end = b.start_tick + b.length_ticks;
            if (b.start_tick < end && start < b_end) {
                b.take = std::min(b.take, take);
                b.start_tick = std::min(b.start_tick, start);
                b.length_ticks = std::max(b_end, end) - b.start_tick;
                return;
            }
        }
        boxes.push_back(TakeBox{take, start, length});
    }

    /// Drop boxes whose span holds no committed events (the live REC box
    /// the transport places on arm is provisional — a take that never
    /// played a note must not leave a ghost block behind). Boxes overlap
    /// after merges, so one event can keep several boxes alive; that is
    /// the honest view of "these takes recorded here".
    void prune_empty_boxes() {
        auto const has_event_in = [&](TakeBox const& b) {
            std::int64_t const local0 = b.start_tick - start_tick;
            std::int64_t const local1 = local0 + b.length_ticks;
            for (auto const& e : events) {
                if (e.tick >= local0 && e.tick < local1) return true;
            }
            return false;
        };
        boxes.erase(std::remove_if(boxes.begin(), boxes.end(),
                                   [&](TakeBox const& b) {
                                       return !has_event_in(b);
                                   }),
                    boxes.end());
    }

    /// Clear ONE take: its events and its box go (right-click a block →
    /// clear that recording). Redo dies with it — same rule as clear().
    void clear_take(int take) {
        events.erase(std::remove_if(events.begin(), events.end(),
                                    [take](ClipEvent const& e) {
                                        return e.take == take;
                                    }),
                     events.end());
        boxes.erase(std::remove_if(boxes.begin(), boxes.end(),
                                   [take](TakeBox const& b) {
                                       return b.take == take;
                                   }),
                    boxes.end());
        redo_.clear();
    }

private:
    struct RedoEntry {
        int take{0};
        std::vector<ClipEvent> events;
        TakeBox box;
        bool had_box{false};
    };
    std::vector<RedoEntry> redo_;
};

}  // namespace ghost_story::music
