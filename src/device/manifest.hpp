// ghost_story — src/device/manifest.hpp
//
// Device manifest: the data file that makes an MPK variant "a data file,
// not a code change" (spec §1). Produced by the runtime probe at first run,
// stored under ~/.local/share/ghost_story/devices/, versioned, checksummed.
//
// The manifest deliberately records *what we observed* (ports, models,
// capabilities, captured control usage) separately from the *assumed*
// control inventory (the default tables below), so future probes can
// upgrade the inventory without code changes.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ghost_story::device {

constexpr int kManifestSchema = 1;

/// One physical control on the device.
struct Control {
    enum class Kind : std::uint8_t {
        Key, Pad, Knob, Joystick, Button, Pedal,
    };
    Kind kind;
    int index;              // 0-based within kind
    std::string id;         // stable id, e.g. "pad.a3", "knob.5", "key.36"
    std::string label;      // human label, e.g. "Pad A3"
    // Default MIDI mapping (may be overridden by per-project profile.toml).
    int midi_note{-1};      // pads/keys
    int midi_cc{-1};        // knobs/joystick/buttons
    int midi_channel{0};
};

/// Default control inventory per known model. A probe that fails to
/// identify the model ships this "generic-mpk" inventory; identification
/// refines it.
std::vector<Control> default_inventory(std::string const& model);

struct Manifest {
    int schema{kManifestSchema};
    std::string model;               // "" when unidentified
    std::string firmware;            // from identity reply, may be empty
    int alsa_client{-1};
    std::string alsa_client_name;
    struct Port { int id; std::string name; };
    std::vector<Port> ports;
    std::vector<Control> controls;
    // Probe observations
    bool identity_request_supported{false};
    bool has_note_repeat_button{false};  // hardware note repeat (mk3+)
    bool has_spring_pitch{true};         // joystick X springs back
    bool pad_rgb{true};
    std::uint64_t probed_at_ns{0};
    std::string checksum;                // sha256 of canonical payload
};

/// Serialise to canonical JSON (sorted keys, stable) + checksum field.
std::string manifest_to_json(Manifest const& m);
/// Parse + verify checksum. Returns nullopt on corruption (caller then
/// quarantines the file and re-probes).
std::optional<Manifest> manifest_from_json(std::string const& json);

/// Standard path: ~/.local/share/ghost_story/devices/device_manifest.json
std::filesystem::path manifest_path();
/// Atomic write (temp + fsync + rename); returns false on I/O failure.
bool save_manifest(Manifest const& m, std::filesystem::path const& path);
std::optional<Manifest> load_manifest(std::filesystem::path const& path);

/// Quarantine a corrupt file next to itself: <name>.corrupt-<ts> (never
/// deleted — spec §6.8).
bool quarantine_file(std::filesystem::path const& path);

[[nodiscard]] std::string control_kind_to_string(Control::Kind k);
[[nodiscard]] std::optional<Control::Kind> control_kind_from_string(
    std::string const& s);

}  // namespace ghost_story::device
