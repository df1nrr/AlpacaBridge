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

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace alpacacore::vendor::openweather {

/// Where to ask and how often. The free OpenWeather plan allows 1000 calls a day:
/// one every 10 minutes is 144.
struct OpenWeatherConfig {
    std::string api_key;
    double latitude = 0.0;
    double longitude = 0.0;
    std::chrono::seconds poll_interval{std::chrono::seconds(600)};
    std::chrono::milliseconds timeout{std::chrono::milliseconds(10000)};
    std::string base_url = "https://api.openweathermap.org/data/2.5/weather";
};

/// One "current weather" reading, metric. NaN for a value the reply did not carry.
struct OpenWeatherCurrent {
    double temperature_c;
    double humidity;
    double dewpoint_c;  ///< computed from temperature and humidity (Magnus); the API sends none
    double pressure_hpa;
    double wind_speed_ms;
    double wind_direction_deg;
    double wind_gust_ms;
    double cloud_cover_pct;
    double rain_rate_mmh;  ///< rain.1h; 0 when the reply has no rain block (it omits it when dry)
    double epoch_s;        ///< observation time (dt)
};

/// The HTTP seam: returns the body for a 2xx answer, throws AlpacaException otherwise.
using OpenWeatherFetcher = std::function<std::string(const std::string& url, std::chrono::milliseconds timeout)>;

/// Parse a /data/2.5/weather reply. nullopt when it is not one (no main block).
std::optional<OpenWeatherCurrent> parse_openweather_current(std::string_view payload);

/// The request URL for @p config (units=metric).
std::string openweather_request_url(const OpenWeatherConfig& config);

/// The default fetcher: an HTTPS GET through libcurl (256 KiB cap, http/https only).
std::string openweather_http_get(const std::string& url, std::chrono::milliseconds timeout);

/// One request: the current weather for @p config, parsed. Throws AlpacaException (DriverException)
/// when the request fails or the reply is not current weather.
OpenWeatherCurrent fetch_openweather_current(const OpenWeatherConfig& config, const OpenWeatherFetcher& fetcher);

/// Dew point (deg C) from temperature (deg C) and relative humidity (%), Magnus-Tetens.
double openweather_dew_point(double temperature_c, double humidity_pct);

std::unique_ptr<ObservingConditionsDriver> create_openweather_observingconditions(int device_number,
                                                                                  const OpenWeatherConfig& config);

std::unique_ptr<ObservingConditionsDriver> create_openweather_observingconditions(int device_number,
                                                                                  const OpenWeatherConfig& config,
                                                                                  OpenWeatherFetcher fetcher);

}  // namespace alpacacore::vendor::openweather
