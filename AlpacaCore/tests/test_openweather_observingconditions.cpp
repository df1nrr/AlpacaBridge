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

// OpenWeather current weather. The HTTP fetch is injected (OpenWeatherFetcher), so the
// cases run with no network. The sweep entry (contract_sweep.h) covers the NotConnected
// and unsupported Action/Command codes (cases 3 and 4).

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/openweather/openweather_observingconditions_driver.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

#include "catch2_compat.h"
#include "driver_rig.h"

namespace {

namespace AlpacaError = alpacacore::AlpacaError;
namespace ow = alpacacore::vendor::openweather;
using alpacacore::test::require_alpaca_error;
using Catch::Approx;

std::string reply(bool rain = false, long long dt = static_cast<long long>(std::time(nullptr))) {
    std::string s = R"({"coord":{"lon":11.4,"lat":47.8},"weather":[{"id":803,"main":"Clouds"}],"base":"stations",)"
                    R"("main":{"temp":4.5,"feels_like":1.2,"temp_min":3.9,"temp_max":5.1,"pressure":1021,"humidity":81},)"
                    R"("visibility":10000,"wind":{"speed":3.6,"deg":250,"gust":7.2},"clouds":{"all":75},)";
    if (rain) s += R"("rain":{"1h":0.42},)";
    s += R"("dt":)" + std::to_string(dt) + R"(,"sys":{"country":"DE"},"timezone":3600,"id":1,"name":"Koenigsdorf","cod":200})";
    return s;
}

struct Fake {
    std::mutex m;
    std::string body = reply();
    std::string last_url;
    std::atomic<int> calls{0};
    std::atomic<bool> fail{false};
    ow::OpenWeatherFetcher fetcher() {
        return [this](const std::string& url, std::chrono::milliseconds) {
            ++calls;
            std::lock_guard<std::mutex> lock(m);
            last_url = url;
            if (fail.load()) throw alpacacore::AlpacaException("scripted failure", AlpacaError::DriverException);
            return body;
        };
    }
};

ow::OpenWeatherConfig config() {
    ow::OpenWeatherConfig c;
    c.api_key = "0123456789abcdef";
    c.latitude = 47.8296;
    c.longitude = 11.4598;
    c.poll_interval = std::chrono::seconds(3600);
    return c;
}

}  // namespace

TEST_CASE("OpenWeather ObservingConditions Driver - Defaults", "[openweather][observingconditions][unit]") {
    Fake fake;
    auto driver = ow::create_openweather_observingconditions(0, config(), fake.fetcher());
    CHECK(driver->get_device_type() == alpacacore::DeviceType::ObservingConditions);
    CHECK_FALSE(driver->get_connected());
    CHECK_FALSE(driver->get_connecting());
    CHECK(driver->get_name() == "OpenWeather");
    CHECK(fake.calls == 0);  // nothing is fetched before a connect
}

TEST_CASE("OpenWeather ObservingConditions Driver - Device metadata", "[openweather][observingconditions][unit]") {
    Fake fake;
    auto driver = ow::create_openweather_observingconditions(2, config(), fake.fetcher());
    CHECK(driver->get_device_number() == 2);
    CHECK(driver->get_description() == "OpenWeather current weather at 47.8296, 11.4598");
    CHECK(driver->get_driver_info() == "AlpacaCore OpenWeather ObservingConditions Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 2);
    CHECK(driver->get_unique_id() == "OPENWEATHER_OC_2");
}

TEST_CASE("OpenWeather ObservingConditions Driver - reply parsing", "[openweather][observingconditions][unit]") {
    const auto c = ow::parse_openweather_current(reply(true, 1700000000));
    REQUIRE(c.has_value());
    CHECK(c->temperature_c == Approx(4.5));
    CHECK(c->humidity == Approx(81.0));
    CHECK(c->pressure_hpa == Approx(1021.0));
    CHECK(c->wind_speed_ms == Approx(3.6));
    CHECK(c->wind_direction_deg == Approx(250.0));
    CHECK(c->wind_gust_ms == Approx(7.2));
    CHECK(c->cloud_cover_pct == Approx(75.0));
    CHECK(c->rain_rate_mmh == Approx(0.42));
    CHECK(c->epoch_s == Approx(1700000000.0));
    CHECK(c->dewpoint_c == Approx(1.5).margin(0.2));  // 4.5 C at 81 %

    const auto dry = ow::parse_openweather_current(reply(false));
    REQUIRE(dry.has_value());
    CHECK(dry->rain_rate_mmh == 0.0);  // no rain block: it is dry

    CHECK_FALSE(ow::parse_openweather_current(R"({"cod":401,"message":"Invalid API key"})").has_value());
    CHECK_FALSE(ow::parse_openweather_current("").has_value());
    CHECK(std::isnan(ow::openweather_dew_point(10.0, 0.0)));
}

TEST_CASE("OpenWeather ObservingConditions Driver - request URL", "[openweather][observingconditions][unit]") {
    auto c = config();
    c.api_key = "abc&x=1";  // nothing but the key's own characters reaches the query
    const auto url = ow::openweather_request_url(c);
    CHECK(url == "https://api.openweathermap.org/data/2.5/weather?lat=47.82960&lon=11.45980&units=metric&appid=abcx1");
}

TEST_CASE("OpenWeather ObservingConditions Driver - connected values", "[openweather][observingconditions][unit]") {
    Fake fake;
    fake.body = reply(true);
    auto driver = ow::create_openweather_observingconditions(0, config(), fake.fetcher());
    driver->set_connected(true);
    REQUIRE(driver->get_connected());
    CHECK(fake.calls == 1);
    CHECK(fake.last_url.find("appid=0123456789abcdef") != std::string::npos);
    CHECK(driver->get_temperature() == Approx(4.5));
    CHECK(driver->get_humidity() == Approx(81.0));
    CHECK(driver->get_pressure() == Approx(1021.0));
    CHECK(driver->get_dew_point() == Approx(1.5).margin(0.2));
    CHECK(driver->get_wind_speed() == Approx(3.6));
    CHECK(driver->get_wind_direction() == Approx(250.0));
    CHECK(driver->get_wind_gust() == Approx(7.2));
    CHECK(driver->get_cloud_cover() == Approx(75.0));
    CHECK(driver->get_rain_rate() == Approx(0.42));
    CHECK(driver->get_average_period() == 0.0);
    CHECK(driver->get_time_since_last_update("Temperature") >= 0.0);
    CHECK(driver->get_time_since_last_update("") < 5.0);
    CHECK(driver->get_sensor_description("CloudCover").find("clouds.all") != std::string::npos);
    driver->set_connected(false);
    CHECK_FALSE(driver->get_connected());
}

TEST_CASE("OpenWeather ObservingConditions Driver - a failed connect keeps the reason", "[openweather][observingconditions][unit]") {
    Fake fake;
    fake.fail = true;
    auto driver = ow::create_openweather_observingconditions(0, config(), fake.fetcher());
    require_alpaca_error([&] { driver->set_connected(true); }, AlpacaError::DriverException);
    CHECK_FALSE(driver->get_connected());
    fake.fail = false;
    fake.body = R"({"cod":401,"message":"Invalid API key"})";
    require_alpaca_error([&] { driver->set_connected(true); }, AlpacaError::DriverException);
    CHECK_FALSE(driver->get_connected());

    auto no_key_cfg = config();
    no_key_cfg.api_key.clear();
    auto no_key = ow::create_openweather_observingconditions(0, no_key_cfg, fake.fetcher());
    require_alpaca_error([&] { no_key->set_connected(true); }, AlpacaError::NotConnected);
}

TEST_CASE("OpenWeather ObservingConditions Driver - Refresh fetches, a failed one keeps the last reading",
          "[openweather][observingconditions][unit]") {
    Fake fake;
    auto driver = ow::create_openweather_observingconditions(0, config(), fake.fetcher());
    driver->set_connected(true);
    {
        std::lock_guard<std::mutex> lock(fake.m);
        fake.body = reply(true);
    }
    driver->refresh();
    CHECK(fake.calls == 2);
    CHECK(driver->get_rain_rate() == Approx(0.42));
    fake.fail = true;
    require_alpaca_error([&] { driver->refresh(); }, AlpacaError::DriverException);
    CHECK(driver->get_rain_rate() == Approx(0.42));
}

TEST_CASE("OpenWeather ObservingConditions Driver - stale observation time is its age", "[openweather][observingconditions][unit]") {
    Fake fake;
    fake.body = reply(false, static_cast<long long>(std::time(nullptr)) - 1800);
    auto driver = ow::create_openweather_observingconditions(0, config(), fake.fetcher());
    driver->set_connected(true);
    CHECK(driver->get_time_since_last_update("Humidity") == Approx(1800.0).margin(5.0));
}

TEST_CASE("OpenWeather ObservingConditions Driver - value range validation", "[openweather][observingconditions][unit]") {
    Fake fake;
    auto driver = ow::create_openweather_observingconditions(0, config(), fake.fetcher());
    // InvalidValue precedes the connection check.
    require_alpaca_error([&] { driver->set_average_period(1.0); }, AlpacaError::InvalidValue);
    require_alpaca_error([&] { (void)driver->get_time_since_last_update("Nonsense"); }, AlpacaError::InvalidValue);
    require_alpaca_error([&] { (void)driver->get_sensor_description("Nonsense"); }, AlpacaError::InvalidValue);
    driver->set_connected(true);
    require_alpaca_error([&] { driver->set_average_period(-1.0); }, AlpacaError::InvalidValue);
    driver->set_average_period(0.0);
}

TEST_CASE("OpenWeather ObservingConditions Driver - state machine contracts", "[openweather][observingconditions][unit]") {
    Fake fake;
    auto driver = ow::create_openweather_observingconditions(0, config(), fake.fetcher());
    require_alpaca_error([&] { (void)driver->get_temperature(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_average_period(); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)driver->get_time_since_last_update("Temperature"); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { driver->refresh(); }, AlpacaError::NotConnected);
    CHECK(driver->get_device_state().empty());
    driver->set_connected(true);
    CHECK_FALSE(driver->get_device_state().empty());
    driver->set_connected(false);
    require_alpaca_error([&] { (void)driver->get_temperature(); }, AlpacaError::NotConnected);
}

TEST_CASE("OpenWeather ObservingConditions Driver - sensors OpenWeather lacks are not implemented",
          "[openweather][observingconditions][unit]") {
    Fake fake;
    auto driver = ow::create_openweather_observingconditions(0, config(), fake.fetcher());
    driver->set_connected(true);
    require_alpaca_error([&] { (void)driver->get_sky_brightness(); }, AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&] { (void)driver->get_sky_quality(); }, AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&] { (void)driver->get_sky_temperature(); }, AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&] { (void)driver->get_star_fwhm(); }, AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&] { (void)driver->get_time_since_last_update("StarFWHM"); },
                         AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&] { (void)driver->action("Foo", ""); }, AlpacaError::ActionNotImplemented);
    require_alpaca_error([&] { (void)driver->command_blind("x", false); }, AlpacaError::MethodNotImplemented);
}
