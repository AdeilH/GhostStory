// ghost_story — tools/mpk_monitor.cpp (dev tool, not shipped)
// Live monitor: connects to the MPK Mini and prints decoded events for
// N seconds. Usage: mpk_monitor [seconds]
#include "midi/alsa_client.hpp"
#include <chrono>
#include <cstdio>
#include <thread>

using namespace ghost_story::midi;

struct PrintSink : EventSink {
    void on_midi(MidiEvent const& ev) override {
        std::printf("[%6.2fs] %-9s ch=%2u d1=%3u d2=%3u\n",
                    std::chrono::duration<double>(std::chrono::nanoseconds{ev.stamp_ns - t0_}).count(),
                    to_string(ev.type), ev.channel, ev.d1, ev.d2);
    }
    void on_device_event(DeviceEvent const& de) override {
        std::printf("[device] event=%d client=%d port=%d\n", (int)de.type, de.client, de.port);
    }
    void on_sysex_raw(std::uint8_t const* data, std::size_t len) override {
        std::printf("[sysex %zu] ", len);
        for (std::size_t i = 0; i < len && i < 64; ++i)
            std::printf("%02X ", data[i]);
        std::printf("\n");
    }
    std::uint64_t t0_{0};
    void set_t0(std::uint64_t t) { t0_ = t; }
};

int main(int argc, char** argv) {
    int const seconds = argc > 1 ? std::atoi(argv[1]) : 10;
    AlsaClient client;
    if (!client.open("ghost_story-monitor", true)) { std::fprintf(stderr, "no ALSA\n"); return 1; }
    PrintSink sink; sink.set_t0(client.heartbeat_ns());
    client.set_sink(&sink);
    client.start();

    auto ports = AlsaClient::enumerate_input_ports();
    bool connected = false;
    for (auto const& p : ports) {
        if (!mpk_model_from_name(p.client_name).empty()) {
            std::printf("connecting to %s (%d:%d)\n", p.client_name.c_str(), p.client, p.port);
            connected = client.connect_input(p.client, p.port);
            client.connect_output(p.client, p.port);
        }
    }
    if (!connected) { std::fprintf(stderr, "no MPK found\n"); return 2; }

    // Identity request
    std::uint8_t const idreq[] = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};
    client.send_sysex(idreq, sizeof(idreq));

    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    std::printf("--- done: %lu events, %lu decode errors\n",
                (unsigned long)client.events_received(), (unsigned long)client.decode_errors());
    client.stop();
    return 0;
}
