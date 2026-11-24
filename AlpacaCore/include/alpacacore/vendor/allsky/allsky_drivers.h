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

#include <alpacacore/observingconditions_driver.h>
#include <alpacacore/safetymonitor_driver.h>
#include <alpacacore/vendor/allsky/allsky_image_analysis.h>
#include <alpacacore/vendor/openweather/openweather_observingconditions_driver.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace alpacacore::vendor::allsky {

/// The HTTP seam for the camera image: returns the body (the JPEG) or throws AlpacaException.
using AllskyFetcher = std::function<std::string(const std::string& url, std::chrono::milliseconds timeout)>;

/// The default fetcher: a GET through libcurl, http/https only, 16 MiB cap.
std::string allsky_http_get(const std::string& url, std::chrono::milliseconds timeout);

/// The clock the night test and the hold-off read (tests pass a fixed one).
using AllskyClock = std::function<std::chrono::system_clock::time_point()>;

struct AllskyCameraConfig {
    std::string image_url;  ///< the latest frame, e.g. https://example.org/allsky/image.jpg
    double latitude = 0.0;
    double longitude = 0.0;
    int clear_sky_stars = 200;  ///< stars a clear night shows: 0 % cloud at or above it
    AllskyRoi roi;
    std::chrono::seconds poll_interval{std::chrono::seconds(120)};
    std::chrono::milliseconds timeout{std::chrono::milliseconds(15000)};
};

/**
 * @brief Cloud cover from an allsky camera (IObservingConditionsV2: CloudCover only).
 *
 * Fetches the latest frame and counts its stars; CloudCover is 100 % minus the share of
 * the clear-sky count. It is only measured while the sun is below -6 degrees: in daylight
 * and twilight CloudCover has no value (ValueNotSet).
 */
std::unique_ptr<ObservingConditionsDriver> create_allsky_observingconditions(int device_number,
                                                                             const AllskyCameraConfig& config,
                                                                             AllskyFetcher fetcher = {},
                                                                             AllskyClock clock = {});

struct WeatherSafetyConfig {
    openweather::OpenWeatherConfig weather;  ///< api_key, latitude, longitude, poll interval
    AllskyCameraConfig camera;               ///< image_url empty: no camera, OpenWeather clouds only
    double max_cloud_cover = 50.0;           ///< %
    double max_wind_speed = 10.0;            ///< m/s
    double max_wind_gust = 15.0;             ///< m/s
    double max_humidity = 95.0;              ///< %
    std::chrono::seconds hold_off{std::chrono::minutes(15)};
    /// A reading older than this is no reading: unsafe.
    std::chrono::seconds max_weather_age{std::chrono::minutes(30)};
    std::chrono::seconds max_camera_age{std::chrono::minutes(15)};
};

/**
 * @brief A rules-based SafetyMonitor over OpenWeather and, optionally, an allsky camera.
 *
 * Unsafe at once when it rains, the wind, gusts or humidity exceed their limits, the
 * clouds exceed theirs (the camera's count at night, OpenWeather's cover otherwise), or a
 * reading is missing or too old; safe only after every rule has held for the hold-off
 * time. IsSafe is false while disconnected (ISafetyMonitor).
 */
std::unique_ptr<SafetyMonitorDriver> create_weather_safetymonitor(int device_number, const WeatherSafetyConfig& config,
                                                                  openweather::OpenWeatherFetcher weather_fetcher = {},
                                                                  AllskyFetcher camera_fetcher = {},
                                                                  AllskyClock clock = {});

}  // namespace alpacacore::vendor::allsky
