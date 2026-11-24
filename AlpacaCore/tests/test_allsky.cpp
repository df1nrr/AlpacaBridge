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

// Allsky camera cloud cover and the weather safety monitor. The image and the weather
// are injected (AllskyFetcher, OpenWeatherFetcher) and the wall clock is fixed, so the
// cases run with no network and decide night or day themselves. Synthetic star fields
// are encoded to JPEG here with libjpeg, as the camera would serve them.

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/allsky/allsky_drivers.h>
#include <alpacacore/vendor/allsky/allsky_image_analysis.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include <jpeglib.h>

#include "catch2_compat.h"
#include "driver_rig.h"

namespace {

namespace AlpacaError = alpacacore::AlpacaError;
namespace as = alpacacore::vendor::allsky;
namespace ow = alpacacore::vendor::openweather;
using alpacacore::test::require_alpaca_error;
using Catch::Approx;
using SysClock = std::chrono::system_clock;

// 2026-11-24 22:00 UTC: night at the Isartalsternwarte (47.8 N, 11.5 E).
const SysClock::time_point kNight = SysClock::time_point(std::chrono::seconds(1795557600));
// 2026-06-21 11:00 UTC: the sun about 65 degrees up.
const SysClock::time_point kNoon = SysClock::time_point(std::chrono::seconds(1782039600));

// A 640x480 sky (two decoder-reduced to 320x240): background, noise and @p stars point sources.
std::string star_field_jpeg(int stars, int background = 30, unsigned seed = 7, bool moon = false) {
    const int w = 640;
    const int h = 480;
    std::vector<float> img(static_cast<std::size_t>(w) * h);
    std::mt19937 rng(seed);
    std::normal_distribution<float> noise(0.0f, 2.0f);
    for (auto& p : img) p = static_cast<float>(background) + noise(rng);
    std::uniform_real_distribution<double> ang(0.0, 6.2831853), rad(0.0, 1.0);
    for (int s = 0; s < stars; ++s) {
        const double r = 0.33 * h * std::sqrt(rad(rng));
        const double a = ang(rng);
        const int sx = static_cast<int>(0.47 * w + r * std::cos(a));
        const int sy = static_cast<int>(0.50 * h + r * std::sin(a));
        for (int dy = -4; dy <= 4; ++dy) {
            for (int dx = -4; dx <= 4; ++dx) {
                const int x = sx + dx, y = sy + dy;
                if (x < 0 || y < 0 || x >= w || y >= h) continue;
                img[static_cast<std::size_t>(y) * w + x] += 90.0f * std::exp(-(dx * dx + dy * dy) / (2.0f * 1.4f * 1.4f));
            }
        }
    }
    if (moon) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                if ((x - 380) * (x - 380) + (y - 260) * (y - 260) < 40 * 40) img[static_cast<std::size_t>(y) * w + x] = 255;
    }
    std::vector<unsigned char> rgb(static_cast<std::size_t>(w) * h * 3);
    for (std::size_t i = 0; i < img.size(); ++i) {
        const auto v = static_cast<unsigned char>(std::clamp(img[i], 0.0f, 255.0f));
        rgb[3 * i] = rgb[3 * i + 1] = rgb[3 * i + 2] = v;
    }
    jpeg_compress_struct cinfo{};
    jpeg_error_mgr jerr{};
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    unsigned char* out = nullptr;
    unsigned long out_size = 0;
    jpeg_mem_dest(&cinfo, &out, &out_size);
    cinfo.image_width = w;
    cinfo.image_height = h;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 95, TRUE);
    jpeg_start_compress(&cinfo, TRUE);
    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row = rgb.data() + static_cast<std::size_t>(cinfo.next_scanline) * w * 3;
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_compress(&cinfo);
    std::string bytes(reinterpret_cast<char*>(out), out_size);
    jpeg_destroy_compress(&cinfo);
    std::free(out);
    return bytes;
}

std::string weather_reply(double wind = 2.0, double humidity = 70.0, double clouds = 20.0, bool rain = false) {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  R"({"weather":[{"main":"Clear"}],"main":{"temp":3.0,"pressure":1020,"humidity":%.0f},)"
                  R"("wind":{"speed":%.1f,"deg":200,"gust":%.1f},"clouds":{"all":%.0f},%s"dt":%lld})",
                  humidity, wind, wind * 1.5, clouds, rain ? R"("rain":{"1h":0.3},)" : "",
                  static_cast<long long>(std::time(nullptr)));
    return buf;
}

// Scripted sources and a settable wall clock.
struct Sky {
    std::mutex m;
    std::string jpeg = star_field_jpeg(150);
    std::string weather = weather_reply();
    bool camera_fails = false;
    std::atomic<long long> now_s{std::chrono::duration_cast<std::chrono::seconds>(kNight.time_since_epoch()).count()};
    as::AllskyFetcher camera() {
        return [this](const std::string&, std::chrono::milliseconds) {
            std::lock_guard<std::mutex> lock(m);
            if (camera_fails) throw alpacacore::AlpacaException("camera offline", AlpacaError::DriverException);
            return jpeg;
        };
    }
    ow::OpenWeatherFetcher openweather() {
        return [this](const std::string&, std::chrono::milliseconds) {
            std::lock_guard<std::mutex> lock(m);
            return weather;
        };
    }
    as::AllskyClock clock() {
        return [this] { return SysClock::time_point(std::chrono::seconds(now_s.load())); };
    }
    void advance(std::chrono::seconds s) { now_s += s.count(); }
    void set(const std::string& j, const std::string& w) {
        std::lock_guard<std::mutex> lock(m);
        jpeg = j;
        weather = w;
    }
};

as::AllskyCameraConfig camera_config() {
    as::AllskyCameraConfig c;
    c.image_url = "http://allsky.invalid/image.jpg";
    c.latitude = 47.83;
    c.longitude = 11.46;
    c.clear_sky_stars = 120;  // what the counter finds in a 150-star synthetic field
    c.poll_interval = std::chrono::seconds(3600);
    return c;
}

as::WeatherSafetyConfig safety_config(bool camera = true) {
    as::WeatherSafetyConfig c;
    c.weather.api_key = "abc";
    c.weather.latitude = 47.83;
    c.weather.longitude = 11.46;
    c.weather.poll_interval = std::chrono::seconds(3600);
    c.camera = camera_config();
    if (!camera) c.camera.image_url.clear();
    c.hold_off = std::chrono::minutes(10);
    return c;
}

}  // namespace

TEST_CASE("Allsky - star counting on synthetic fields", "[allsky][observingconditions][unit]") {
    const auto clear = as::analyse_allsky_image(as::decode_allsky_jpeg(star_field_jpeg(120)));
    CHECK(clear.stars >= 100);
    CHECK(clear.stars <= 125);
    const auto overcast = as::analyse_allsky_image(as::decode_allsky_jpeg(star_field_jpeg(0, 80)));
    CHECK(overcast.stars <= 3);  // noise alone is not stars
    CHECK(overcast.sky_median == Approx(80.0).margin(3.0));
    const auto moonlit = as::analyse_allsky_image(as::decode_allsky_jpeg(star_field_jpeg(60, 30, 7, true)));
    CHECK(moonlit.saturated > 0.0);
    CHECK(moonlit.stars <= 62);  // the moon and its glare are no stars
    CHECK(as::allsky_cloud_cover(0, 150) == 100.0);
    CHECK(as::allsky_cloud_cover(75, 150) == 50.0);
    CHECK(as::allsky_cloud_cover(400, 150) == 0.0);
    require_alpaca_error([] { (void)as::decode_allsky_jpeg("<html>404</html>"); }, AlpacaError::DriverException);
    const auto whole = star_field_jpeg(50);
    require_alpaca_error([&] { (void)as::decode_allsky_jpeg(whole.substr(0, whole.size() / 2)); },
                         AlpacaError::DriverException);  // caught mid-write: no EOI marker
}

TEST_CASE("Allsky - sun altitude", "[allsky][observingconditions][unit]") {
    CHECK(as::sun_altitude_deg(kNoon, 47.83, 11.46) == Approx(65.5).margin(1.0));
    CHECK(as::sun_altitude_deg(kNight, 47.83, 11.46) < -40.0);
}

TEST_CASE("Allsky ObservingConditions Driver - Defaults and metadata", "[allsky][observingconditions][unit]") {
    Sky sky;
    auto d = as::create_allsky_observingconditions(3, camera_config(), sky.camera(), sky.clock());
    CHECK_FALSE(d->get_connected());
    CHECK(d->get_name() == "Allsky Camera");
    CHECK(d->get_device_number() == 3);
    CHECK(d->get_driver_info() == "AlpacaCore Allsky ObservingConditions Driver");
    CHECK(d->get_driver_version() == alpacacore::kVersion);
    CHECK(d->get_interface_version() == 2);
    CHECK(d->get_unique_id() == "ALLSKY_OC_3");
}

TEST_CASE("Allsky ObservingConditions Driver - cloud cover at night, none by day", "[allsky][observingconditions][unit]") {
    Sky sky;
    auto d = as::create_allsky_observingconditions(0, camera_config(), sky.camera(), sky.clock());
    d->set_connected(true);
    CHECK(d->get_cloud_cover() <= 10.0);
    sky.set(star_field_jpeg(0, 70), sky.weather);
    d->refresh();
    CHECK(d->get_cloud_cover() == 100.0);
    CHECK(d->get_time_since_last_update("CloudCover") == Approx(0.0).margin(1.0));
    sky.advance(std::chrono::seconds(90));
    CHECK(d->get_time_since_last_update("") == Approx(90.0).margin(1.0));
    d->refresh();  // the camera serves the same frame again: the reading does not get younger
    CHECK(d->get_time_since_last_update("") == Approx(90.0).margin(1.0));
    sky.now_s = std::chrono::duration_cast<std::chrono::seconds>(kNoon.time_since_epoch()).count();
    d->refresh();
    require_alpaca_error([&] { (void)d->get_cloud_cover(); }, AlpacaError::ValueNotSet);
    CHECK(d->get_sensor_description("CloudCover").find("120 stars") != std::string::npos);
}

TEST_CASE("Allsky ObservingConditions Driver - connect failures and range checks", "[allsky][observingconditions][unit]") {
    Sky sky;
    sky.camera_fails = true;
    auto d = as::create_allsky_observingconditions(0, camera_config(), sky.camera(), sky.clock());
    require_alpaca_error([&] { d->set_connected(true); }, AlpacaError::DriverException);
    CHECK_FALSE(d->get_connected());
    auto no_url_cfg = camera_config();
    no_url_cfg.image_url.clear();
    auto no_url = as::create_allsky_observingconditions(0, no_url_cfg, sky.camera(), sky.clock());
    require_alpaca_error([&] { no_url->set_connected(true); }, AlpacaError::NotConnected);
    // InvalidValue precedes the connection check.
    require_alpaca_error([&] { d->set_average_period(2.0); }, AlpacaError::InvalidValue);
    require_alpaca_error([&] { (void)d->get_time_since_last_update("Bogus"); }, AlpacaError::InvalidValue);
}

TEST_CASE("Allsky ObservingConditions Driver - state machine and unsupported sensors", "[allsky][observingconditions][unit]") {
    Sky sky;
    auto d = as::create_allsky_observingconditions(0, camera_config(), sky.camera(), sky.clock());
    require_alpaca_error([&] { (void)d->get_cloud_cover(); }, AlpacaError::NotConnected);
    CHECK(d->get_device_state().empty());
    d->set_connected(true);
    CHECK_FALSE(d->get_device_state().empty());
    require_alpaca_error([&] { (void)d->get_temperature(); }, AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&] { (void)d->get_sky_quality(); }, AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&] { (void)d->get_time_since_last_update("Humidity"); }, AlpacaError::PropertyNotImplemented);
    require_alpaca_error([&] { (void)d->action("x", ""); }, AlpacaError::ActionNotImplemented);
    require_alpaca_error([&] { (void)d->command_bool("x", false); }, AlpacaError::MethodNotImplemented);
    d->set_connected(false);
    require_alpaca_error([&] { (void)d->get_cloud_cover(); }, AlpacaError::NotConnected);
}

TEST_CASE("Weather SafetyMonitor Driver - Defaults and metadata", "[allsky][safetymonitor][unit]") {
    Sky sky;
    auto d = as::create_weather_safetymonitor(1, safety_config(), sky.openweather(), sky.camera(), sky.clock());
    CHECK_FALSE(d->get_connected());
    CHECK_FALSE(d->get_is_safe());  // false, not NotConnected, while disconnected
    CHECK(d->get_name() == "Weather Safety Monitor");
    CHECK(d->get_device_type() == alpacacore::DeviceType::SafetyMonitor);
    CHECK(d->get_interface_version() == 3);
    CHECK(d->get_unique_id() == "WEATHER_SAFETY_1");
    CHECK(d->get_driver_version() == alpacacore::kVersion);
    CHECK(d->get_device_state().empty());
}

TEST_CASE("Weather SafetyMonitor Driver - safe only after the hold-off, unsafe at once", "[allsky][safetymonitor][unit]") {
    Sky sky;
    auto d = as::create_weather_safetymonitor(0, safety_config(), sky.openweather(), sky.camera(), sky.clock());
    d->set_connected(true);
    CHECK_FALSE(d->get_is_safe());  // good conditions, hold-off running
    CHECK(d->action("Status", "").find("hold-off") != std::string::npos);
    sky.advance(std::chrono::minutes(9));
    CHECK_FALSE(d->get_is_safe());
    sky.advance(std::chrono::minutes(2));  // 11 min: past the hold-off, the image still recent
    CHECK(d->get_is_safe());
    CHECK_FALSE(d->get_device_state().empty());
    d->set_connected(false);
    CHECK_FALSE(d->get_is_safe());
}

TEST_CASE("Weather SafetyMonitor Driver - each rule makes it unsafe", "[allsky][safetymonitor][unit]") {
    struct Case {
        const char* what;
        std::string jpeg;
        std::string weather;
    };
    const std::vector<Case> cases = {
        {"rain", star_field_jpeg(150), weather_reply(2, 70, 20, true)},
        {"wind", star_field_jpeg(150), weather_reply(12, 70, 20)},
        {"humidity", star_field_jpeg(150), weather_reply(2, 98, 20)},
        {"allsky clouds", star_field_jpeg(10, 60), weather_reply(2, 70, 20)},
    };
    for (const auto& c : cases) {
        INFO(c.what);
        Sky sky;
        sky.set(c.jpeg, c.weather);
        auto cfg = safety_config();
        cfg.hold_off = std::chrono::seconds(0);
        auto d = as::create_weather_safetymonitor(0, cfg, sky.openweather(), sky.camera(), sky.clock());
        d->set_connected(true);
        CHECK_FALSE(d->get_is_safe());
        CHECK(d->action("status", "").find(c.what) != std::string::npos);
    }
}

TEST_CASE("Weather SafetyMonitor Driver - by day the clouds come from OpenWeather", "[allsky][safetymonitor][unit]") {
    Sky sky;
    sky.now_s = std::chrono::duration_cast<std::chrono::seconds>(kNoon.time_since_epoch()).count();
    sky.set(star_field_jpeg(0, 200), weather_reply(2, 60, 10));  // a bright frame with no stars: daylight
    auto cfg = safety_config();
    cfg.hold_off = std::chrono::seconds(0);
    auto d = as::create_weather_safetymonitor(0, cfg, sky.openweather(), sky.camera(), sky.clock());
    d->set_connected(true);
    CHECK(d->get_is_safe());
    CHECK(d->action("status", "").find("OpenWeather clouds") != std::string::npos);

    Sky cloudy;
    cloudy.now_s = sky.now_s.load();
    cloudy.set(star_field_jpeg(0, 200), weather_reply(2, 60, 90));
    auto d2 = as::create_weather_safetymonitor(0, cfg, cloudy.openweather(), cloudy.camera(), cloudy.clock());
    d2->set_connected(true);
    CHECK_FALSE(d2->get_is_safe());
}

TEST_CASE("Weather SafetyMonitor Driver - old readings are unsafe", "[allsky][safetymonitor][unit]") {
    Sky sky;
    auto cfg = safety_config();
    cfg.hold_off = std::chrono::seconds(0);
    auto d = as::create_weather_safetymonitor(0, cfg, sky.openweather(), sky.camera(), sky.clock());
    d->set_connected(true);
    CHECK(d->get_is_safe());
    sky.advance(std::chrono::minutes(16));  // the camera's last image is now too old (nothing polled)
    CHECK_FALSE(d->get_is_safe());
    CHECK(d->action("status", "").find("no recent allsky image") != std::string::npos);
    sky.advance(std::chrono::minutes(20));
    CHECK(d->action("status", "").find("no recent OpenWeather reading") != std::string::npos);
}

TEST_CASE("Weather SafetyMonitor Driver - OpenWeather only, and connect failures", "[allsky][safetymonitor][unit]") {
    Sky sky;
    auto cfg = safety_config(false);
    cfg.hold_off = std::chrono::seconds(0);
    auto d = as::create_weather_safetymonitor(0, cfg, sky.openweather(), sky.camera(), sky.clock());
    d->set_connected(true);
    CHECK(d->get_is_safe());

    auto no_key_cfg = safety_config();
    no_key_cfg.weather.api_key.clear();
    auto no_key = as::create_weather_safetymonitor(0, no_key_cfg, sky.openweather(), sky.camera(), sky.clock());
    require_alpaca_error([&] { no_key->set_connected(true); }, AlpacaError::NotConnected);

    Sky dark;
    dark.camera_fails = true;
    auto broken = as::create_weather_safetymonitor(0, safety_config(), dark.openweather(), dark.camera(), dark.clock());
    require_alpaca_error([&] { broken->set_connected(true); }, AlpacaError::DriverException);
    CHECK_FALSE(broken->get_connected());
    require_alpaca_error([&] { (void)broken->action("status", ""); }, AlpacaError::NotConnected);
    require_alpaca_error([&] { (void)broken->action("reboot", ""); }, AlpacaError::ActionNotImplemented);
    require_alpaca_error([&] { (void)broken->command_string("x", false); }, AlpacaError::MethodNotImplemented);
}
