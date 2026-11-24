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

// Connect/disconnect/operate concurrency stress for the allsky camera and the weather
// safety monitor (issue #101). Image and weather are injected, so connects succeed and
// the poll threads (one round a second here) run through the storm.

#include <alpacacore/vendor/allsky/allsky_drivers.h>

#include <chrono>
#include <ctime>
#include <memory>
#include <string>

#include "catch2_compat.h"
#include "concurrency_stress.h"

using alpacacore::AlpacaDriver;
namespace as = alpacacore::vendor::allsky;

namespace {

// A 64x64 grey JPEG (generated with Pillow) is enough: the storm exercises the lifecycle, not the analysis.
std::string tiny_jpeg() {
    static const unsigned char k[] = {
        0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01,
        0x00, 0x00, 0xFF, 0xDB, 0x00, 0x43, 0x00, 0x10, 0x0B, 0x0C, 0x0E, 0x0C, 0x0A, 0x10, 0x0E, 0x0D, 0x0E, 0x12,
        0x11, 0x10, 0x13, 0x18, 0x28, 0x1A, 0x18, 0x16, 0x16, 0x18, 0x31, 0x23, 0x25, 0x1D, 0x28, 0x3A, 0x33, 0x3D,
        0x3C, 0x39, 0x33, 0x38, 0x37, 0x40, 0x48, 0x5C, 0x4E, 0x40, 0x44, 0x57, 0x45, 0x37, 0x38, 0x50, 0x6D, 0x51,
        0x57, 0x5F, 0x62, 0x67, 0x68, 0x67, 0x3E, 0x4D, 0x71, 0x79, 0x70, 0x64, 0x78, 0x5C, 0x65, 0x67, 0x63, 0xFF,
        0xC0, 0x00, 0x0B, 0x08, 0x00, 0x40, 0x00, 0x40, 0x01, 0x01, 0x11, 0x00, 0xFF, 0xC4, 0x00, 0x1F, 0x00, 0x00,
        0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02,
        0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0xFF, 0xC4, 0x00, 0xB5, 0x10, 0x00, 0x02, 0x01, 0x03,
        0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04, 0x04, 0x00, 0x00, 0x01, 0x7D, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11,
        0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08,
        0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16, 0x17, 0x18,
        0x19, 0x1A, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45,
        0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67,
        0x68, 0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
        0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9,
        0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9,
        0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8,
        0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01,
        0x00, 0x00, 0x3F, 0x00, 0xA3, 0x45, 0x14, 0x51, 0x45, 0x14, 0x51, 0x45, 0x14, 0x51, 0x45, 0x14, 0x51, 0x45,
        0x14, 0x51, 0x45, 0x14, 0x51, 0x45, 0x14, 0x51, 0x45, 0x14, 0x51, 0x45, 0x14, 0x51, 0x45, 0x14, 0x51, 0x45,
        0x14, 0x51, 0x45, 0x14, 0x51, 0x45, 0x14, 0x51, 0x45, 0x14, 0x51, 0x45, 0x14, 0x51, 0x45, 0x14, 0x57, 0xFF,
        0xD9,
    };
    return std::string(reinterpret_cast<const char*>(k), sizeof(k));
}

as::AllskyFetcher camera() {
    return [](const std::string&, std::chrono::milliseconds) { return tiny_jpeg(); };
}

alpacacore::vendor::openweather::OpenWeatherFetcher weather() {
    return [](const std::string&, std::chrono::milliseconds) {
        return std::string(R"({"main":{"temp":3,"pressure":1020,"humidity":70},"wind":{"speed":2},"clouds":{"all":10},"dt":)") +
               std::to_string(static_cast<long long>(std::time(nullptr))) + "}";
    };
}

as::AllskyCameraConfig fast_camera() {
    as::AllskyCameraConfig c;
    c.image_url = "http://allsky.invalid/image.jpg";
    c.poll_interval = std::chrono::seconds(1);
    return c;
}

as::WeatherSafetyConfig fast_safety() {
    as::WeatherSafetyConfig c;
    c.weather.api_key = "abc";
    c.weather.poll_interval = std::chrono::seconds(1);
    c.camera = fast_camera();
    return c;
}

}  // namespace

TEST_CASE("Allsky camera - concurrent connect/disconnect/operate stress", "[allsky][observingconditions][stress]") {
    auto driver = as::create_allsky_observingconditions(0, fast_camera(), camera());
    alpacacore::test::StressCallGuard guard({alpacacore::AlpacaError::NotConnected,
                                             alpacacore::AlpacaError::ValueNotSet,
                                             alpacacore::AlpacaError::PropertyNotImplemented});
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& oc = static_cast<alpacacore::ObservingConditionsDriver&>(d);
        guard([&] { static_cast<void>(oc.get_cloud_cover()); });
        guard([&] { static_cast<void>(oc.get_time_since_last_update("")); });
        guard([&] { static_cast<void>(oc.get_device_state()); });
        guard([&] { oc.refresh(); });
    });
    CHECK(alpacacore::test::settle_connected(*driver, false));
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Weather safety monitor - concurrent connect/disconnect/operate stress", "[allsky][safetymonitor][stress]") {
    auto driver = as::create_weather_safetymonitor(0, fast_safety(), weather(), camera());
    alpacacore::test::StressCallGuard guard({alpacacore::AlpacaError::NotConnected});
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& sm = static_cast<alpacacore::SafetyMonitorDriver&>(d);
        guard([&] { static_cast<void>(sm.get_is_safe()); });
        guard([&] { static_cast<void>(sm.get_device_state()); });
        guard([&] { static_cast<void>(sm.action("status", "")); });
    });
    CHECK(alpacacore::test::settle_connected(*driver, false));
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Allsky camera - destruction races an in-flight connect", "[allsky][observingconditions][stress]") {
    alpacacore::test::run_destruction_during_connect_stress(
        []() { return as::create_allsky_observingconditions(0, fast_camera(), camera()); });
}

TEST_CASE("Weather safety monitor - destruction races an in-flight connect", "[allsky][safetymonitor][stress]") {
    alpacacore::test::run_destruction_during_connect_stress(
        []() { return as::create_weather_safetymonitor(0, fast_safety(), weather(), camera()); });
}

TEST_CASE("Allsky camera - racing disconnect is never dropped", "[allsky][observingconditions][stress]") {
    auto driver = as::create_allsky_observingconditions(0, fast_camera(), camera());
    CHECK(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver) == false);
}

TEST_CASE("Weather safety monitor - racing disconnect is never dropped", "[allsky][safetymonitor][stress]") {
    auto driver = as::create_weather_safetymonitor(0, fast_safety(), weather(), camera());
    CHECK(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver) == false);
}
