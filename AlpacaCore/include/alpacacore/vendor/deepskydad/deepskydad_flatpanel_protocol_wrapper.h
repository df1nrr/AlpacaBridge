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

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace alpacacore::vendor::deepskydad {

/**
 * @brief Connection settings for a Deep Sky Dad flat panel (FP1 / FP2).
 *
 * The panel enumerates as a native USB CDC port (/dev/ttyACM*) and runs at a
 * fixed 115200 8N1 (INDI deepskydad_fp.cpp; StellarMate device page).
 */
struct FlatPanelConnectionConfig {
    std::string serial_port;  // e.g. "/dev/ttyACM0"; empty with auto_detect_index >= 0 scans at connect
    int auto_detect_index = -1;
    int reply_timeout_ms = 3000;  // INDI FLAT_TIMEOUT (3 s)
};

/// Which panel the [GFRM] firmware string names. FP1 and FP2 share the command
/// set but report the cover differently (FP1: [GPOS] angle 0 open / 270
/// closed; FP2: [GOPS] 0 closed / 1 open, anything else part-way).
enum class FlatPanelModel : std::uint8_t { FP1, FP2 };

/// One poll of the panel ([GMOV], [GLON], [GOPS]/[GPOS], [GLBR]).
struct FlatPanelStatus {
    bool motor_moving = false;  // [GMOV] 1
    bool light_on = false;      // [GLON] 1
    int cover_raw = -1;         // [GOPS] (FP2) or [GPOS] (FP1), as the panel reports it
    int brightness = 0;         // [GLBR], 0..kDeepSkyDadMaxBrightness
};

/// Cover position the raw reading names, per model. Part-way (or an FP1 angle
/// that is neither end) is Neither.
enum class FlatPanelCoverEnd : std::uint8_t { Closed, Open, Neither };

FlatPanelCoverEnd cover_end_for(FlatPanelModel model, int cover_raw);

/// Highest [SLBR] value (INDI LightIntensity max 4096).
constexpr int kDeepSkyDadMaxBrightness = 4096;

/// Parse "(...)" into its payload. False when the reply is not framed in parentheses.
bool parse_deepskydad_reply(const std::string& reply, std::string& payload);

/// The model a [GFRM] payload names, or false when it is not a Deep Sky Dad flat panel.
bool parse_deepskydad_firmware(const std::string& payload, FlatPanelModel& model);

/// USB descriptor tokens of the panel's port, shared by the scan and the
/// catalog's attached-device identity (issue #2312).
const std::vector<std::string>& deepskydad_flatpanel_usb_tokens();

struct FlatPanelPortInfo {
    std::string port_path;
    std::string firmware;  // empty when a connected sibling holds the port (not probed)
};

/// Ports that answer [GFRM] as a Deep Sky Dad flat panel, in scan order.
std::vector<FlatPanelPortInfo> enumerate_deepskydad_flatpanel_ports();

/// The [GFRM] handshake on one port under a probe claim.
bool probe_deepskydad_flatpanel_port(const std::string& port_path, std::string& firmware);

/**
 * @brief Serial protocol wrapper for the Deep Sky Dad FP1/FP2 flat panel.
 *
 * Commands are "[XXXX...]"; every reply is "(...)". Protocol per INDI
 * drivers/auxiliary/deepskydad_fp.cpp (not confirmed against the vendor's
 * ASCOM driver): [GFRM] firmware, [GMOV] motor running, [GLON] light,
 * [GOPS]/[GPOS] cover, [GLBR]/[SLBRnnnn] brightness, [SLON0|1] light,
 * [STRG0|270] + [SMOV] cover target and move (reply "(OK)").
 *
 * One mutex serialises the wire; the loss verdict is the util::SerialLink's
 * (lock-free) and its text never waits on an exchange.
 */
class DeepSkyDadFlatPanelProtocolWrapper {
public:
    DeepSkyDadFlatPanelProtocolWrapper();
    ~DeepSkyDadFlatPanelProtocolWrapper();

    DeepSkyDadFlatPanelProtocolWrapper(const DeepSkyDadFlatPanelProtocolWrapper&) = delete;
    DeepSkyDadFlatPanelProtocolWrapper& operator=(const DeepSkyDadFlatPanelProtocolWrapper&) = delete;

    /// Open the port and run the [GFRM] handshake. Returns the firmware string.
    std::string connect(const FlatPanelConnectionConfig& config);
    void disconnect();

    FlatPanelModel model() const noexcept;

    /// False once an exchange saw the port gone (decision 0009 rule 2). Lock-free.
    bool link_alive() const noexcept;
    /// The loss reason, kept until the next connect or disconnect; empty when none.
    std::string link_fault() const;

    FlatPanelStatus get_status();
    void move_cover(bool open);
    /// on: [SLBRnnnn] then [SLON1]; off: [SLON0].
    void set_light(bool on, int brightness);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace alpacacore::vendor::deepskydad
