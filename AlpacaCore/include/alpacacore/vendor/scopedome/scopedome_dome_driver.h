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

#include <alpacacore/dome_driver.h>
#include <alpacacore/util/task_clock.h>

#include <memory>
#include <string>

namespace alpacacore::vendor::scopedome {

/// Per-dome settings (catalog fields; azimuths in degrees, 0 = north, clockwise).
struct ScopeDomeDomeConfig {
    std::string serial_port;
    double home_azimuth = 0.0;       ///< where the home sensor sits; the card counts from there
    double park_azimuth = 0.0;       ///< where Park rotates to (SetPark changes it until a restart)
    double azimuth_tolerance = 1.0;  ///< a slew ends within this of its target (INDI's refine bound)
    double shutter_timeout_s = 180;  ///< a shutter that has not reached its limit switch by then failed
};

/**
 * @brief ScopeDome USB Card 2.1 dome (IDomeV3): rotation, home, park and shutter 1.
 *
 * Azimuth comes from the card's rotation counter, zeroed at the home sensor by
 * FindHome. A slew is refined (up to three re-sends) until it ends within the
 * tolerance. The shutter is driven by the card's OPEN1/CLOSE1 outputs, stopped
 * by the driver when the matching limit input closes. No altitude, slaving or
 * azimuth sync.
 */
std::unique_ptr<DomeDriver> create_scopedome_dome(int device_number, const ScopeDomeDomeConfig& config,
                                                  util::TaskClock& clock = util::default_task_clock());

}  // namespace alpacacore::vendor::scopedome
