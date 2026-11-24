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

#include <alpacacore/covercalibrator_driver.h>
#include <alpacacore/util/connection_resolver.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/deepskydad/deepskydad_flatpanel_protocol_wrapper.h>

#include <memory>
#include <string>

namespace alpacacore::vendor::deepskydad {

/**
 * @brief Deep Sky Dad FP1/FP2 flat panel (motorized flap + LED panel) on a configured port.
 *
 * @param device_number Alpaca device number
 * @param serial_port   Serial port path (e.g. "/dev/ttyACM0")
 * @param clock         Clock for the state poll and the cover-move timing (tests pass a FakeTaskClock)
 */
std::unique_ptr<CoverCalibratorDriver> create_deepskydad_covercalibrator(
    int device_number, const std::string& serial_port, util::TaskClock& clock = util::default_task_clock());

/// The panel found at connect: the @p panel_index-th (0-based) panel the scan finds.
std::unique_ptr<CoverCalibratorDriver> create_deepskydad_covercalibrator_by_index(int device_number, int panel_index);

/// Test seam (serial-autodetect rules): the port comes from @p resolver at connect.
std::unique_ptr<CoverCalibratorDriver> create_deepskydad_covercalibrator_deferred(
    int device_number, util::ConnectionResolver<FlatPanelConnectionConfig> resolver,
    util::TaskClock& clock = util::default_task_clock());

/// The scan body of the by-index factory: the port of the @p panel_index-th panel found.
FlatPanelConnectionConfig resolve_deepskydad_flatpanel_by_index(int panel_index);

}  // namespace alpacacore::vendor::deepskydad
