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

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace alpacacore::vendor::scopedome {

// ScopeDome USB Card 2.1 wire protocol (115200 8N1, binary frames).
//
// A frame is: 0xAA, payload length, command, CRC-8, payload (little endian).
// The CRC is the Dallas/Maxim CRC-8 (reflected polynomial 0x8C, initial 0) over
// the header, length, command and payload bytes, in that order. A command with
// no reply value is answered by a 4-byte frame echoing the command, or carrying
// one of the error codes below. A query is answered by a frame of the same
// command with the value as its payload.
//
// Source: the protocol as the INDI ScopeDome driver (scopedome_usb21.cpp, based
// on the ScopeDome Windows ASCOM driver 5.1.30) speaks it. Unconfirmed on
// hardware by this project until a rig run.

inline constexpr std::uint8_t kScopeDomeFrameHeader = 0xAA;

// Command numbers (the card's enum; only the ones this driver uses).
enum class ScopeDomeCommand : std::uint8_t {
    Ack = 1,
    FunctionNotSupported = 2,
    MotionConflict = 3,
    ParamError = 4,
    FuncBufferError = 5,
    ConnectionTest = 15,
    ClearDigitalChannel = 17,
    SetDigitalChannel = 19,
    GetCounter = 22,
    ResetCounter = 23,
    CcwRotation = 29,
    CwRotation = 30,
    GetVersionFirmware = 39,
    GetStatus = 43,
    GetCounterExt = 61,
    ResetCounterExt = 62,
    GetAllDigitalExt = 68,
    GetImpPerTurn = 97,
    Stop = 98,
    FindHome = 106,
    GetVersionFirmwareRotary = 115,
    StopSafeCommunication = 149,
    IsFullSystemCalReq = 153,
};

// Bits of the GetStatus word.
inline constexpr std::uint16_t kScopeDomeStatusMoving = 0x0002;
inline constexpr std::uint16_t kScopeDomeStatusHoming = 0x0004;
inline constexpr std::uint16_t kScopeDomeStatusCalibrating = 0x0080;

// Digital channels (GetAllDigitalExt bit index; Set/ClearDigitalChannel argument).
enum class ScopeDomeChannel : std::uint8_t {
    OutCcw = 0,
    OutCw = 1,
    OutOpen1 = 2,
    OutClose1 = 3,
    InHome = 13,
    InOpen1 = 14,
    InClosed1 = 15,
    InRotaryLink = 35,
};

inline constexpr std::size_t kScopeDomeDigitalBytes = 5;

/// CRC-8 step (Dallas/Maxim, reflected 0x8C).
std::uint8_t scopedome_crc8(std::uint8_t crc, std::uint8_t data);

/// Encode one frame.
std::string encode_scopedome_frame(ScopeDomeCommand command, const std::string& payload = std::string());

struct ScopeDomeFrame {
    std::uint8_t command = 0;
    std::string payload;
};

/// Decode a complete frame (header, length, command, CRC, payload). False when the
/// header, the length or the CRC does not match.
bool decode_scopedome_frame(const std::string& bytes, ScopeDomeFrame& out);

/// One poll's worth of card state.
struct ScopeDomeCardState {
    std::uint16_t status = 0;
    std::int16_t counter = 0;  // rotation steps since the last counter reset (home); grows counter-clockwise
    std::array<std::uint8_t, kScopeDomeDigitalBytes> digital{};

    bool input(ScopeDomeChannel channel) const {
        const auto ch = static_cast<unsigned>(channel);
        return (digital[ch >> 3] & (1u << (ch & 7u))) != 0;
    }
    bool moving() const { return (status & kScopeDomeStatusMoving) != 0; }
    bool homing() const { return (status & kScopeDomeStatusHoming) != 0; }
};

/// What the card reports once at connect.
struct ScopeDomeCardInfo {
    double firmware_main = 0.0;
    double firmware_rotary = 0.0;
    std::uint32_t steps_per_turn = 0;
    bool calibration_needed = false;
};

struct ScopeDomeConnectionConfig {
    std::string serial_port;
    int reply_timeout_ms = 2000;
};

/// The ScopeDome USB Card 2.1. One I/O lock serialises every exchange; every
/// call but link_alive()/link_fault() does I/O and throws AlpacaException
/// (NotConnected when the port is gone, DriverException when the card refused
/// or did not answer).
class ScopeDomeUsb21ProtocolWrapper {
public:
    ScopeDomeUsb21ProtocolWrapper();
    ~ScopeDomeUsb21ProtocolWrapper();
    ScopeDomeUsb21ProtocolWrapper(const ScopeDomeUsb21ProtocolWrapper&) = delete;
    ScopeDomeUsb21ProtocolWrapper& operator=(const ScopeDomeUsb21ProtocolWrapper&) = delete;

    /// Open the port, check the card answers ConnectionTest, turn off its "safe
    /// communication" watchdog and read the firmware and steps per turn.
    ScopeDomeCardInfo connect(const ScopeDomeConnectionConfig& config);
    void disconnect();

    bool link_alive() const noexcept;
    std::string link_fault() const;

    ScopeDomeCardState read_state();

    /// Rotate by @p steps: positive clockwise, negative counter-clockwise.
    void rotate(int steps);
    void stop();
    void find_home();
    void reset_counters();
    void set_output(ScopeDomeChannel channel, bool on);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace alpacacore::vendor::scopedome
