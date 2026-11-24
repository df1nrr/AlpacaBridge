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

// Allsky camera and weather-safety catalog field declarations, shared by allsky_schema.cpp
// (no vendor header, compiled in every build) and allsky_catalog.cpp (the factories). The
// OpenWeather API key is a Plain field: a Role::Secret field is never persisted.

#include <alpacacore/catalog/device_catalog.h>

#include <cstdint>
#include <string>
#include <vector>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

namespace alpacacore::catalog {

inline const Field<std::string> kAllskyImageUrl{.key = "imageUrl", .default_value = ""};
inline const Field<double> kAllskyLatitude{.key = "latitude", .default_value = 0.0, .min = -90.0, .max = 90.0};
inline const Field<double> kAllskyLongitude{.key = "longitude", .default_value = 0.0, .min = -180.0, .max = 180.0};
inline const Field<std::int64_t> kAllskyClearSkyStars{
    .key = "clearSkyStars", .default_value = 200, .min = 10, .max = 5000};
inline const Field<double> kAllskyRoiCenterX{.key = "roiCenterX", .default_value = 0.47, .min = 0.0, .max = 1.0};
inline const Field<double> kAllskyRoiCenterY{.key = "roiCenterY", .default_value = 0.50, .min = 0.0, .max = 1.0};
inline const Field<double> kAllskyRoiRadius{.key = "roiRadius", .default_value = 0.36, .min = 0.05, .max = 1.0};
inline const Field<std::int64_t> kAllskyPollIntervalSeconds{
    .key = "pollIntervalSeconds", .default_value = 60, .min = 30, .max = 3600};

inline const Field<std::string> kSafetyApiKey{.key = "apiKey", .default_value = ""};
inline const Field<std::int64_t> kSafetyWeatherPollIntervalSeconds{
    .key = "weatherPollIntervalSeconds", .default_value = 600, .min = 120, .max = 3600};
inline const Field<double> kSafetyMaxCloudCover{.key = "maxCloudCover", .default_value = 50.0, .min = 0.0, .max = 100.0};
inline const Field<double> kSafetyMaxWindSpeed{.key = "maxWindSpeed", .default_value = 10.0, .min = 0.0, .max = 60.0};
inline const Field<double> kSafetyMaxWindGust{.key = "maxWindGust", .default_value = 15.0, .min = 0.0, .max = 80.0};
inline const Field<double> kSafetyMaxHumidity{.key = "maxHumidity", .default_value = 95.0, .min = 0.0, .max = 100.0};
inline const Field<std::int64_t> kSafetyHoldOffMinutes{
    .key = "holdOffMinutes", .default_value = 15, .min = 0, .max = 240};

inline const std::vector<FieldRef>& allsky_observingconditions_fields() {
    static const std::vector<FieldRef> fields{kAllskyImageUrl.ref(),    kAllskyLatitude.ref(),   kAllskyLongitude.ref(),
                                              kAllskyClearSkyStars.ref(), kAllskyRoiCenterX.ref(), kAllskyRoiCenterY.ref(),
                                              kAllskyRoiRadius.ref(),   kAllskyPollIntervalSeconds.ref()};
    return fields;
}

inline const std::vector<FieldRef>& allsky_safetymonitor_fields() {
    static const std::vector<FieldRef> fields{
        kSafetyApiKey.ref(),        kAllskyLatitude.ref(),     kAllskyLongitude.ref(),
        kAllskyImageUrl.ref(),      kAllskyClearSkyStars.ref(), kAllskyRoiCenterX.ref(),
        kAllskyRoiCenterY.ref(),    kAllskyRoiRadius.ref(),    kAllskyPollIntervalSeconds.ref(),
        kSafetyWeatherPollIntervalSeconds.ref(), kSafetyMaxCloudCover.ref(), kSafetyMaxWindSpeed.ref(),
        kSafetyMaxWindGust.ref(),   kSafetyMaxHumidity.ref(),  kSafetyHoldOffMinutes.ref()};
    return fields;
}

}  // namespace alpacacore::catalog

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
