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

// The ScopeDome factory, compiled only under ALPACACORE_ENABLE_SCOPEDOME.

#include <alpacacore/vendor/scopedome/scopedome_dome_driver.h>

#include "../../catalog/builtin_descriptors.h"
#include "scopedome_fields.h"

namespace alpacacore::catalog {

void register_scopedome_factory(DeviceCatalog& catalog) {
    Factory dome;
    dome.key = DeviceKey{"scopedome", DeviceType::Dome};
    dome.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        vendor::scopedome::ScopeDomeDomeConfig cfg;
        cfg.serial_port = config.get(kScopeDomePortPath);
        cfg.home_azimuth = config.get(kScopeDomeHomeAzimuth);
        cfg.park_azimuth = config.get(kScopeDomeParkAzimuth);
        cfg.azimuth_tolerance = config.get(kScopeDomeAzimuthTolerance);
        cfg.shutter_timeout_s = config.get(kScopeDomeShutterTimeout);
        return vendor::scopedome::create_scopedome_dome(device_number, cfg);
    };
    catalog.add(std::move(dome));
}

}  // namespace alpacacore::catalog
