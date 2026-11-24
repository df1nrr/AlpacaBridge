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

// The allsky factories, compiled only under ALPACACORE_ENABLE_ALLSKY.

#include <alpacacore/vendor/allsky/allsky_drivers.h>

#include "../../catalog/builtin_descriptors.h"
#include "allsky_fields.h"

namespace alpacacore::catalog {

namespace {

vendor::allsky::AllskyCameraConfig camera_config(const DeviceConfig& config) {
    vendor::allsky::AllskyCameraConfig cfg;
    cfg.image_url = config.get(kAllskyImageUrl);
    cfg.latitude = config.get(kAllskyLatitude);
    cfg.longitude = config.get(kAllskyLongitude);
    cfg.clear_sky_stars = static_cast<int>(config.get(kAllskyClearSkyStars));
    cfg.roi.center_x = config.get(kAllskyRoiCenterX);
    cfg.roi.center_y = config.get(kAllskyRoiCenterY);
    cfg.roi.radius = config.get(kAllskyRoiRadius);
    cfg.poll_interval = std::chrono::seconds(config.get(kAllskyPollIntervalSeconds));
    return cfg;
}

}  // namespace

void register_allsky_factory(DeviceCatalog& catalog) {
    Factory camera;
    camera.key = DeviceKey{"allsky", DeviceType::ObservingConditions};
    camera.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        return vendor::allsky::create_allsky_observingconditions(device_number, camera_config(config));
    };
    catalog.add(std::move(camera));

    Factory safety;
    safety.key = DeviceKey{"allsky", DeviceType::SafetyMonitor};
    safety.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        vendor::allsky::WeatherSafetyConfig cfg;
        cfg.camera = camera_config(config);
        cfg.weather.api_key = config.get(kSafetyApiKey);
        cfg.weather.latitude = cfg.camera.latitude;
        cfg.weather.longitude = cfg.camera.longitude;
        cfg.weather.poll_interval = std::chrono::seconds(config.get(kSafetyWeatherPollIntervalSeconds));
        cfg.max_cloud_cover = config.get(kSafetyMaxCloudCover);
        cfg.max_wind_speed = config.get(kSafetyMaxWindSpeed);
        cfg.max_wind_gust = config.get(kSafetyMaxWindGust);
        cfg.max_humidity = config.get(kSafetyMaxHumidity);
        cfg.hold_off = std::chrono::minutes(config.get(kSafetyHoldOffMinutes));
        return vendor::allsky::create_weather_safetymonitor(device_number, cfg);
    };
    catalog.add(std::move(safety));
}

}  // namespace alpacacore::catalog
