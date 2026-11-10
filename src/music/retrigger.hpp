// ghost_story — src/music/retrigger.hpp
//
// Keys play-once guard: a single physical key press must sound exactly one
// note-on. Hardware that re-fires a held note — the MPK mini's onboard
// arpeggiator, a stuck encoder, double-triggering — sends extra NoteOns for
// a (channel, note) that is already sounding; this guard drops them.
//
// Design contract (mirrors the MIDI-thread constraints of PlayEngine::
// on_midi): wait-free, no allocation, no locks. State is a fixed 16 x 128
// table of last-note-on timestamps (steady-clock ns) owned by one thread.
//
// Self-healing: a mark older than kStuckNoteNs counts as released. Hardware
// latch/all-notes-off clears (CC 123) arrive without per-note NoteOffs, so
// without the expiry a latched key would stay silent forever.
#pragma once

#include <array>
#include <cstdint>

namespace ghost_story::music {

class RetriggerGuard {
public:
    /// Once-mode off: every NoteOn passes (stock repeat behaviour).
    void set_active(bool on) noexcept { active_ = on; }
    [[nodiscard]] bool active() const noexcept { return active_; }

    /// `is_note_on` true = genuine NoteOn (velocity > 0); false = any
    /// note-off form (NoteOff or NoteOn-with-velocity-0, which MIDI treats
    /// identically). Returns true when the event may sound.
    [[nodiscard]] bool allow(int channel, int note, bool is_note_on,
                             std::uint64_t now_ns) noexcept {
        if (channel < 0 || channel > 15 || note < 0 || note > 127) {
            return true;  // out-of-range never marks, never blocks
        }
        auto& last = last_on_ns_[static_cast<std::size_t>(channel)]
                               [static_cast<std::size_t>(note)];
        if (!is_note_on) {
            last = 0;  // any note-off form releases the key
            return true;
        }
        if (!active_) {
            last = now_ns;  // keep the stamp so enabling mid-hold is sane
            return true;
        }
        if (last == 0 || now_ns - last >= kStuckNoteNs) {
            // Fresh press (or stale mark self-expired). 0 is the "no mark"
            // sentinel — clamp so a t==0 stamp still counts as marked.
            last = now_ns == 0 ? 1 : now_ns;
            return true;
        }
        return false;  // re-fire of an already-held key: suppress
    }

    /// Panic path: release every mark (transport stop, all-sound-off).
    void clear() noexcept {
        for (auto& ch : last_on_ns_) {
            ch.fill(0);
        }
    }

    /// A mark older than this is treated as released even without a
    /// NoteOff (latch/unlatch via all-notes-off never sends per-note offs).
    static constexpr std::uint64_t kStuckNoteNs = 5ULL * 1'000'000'000ULL;

private:
    std::array<std::array<std::uint64_t, 128>, 16> last_on_ns_{};
    bool active_{true};
};

}  // namespace ghost_story::music
