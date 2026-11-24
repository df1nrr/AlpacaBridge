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

// Connect/disconnect/operate concurrency stress for the OpenWeather ObservingConditions
// driver (issue #101). The fetch is injected, so connects succeed and the poll thread
// (one fetch a second here) runs through the storm.

#include <alpacacore/observingconditions_driver.h>
#include <alpacacore/vendor/openweather/openweather_observingconditions_driver.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <memory>
#include <string>

#include "catch2_compat.h"
#include "concurrency_stress.h"

using alpacacore::AlpacaDriver;
namespace ow = alpacacore::vendor::openweather;

namespace {

ow::OpenWeatherFetcher fake_fetcher(std::atomic<int>& calls) {
    return [&calls](const std::string&, std::chrono::milliseconds) {
        ++calls;
        return std::string(R"({"main":{"temp":4.5,"pressure":1021,"humidity":81},"wind":{"speed":3.6},)"
                           R"("clouds":{"all":75},"dt":)") +
               std::to_string(static_cast<long long>(std::time(nullptr))) + "}";
    };
}

ow::OpenWeatherConfig fast_config() {
    ow::OpenWeatherConfig c;
    c.api_key = "abc";
    c.poll_interval = std::chrono::seconds(1);
    return c;
}

}  // namespace

TEST_CASE("OpenWeather observing conditions - concurrent connect/disconnect/operate stress",
          "[openweather][observingconditions][stress]") {
    std::atomic<int> calls{0};
    auto driver = ow::create_openweather_observingconditions(0, fast_config(), fake_fetcher(calls));
    alpacacore::test::StressCallGuard guard({alpacacore::AlpacaError::NotConnected,
                                             alpacacore::AlpacaError::PropertyNotImplemented,
                                             alpacacore::AlpacaError::ValueNotSet});
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& oc = static_cast<alpacacore::ObservingConditionsDriver&>(d);
        guard([&] { static_cast<void>(oc.get_temperature()); });
        guard([&] { static_cast<void>(oc.get_cloud_cover()); });
        guard([&] { static_cast<void>(oc.get_wind_gust()); });
        guard([&] { static_cast<void>(oc.get_time_since_last_update("")); });
        guard([&] { static_cast<void>(oc.get_device_state()); });
        guard([&] { oc.refresh(); });
    });
    CHECK(alpacacore::test::settle_connected(*driver, false));
    CHECK(calls.load() > 0);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("OpenWeather observing conditions - destruction races an in-flight connect",
          "[openweather][observingconditions][stress]") {
    alpacacore::test::run_destruction_during_connect_stress([]() {
        static std::atomic<int> calls{0};
        return ow::create_openweather_observingconditions(0, fast_config(), fake_fetcher(calls));
    });
}

TEST_CASE("OpenWeather observing conditions - racing disconnect is never dropped",
          "[openweather][observingconditions][stress]") {
    std::atomic<int> calls{0};
    auto driver = ow::create_openweather_observingconditions(0, fast_config(), fake_fetcher(calls));
    CHECK(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver) == false);
}
