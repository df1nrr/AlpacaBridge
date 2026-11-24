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

// OpenWeather catalog field declarations, shared by openweather_schema.cpp (no vendor
// header, compiled in every build) and openweather_catalog.cpp (the factories).
// The API key is a Plain field, not Role::Secret: a Secret field is never persisted,
// and the driver needs the key after a restart. The web form masks it.

#include <alpacacore/catalog/device_catalog.h>

#include <cstdint>
#include <string>
#include <vector>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

namespace alpacacore::catalog {

inline const Field<std::string> kOpenWeatherApiKey{.key = "apiKey", .default_value = ""};
inline const Field<double> kOpenWeatherLatitude{.key = "latitude", .default_value = 0.0, .min = -90.0, .max = 90.0};
inline const Field<double> kOpenWeatherLongitude{
    .key = "longitude", .default_value = 0.0, .min = -180.0, .max = 180.0};
// The free plan allows 1000 calls a day: 120 s is 720.
inline const Field<std::int64_t> kOpenWeatherPollIntervalSeconds{
    .key = "pollIntervalSeconds", .default_value = 600, .min = 120, .max = 3600};

inline const std::vector<FieldRef>& openweather_observingconditions_fields() {
    static const std::vector<FieldRef> fields{kOpenWeatherApiKey.ref(), kOpenWeatherLatitude.ref(),
                                              kOpenWeatherLongitude.ref(), kOpenWeatherPollIntervalSeconds.ref()};
    return fields;
}

}  // namespace alpacacore::catalog

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
