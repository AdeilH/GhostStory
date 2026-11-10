// ghost_story — src/audio/sfz.hpp
//
// Practical-subset SFZ v1 parser + Sampler loader (spec §4.1).
//
// Supported opcodes (everything else is ignored, per the SFZ tradition of
// generous readers):
//   sample             file path, "quoted paths with spaces" supported;
//                      resolved relative to the SFZ file's directory,
//                      backslashes accepted and converted to '/'
//   key                single key (0..127)
//   lokey / hikey      key range (0..127, clamped, lo > hi => region skipped)
//   lovel / hivel      velocity range (0..127, clamped)
//   pitch_keycenter    root key; defaults to `key` when unset
//   transpose          semitone shift applied to the zone's midi note
//   volume             region gain in dB, clamped to -90..+12
//   tune               pitch fine-tune in cents, clamped to -100..+100
//
// Headers: <region>, <group> and <global>. <group>/<global> opcodes act as
// defaults for all following <region>s (a <group> resets the defaults).
// Opcodes appearing before any header are treated as group-level defaults
// (common in real files). '//' starts a comment that runs to end of line;
// inside a token a '"' toggles quoting so quoted paths may contain spaces.
// A <region> with no usable `sample` (missing, empty, or escaping the base
// directory via a ".." component — path-traversal guard) is skipped and
// counted in SfzParseResult::skipped_regions. Tokens that are neither a
// header nor key=value are skipped and counted in skipped_lines. Unknown
// opcodes are silently ignored (they are data, not errors).
//
// Deviation note (deliberate): skips are *counted*, not logged — this
// parser is a library with no logging dependency, mirroring wav_file.hpp
// (which reports problems via `error` only). The logger stays out of the
// control path.
//
// tune_cents is parsed and kept so an integrator can apply it (as cents on
// top of transpose); sampler_load_sfz does NOT apply it — the Sampler API
// exposes only whole-semitone per-note mapping.
//
// Robustness contract (mirrors wav_file/toml_min): never throws on user
// input; malformed content degrades to skipped counts. Unreadable SFZ file
// => nullopt + `error`. Sample memory is capped by wav_load's own 512 MB
// limit, which this module inherits (it never reads sample data itself).
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "audio/sampler.hpp"

namespace ghost_story::audio {

/// One parsed <region> (group defaults already folded in).
struct SfzRegion {
    std::string sample_path;   // resolved relative to the SFZ file dir
    int key{-1};               // key opcode
    int lo_key{0}, hi_key{127};
    int lo_vel{0}, hi_vel{127};
    int pitch_keycenter{-1};   // root key; default = key
    int transpose{0};          // semitones
    float volume_db{0.0f};     // volume opcode, clamped to -90..+12
    float tune_cents{0.0f};    // tune opcode, clamped to -100..100 (kept,
                               // NOT applied by sampler_load_sfz; an
                               // integrator may apply it as cents on top
                               // of transpose)
};

struct SfzParseResult {
    std::vector<SfzRegion> regions;
    int skipped_lines{0};    // malformed tokens (not key=value, not header)
    int skipped_regions{0};  // regions dropped: no sample, bad/escaping path
};

/// Parse an SFZ file. Robust: never throws; malformed lines are skipped and
/// counted. <group>/<region> headers both supported (<global> too); group
/// opcodes act as defaults for following regions. Returns the regions;
/// nullopt + `error` only when the file itself is unreadable.
[[nodiscard]] std::optional<SfzParseResult> sfz_load(std::string const& path,
                                                     std::string& error);

/// Convenience: load an SFZ and feed a Sampler. One zone per MIDI note,
/// first matching region wins (later regions mapping to the same note are
/// skipped). Zone note = pitch_keycenter if set, else key if set, else
/// lo_key, plus transpose (which therefore shifts the effective mapping).
/// Per-region volume_db is folded by pre-scaling the loaded WavData samples
/// by 10^(db/20) before add_zone_data (the Sampler has no per-zone gain).
/// Returns false + `error` when the SFZ is unreadable or produced zero
/// usable zones; individual broken regions are skipped without failing the
/// whole load.
bool sampler_load_sfz(Sampler& sampler, std::string const& sfz_path,
                      std::string& error);

}  // namespace ghost_story::audio
