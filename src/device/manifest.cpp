// addu94 — src/device/manifest.cpp
#include "device/manifest.hpp"

#include <nlohmann/json.hpp>

#include "log/logger.hpp"

#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <unistd.h>

// Minimal, dependency-free SHA-256 (FIPS 180-4). Used for manifest/config
// checksums. Constant-time concerns do not apply (checksums, not secrets).
namespace {

class Sha256 {
public:
    void update(std::uint8_t const* data, std::size_t len) {
        total_ += len;
        while (len > 0) {
            std::size_t const take = (64 - buf_len_ < len) ? (64 - buf_len_) : len;
            std::memcpy(buf_ + buf_len_, data, take);
            buf_len_ += take;
            data += take;
            len -= take;
            if (buf_len_ == 64) {
                transform(buf_);
                buf_len_ = 0;
            }
        }
    }
    void update(std::string const& s) {
        update(reinterpret_cast<std::uint8_t const*>(s.data()), s.size());
    }
    std::string hex() {
        std::uint8_t pad = 0x80;
        std::uint64_t bitlen = total_ * 8;
        update(&pad, 1);
        std::uint8_t zero = 0;
        while (buf_len_ != 56) update(&zero, 1);
        std::uint8_t lenb[8];
        for (int i = 0; i < 8; ++i) lenb[i] = static_cast<std::uint8_t>(bitlen >> (56 - i * 8));
        update(lenb, 8);
        std::string out;
        out.reserve(64);
        static char const* digits = "0123456789abcdef";
        for (auto h : h_) {
            for (int shift = 28; shift >= 0; shift -= 4) {
                out += digits[(h >> shift) & 0xF];
            }
        }
        return out;
    }

private:
    static std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void transform(std::uint8_t const* p) {
        static std::uint32_t const k[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (std::uint32_t(p[i*4]) << 24) | (std::uint32_t(p[i*4+1]) << 16) |
                   (std::uint32_t(p[i*4+2]) << 8) | std::uint32_t(p[i*4+3]);
        }
        for (int i = 16; i < 64; ++i) {
            std::uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
            std::uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        std::uint32_t a=h_[0],b=h_[1],c=h_[2],d=h_[3],e=h_[4],f=h_[5],g=h_[6],h=h_[7];
        for (int i = 0; i < 64; ++i) {
            std::uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
            std::uint32_t ch = (e & f) ^ (~e & g);
            std::uint32_t t1 = h + S1 + ch + k[i] + w[i];
            std::uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
            std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            std::uint32_t t2 = S0 + maj;
            h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h_[0]+=a; h_[1]+=b; h_[2]+=c; h_[3]+=d; h_[4]+=e; h_[5]+=f; h_[6]+=g; h_[7]+=h;
    }
    std::uint32_t h_[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                           0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    std::uint8_t buf_[64]{};
    std::size_t buf_len_{0};
    std::uint64_t total_{0};
};

std::string sha256_hex(std::string const& s) {
    Sha256 h;
    h.update(s);
    return h.hex();
}

}  // namespace

namespace addu94::device {

std::string control_kind_to_string(Control::Kind k) {
    switch (k) {
    case Control::Kind::Key: return "key";
    case Control::Kind::Pad: return "pad";
    case Control::Kind::Knob: return "knob";
    case Control::Kind::Joystick: return "joystick";
    case Control::Kind::Button: return "button";
    case Control::Kind::Pedal: return "pedal";
    }
    return "?";
}

std::optional<Control::Kind> control_kind_from_string(std::string const& s) {
    if (s == "key") return Control::Kind::Key;
    if (s == "pad") return Control::Kind::Pad;
    if (s == "knob") return Control::Kind::Knob;
    if (s == "joystick") return Control::Kind::Joystick;
    if (s == "button") return Control::Kind::Button;
    if (s == "pedal") return Control::Kind::Pedal;
    return std::nullopt;
}

std::vector<Control> default_inventory(std::string const& model) {
    std::vector<Control> c;
    c.reserve(64);
    // 25 keys (C1..C4, note 36..60) — MK units; Play is 25 too.
    for (int i = 0; i < 25; ++i) {
        c.push_back(Control{Control::Kind::Key, i,
                            "key." + std::to_string(36 + i),
                            "Key " + std::to_string(36 + i), 36 + i, -1, 0});
    }
    // 16 pads: bank A notes 36..43 (MPC layout), bank B 44..51.
    static char const* const bank_row[4] = {"1", "2", "3", "4"};
    for (int bank = 0; bank < 2; ++bank) {
        for (int i = 0; i < 8; ++i) {
            int const idx = bank * 8 + i;
            c.push_back(Control{
                Control::Kind::Pad, idx,
                "pad." + std::string(bank == 0 ? "a" : "b") + bank_row[i / 4] +
                    std::to_string(i % 4 + 1),
                std::string(bank == 0 ? "Pad A" : "Pad B") +
                    std::to_string(i + 1),
                36 + (i % 4) * 4 + (i / 4) + bank * 8, -1, 0});
        }
    }
    // 8 knobs: default CCs on channel 0 (probe verifies pot vs encoder).
    for (int i = 0; i < 8; ++i) {
        c.push_back(Control{Control::Kind::Knob, i, "knob." + std::to_string(i + 1),
                            "Knob " + std::to_string(i + 1), -1, 70 + i, 0});
    }
    // Joystick: X = pitch bend, Y = CC1 mod (probe may refine).
    c.push_back(Control{Control::Kind::Joystick, 0, "joy.x", "Joystick X", -1, -1, 0});
    c.push_back(Control{Control::Kind::Joystick, 1, "joy.y", "Joystick Y", -1, 1, 0});
    // Buttons (functionality is app-side; listed so the coverage audit sees them).
    c.push_back(Control{Control::Kind::Button, 0, "btn.octave_up", "Octave Up", -1, -1, 0});
    c.push_back(Control{Control::Kind::Button, 1, "btn.octave_down", "Octave Down", -1, -1, 0});
    c.push_back(Control{Control::Kind::Button, 2, "btn.arp", "ARP", -1, -1, 0});
    if (model == "mpk-mini-mk3" || model == "mpk-mini-play") {
        c.push_back(Control{Control::Kind::Button, 3, "btn.note_repeat", "Note Repeat", -1, -1, 0});
    }
    // Sustain pedal (CC64 via jack).
    c.push_back(Control{Control::Kind::Pedal, 0, "pedal.sustain", "Sustain Pedal", -1, 64, 0});
    return c;
}

std::string manifest_to_json(Manifest const& m) {
    nlohmann::json j;
    j["schema"] = m.schema;
    j["model"] = m.model;
    j["firmware"] = m.firmware;
    j["alsa_client"] = m.alsa_client;
    j["alsa_client_name"] = m.alsa_client_name;
    j["ports"] = nlohmann::json::array();
    for (auto const& p : m.ports) j["ports"].push_back({{"id", p.id}, {"name", p.name}});
    j["controls"] = nlohmann::json::array();
    for (auto const& c : m.controls) {
        j["controls"].push_back({
            {"kind", control_kind_to_string(c.kind)},
            {"index", c.index},
            {"id", c.id},
            {"label", c.label},
            {"note", c.midi_note},
            {"cc", c.midi_cc},
            {"channel", c.midi_channel},
        });
    }
    j["identity_request_supported"] = m.identity_request_supported;
    j["has_note_repeat_button"] = m.has_note_repeat_button;
    j["has_spring_pitch"] = m.has_spring_pitch;
    j["pad_rgb"] = m.pad_rgb;
    j["probed_at_ns"] = m.probed_at_ns;
    // checksum over canonical payload (everything except the checksum field)
    std::string canonical = j.dump();
    j["checksum"] = sha256_hex(canonical);
    return j.dump(2);
}

std::optional<Manifest> manifest_from_json(std::string const& json_text) {
    nlohmann::json j = nlohmann::json::parse(json_text, nullptr, false);
    if (j.is_discarded()) return std::nullopt;
    try {
        int const schema = j.at("schema").get<int>();
        if (schema != kManifestSchema) return std::nullopt;

        // verify checksum
        if (j.contains("checksum")) {
            std::string const stored = j["checksum"].get<std::string>();
            nlohmann::json copy = j;
            copy.erase("checksum");
            if (sha256_hex(copy.dump()) != stored) return std::nullopt;
        }

        Manifest m;
        m.model = j.at("model").get<std::string>();
        m.firmware = j.value("firmware", std::string{});
        m.alsa_client = j.value("alsa_client", -1);
        m.alsa_client_name = j.value("alsa_client_name", std::string{});
        for (auto const& p : j.value("ports", nlohmann::json::array())) {
            m.ports.push_back({p.at("id").get<int>(), p.at("name").get<std::string>()});
        }
        for (auto const& c : j.value("controls", nlohmann::json::array())) {
            auto kind = control_kind_from_string(c.at("kind").get<std::string>());
            if (!kind) return std::nullopt;
            m.controls.push_back(Control{
                *kind, c.at("index").get<int>(), c.at("id").get<std::string>(),
                c.value("label", std::string{}), c.value("note", -1),
                c.value("cc", -1), c.value("channel", 0)});
        }
        m.identity_request_supported = j.value("identity_request_supported", false);
        m.has_note_repeat_button = j.value("has_note_repeat_button", false);
        m.has_spring_pitch = j.value("has_spring_pitch", true);
        m.pad_rgb = j.value("pad_rgb", true);
        m.probed_at_ns = j.value("probed_at_ns", std::uint64_t{0});
        m.checksum = j.value("checksum", std::string{});
        return m;
    } catch (std::exception const& e) {
        log::warn("device", "manifest parse failed: {}", e.what());
        return std::nullopt;
    }
}

std::filesystem::path manifest_path() {
    char const* xdg = std::getenv("XDG_DATA_HOME");
    std::filesystem::path base;
    if (xdg != nullptr && *xdg != '\0') {
        base = std::filesystem::path{xdg};
    } else {
        char const* home = std::getenv("HOME");
        base = (home != nullptr)
                   ? std::filesystem::path{home} / ".local" / "share"
                   : std::filesystem::path{"."};
    }
    return base / "addu94" / "devices" / "device_manifest.json";
}

bool save_manifest(Manifest const& m, std::filesystem::path const& path) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path const tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.good()) return false;
        f << manifest_to_json(m);
        f.flush();
        if (!f.good()) return false;
    }
    // fsync for crash-safety (spec §6.3 atomic-save pattern)
    int fd = ::open(tmp.c_str(), O_RDONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) return false;
    return true;
}

std::optional<Manifest> load_manifest(std::filesystem::path const& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    return manifest_from_json(ss.str());
}

bool quarantine_file(std::filesystem::path const& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return false;
    auto const ts = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path const dest =
        path.string() + ".corrupt-" + std::to_string(ts);
    std::filesystem::rename(path, dest, ec);
    if (ec) return false;
    log::warn("device", "quarantined corrupt file to {}", dest.string());
    return true;
}

}  // namespace addu94::device
