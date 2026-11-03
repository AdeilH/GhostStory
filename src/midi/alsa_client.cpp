// addu94 — src/midi/alsa_client.cpp
#include "midi/alsa_client.hpp"

#include "log/logger.hpp"

#include <chrono>
#include <cstring>
#include <mutex>
#include <poll.h>

namespace addu94::midi {

namespace {

std::uint64_t now_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

constexpr std::string_view kMpkNamePatterns[] = {
    "MPK mini", "MPK Mini", "MPKmini", "mpk mini", "MPK2", "MPK3",
};

}  // namespace

std::string_view mpk_model_from_name(std::string_view client_name) {
    for (auto const pat : kMpkNamePatterns) {
        if (client_name.find(pat) != std::string_view::npos) return "mpk-mini";
    }
    return {};
}

AlsaClient::~AlsaClient() { close(); }

bool AlsaClient::open(std::string_view client_name, bool create_virtual) {
    if (seq_ != nullptr) return true;
    if (snd_seq_open(&seq_, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) {
        log::error("midi", "snd_seq_open failed: {}", snd_strerror(errno));
        seq_ = nullptr;
        return false;
    }
    snd_seq_nonblock(seq_, 1);  // input thread uses poll(); reads never block
    std::string const name{client_name};
    snd_seq_set_client_name(seq_, name.c_str());
    client_id_ = snd_seq_client_id(seq_);

    // Input port: devices connect their output here.
    in_port_ = snd_seq_create_simple_port(
        seq_, "input",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    // Output port: SysEx / LED / identity traffic to devices.
    out_port_ = snd_seq_create_simple_port(
        seq_, "output",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (in_port_ < 0 || out_port_ < 0) {
        log::error("midi", "failed to create ports: {}", snd_strerror(errno));
        close();
        return false;
    }
    if (create_virtual) {
        virt_port_ = snd_seq_create_simple_port(
            seq_, "virtual",
            SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE |
                SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
            SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        if (virt_port_ < 0) {
            log::error("midi", "failed to create virtual port");
            close();
            return false;
        }
    }
    // Subscribe to system announce (client 0 port 1) for hotplug events.
    if (snd_seq_connect_from(seq_, in_port_, 0, 1) < 0) {
        log::warn("midi", "cannot watch hotplug announce port (continuing)");
    }
    log::info("midi", "opened seq client {} '{}' (in={}, out={}, virt={})",
              client_id_, name, in_port_, out_port_, virt_port_);
    return true;
}

void AlsaClient::close() noexcept {
    stop();
    if (seq_ != nullptr) {
        snd_seq_close(seq_);
        seq_ = nullptr;
    }
    client_id_ = -1;
    in_port_ = out_port_ = virt_port_ = -1;
}

bool AlsaClient::start() {
    if (seq_ == nullptr || running_.load(std::memory_order_acquire)) return false;
    running_.store(true, std::memory_order_release);
    try {
        input_thread_ = std::thread([this] { input_loop(); });
    } catch (...) {
        running_.store(false, std::memory_order_release);
        log::error("midi", "failed to start input thread");
        return false;
    }
    return true;
}

void AlsaClient::stop() noexcept {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    if (input_thread_.joinable()) input_thread_.join();
}

void AlsaClient::set_sink(EventSink* sink) noexcept { sink_ = sink; }

std::vector<PortRef> AlsaClient::enumerate_input_ports() {
    std::vector<PortRef> out;
    snd_seq_t* seq = nullptr;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, 0) < 0) return out;

    snd_seq_client_info_t* cinfo = nullptr;
    snd_seq_port_info_t* pinfo = nullptr;
    snd_seq_client_info_malloc(&cinfo);
    snd_seq_port_info_malloc(&pinfo);
    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(seq, cinfo) >= 0) {
        int const client = snd_seq_client_info_get_client(cinfo);
        char const* cname = snd_seq_client_info_get_name(cinfo);
        snd_seq_port_info_set_client(pinfo, client);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(seq, pinfo) >= 0) {
            unsigned const caps = snd_seq_port_info_get_capability(pinfo);
            // readable by us (device sends) and subscribable
            if ((caps & (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ)) ==
                (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ)) {
                unsigned const type = snd_seq_port_info_get_type(pinfo);
                out.push_back(PortRef{
                    client, snd_seq_port_info_get_port(pinfo), cname,
                    snd_seq_port_info_get_name(pinfo),
                    (type & SND_SEQ_PORT_TYPE_MIDI_GENERIC) != 0});
            }
        }
    }
    snd_seq_client_info_free(cinfo);
    snd_seq_port_info_free(pinfo);
    snd_seq_close(seq);
    return out;
}

bool AlsaClient::connect_input(int client, int port) {
    std::scoped_lock lock(conn_mutex_);
    for (auto const& [c, p] : connected_inputs_) {
        if (c == client && p == port) return true;
    }
    if (snd_seq_connect_from(seq_, in_port_, client, port) < 0) {
        log::warn("midi", "connect_input {}:{} failed: {}", client, port,
                  snd_strerror(errno));
        return false;
    }
    connected_inputs_.emplace_back(client, port);
    log::info("midi", "connected input {}:{} -> addu94", client, port);
    return true;
}

bool AlsaClient::connect_output(int client, int port) {
    if (snd_seq_connect_to(seq_, out_port_, client, port) < 0) {
        log::warn("midi", "connect_output {}:{} failed: {}", client, port,
                  snd_strerror(errno));
        return false;
    }
    log::info("midi", "connected addu94 output -> {}:{}", client, port);
    return true;
}

void AlsaClient::disconnect_all() noexcept {
    std::scoped_lock lock(conn_mutex_);
    for (auto const& [c, p] : connected_inputs_) {
        snd_seq_disconnect_from(seq_, in_port_, c, p);
    }
    connected_inputs_.clear();
}

bool AlsaClient::send_sysex(std::uint8_t const* data, std::size_t len) {
    if (seq_ == nullptr || data == nullptr || len == 0) return false;
    snd_seq_event_t ev{};
    snd_seq_ev_clear(&ev);
    ev.type = SND_SEQ_EVENT_SYSEX;
    ev.flags |= SND_SEQ_EVENT_LENGTH_VARIABLE;
    ev.data.ext.len = static_cast<unsigned int>(len);
    ev.data.ext.ptr = const_cast<unsigned char*>(data);
    snd_seq_ev_set_source(&ev, static_cast<unsigned char>(out_port_));
    snd_seq_ev_set_subs(&ev);  // broadcast to all connected output subs
    snd_seq_ev_set_direct(&ev);
    return snd_seq_event_output_direct(seq_, &ev) >= 0;
}

void AlsaClient::input_loop() {
    int const npfds = snd_seq_poll_descriptors_count(seq_, POLLIN);
    std::vector<pollfd> pfds(static_cast<std::size_t>(npfds));
    // Fill descriptors ONCE, before the loop: zero-initialised pfds
    // (fd=0, events=0) would return 0 from poll() forever and the refill
    // below would never be reached.
    snd_seq_poll_descriptors(seq_, pfds.data(), static_cast<unsigned int>(npfds),
                             POLLIN);

    while (running_.load(std::memory_order_acquire)) {
        heartbeat_ns_.store(now_ns(), std::memory_order_release);
        // 100 ms poll timeout keeps the heartbeat honest without burning CPU.
        int const ready = ::poll(pfds.data(), static_cast<nfds_t>(npfds), 100);
        if (ready < 0) {
            if (errno == EINTR) continue;
            decode_errors_.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        if (ready == 0) continue;
        unsigned short revents = 0;
        if (snd_seq_poll_descriptors_revents(seq_, pfds.data(),
                                             static_cast<unsigned int>(npfds),
                                             &revents) < 0) {
            continue;
        }
        if ((revents & POLLIN) == 0) continue;
        // Drain everything available; never block.
        snd_seq_event_t* ev = nullptr;
        while (running_.load(std::memory_order_relaxed) &&
               snd_seq_event_input(seq_, &ev) >= 0 && ev != nullptr) {
            handle_seq_event(ev);
            snd_seq_free_event(ev);
        }
    }
    heartbeat_ns_.store(now_ns(), std::memory_order_release);
}

void AlsaClient::deliver(MidiEvent ev) noexcept {
    if (sink_ == nullptr) return;
    sink_->on_midi(ev);
}

void AlsaClient::handle_seq_event(snd_seq_event_t* ev) {
    events_received_.fetch_add(1, std::memory_order_relaxed);
    MidiEvent m{};
    m.stamp_ns = now_ns();
    m.raw_status = static_cast<std::uint8_t>(ev->type);

    switch (ev->type) {
    case SND_SEQ_EVENT_NOTEON:
        m.type = Type::NoteOn;
        m.channel = static_cast<std::uint8_t>(ev->data.note.channel);
        m.d1 = ev->data.note.note;
        m.d2 = ev->data.note.velocity;
        break;
    case SND_SEQ_EVENT_NOTEOFF:
        m.type = Type::NoteOff;
        m.channel = static_cast<std::uint8_t>(ev->data.note.channel);
        m.d1 = ev->data.note.note;
        m.d2 = ev->data.note.velocity;
        break;
    case SND_SEQ_EVENT_CONTROLLER:
        m.type = Type::CC;
        m.channel = static_cast<std::uint8_t>(ev->data.control.channel);
        m.d1 = static_cast<std::uint8_t>(ev->data.control.param & 0x7F);
        m.d2 = static_cast<std::uint8_t>(ev->data.control.value & 0x7F);
        break;
    case SND_SEQ_EVENT_PITCHBEND: {
        // ALSA gives signed -8192..8191; convert to 14-bit unsigned.
        int v = ev->data.control.value + 8192;
        if (v < 0) v = 0;
        if (v > 16383) v = 16383;
        m.type = Type::PitchBend;
        m.channel = static_cast<std::uint8_t>(ev->data.control.channel);
        m.d1 = static_cast<std::uint8_t>(v & 0x7F);
        m.d2 = static_cast<std::uint8_t>((v >> 7) & 0x7F);
        break;
    }
    case SND_SEQ_EVENT_PGMCHANGE:
        m.type = Type::ProgramChange;
        m.channel = static_cast<std::uint8_t>(ev->data.control.channel);
        m.d1 = static_cast<std::uint8_t>(ev->data.control.value & 0x7F);
        break;
    case SND_SEQ_EVENT_CHANPRESS:
        m.type = Type::ChannelAftertouch;
        m.channel = static_cast<std::uint8_t>(ev->data.control.channel);
        m.d1 = static_cast<std::uint8_t>(ev->data.control.value & 0x7F);
        break;
    case SND_SEQ_EVENT_KEYPRESS:
        m.type = Type::PolyAftertouch;
        m.channel = static_cast<std::uint8_t>(ev->data.note.channel);
        m.d1 = ev->data.note.note;
        m.d2 = ev->data.note.velocity;
        break;
    case SND_SEQ_EVENT_SONGPOS: {
        int v = ev->data.control.value & 0x3FFF;
        m.type = Type::SongPosition;
        m.d1 = static_cast<std::uint8_t>(v & 0x7F);
        m.d2 = static_cast<std::uint8_t>((v >> 7) & 0x7F);
        break;
    }
    case SND_SEQ_EVENT_CLOCK: m.type = Type::ClockTick; break;
    case SND_SEQ_EVENT_START: m.type = Type::ClockStart; break;
    case SND_SEQ_EVENT_CONTINUE: m.type = Type::ClockContinue; break;
    case SND_SEQ_EVENT_STOP: m.type = Type::ClockStop; break;
    case SND_SEQ_EVENT_SYSEX: {
        auto const* data = static_cast<unsigned char const*>(ev->data.ext.ptr);
        auto const len = static_cast<std::size_t>(ev->data.ext.len);
        // Full payload to the (optional) raw sink — probe/capture path.
        if (sink_ != nullptr && data != nullptr && len >= 2 &&
            data[0] == 0xF0 && data[len - 1] == 0xF7) {
            sink_->on_sysex_raw(data, len);
        }
        m.type = Type::SysEx;
        m.sysex_len = static_cast<std::uint8_t>(
            len < MidiEvent::kSysExInline ? len : MidiEvent::kSysExInline);
        m.sysex_truncated = len > MidiEvent::kSysExInline;
        for (std::size_t i = 0; i < m.sysex_len; ++i) m.sysex[i] = data[i];
        break;
    }
    case SND_SEQ_EVENT_PORT_START: {
        if (sink_ != nullptr) {
            sink_->on_device_event(DeviceEvent{
                DeviceEventType::PortAdded, ev->data.addr.client,
                ev->data.addr.port, {}});
        }
        return;
    }
    case SND_SEQ_EVENT_PORT_EXIT: {
        if (sink_ != nullptr) {
            sink_->on_device_event(DeviceEvent{
                DeviceEventType::PortRemoved, ev->data.addr.client,
                ev->data.addr.port, {}});
        }
        return;
    }
    case SND_SEQ_EVENT_CLIENT_EXIT: {
        if (sink_ != nullptr) {
            sink_->on_device_event(DeviceEvent{
                DeviceEventType::ClientRemoved, ev->data.addr.client,
                ev->data.addr.port, {}});
        }
        return;
    }
    default:
        decode_errors_.fetch_add(1, std::memory_order_relaxed);
        m.type = Type::Other;
        break;
    }
    deliver(m);
}

}  // namespace addu94::midi
