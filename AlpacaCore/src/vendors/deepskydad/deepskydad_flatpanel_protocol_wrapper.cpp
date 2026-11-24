// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/serial_by_id_scan.h>
#include <alpacacore/util/serial_io.h>
#include <alpacacore/vendor/deepskydad/deepskydad_flatpanel_protocol_wrapper.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <alpacacore/util/serial_link.h>
#include <alpacacore/util/serial_port_registry.h>
#include <alpacacore/util/serial_read.h>
#include <termios.h>
#endif

namespace alpacacore::vendor::deepskydad {

namespace {

constexpr std::size_t kMaxReplyLen = 96;  // [GFRM] carries the longest reply, a firmware banner
constexpr int kHandshakeRetries = 3;
constexpr int kHandshakeReadTimeoutMs = 1500;
constexpr const char* kSerialOwner = "Deep Sky Dad flat panel";

#ifndef _WIN32
void configure_line(struct termios& tty) { util::make_raw_8n1(tty, B115200); }
#endif

bool parse_int_payload(const std::string& payload, int& out) {
    int value = 0;
    char trailing = 0;
    if (std::sscanf(payload.c_str(), "%d%c", &value, &trailing) != 1) {
        return false;
    }
    out = value;
    return true;
}

}  // namespace

bool parse_deepskydad_reply(const std::string& reply, std::string& payload) {
    // The panel can emit stray CR/LF before a reply; read_until drops those. A
    // reply is framed "(...)": take what lies between the first '(' and the ')'.
    const auto open = reply.find('(');
    if (open == std::string::npos || reply.empty() || reply.back() != ')' || reply.size() < open + 2) {
        return false;
    }
    payload = reply.substr(open + 1, reply.size() - open - 2);
    return true;
}

bool parse_deepskydad_firmware(const std::string& payload, FlatPanelModel& model) {
    // INDI tells the models apart with strstr(response, "DeepSkyDad.FP2").
    if (payload.find("DeepSkyDad.FP2") != std::string::npos) {
        model = FlatPanelModel::FP2;
        return true;
    }
    // TODO(deepskydad): the FP1 banner is assumed to read "DeepSkyDad.FP1"; INDI only
    // checks for FP2 and treats every other [GFRM] answer as an FP1.
    if (payload.find("DeepSkyDad.FP") != std::string::npos) {
        model = FlatPanelModel::FP1;
        return true;
    }
    return false;
}

FlatPanelCoverEnd cover_end_for(FlatPanelModel model, int cover_raw) {
    if (model == FlatPanelModel::FP2) {
        // [GOPS]: 0 closed, 1 open, anything else neither (INDI getStatus()).
        if (cover_raw == 0) return FlatPanelCoverEnd::Closed;
        if (cover_raw == 1) return FlatPanelCoverEnd::Open;
        return FlatPanelCoverEnd::Neither;
    }
    // FP1 [GPOS] is the flap angle: 270 closed, 0 open.
    if (cover_raw == 270) return FlatPanelCoverEnd::Closed;
    if (cover_raw == 0) return FlatPanelCoverEnd::Open;
    return FlatPanelCoverEnd::Neither;
}

const std::vector<std::string>& deepskydad_flatpanel_usb_tokens() {
    // TODO(deepskydad): the panel's USB VID:PID is unconfirmed; it enumerates as a
    // CDC-ACM port (/dev/ttyACM*). Match the vendor name in the descriptor; the
    // [GFRM] handshake is what proves the device.
    static const std::vector<std::string> tokens{"DeepSkyDad", "Deep_Sky_Dad", "Deep Sky Dad"};
    return tokens;
}

#ifndef _WIN32
namespace {

// One [GFRM] exchange on an open probe link.
bool probe_link(util::SerialLink& link, std::string& firmware) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        // A native-USB MCU can still be enumerating its CDC interface right after the open.
        std::this_thread::sleep_for(std::chrono::milliseconds(attempt == 0 ? 100 : 800));
        tcflush(link.fd(), TCIOFLUSH);
        if (!link.write_all(std::string("[GFRM]"))) {
            return false;
        }
        util::ReadSpec spec;
        spec.stop = util::ReadSpec::Stop::Terminator;
        spec.terminators = ")";
        spec.max_len = kMaxReplyLen;
        const auto r = util::read_until(
            link, spec, std::chrono::steady_clock::now() + std::chrono::milliseconds(kHandshakeReadTimeoutMs));
        if (r.outcome == util::ReadOutcome::LinkDead) {
            return false;
        }
        std::string payload;
        FlatPanelModel model{};
        if (r.outcome == util::ReadOutcome::Terminator && parse_deepskydad_reply(r.bytes, payload) &&
            parse_deepskydad_firmware(payload, model)) {
            firmware = payload;
            return true;
        }
    }
    return false;
}

bool raw_port_is_candidate(const std::string& port_path) {
    // A raw ttyACM node with no by-id name: only a descriptor naming the vendor
    // qualifies, so the scan never writes to an unrelated CDC device.
    const auto descriptor = util::read_raw_tty_usb_descriptor(port_path);
    return descriptor && util::usb_tty_descriptor_matches(*descriptor, deepskydad_flatpanel_usb_tokens());
}

}  // namespace
#endif

bool probe_deepskydad_flatpanel_port(const std::string& port_path, std::string& firmware) {
#ifndef _WIN32
    std::string error;
    util::SerialLink link =
        util::SerialLink::open(port_path, util::SerialClaimKind::Probe, kSerialOwner, configure_line, &error);
    if (!link) {
        ALPACA_LOG_DEBUG("DeepSkyDad", "Flat panel probe skipped " + port_path + ": " + error);
        return false;
    }
    return probe_link(link, firmware);
#else
    (void)port_path;
    (void)firmware;
    return false;
#endif
}

std::vector<FlatPanelPortInfo> enumerate_deepskydad_flatpanel_ports() {
    std::vector<FlatPanelPortInfo> results;
#ifndef _WIN32
    struct Candidate {
        std::string path;
        bool sibling = false;
    };
    std::vector<Candidate> candidates;
    std::set<std::string> seen;
    auto add_candidate = [&](const std::string& path) {
        if (!seen.insert(path).second) return;
        switch (util::serial_candidate_use(path, kSerialOwner)) {
            case util::SerialCandidateUse::HeldByOther:
                return;
            case util::SerialCandidateUse::HeldBySibling:
                candidates.push_back({path, true});
                return;
            case util::SerialCandidateUse::Free:
                candidates.push_back({path, false});
                return;
        }
    };

    const std::filesystem::path by_id("/dev/serial/by-id");
    if (util::path_exists(by_id)) {
        for (const auto& sym : util::list_serial_by_id(by_id)) {
            bool match = false;
            for (const auto& token : deepskydad_flatpanel_usb_tokens()) {
                if (sym.name.find(token) != std::string::npos) match = true;
            }
            if (!match) continue;
            std::error_code ec;
            const std::string resolved = std::filesystem::canonical(sym.path, ec).string();
            if (!ec) add_candidate(resolved);
        }
    }
    // Raw fallback, filtered by descriptor and deduped against the by-id pass.
    for (int i = 0; i < 10; ++i) {
        const std::string port = "/dev/ttyACM" + std::to_string(i);
        if (!util::path_exists(port)) continue;
        std::error_code ec;
        const std::string resolved = std::filesystem::canonical(port, ec).string();
        if (ec || seen.count(resolved) != 0) continue;
        if (!raw_port_is_candidate(resolved)) continue;
        add_candidate(resolved);
    }

    // Probe concurrently (issue #218), emit in candidate order.
    std::vector<std::string> firmware(candidates.size());
    std::vector<char> found(candidates.size(), 0);
    std::vector<std::thread> workers;
    workers.reserve(candidates.size());
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (candidates[i].sibling) continue;
        workers.emplace_back(
            [&, i] { found[i] = probe_deepskydad_flatpanel_port(candidates[i].path, firmware[i]) ? 1 : 0; });
    }
    for (auto& w : workers) w.join();
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (candidates[i].sibling) {
            results.push_back({candidates[i].path, std::string()});
        } else if (found[i]) {
            ALPACA_LOG_INFO("DeepSkyDad", "Found flat panel on " + candidates[i].path);
            results.push_back({candidates[i].path, firmware[i]});
        }
    }
#endif
    return results;
}

class DeepSkyDadFlatPanelProtocolWrapper::Impl {
public:
    ~Impl() { disconnect(); }

    std::string connect(const FlatPanelConnectionConfig& config) {
        std::lock_guard<std::mutex> lock(mutex_);
        disconnect_locked();
        config_ = config;
#ifndef _WIN32
        std::string error;
        link_ = util::SerialLink::open(config_.serial_port, util::SerialClaimKind::Link, kSerialOwner, configure_line,
                                       &error);
        if (!link_) {
            throw AlpacaException(error, AlpacaError::NotConnected);
        }
        std::string firmware;
        FlatPanelModel model{};
        bool ok = false;
        for (int attempt = 0; attempt < kHandshakeRetries && !ok; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(attempt == 0 ? 100 : 800));
            try {
                const std::string payload = exchange_locked("[GFRM]", kHandshakeReadTimeoutMs);
                if (parse_deepskydad_firmware(payload, model)) {
                    firmware = payload;
                    ok = true;
                }
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("DeepSkyDad",
                                "Flat panel handshake attempt " + std::to_string(attempt + 1) + " failed: " + e.what());
            }
        }
        if (!ok) {
            disconnect_locked();
            throw AlpacaException("No Deep Sky Dad flat panel answered [GFRM] on " + config_.serial_port +
                                      "; check the port and that the panel is powered",
                                  AlpacaError::NotConnected);
        }
        // A hangup the retries absorbed is not a loss (#2262).
        link_.clear_loss();
        model_.store(model);
        connected_ = true;
        ALPACA_LOG_INFO("DeepSkyDad", "Flat panel connected on " + config_.serial_port + " (" + firmware + ")");
        return firmware;
#else
        throw AlpacaException("Deep Sky Dad flat panel driver is Linux-only", AlpacaError::DriverException);
#endif
    }

    void disconnect() {
        std::lock_guard<std::mutex> lock(mutex_);
        disconnect_locked();
#ifndef _WIN32
        link_.clear_loss();
#endif
    }

    FlatPanelModel model() const noexcept { return model_.load(); }

    bool link_alive() const noexcept {
#ifndef _WIN32
        return !link_.lost();
#else
        return true;
#endif
    }

    std::string link_fault() const {
#ifndef _WIN32
        return link_.lost() ? link_.fault() : std::string();
#else
        return {};
#endif
    }

    FlatPanelStatus get_status() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        FlatPanelStatus s;
        s.motor_moving = read_int_locked("[GMOV]") == 1;
        s.light_on = read_int_locked("[GLON]") == 1;
        s.cover_raw = read_int_locked(model_.load() == FlatPanelModel::FP2 ? "[GOPS]" : "[GPOS]");
        s.brightness = read_int_locked("[GLBR]");
        return s;
    }

    void move_cover(bool open) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        // INDI ParkCap()/UnParkCap(): the target angle, then the move.
        expect_ok_locked(open ? "[STRG0]" : "[STRG270]");
        expect_ok_locked("[SMOV]");
    }

    void set_light(bool on, int brightness) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        if (!on) {
            expect_ok_locked("[SLON0]");
            return;
        }
        if (brightness < 0 || brightness > kDeepSkyDadMaxBrightness) {
            throw AlpacaException("Brightness out of range", AlpacaError::InvalidValue);
        }
        char cmd[16];
        std::snprintf(cmd, sizeof(cmd), "[SLBR%04d]", brightness);
        expect_ok_locked(cmd);
        expect_ok_locked("[SLON1]");
    }

private:
    void ensure_connected_locked() const {
        if (!connected_) {
            throw AlpacaException("Deep Sky Dad flat panel not connected", AlpacaError::NotConnected);
        }
#ifndef _WIN32
        if (link_.lost()) {
            throw AlpacaException(link_.fault(), AlpacaError::NotConnected);
        }
#endif
    }

    void disconnect_locked() {
        connected_ = false;
#ifndef _WIN32
        link_.close();
#endif
    }

#ifndef _WIN32
    // The port is gone: release it so a replugged panel can be reopened, and fail NotConnected.
    [[noreturn]] void fail_link_locked() {
        const std::string reason = link_.fault();
        if (connected_) {
            ALPACA_LOG_ERROR("DeepSkyDad",
                             "Flat panel link lost on " + config_.serial_port + ": " + reason + " (port released)");
            connected_ = false;
            link_.close();  // keeps the loss verdict for link_alive()/link_fault()
        }
        throw AlpacaException(reason.empty() ? std::string("Flat panel link lost") : reason, AlpacaError::NotConnected);
    }
#endif

    // One command and its "(...)" reply; returns the payload. INDI flushes before every write.
    std::string exchange_locked(const std::string& cmd, int timeout_ms) {
#ifndef _WIN32
        ALPACA_LOG_TRACE("DeepSkyDad", "Flat panel command: " + cmd);
        tcflush(link_.fd(), TCIOFLUSH);
        if (!link_.write_all(cmd)) {
            if (link_.lost()) fail_link_locked();
            throw AlpacaException("Flat panel write failed", AlpacaError::DriverException);
        }
        util::ReadSpec spec;
        spec.stop = util::ReadSpec::Stop::Terminator;
        spec.terminators = ")";
        spec.max_len = kMaxReplyLen;
        const auto r =
            util::read_until(link_, spec, std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms));
        switch (r.outcome) {
            case util::ReadOutcome::Terminator:
                break;
            case util::ReadOutcome::LinkDead:
                fail_link_locked();
            case util::ReadOutcome::Overflow:
                throw AlpacaException("Flat panel reply to " + cmd + " too long", AlpacaError::DriverException);
            default:
                throw AlpacaException("Flat panel did not answer " + cmd, AlpacaError::DriverException);
        }
        std::string payload;
        if (!parse_deepskydad_reply(r.bytes, payload)) {
            throw AlpacaException("Flat panel sent a malformed reply to " + cmd, AlpacaError::DriverException);
        }
        ALPACA_LOG_TRACE("DeepSkyDad", "Flat panel reply: " + payload);
        return payload;
#else
        (void)cmd;
        (void)timeout_ms;
        throw AlpacaException("Deep Sky Dad flat panel driver is Linux-only", AlpacaError::DriverException);
#endif
    }

    int read_int_locked(const std::string& cmd) {
        const std::string payload = exchange_locked(cmd, config_.reply_timeout_ms);
        int value = 0;
        if (!parse_int_payload(payload, value)) {
            throw AlpacaException("Flat panel sent a non-numeric reply to " + cmd, AlpacaError::DriverException);
        }
        return value;
    }

    void expect_ok_locked(const std::string& cmd) {
        const std::string payload = exchange_locked(cmd, config_.reply_timeout_ms);
        if (payload != "OK") {
            throw AlpacaException("Flat panel refused " + cmd, AlpacaError::DriverException);
        }
    }

    mutable std::mutex mutex_;  // the per-link I/O lock: one exchange at a time
    FlatPanelConnectionConfig config_;
    bool connected_ = false;
    std::atomic<FlatPanelModel> model_{FlatPanelModel::FP2};
#ifndef _WIN32
    util::SerialLink link_;
#endif
};

DeepSkyDadFlatPanelProtocolWrapper::DeepSkyDadFlatPanelProtocolWrapper() : impl_(std::make_unique<Impl>()) {}
DeepSkyDadFlatPanelProtocolWrapper::~DeepSkyDadFlatPanelProtocolWrapper() = default;

std::string DeepSkyDadFlatPanelProtocolWrapper::connect(const FlatPanelConnectionConfig& config) {
    return impl_->connect(config);
}
void DeepSkyDadFlatPanelProtocolWrapper::disconnect() { impl_->disconnect(); }
FlatPanelModel DeepSkyDadFlatPanelProtocolWrapper::model() const noexcept { return impl_->model(); }
bool DeepSkyDadFlatPanelProtocolWrapper::link_alive() const noexcept { return impl_->link_alive(); }
std::string DeepSkyDadFlatPanelProtocolWrapper::link_fault() const { return impl_->link_fault(); }
FlatPanelStatus DeepSkyDadFlatPanelProtocolWrapper::get_status() { return impl_->get_status(); }
void DeepSkyDadFlatPanelProtocolWrapper::move_cover(bool open) { impl_->move_cover(open); }
void DeepSkyDadFlatPanelProtocolWrapper::set_light(bool on, int brightness) { impl_->set_light(on, brightness); }

}  // namespace alpacacore::vendor::deepskydad
