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
#include <alpacacore/vendor/scopedome/scopedome_usb21_protocol_wrapper.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#ifndef _WIN32
#include <alpacacore/util/serial_link.h>
#include <alpacacore/util/serial_port_registry.h>
#include <alpacacore/util/serial_read.h>
#include <termios.h>
#endif

namespace alpacacore::vendor::scopedome {

namespace {

constexpr const char* kSerialOwner = "ScopeDome USB Card 2.1";
constexpr std::size_t kFrameOverhead = 4;
constexpr int kExchangeAttempts = 2;  // INDI retries each exchange once

#ifndef _WIN32
void configure_line(struct termios& tty) { util::make_raw_8n1(tty, B115200); }
#endif

const char* command_name(std::uint8_t command) {
    switch (static_cast<ScopeDomeCommand>(command)) {
        case ScopeDomeCommand::ConnectionTest: return "ConnectionTest";
        case ScopeDomeCommand::ClearDigitalChannel: return "ClearDigitalChannel";
        case ScopeDomeCommand::SetDigitalChannel: return "SetDigitalChannel";
        case ScopeDomeCommand::GetCounter: return "GetCounter";
        case ScopeDomeCommand::ResetCounter: return "ResetCounter";
        case ScopeDomeCommand::CcwRotation: return "CCWRotation";
        case ScopeDomeCommand::CwRotation: return "CWRotation";
        case ScopeDomeCommand::GetVersionFirmware: return "GetVersionFirmware";
        case ScopeDomeCommand::GetStatus: return "GetStatus";
        case ScopeDomeCommand::GetCounterExt: return "GetCounterExt";
        case ScopeDomeCommand::ResetCounterExt: return "ResetCounterExt";
        case ScopeDomeCommand::GetAllDigitalExt: return "GetAllDigitalExt";
        case ScopeDomeCommand::GetImpPerTurn: return "GetImpPerTurn";
        case ScopeDomeCommand::Stop: return "Stop";
        case ScopeDomeCommand::FindHome: return "FindHome";
        case ScopeDomeCommand::GetVersionFirmwareRotary: return "GetVersionFirmwareRotary";
        case ScopeDomeCommand::StopSafeCommunication: return "StopSafeCommunication";
        case ScopeDomeCommand::IsFullSystemCalReq: return "IsFullSystemCalReq";
        default: return "command";
    }
}

template <typename T>
std::string le_bytes(T value) {
    std::string out(sizeof(T), '\0');
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        out[i] = static_cast<char>((static_cast<std::uint64_t>(value) >> (8 * i)) & 0xFFu);
    }
    return out;
}

template <typename T>
T from_le(const std::string& bytes) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < sizeof(T) && i < bytes.size(); ++i) {
        v |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[i])) << (8 * i);
    }
    return static_cast<T>(v);
}

}  // namespace

std::uint8_t scopedome_crc8(std::uint8_t crc, std::uint8_t data) {
    crc ^= data;
    for (int i = 0; i < 8; ++i) {
        crc = (crc & 1u) != 0 ? static_cast<std::uint8_t>((crc >> 1) ^ 0x8Cu) : static_cast<std::uint8_t>(crc >> 1);
    }
    return crc;
}

std::string encode_scopedome_frame(ScopeDomeCommand command, const std::string& payload) {
    std::string frame(kFrameOverhead, '\0');
    frame[0] = static_cast<char>(kScopeDomeFrameHeader);
    frame[1] = static_cast<char>(payload.size());
    frame[2] = static_cast<char>(static_cast<std::uint8_t>(command));
    std::uint8_t crc = scopedome_crc8(0, kScopeDomeFrameHeader);
    crc = scopedome_crc8(crc, static_cast<std::uint8_t>(payload.size()));
    crc = scopedome_crc8(crc, static_cast<std::uint8_t>(command));
    for (char c : payload) crc = scopedome_crc8(crc, static_cast<std::uint8_t>(c));
    frame[3] = static_cast<char>(crc);
    frame += payload;
    return frame;
}

bool decode_scopedome_frame(const std::string& bytes, ScopeDomeFrame& out) {
    if (bytes.size() < kFrameOverhead) return false;
    const auto byte = [&](std::size_t i) { return static_cast<std::uint8_t>(bytes[i]); };
    if (byte(0) != kScopeDomeFrameHeader) return false;
    const std::size_t len = byte(1);
    if (bytes.size() != kFrameOverhead + len) return false;
    std::uint8_t crc = scopedome_crc8(0, byte(0));
    crc = scopedome_crc8(crc, byte(1));
    crc = scopedome_crc8(crc, byte(2));
    for (std::size_t i = 0; i < len; ++i) crc = scopedome_crc8(crc, byte(kFrameOverhead + i));
    if (crc != byte(3)) return false;
    out.command = byte(2);
    out.payload = bytes.substr(kFrameOverhead);
    return true;
}

class ScopeDomeUsb21ProtocolWrapper::Impl {
public:
    ~Impl() { disconnect(); }

    ScopeDomeCardInfo connect(const ScopeDomeConnectionConfig& config) {
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
        clear_loss();
        connected_ = true;
        try {
            // The FTDI bridge resets nothing, but give the card a moment after the open.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const auto test = exchange_locked(ScopeDomeCommand::ConnectionTest, std::string(), 0);
            if (test.command != static_cast<std::uint8_t>(ScopeDomeCommand::ConnectionTest)) {
                throw AlpacaException("not a ScopeDome card", AlpacaError::NotConnected);
            }
            // The card's "safe communication" mode resets the link after a few seconds of silence.
            command_locked(ScopeDomeCommand::StopSafeCommunication, std::string());
            ScopeDomeCardInfo info;
            info.firmware_main =
                from_le<std::uint16_t>(query_locked(ScopeDomeCommand::GetVersionFirmware, 2).payload) / 100.0;
            try {
                const auto rotary = from_le<std::uint8_t>(query_locked(ScopeDomeCommand::GetVersionFirmwareRotary, 1).payload);
                info.firmware_rotary = (rotary + 9) / 10.0;  // INDI's decoding
            } catch (const AlpacaException& e) {
                if (lost_.load()) throw;
                ALPACA_LOG_WARN("ScopeDome", std::string("Rotary firmware version not read: ") + e.what());
            }
            info.steps_per_turn = from_le<std::uint32_t>(query_locked(ScopeDomeCommand::GetImpPerTurn, 4).payload);
            try {
                info.calibration_needed =
                    from_le<std::uint8_t>(query_locked(ScopeDomeCommand::IsFullSystemCalReq, 1).payload) != 0;
            } catch (const AlpacaException& e) {
                if (lost_.load()) throw;
                ALPACA_LOG_WARN("ScopeDome", std::string("Calibration flag not read: ") + e.what());
            }
            clear_loss();  // a hangup the retries absorbed is not a loss
            return info;
        } catch (const AlpacaException& e) {
            disconnect_locked();
            throw AlpacaException("No ScopeDome USB Card 2.1 answered on " + config_.serial_port + " (" + e.what() +
                                      "); check the port and that the card is powered",
                                  AlpacaError::NotConnected);
        }
#else
        throw AlpacaException("ScopeDome driver is Linux-only", AlpacaError::DriverException);
#endif
    }

    void disconnect() {
        std::lock_guard<std::mutex> lock(mutex_);
        disconnect_locked();
        clear_loss();
    }

    bool link_alive() const noexcept { return !lost_.load(); }

    std::string link_fault() const { return lost_.load() ? fault_text() : std::string(); }

    ScopeDomeCardState read_state() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        ScopeDomeCardState s;
        s.status = from_le<std::uint16_t>(query_locked(ScopeDomeCommand::GetStatus, 2).payload);
        s.counter = from_le<std::int16_t>(query_locked(ScopeDomeCommand::GetCounter, 2).payload);
        const auto digital = query_locked(ScopeDomeCommand::GetAllDigitalExt, kScopeDomeDigitalBytes).payload;
        for (std::size_t i = 0; i < kScopeDomeDigitalBytes; ++i) {
            s.digital[i] = static_cast<std::uint8_t>(digital[i]);
        }
        return s;
    }

    void rotate(int steps) {
        if (steps == 0) return;
        const int magnitude = steps < 0 ? -steps : steps;
        if (magnitude > 0xFFFF) {
            throw AlpacaException("Rotation of " + std::to_string(magnitude) + " steps exceeds the card's limit",
                                  AlpacaError::InvalidValue);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        command_locked(steps < 0 ? ScopeDomeCommand::CcwRotation : ScopeDomeCommand::CwRotation,
                       le_bytes(static_cast<std::uint16_t>(magnitude)));
    }

    void stop() { simple(ScopeDomeCommand::Stop); }
    void find_home() { simple(ScopeDomeCommand::FindHome); }

    void reset_counters() {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        command_locked(ScopeDomeCommand::ResetCounter, std::string());
        command_locked(ScopeDomeCommand::ResetCounterExt, std::string());
    }

    void set_output(ScopeDomeChannel channel, bool on) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        command_locked(on ? ScopeDomeCommand::SetDigitalChannel : ScopeDomeCommand::ClearDigitalChannel,
                       le_bytes(static_cast<std::uint8_t>(channel)));
    }

private:
    void simple(ScopeDomeCommand command) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_connected_locked();
        command_locked(command, std::string());
    }

    void ensure_connected_locked() const {
        if (!connected_) {
            throw AlpacaException("ScopeDome card not connected", AlpacaError::NotConnected);
        }
        if (lost_.load()) {
            throw AlpacaException(fault_text(), AlpacaError::NotConnected);
        }
    }

    void disconnect_locked() {
        connected_ = false;
#ifndef _WIN32
        link_.close();
#endif
    }

    // A query: the reply carries the same command and a payload of @p len bytes.
    ScopeDomeFrame query_locked(ScopeDomeCommand command, std::size_t len) {
        auto reply = exchange_locked(command, std::string(), len);
        if (reply.command != static_cast<std::uint8_t>(command)) {
            throw_refusal(command, reply.command);
        }
        return reply;
    }

    // A command with no value: the reply is the 4-byte echo, or an error code.
    void command_locked(ScopeDomeCommand command, const std::string& payload) {
        const auto reply = exchange_locked(command, payload, 0);
        if (reply.command != static_cast<std::uint8_t>(command) &&
            reply.command != static_cast<std::uint8_t>(ScopeDomeCommand::Ack)) {
            throw_refusal(command, reply.command);
        }
    }

    [[noreturn]] static void throw_refusal(ScopeDomeCommand command, std::uint8_t answer) {
        std::string why;
        switch (static_cast<ScopeDomeCommand>(answer)) {
            case ScopeDomeCommand::MotionConflict: why = "motion conflict"; break;
            case ScopeDomeCommand::FunctionNotSupported: why = "not supported by this firmware"; break;
            case ScopeDomeCommand::ParamError: why = "parameter error"; break;
            case ScopeDomeCommand::FuncBufferError: why = "buffer error"; break;
            default: why = "unexpected reply " + std::to_string(answer); break;
        }
        throw AlpacaException(std::string("ScopeDome card refused ") + command_name(static_cast<std::uint8_t>(command)) +
                                  ": " + why,
                              AlpacaError::DriverException);
    }

    // One request and its reply frame; a timeout or a bad CRC is retried once (INDI).
    ScopeDomeFrame exchange_locked(ScopeDomeCommand command, const std::string& payload, std::size_t reply_len) {
#ifndef _WIN32
        const std::string frame = encode_scopedome_frame(command, payload);
        std::string last_error;
        for (int attempt = 0; attempt < kExchangeAttempts; ++attempt) {
            tcflush(link_.fd(), TCIOFLUSH);
            if (!util::tty_write_all(link_.fd(), frame)) {
                if (!link_.node_present()) {
                    mark_lost("ScopeDome card unplugged (" + config_.serial_port + " is gone)");
                    fail_link_locked();
                }
                last_error = "write failed";
                continue;
            }
            util::ReadSpec spec;
            spec.stop = util::ReadSpec::Stop::Length;
            spec.skip.clear();  // binary
            // An error reply is a 4-byte frame even to a query: read the head first.
            spec.length = kFrameOverhead;
            spec.max_len = kFrameOverhead;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.reply_timeout_ms);
            auto head = util::read_until(link_.fd(), spec, deadline);
            if (head.outcome == util::ReadOutcome::LinkDead) {
                mark_lost("ScopeDome card link lost on " + config_.serial_port + " (the port hung up)");
                fail_link_locked();
            }
            if (head.outcome != util::ReadOutcome::Length) {
                last_error = std::string("no answer to ") + command_name(static_cast<std::uint8_t>(command));
                continue;
            }
            std::string bytes = head.bytes;
            const std::size_t len = static_cast<std::uint8_t>(bytes[1]);
            if (static_cast<std::uint8_t>(bytes[0]) == kScopeDomeFrameHeader && len > 0) {
                spec.length = len;
                spec.max_len = len;
                auto body = util::read_until(link_.fd(), spec, deadline);
                if (body.outcome == util::ReadOutcome::LinkDead) {
                    mark_lost("ScopeDome card link lost on " + config_.serial_port + " (the port hung up)");
                    fail_link_locked();
                }
                if (body.outcome != util::ReadOutcome::Length) {
                    last_error = "short reply";
                    continue;
                }
                bytes += body.bytes;
            }
            ScopeDomeFrame reply;
            if (!decode_scopedome_frame(bytes, reply)) {
                last_error = std::string("corrupt reply to ") + command_name(static_cast<std::uint8_t>(command));
                continue;
            }
            if (reply.command == static_cast<std::uint8_t>(command) && reply.payload.size() != reply_len) {
                last_error = std::string("reply to ") + command_name(static_cast<std::uint8_t>(command)) +
                             " has the wrong length";
                continue;
            }
            return reply;
        }
        throw AlpacaException("ScopeDome card: " + last_error, AlpacaError::DriverException);
#else
        (void)command;
        (void)payload;
        (void)reply_len;
        throw AlpacaException("ScopeDome driver is Linux-only", AlpacaError::DriverException);
#endif
    }

#ifndef _WIN32
    // The port is gone: release it so a replugged card can be reopened, and fail NotConnected.
    [[noreturn]] void fail_link_locked() {
        const std::string reason = fault_text();
        if (connected_) {
            ALPACA_LOG_ERROR("ScopeDome", "Card link lost: " + reason + " (port released)");
            connected_ = false;
            link_.close();
        }
        throw AlpacaException(reason.empty() ? std::string("ScopeDome card link lost") : reason,
                              AlpacaError::NotConnected);
    }
#endif

    void mark_lost(const std::string& why) {
        {
            std::lock_guard<std::mutex> lock(fault_mutex_);
            if (!lost_.load()) fault_ = why;
        }
        lost_.store(true);
    }
    void clear_loss() {
        std::lock_guard<std::mutex> lock(fault_mutex_);
        lost_.store(false);
        fault_.clear();
    }
    std::string fault_text() const {
        std::lock_guard<std::mutex> lock(fault_mutex_);
        return fault_;
    }

    mutable std::mutex mutex_;  // the per-link I/O lock: one exchange at a time
    ScopeDomeConnectionConfig config_;
    bool connected_ = false;
    std::atomic<bool> lost_{false};
    mutable std::mutex fault_mutex_;  // leaf: guards fault_ only
    std::string fault_;
#ifndef _WIN32
    util::SerialLink link_;
#endif
};

ScopeDomeUsb21ProtocolWrapper::ScopeDomeUsb21ProtocolWrapper() : impl_(std::make_unique<Impl>()) {}
ScopeDomeUsb21ProtocolWrapper::~ScopeDomeUsb21ProtocolWrapper() = default;

ScopeDomeCardInfo ScopeDomeUsb21ProtocolWrapper::connect(const ScopeDomeConnectionConfig& config) {
    return impl_->connect(config);
}
void ScopeDomeUsb21ProtocolWrapper::disconnect() { impl_->disconnect(); }
bool ScopeDomeUsb21ProtocolWrapper::link_alive() const noexcept { return impl_->link_alive(); }
std::string ScopeDomeUsb21ProtocolWrapper::link_fault() const { return impl_->link_fault(); }
ScopeDomeCardState ScopeDomeUsb21ProtocolWrapper::read_state() { return impl_->read_state(); }
void ScopeDomeUsb21ProtocolWrapper::rotate(int steps) { impl_->rotate(steps); }
void ScopeDomeUsb21ProtocolWrapper::stop() { impl_->stop(); }
void ScopeDomeUsb21ProtocolWrapper::find_home() { impl_->find_home(); }
void ScopeDomeUsb21ProtocolWrapper::reset_counters() { impl_->reset_counters(); }
void ScopeDomeUsb21ProtocolWrapper::set_output(ScopeDomeChannel channel, bool on) { impl_->set_output(channel, on); }

}  // namespace alpacacore::vendor::scopedome
