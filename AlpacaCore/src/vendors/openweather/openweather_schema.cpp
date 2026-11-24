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

// The OpenWeather schemas. No vendor header: compiles in every build.

#include "../../catalog/builtin_descriptors.h"
#include "openweather_fields.h"

namespace alpacacore::catalog {

void register_openweather_schema(DeviceCatalog& catalog) {
    Schema schema;
    schema.key = DeviceKey{"openweather", DeviceType::ObservingConditions};
    schema.display_name = "OpenWeather";
    schema.build_option = "ALPACACORE_ENABLE_OPENWEATHER";
    schema.fields = openweather_observingconditions_fields();
    catalog.add(std::move(schema));
}

}  // namespace alpacacore::catalog
