// addu94 — src/music/recorder.hpp
//
// Loop recorder (spec §2.5): converts live MIDI + transport position into
// clip events. Loop-aware (wraps tick into the loop window), quantizing
// (on record, per grid), take-per-overdub. Pure logic — tested without
// audio or hardware (virtual MIDI feeds it in CI).
#pragma once

#include "music/clip.hpp"
#include "music/quantize.hpp"

namespace addu94::music {

class Recorder {
public:
    void set_clip(Clip* clip) noexcept { clip_ = clip; }
    void set_grid(Grid g) noexcept { grid_ = g; }
    void begin_take() noexcept {
        if (clip_ != nullptr) take_ = clip_->next_take;
    }

    /// Record a note-on at transport `tick` (absolute loop tick). The pair
    /// is kept pending until the matching note-off (sustain across loop
    /// wrap is clamped to the loop end).
    void note_on(std::int64_t tick, int channel, int note, int velocity) {
        if (clip_ == nullptr) return;
        pending_.push_back(Pending{tick, channel, note, velocity});
    }

    /// Returns true when the off closed a pending on.
    bool note_off(std::int64_t tick, int channel, int note) {
        if (clip_ == nullptr) return false;
        for (std::size_t i = 0; i < pending_.size(); ++i) {
            auto& p = pending_[i];
            if (p.note == note && p.channel == channel) {
                auto const on_tick = quantize_tick(grid_, p.tick, 100) %
                                     clip_->length_ticks;
                auto const off_tick =
                    quantize_tick(grid_, tick, 100) % clip_->length_ticks;
                ClipEvent on{on_tick, static_cast<std::uint8_t>(channel),
                             static_cast<std::uint8_t>(note),
                             static_cast<std::uint8_t>(p.velocity), true,
                             take_};
                ClipEvent off{off_tick, static_cast<std::uint8_t>(channel),
                              static_cast<std::uint8_t>(note), 0, false,
                              take_};
                clip_->insert_sorted(on);
                clip_->insert_sorted(off);
                pending_.erase(pending_.begin() +
                               static_cast<std::ptrdiff_t>(i));
                clip_->next_take = take_ + 1;
                return true;
            }
        }
        return false;
    }

    /// Force-close pending notes at loop end (avoids stuck notes when
    /// recording stops mid-note).
    void flush_pending() {
        if (clip_ == nullptr) return;
        for (auto const& p : pending_) {
            ClipEvent on{quantize_tick(grid_, p.tick, 100) %
                             clip_->length_ticks,
                         static_cast<std::uint8_t>(p.channel),
                         static_cast<std::uint8_t>(p.note),
                         static_cast<std::uint8_t>(p.velocity), true, take_};
            ClipEvent off{clip_->length_ticks - 1,
                          static_cast<std::uint8_t>(p.channel),
                          static_cast<std::uint8_t>(p.note), 0, false, take_};
            clip_->insert_sorted(on);
            clip_->insert_sorted(off);
        }
        if (!pending_.empty()) clip_->next_take = take_ + 1;
        pending_.clear();
    }

    [[nodiscard]] std::size_t pending_count() const noexcept {
        return pending_.size();
    }

private:
    struct Pending {
        std::int64_t tick;
        int channel;
        int note;
        int velocity;
    };
    Clip* clip_{nullptr};
    Grid grid_{Grid::Sixteenth};
    int take_{0};
    std::vector<Pending> pending_;
};

}  // namespace addu94::music
