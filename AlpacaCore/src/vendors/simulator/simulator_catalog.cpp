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

// The simulator factories, compiled only under ALPACACORE_ENABLE_SIMULATOR. No hardware.

#include <alpacacore/vendor/simulator/simulator_dome_driver.h>
#include <alpacacore/vendor/simulator/simulator_dome_model.h>

#include "../../catalog/builtin_descriptors.h"
#include "simulator_fields.h"

namespace alpacacore::catalog {

void register_simulator_factory(DeviceCatalog& catalog) {
    Factory dome;
    dome.key = DeviceKey{"simulator", DeviceType::Dome};
    dome.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        vendor::simulator::DomeSimulatorConfig sim;
        sim.slew_rate_deg_per_s = config.get(kSimulatorDomeSlewRate);
        sim.shutter_time_s = config.get(kSimulatorDomeShutterTime);
        sim.home_azimuth = config.get(kSimulatorDomeHomeAzimuth);
        sim.park_azimuth = config.get(kSimulatorDomeParkAzimuth);
        sim.start_azimuth = sim.park_azimuth;  // a fresh simulator starts parked-in-position, shutter closed
        return vendor::simulator::create_simulator_dome(device_number, sim);
    };
    catalog.add(std::move(dome));
}

}  // namespace alpacacore::catalog
