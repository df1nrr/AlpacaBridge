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

// The allsky schemas: the camera's cloud cover and the weather safety monitor.
// No vendor header: compiles in every build.

#include "../../catalog/builtin_descriptors.h"
#include "allsky_fields.h"

namespace alpacacore::catalog {

void register_allsky_schema(DeviceCatalog& catalog) {
    Schema camera;
    camera.key = DeviceKey{"allsky", DeviceType::ObservingConditions};
    camera.display_name = "Allsky camera (cloud cover)";
    camera.build_option = "ALPACACORE_ENABLE_ALLSKY";
    camera.fields = allsky_observingconditions_fields();
    catalog.add(std::move(camera));

    Schema safety;
    safety.key = DeviceKey{"allsky", DeviceType::SafetyMonitor};
    safety.display_name = "Weather safety (OpenWeather + allsky camera)";
    safety.build_option = "ALPACACORE_ENABLE_ALLSKY";
    safety.fields = allsky_safetymonitor_fields();
    catalog.add(std::move(safety));
}

}  // namespace alpacacore::catalog
