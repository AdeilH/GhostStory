// addu94 — src/midi/types.hpp
//
// Core MIDI event model. MidiEvent is fixed-size and trivially copyable so
// it rides the SPSC queues between the ALSA input thread, the mapping layer
// and the music engine (RT paths — no allocation ever).
//
// SysEx policy: the realtime event path carries only short inline payloads
// (up to kSysExInline bytes; longer blobs are truncated with a flag — they
// only matter for probe/capture, which uses the non-RT RawSysexRecord path
// instead). This keeps the RT event small, cache-friendly and lock-free.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace addu94::midi {

enum class Type : std::uint8_t {
    NoteOff,
    NoteOn,
    PolyAftertouch,
    CC,
    ProgramChange,
    ChannelAftertouch,
    PitchBend,
    SysEx,        // inline payload, see SysEx policy above
    ClockTick,    // F8
    ClockStart,   // FA
    ClockContinue,// FB
    ClockStop,    // FC
    SongPosition, // F2 (d1 = LSB, d2 = MSB)
    Other,
};

struct MidiEvent {
    Type type{Type::Other};
    std::uint8_t channel{0};   // 0-15
    std::uint8_t d1{0};        // note / cc number / LSB
    std::uint8_t d2{0};        // velocity / cc value / MSB
    std::uint8_t raw_status{0};// original status byte (for audit/probe)
    std::uint64_t stamp_ns{0}; // CLOCK_MONOTONIC at input (arrival stamp)

    static constexpr std::size_t kSysExInline = 24;
    std::uint8_t sysex_len{0};
    bool sysex_truncated{false};
    std::array<std::uint8_t, kSysExInline> sysex{};

    // Convenience accessors ------------------------------------------------
    [[nodiscard]] bool is_note_on() const noexcept { return type == Type::NoteOn && d2 > 0; }
    [[nodiscard]] bool is_note_off() const noexcept {
        return type == Type::NoteOff || (type == Type::NoteOn && d2 == 0);
    }
    [[nodiscard]] int note() const noexcept { return d1; }
    [[nodiscard]] int velocity() const noexcept { return d2; }
    [[nodiscard]] int controller() const noexcept { return d1; }
    [[nodiscard]] int value() const noexcept { return d2; }
    [[nodiscard]] int pitch_bend_14() const noexcept {  // 0..16383, 8192 = center
        return (static_cast<int>(d2) << 7) | static_cast<int>(d1);
    }
};

static_assert(sizeof(MidiEvent) <= 64, "RT event must stay cache-cell small");

/// Human-ish name for logs/UI.
[[nodiscard]] constexpr const char* to_string(Type t) noexcept {
    switch (t) {
    case Type::NoteOff: return "NoteOff";
    case Type::NoteOn: return "NoteOn";
    case Type::PolyAftertouch: return "PolyAT";
    case Type::CC: return "CC";
    case Type::ProgramChange: return "ProgramChange";
    case Type::ChannelAftertouch: return "ChanAT";
    case Type::PitchBend: return "PitchBend";
    case Type::SysEx: return "SysEx";
    case Type::ClockTick: return "ClockTick";
    case Type::ClockStart: return "ClockStart";
    case Type::ClockContinue: return "ClockContinue";
    case Type::ClockStop: return "ClockStop";
    case Type::SongPosition: return "SongPosition";
    case Type::Other: return "Other";
    }
    return "?";
}

/// Classify a raw MIDI byte stream status byte into our Type (channel events
/// lose their channel into `channel`). Returns Type::Other for unknown.
[[nodiscard]] constexpr Type classify(std::uint8_t status) noexcept {
    switch (status & 0xF0u) {
    case 0x80: return Type::NoteOff;
    case 0x90: return Type::NoteOn;
    case 0xA0: return Type::PolyAftertouch;
    case 0xB0: return Type::CC;
    case 0xC0: return Type::ProgramChange;
    case 0xD0: return Type::ChannelAftertouch;
    case 0xE0: return Type::PitchBend;
    default: break;
    }
    switch (status) {
    case 0xF2: return Type::SongPosition;
    case 0xF8: return Type::ClockTick;
    case 0xFA: return Type::ClockStart;
    case 0xFB: return Type::ClockContinue;
    case 0xFC: return Type::ClockStop;
    default: return Type::Other;
    }
}

}  // namespace addu94::midi
