// addu94 — src/device/probe.cpp
#include "device/probe.hpp"

#include "log/logger.hpp"

#include <algorithm>
#include <sstream>
#include <thread>

namespace addu94::device {

namespace {

// MIDI Identity Request: Universal Non-Realtime, sub-ID 1 = Identity Request.
constexpr std::uint8_t kIdentityRequest[] = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};

// Identity Reply: F0 7E <id> 06 02 <manufacturer> <family:2> <model:2> <ver:4> F7
bool parse_identity_reply(std::uint8_t const* data, std::size_t len,
                          std::string& out_info) {
    if (len < 9 || data[0] != 0xF0 || data[1] != 0x7E || data[3] != 0x06 ||
        data[4] != 0x02 || data[len - 1] != 0xF7) {
        return false;
    }
    std::uint16_t const family = static_cast<std::uint16_t>(data[6] << 8 | data[7]);
    std::uint16_t const model = static_cast<std::uint16_t>(data[8] << 8 | data[9]);
    std::ostringstream ss;
    ss << "manufacturer=0x" << std::hex << static_cast<int>(data[5])
       << " family=0x" << family << " model=0x" << model << " version=";
    for (std::size_t i = 10; i + 1 < len - 1 && i < 14; ++i) {
        ss << std::hex << static_cast<int>(data[i]);
    }
    out_info = ss.str();
    return true;
}

struct CaptureSink : midi::EventSink {
    Probe* probe{nullptr};
    midi::AlsaClient* client{nullptr};
    std::atomic<std::uint64_t> sysex_seen{0};
    std::string identity_info;
    std::atomic<bool> got_identity{false};

    void on_midi(midi::MidiEvent const&) override {}
    void on_device_event(midi::DeviceEvent const&) override {}
    void on_sysex_raw(std::uint8_t const* data, std::size_t len) override {
        if (probe == nullptr || client == nullptr) return;
        CaptureRecord rec;
        rec.stamp_ns = now_ns();
        rec.client = static_cast<std::uint16_t>(client->client_id());
        rec.port = 0;
        rec.bytes.assign(data, data + len);
        probe->push_capture(std::move(rec));
        sysex_seen.fetch_add(1, std::memory_order_relaxed);
        if (!got_identity && parse_identity_reply(data, len, identity_info)) {
            got_identity = true;
        }
    }

    static std::uint64_t now_ns() noexcept {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }
};

}  // namespace

ProbeResult Probe::run(int capture_seconds) {
    ProbeResult result;
    midi::AlsaClient client;
    if (!client.open("addu94-probe", /*create_virtual=*/false)) {
        result.summary = "ALSA sequencer unavailable";
        return result;
    }

    // 1. Enumerate and match.
    auto const ports = midi::AlsaClient::enumerate_input_ports();
    for (auto const& p : ports) {
        if (!p.midi_generic) continue;  // skip app ports (other DAWs etc.)
        auto model = midi::mpk_model_from_name(p.client_name);
        if (model.empty()) continue;
        result.device_found = true;
        result.manifest.model = std::string{model};
        result.manifest.alsa_client = p.client;
        result.manifest.alsa_client_name = p.client_name;
        result.manifest.ports.push_back({p.port, p.port_name});
    }
    // No MPK found — manifest records generic inventory and the observed
    // environment so `doctor` can tell the user exactly what's attached.
    if (result.manifest.model.empty()) {
        result.manifest.model = "";
    }

    // 2. Identity request when we found a device (best-effort).
    if (result.device_found) {
        CaptureSink sink;
        sink.probe = this;
        sink.client = &client;
        client.set_sink(&sink);
        client.start();
        auto const& port = result.manifest.ports.front();
        // The reply comes BACK on the device's output port: we must
        // subscribe it to our input, not only point our output at it.
        if (client.connect_input(result.manifest.alsa_client, port.id) &&
            client.connect_output(result.manifest.alsa_client, port.id)) {
            client.send_sysex(kIdentityRequest, sizeof(kIdentityRequest));
        }

        auto const deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(capture_seconds > 0
                                                       ? capture_seconds
                                                       : 2);
        while (std::chrono::steady_clock::now() < deadline) {
            if (sink.got_identity && capture_seconds <= 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        client.stop();

        if (sink.got_identity) {
            result.manifest.identity_request_supported = true;
            result.manifest.firmware = sink.identity_info;
        }
    }

    // 3. Inventory: refine per model; generic otherwise.
    result.manifest.controls = default_inventory(
        result.manifest.model.empty() ? "generic-mpk" : result.manifest.model);
    // mk3/Play ship a note-repeat button; mk1/mk2 need software repeat.
    result.manifest.has_note_repeat_button =
        result.manifest.model == "mpk-mini-mk3" ||
        result.manifest.model == "mpk-mini-play";
    result.manifest.pad_rgb = result.manifest.model != "mpk-mini-mk1";
    result.manifest.probed_at_ns = static_cast<std::uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count());

    // 4. Human summary.
    std::ostringstream ss;
    if (result.device_found) {
        ss << "MPK found: " << result.manifest.alsa_client_name
           << " (client " << result.manifest.alsa_client << "), model="
           << (result.manifest.model.empty() ? "generic-mpk"
                                             : result.manifest.model)
           << ", controls=" << result.manifest.controls.size()
           << ", identity=" << (result.manifest.identity_request_supported
                                    ? "yes"
                                    : "no");
        if (!captured_.empty()) {
            ss << ", sysex_captured=" << captured_.size();
        }
    } else {
        ss << "No MPK Mini found among " << ports.size()
           << " MIDI ports; manifest records generic inventory. "
              "Plug the device and run `addu94 probe` again.";
    }
    result.summary = ss.str();
    return result;
}

ProbeResult Probe::quick_scan() {
    Probe p;
    return p.run(0);
}

}  // namespace addu94::device
