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
#include <alpacacore/vendor/simulator/simulator_dome_model.h>

#include <memory>

namespace alpacacore::vendor::simulator {

/**
 * @brief A simulated observatory dome (IDomeV3) with no hardware.
 *
 * Rotates at a fixed rate the short way round, opens and closes its shutter in
 * a fixed time, parks, finds home and syncs. No altitude control and no
 * slaving. For testing clients (ARA, NINA) and the Dome HTTP surface.
 *
 * @param device_number Alpaca device number
 * @param config        Rates and positions
 * @param clock         The clock the motion runs on (tests pass a FakeTaskClock)
 */
std::unique_ptr<DomeDriver> create_simulator_dome(int device_number, const DomeSimulatorConfig& config = {},
                                                  util::TaskClock& clock = util::default_task_clock());

}  // namespace alpacacore::vendor::simulator
