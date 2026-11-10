// ghost_story — src/music/arp.hpp
//
// Software arpeggiator + note repeat (spec §5): for units whose hardware
// ARP/note-repeat isn't reachable, these run at the transport grid.
// Pure functions over (held notes, tick window) — property-tested, no
// audio dependencies.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace ghost_story::music {

enum class ArpMode : std::uint8_t { Up, Down, UpDown, Random, Order, Chord };

[[nodiscard]] constexpr const char* to_string(ArpMode m) noexcept {
    switch (m) {
    case ArpMode::Up: return "up";
    case ArpMode::Down: return "down";
    case ArpMode::UpDown: return "up-down";
    case ArpMode::Random: return "random";
    case ArpMode::Order: return "order";
    case ArpMode::Chord: return "chord";
    }
    return "?";
}

/// Arp state machine. Feed `on_grid_tick` for every grid line crossing;
/// it returns the notes to strike at that line.
class Arp {
public:
    void set_mode(ArpMode m) noexcept { mode_ = m; }
    void set_octaves(int o) noexcept { octaves_ = o < 1 ? 1 : (o > 4 ? 4 : o); }
    void set_gate(float g) noexcept { gate_ = g < 0.1f ? 0.1f : (g > 1.f ? 1.f : g); }

    void note_on(int note) {
        if (std::find(held_.begin(), held_.end(), note) == held_.end()) {
            held_.push_back(note);
            step_ = 0;
        }
    }
    void note_off(int note) {
        std::erase(held_, note);
        step_ = 0;
    }
    [[nodiscard]] bool empty() const noexcept { return held_.empty(); }
    [[nodiscard]] std::size_t held_count() const noexcept { return held_.size(); }

    /// Notes to strike on this grid line. `tick_counter` is reserved for
    /// future gate/length handling; `rng` = arbitrary changing value (e.g.
    /// tick) used for Random mode. Returned notes are absolute MIDI notes.
    [[nodiscard]] std::vector<int> strike(std::uint64_t /*tick_counter*/,
                                          std::uint64_t rng) const {
        std::vector<int> out;
        if (held_.empty()) return out;
        std::vector<int> pool;
        pool.reserve(held_.size() * static_cast<std::size_t>(octaves_));
        for (int oct = 0; oct < octaves_; ++oct) {
            for (int n : held_) pool.push_back(n + 12 * oct);
        }
        if (mode_ == ArpMode::Chord) {
            out = pool;
            return out;
        }
        if (mode_ == ArpMode::Random) {
            out.push_back(pool[static_cast<std::size_t>(rng) % pool.size()]);
            return out;
        }
        if (mode_ == ArpMode::Order) {
            out.push_back(held_[step_ % held_.size()]);
            return out;
        }
        // Up / Down / UpDown walk the sorted pool.
        std::vector<int> sorted = pool;
        std::sort(sorted.begin(), sorted.end());
        std::size_t const n = sorted.size();
        if (mode_ == ArpMode::Up) {
            out.push_back(sorted[step_ % n]);
        } else if (mode_ == ArpMode::Down) {
            out.push_back(sorted[(n - 1) - (step_ % n)]);
        } else {  // UpDown: bounce without repeating ends
            std::size_t const period = (n > 1) ? 2 * n - 2 : 1;
            std::size_t const s = step_ % period;
            std::size_t const idx = s < n ? s : period - s;
            out.push_back(sorted[idx]);
        }
        return out;
    }

    void advance_step() noexcept { ++step_; }

private:
    ArpMode mode_{ArpMode::Up};
    int octaves_{1};
    float gate_{0.5f};
    std::vector<int> held_;
    std::uint64_t step_{0};
};

/// Note repeat: holding a pad re-triggers it at the grid rate (spec §5).
/// Pure: given held pads and whether this tick is a grid line, return
/// which pads to (re)strike. `latched` pads repeat without being held.
class NoteRepeat {
public:
    void set_latch(bool on) noexcept { latched_ = on; }
    [[nodiscard]] bool latched() const noexcept { return latched_; }

    void press(int note) {
        if (std::find(held_.begin(), held_.end(), note) == held_.end())
            held_.push_back(note);
    }
    void release(int note) { std::erase(held_, note); }

    /// Pads to strike on a grid line: all held (or latched) notes.
    /// The FIRST press of a note should strike immediately regardless of
    /// grid — caller handles that; this returns repeats only.
    [[nodiscard]] std::vector<int> repeating() const {
        return latched_ ? latched_notes_ : held_;
    }

    void set_latched_notes(std::vector<int> notes) {
        latched_notes_ = std::move(notes);
    }

private:
    bool latched_{false};
    std::vector<int> held_;
    std::vector<int> latched_notes_;
};

}  // namespace ghost_story::music
