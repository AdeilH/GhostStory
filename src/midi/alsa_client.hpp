// addu94 — src/midi/alsa_client.hpp
//
// ALSA sequencer client: dedicated realtime-ish input thread feeding events
// to a sink through the RT contract (no locks, no allocation on the input
// path beyond ALSA's own internals), device hotplug watching (system
// announce port), auto-connect, SysEx send for probe/identity, and virtual
// ports so every test runs hardware-free (spec §11).
//
// Threading model:
//  - input thread (owned): poll() on sequencer fds → decode → sink->on_midi
//    → heartbeat update. No logging in the hot path; stats via atomics.
//  - control (any other thread): open/start/stop/connect/send_sysex.
#pragma once

#include "midi/types.hpp"

#include <alsa/asoundlib.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace addu94::midi {

/// Discovered ALSA port (an input-capable port of a device/client).
struct PortRef {
    int client{-1};
    int port{-1};
    std::string client_name;
    std::string port_name;
    bool midi_generic{false};
};

/// Device-graph notifications (hotplug) delivered on the input thread.
enum class DeviceEventType : std::uint8_t { PortAdded, PortRemoved, ClientRemoved };

struct DeviceEvent {
    DeviceEventType type;
    int client;
    int port;
    std::string name;
};

/// Consumer of decoded MIDI. on_midi is called from the input thread —
/// implementations must be RT-safe (no locks, no allocation, no logging).
/// on_sysex_raw is optional (probe/capture); it receives the complete
/// F0..F7 payload including delimiters.
struct EventSink {
    virtual ~EventSink() = default;
    virtual void on_midi(MidiEvent const& ev) = 0;
    virtual void on_device_event(DeviceEvent const& de) = 0;
    virtual void on_sysex_raw(std::uint8_t const* /*data*/, std::size_t /*len*/) {}
};

class AlsaClient {
public:
    AlsaClient() = default;
    ~AlsaClient();
    AlsaClient(AlsaClient const&) = delete;
    AlsaClient& operator=(AlsaClient const&) = delete;

    /// Open a client. When `create_virtual` is true a duplex virtual port
    /// (for tests and internal routing) is created. Returns false on ALSA
    /// failure (details logged).
    bool open(std::string_view client_name, bool create_virtual);
    void close() noexcept;

    bool start();
    void stop() noexcept;

    [[nodiscard]] bool is_open() const noexcept { return seq_ != nullptr; }
    [[nodiscard]] int client_id() const noexcept { return client_id_; }
    /// Port ids (for tests / advanced routing).
    [[nodiscard]] int input_port() const noexcept { return in_port_; }
    [[nodiscard]] int output_port() const noexcept { return out_port_; }
    [[nodiscard]] int virtual_port() const noexcept { return virt_port_; }
    /// Raw handle — only for tests and probe (never on the RT path).
    [[nodiscard]] snd_seq_t* raw() noexcept { return seq_; }

    /// Enumerate all readable MIDI ports (non-RT).
    [[nodiscard]] static std::vector<PortRef> enumerate_input_ports();

    /// Subscribe the given external port's output to our input port.
    bool connect_input(int client, int port);
    /// Subscribe our output port to the device's writable port (SysEx/LEDs).
    bool connect_output(int client, int port);
    void disconnect_all() noexcept;

    /// Send a SysEx blob to a connected device output (non-RT, probe path).
    bool send_sysex(std::uint8_t const* data, std::size_t len);

    /// Watchdog heartbeat: CLOCK_MONOTONIC ns of last input-thread progress.
    [[nodiscard]] std::uint64_t heartbeat_ns() const noexcept {
        return heartbeat_ns_.load(std::memory_order_acquire);
    }

    // Stats (relaxed; diagnostics only).
    [[nodiscard]] std::uint64_t events_received() const noexcept {
        return events_received_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t decode_errors() const noexcept {
        return decode_errors_.load(std::memory_order_relaxed);
    }

    void set_sink(EventSink* sink) noexcept;

private:
    void input_loop();
    void handle_seq_event(snd_seq_event_t* ev);
    void deliver(MidiEvent ev) noexcept;

    snd_seq_t* seq_{nullptr};
    int client_id_{-1};
    int in_port_{-1};
    int out_port_{-1};
    int virt_port_{-1};

    EventSink* sink_{nullptr};  // set before start(), never mutated while running

    std::thread input_thread_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> heartbeat_ns_{0};
    std::atomic<std::uint64_t> events_received_{0};
    std::atomic<std::uint64_t> decode_errors_{0};

    std::mutex conn_mutex_;  // control-thread only (connect/disconnect/scan)
    std::vector<std::pair<int, int>> connected_inputs_;
};

/// Best-effort model match for an MPK Mini variant from ALSA names.
/// Returns "" when the port is not a recognised MPK.
[[nodiscard]] std::string_view mpk_model_from_name(std::string_view client_name);

}  // namespace addu94::midi
