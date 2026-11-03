// addu94 — src/device/probe.hpp
//
// Runtime device probe (spec §1): enumerate ALSA ports, match MPK variants,
// send MIDI Identity Request, capture raw traffic (60 s window or
// until stopped), and write the manifest. All empirical — nothing hardcoded.
#pragma once

#include "device/manifest.hpp"
#include "midi/alsa_client.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace addu94::device {

struct ProbeResult {
    bool device_found{false};
    Manifest manifest;
    std::string summary;  // human-readable one-pager
};

/// Raw capture record (spec §1: log every SysEx/unrecognized message).
struct CaptureRecord {
    std::uint64_t stamp_ns;
    std::uint16_t client;
    std::uint16_t port;
    std::vector<std::uint8_t> bytes;
};

class Probe {
public:
    /// Run a probe. `capture_seconds` > 0 also records raw traffic for that
    /// window (`addu94 probe --capture`). Blocking; safe on control thread.
    ProbeResult run(int capture_seconds = 0);

    /// Convenience: scan-only (no capture), used by tests.
    static ProbeResult quick_scan();

    /// Called by the capture sink (input thread) — moves one record in.
    void push_capture(CaptureRecord&& rec) { captured_.push_back(std::move(rec)); }

    std::vector<CaptureRecord> const& captured() const { return captured_; }

private:
    std::vector<CaptureRecord> captured_;
};

}  // namespace addu94::device
