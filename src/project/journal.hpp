// addu94 — src/project/journal.hpp
//
// Write-ahead journal for the project document (spec §6.3). Every edit is
// appended to <dir>/project.journal as one JSON line and fsynced before it
// is acknowledged, so a crash at any point loses at most the append that
// was in flight — far under the "≤ 1 second of lost edits" budget.
//
// Journal file format: one JSON object per line, keys sorted (deterministic
// bytes):
//   {"f":0.0,"i":0,"op":"name","s":"...","seq":N,"ts":N}
// op names are the JournalOp enum names lowercased with underscores
// ("checkpoint", "set_tempo", "clip_record", ...).
//
// Durability protocol:
//   - append(): write line + fflush + fsync per entry (appends are small
//     and rare compared to the audio path).
//   - save_atomic(): write project.json.tmp + fsync + rename to
//     project.json (atomic), then the journal rotates to
//     project.journal.1 and a fresh empty journal starts (one previous
//     generation is kept; nothing is deleted silently).
//   - load_project(): parse project.json (quarantining a corrupt file as
//     project.json.corrupt-<ts> and falling back to defaults), then replay
//     the journal on top.
#pragma once

#include "project/document.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace addu94::project {

// One journaled edit. ops are small, forward-only, and replayable.
enum class JournalOp : std::uint8_t {
    Checkpoint,       // payload = full project JSON (full save point)
    SetTempo,         // i = bpm*100 (int)
    SetMeter,         // i = num*1000+den
    SetKey, SetScale, // s
    AddTrack,         // s = track name
    RemoveTrack,      // i = index
    // s = "volume|pan|mute|solo" with the value in f (mute/solo: f != 0),
    // or "name=<value>" / "instrument=<value>"; i = track index.
    SetTrackParam,
    // s = JSON object of one ClipEvent, i = track index, f = clip index
    // (the clip slot is created on demand when missing).
    ClipRecord,
    ClipClear,        // i = track index, f = clip index (no-op if absent)
};

struct JournalEntry {
    std::uint64_t seq{0};
    std::uint64_t ts_ns{0};
    JournalOp op{JournalOp::Checkpoint};
    std::int64_t i{0};
    double f{0.0};
    std::string s;
};

class Journal {
public:
    Journal() = default;
    ~Journal();
    Journal(Journal const&) = delete;
    Journal& operator=(Journal const&) = delete;

    /// Opens/creates <dir>/project.journal. Reads existing entries to
    /// restore the seq counter (crash recovery), validating each line; a
    /// corrupt tail (truncated last line) is tolerated: the valid prefix is
    /// kept and the file is truncated back to it.
    bool open(std::string const& dir, std::string& error);
    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept;

    /// Append one entry (write line + flush + fsync). Returns false on I/O
    /// error. seq == 0 is filled with next_seq(); ts_ns == 0 with now.
    bool append(JournalEntry const& e);

    /// Replay all entries in order onto the project. Returns false if the
    /// journal contains an op that cannot be applied (error set).
    bool replay(Project& p, std::string& error) const;

    /// Full checkpoint: append a Checkpoint entry with the complete project
    /// JSON.
    bool checkpoint(Project const& p);

    /// Atomic full save: write project.json.tmp + fsync + rename to
    /// project.json (spec §6.3). Then truncate the journal (safe: the
    /// checkpoint is durable) by rotating to project.journal.1 — one
    /// previous generation is kept, never deleted silently.
    bool save_atomic(Project const& p, std::string const& dir,
                     std::string& error);

    [[nodiscard]] std::vector<JournalEntry> const& entries() const noexcept;
    [[nodiscard]] std::uint64_t next_seq() const noexcept;

private:
    bool attach_file(std::string const& path, std::string const& dir,
                     std::string& error);

    std::string dir_;
    std::FILE* file_{nullptr};
    std::vector<JournalEntry> entries_;
    std::uint64_t next_seq_{1};
};

/// High-level load: 1) if project.json exists, parse it (a corrupt file is
/// quarantined via a .corrupt-<ts> rename and we start from the default
/// project); 2) replay the journal on top. Returns false only on
/// unrecoverable errors (I/O failure, journal entry that cannot be
/// applied). The presence of un-checkpointed journal entries is
/// informational and is not surfaced in the return value; callers can
/// inspect Journal::entries() after a load if they need it.
bool load_project(std::string const& dir, Project& out, std::string& error);

}  // namespace addu94::project
