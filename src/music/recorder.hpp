// ghost_story — src/music/recorder.hpp
//
// Loop recorder (spec §2.5): converts live MIDI + transport position into
// clip events. Loop-aware (wraps tick into the loop window), quantizing
// (on record, per grid), take-per-overdub. Pure logic — tested without
// audio or hardware (virtual MIDI feeds it in CI).
//
// Per-track clips: each pending note binds the Clip it landed in at
// note-on, so switching the selected track mid-note can never misroute the
// note-off — it commits into the note's own clip. Pending matching uses the
// RAW (channel, note) pair while the committed events carry the STORED
// (sounding) ones: keys recorded through octave/scale/pad transforms keep
// their raw identity for the matching off and their transform for playback.
#pragma once

#include <cstdint>
#include <vector>

#include "music/clip.hpp"
#include "music/quantize.hpp"

namespace ghost_story::music {

/// Event ceiling per clip — must match ClipPlayer::kMaxEvents (the RT
/// snapshot array is fixed at this size by design).
inline constexpr std::size_t kMaxClipEvents = 8192;

class Recorder {
public:
    void set_grid(Grid g) noexcept { grid_ = g; }

    /// Note-on at ABSOLUTE transport `tick` into `target`. Returns false
    /// when the clip is at kMaxClipEvents (the take is rejected —
    /// PlayEngine logs once, not per note). The pair stays pending until
    /// the matching raw note-off (sustain across loop wrap clamps to the
    /// loop end). Ticks are stored absolute and converted to clip-local at
    /// commit ((abs − start_tick) wrapped into the loop) — placement is
    /// the clip's own business, not the caller's. The first note into a
    /// clip invalidates its redo stack (same semantic as the old
    /// begin_take: a fresh take would otherwise restore under a reused
    /// take index).
    bool note_on(std::int64_t tick, int raw_channel, int raw_note,
                 int stored_channel, int stored_note, int velocity,
                 Clip& target) {
        std::size_t in_flight = 0;
        for (auto const& p : pending_) {
            if (p.clip == &target) ++in_flight;
        }
        if (target.events.size() + in_flight * 2 + 2 > kMaxClipEvents) {
            return false;
        }
        if (target.has_redo()) target.clear_redo();
        pending_.push_back(Pending{tick, raw_channel, raw_note, stored_channel,
                                   stored_note, velocity,
                                   target.next_take, &target});
        return true;
    }

    /// Commit the pending that matches the RAW (channel, note) pair.
    /// Returns the clip the pair landed in, or nullptr when no pending
    /// matched (the caller journals only real commits).
    Clip* note_off(std::int64_t tick, int raw_channel, int raw_note) {
        for (std::size_t i = 0; i < pending_.size(); ++i) {
            auto& p = pending_[i];
            if (p.raw_note != raw_note || p.raw_channel != raw_channel) {
                continue;
            }
            Clip* const target = p.clip;  // p dies at the erase below
            // Absolute → clip-local: quantize to the GLOBAL grid (bars
            // stay on bars no matter where the clip sits), then shift into
            // the clip's own window and wrap.
            auto const on_tick = wrap_local(
                quantize_tick(grid_, p.tick, 100), target);
            auto const off_tick = wrap_local(
                quantize_tick(grid_, tick, 100), target);
            ClipEvent on{on_tick, static_cast<std::uint8_t>(p.channel),
                         static_cast<std::uint8_t>(p.note),
                         static_cast<std::uint8_t>(p.velocity), true, p.take};
            ClipEvent off{off_tick, static_cast<std::uint8_t>(p.channel),
                          static_cast<std::uint8_t>(p.note), 0, false, p.take};
            target->insert_sorted(on);
            target->insert_sorted(off);
            target->next_take = p.take + 1;
            pending_.erase(pending_.begin() +
                           static_cast<std::ptrdiff_t>(i));
            return target;
        }
        return nullptr;
    }

    /// Force-close every pending into its own clip at the loop end (avoids
    /// stuck notes when recording stops mid-note).
    void flush_pending() {
        for (auto const& p : pending_) {
            ClipEvent on{wrap_local(quantize_tick(grid_, p.tick, 100),
                                    p.clip),
                         static_cast<std::uint8_t>(p.channel),
                         static_cast<std::uint8_t>(p.note),
                         static_cast<std::uint8_t>(p.velocity), true, p.take};
            ClipEvent off{p.clip->length_ticks - 1,
                          static_cast<std::uint8_t>(p.channel),
                          static_cast<std::uint8_t>(p.note), 0, false, p.take};
            p.clip->insert_sorted(on);
            p.clip->insert_sorted(off);
            p.clip->next_take = p.take + 1;
        }
        pending_.clear();
    }

    [[nodiscard]] std::size_t pending_count() const noexcept {
        return pending_.size();
    }

private:
    struct Pending {
        std::int64_t tick;   // ABSOLUTE transport tick at note-on
        int raw_channel;     // matching identity (what the device sent)
        int raw_note;
        int channel;         // stored/sounding identity (post-transform)
        int note;
        int velocity;
        int take;            // resolved from the target clip at note-on
        Clip* clip;
    };
    Grid grid_{Grid::Sixteenth};
    std::vector<Pending> pending_;

    /// Absolute (already-quantized) tick → clip-local, wrapped into
    /// [0, length): shift by the clip's placement, negative-safe.
    static std::int64_t wrap_local(std::int64_t abs_tick, Clip const* c) {
        std::int64_t const len = c->length_ticks > 0 ? c->length_ticks : 1;
        std::int64_t const local = abs_tick - c->start_tick;
        std::int64_t const wrapped = local % len;
        return wrapped < 0 ? wrapped + len : wrapped;
    }
};

}  // namespace ghost_story::music
