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

// The OpenWeather factory, compiled only under ALPACACORE_ENABLE_OPENWEATHER.

#include <alpacacore/vendor/openweather/openweather_observingconditions_driver.h>

#include "../../catalog/builtin_descriptors.h"
#include "openweather_fields.h"

namespace alpacacore::catalog {

void register_openweather_factory(DeviceCatalog& catalog) {
    Factory factory;
    factory.key = DeviceKey{"openweather", DeviceType::ObservingConditions};
    factory.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        vendor::openweather::OpenWeatherConfig cfg;
        cfg.api_key = config.get(kOpenWeatherApiKey);
        cfg.latitude = config.get(kOpenWeatherLatitude);
        cfg.longitude = config.get(kOpenWeatherLongitude);
        cfg.poll_interval = std::chrono::seconds(config.get(kOpenWeatherPollIntervalSeconds));
        return vendor::openweather::create_openweather_observingconditions(device_number, cfg);
    };
    catalog.add(std::move(factory));
}

}  // namespace alpacacore::catalog
