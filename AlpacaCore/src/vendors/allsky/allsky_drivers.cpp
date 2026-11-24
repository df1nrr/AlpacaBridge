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

#include <alpacacore/alpaca_errors.h>
#include <alpacacore/async_connectable.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/vendor/allsky/allsky_drivers.h>
#include <alpacacore/version.h>
#include <curl/curl.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace alpacacore::vendor::allsky {

namespace {

constexpr const char* kTag = "Allsky";
constexpr std::size_t kMaxImageBytes = std::size_t{16} * 1024 * 1024;

size_t curl_write(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buffer = static_cast<std::string*>(userdata);
    if (buffer == nullptr || size * nmemb > kMaxImageBytes - buffer->size()) return 0;
    buffer->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string normalize_property_name(std::string_view name) {
    std::string out;
    for (char ch : name) {
        if (std::isalnum(static_cast<unsigned char>(ch))) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
    }
    return out;
}

bool is_ascom_sensor_name(const std::string& n) {
    static const char* const kNames[] = {"cloudcover",     "dewpoint",   "humidity",       "pressure", "rainrate",
                                         "skybrightness",  "skyquality", "skytemperature", "starfwhm", "temperature",
                                         "winddirection", "windgust",   "windspeed"};
    for (const char* k : kNames) {
        if (n == k) return true;
    }
    return false;
}

std::chrono::system_clock::time_point system_now() { return std::chrono::system_clock::now(); }

// A poll thread that runs @p body every @p period until stopped; stop() joins it.
class Poller {
public:
    ~Poller() { stop(); }
    void start(std::chrono::milliseconds period, std::function<void()> body) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) return;
        running_ = true;
        thread_ = std::thread([this, period, body = std::move(body)] {
            for (;;) {
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    if (cv_.wait_for(lock, period, [this] { return !running_; })) return;
                }
                body();
            }
        });
    }
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) return;
            running_ = false;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool running_ = false;
    std::thread thread_;
};

std::string fmt(double v, const char* f = "%.1f") {
    char buf[32];
    std::snprintf(buf, sizeof(buf), f, v);
    return buf;
}

// The Action/Command members neither driver supports, and the unsupported sensors.
#define ALLSKY_NO_COMMANDS                                                                \
    std::string command_blind(std::string_view, bool) override {                          \
        throw AlpacaException("Command not supported", AlpacaError::MethodNotImplemented); \
    }                                                                                     \
    bool command_bool(std::string_view, bool) override {                                  \
        throw AlpacaException("Command not supported", AlpacaError::MethodNotImplemented); \
    }                                                                                     \
    std::string command_string(std::string_view, bool) override {                         \
        throw AlpacaException("Command not supported", AlpacaError::MethodNotImplemented); \
    }

// One camera measurement.
struct CameraReading {
    AllskyAnalysis analysis;
    double cloud_cover = 0.0;
    bool night = false;
    std::chrono::system_clock::time_point at{};
};

CameraReading measure_camera(const AllskyCameraConfig& cfg, const AllskyFetcher& fetcher, const AllskyClock& clock) {
    const std::string jpeg = fetcher(cfg.image_url, cfg.timeout);
    CameraReading r;
    r.analysis = analyse_allsky_image(decode_allsky_jpeg(jpeg), cfg.roi);
    r.cloud_cover = allsky_cloud_cover(r.analysis.stars, cfg.clear_sky_stars);
    r.at = clock();
    r.night = sun_altitude_deg(r.at, cfg.latitude, cfg.longitude) < kAllskyNightSunAltitude;
    ALPACA_LOG_DEBUG(kTag, "Image: " + std::to_string(r.analysis.stars) + " stars, cloud " + fmt(r.cloud_cover, "%.0f") +
                               " %, sky median " + fmt(r.analysis.sky_median) + (r.night ? ", night" : ", day"));
    return r;
}

// ---------------------------------------------------------------- ObservingConditions

class AllskyObservingConditionsDriver final : public ObservingConditionsDriver, protected alpacacore::AsyncConnectable {
public:
    ALPACA_EXPOSE_CONNECT_ERROR()

    AllskyObservingConditionsDriver(int device_number, AllskyCameraConfig config, AllskyFetcher fetcher,
                                    AllskyClock clock)
        : AsyncConnectable(kTag),
          device_number_(device_number),
          config_(std::move(config)),
          fetcher_(fetcher ? std::move(fetcher) : AllskyFetcher(allsky_http_get)),
          clock_(clock ? std::move(clock) : AllskyClock(system_now)) {}

    ~AllskyObservingConditionsDriver() override {
        shutdown_connection();
        poller_.stop();
    }

    int get_device_number() const override { return device_number_; }
    std::string get_name() const override { return "Allsky Camera"; }
    DeviceType get_device_type() const override { return DeviceType::ObservingConditions; }
    std::string fallback_unique_id() const override { return "ALLSKY_OC_" + std::to_string(device_number_); }
    std::string get_description() const override { return "Cloud cover from the star count of an allsky camera image"; }
    std::string get_driver_info() const override { return "AlpacaCore Allsky ObservingConditions Driver"; }
    std::string get_driver_version() const override { return alpacacore::kVersion; }
    int get_interface_version() const override { return 2; }

    bool get_connected() const override { return connected_.load(); }
    void connect() override { start_connection_task(true); }
    void disconnect() override { start_connection_task(false); }
    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        std::lock_guard<std::mutex> transition(transition_mutex_);
        if (!connected && record_disconnect_if_connect_in_flight(connected_.load())) return;
        if (connected && consume_pending_disconnect(connected_.load())) return;
        if (connected == connected_.load()) return;
        if (!connected) {
            poller_.stop();
            connected_.store(false);
            std::lock_guard<std::mutex> lock(mutex_);
            reading_.reset();
            return;
        }
        if (config_.image_url.empty()) {
            throw AlpacaException("Allsky: set imageUrl to the camera's latest image", AlpacaError::NotConnected);
        }
        store(measure_camera(config_, fetcher_, clock_));  // an unreachable or broken image fails the connect
        connected_.store(true);
        poller_.start(config_.poll_interval, [this] {
            try {
                store(measure_camera(config_, fetcher_, clock_));
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN(kTag, std::string("Image poll failed: ") + e.what());
            }
        });
    }

    std::vector<std::string> get_supported_actions() const override { return {}; }
    std::string action(std::string_view name, std::string_view) override {
        throw AlpacaException("Action not supported: " + std::string(name), AlpacaError::ActionNotImplemented);
    }
    bool can_action(std::string_view) const override { return false; }
    ALLSKY_NO_COMMANDS

    double get_average_period() const override {
        require_connected();
        return 0.0;
    }
    void set_average_period(double period) override {
        if (!std::isfinite(period) || period != 0.0) {
            throw AlpacaException("AveragePeriod must be 0: each image is one reading", AlpacaError::InvalidValue);
        }
        require_connected();
    }

    double get_cloud_cover() const override {
        require_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!reading_) throw AlpacaException("No allsky image measured yet", AlpacaError::ValueNotSet);
        if (!reading_->night) {
            throw AlpacaException("Cloud cover is only measured at night (sun below -6 degrees)",
                                  AlpacaError::ValueNotSet);
        }
        return reading_->cloud_cover;
    }
    double get_dew_point() const override { return none("DewPoint"); }
    double get_humidity() const override { return none("Humidity"); }
    double get_pressure() const override { return none("Pressure"); }
    double get_rain_rate() const override { return none("RainRate"); }
    double get_sky_brightness() const override { return none("SkyBrightness"); }
    double get_sky_quality() const override { return none("SkyQuality"); }
    double get_sky_temperature() const override { return none("SkyTemperature"); }
    double get_star_fwhm() const override { return none("StarFWHM"); }
    double get_temperature() const override { return none("Temperature"); }
    double get_wind_direction() const override { return none("WindDirection"); }
    double get_wind_gust() const override { return none("WindGust"); }
    double get_wind_speed() const override { return none("WindSpeed"); }

    double get_time_since_last_update(std::string_view property_name) const override {
        const std::string key = normalize_property_name(property_name);
        if (!key.empty() && !is_ascom_sensor_name(key)) {
            throw AlpacaException("Unknown sensor property", AlpacaError::InvalidValue);
        }
        require_connected();
        if (!key.empty() && key != "cloudcover") {
            throw AlpacaException("Sensor not implemented", AlpacaError::PropertyNotImplemented);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!reading_) return -1.0;
        return std::chrono::duration<double>(clock_() - reading_->at).count();
    }

    std::string get_sensor_description(std::string_view property_name) const override {
        const std::string key = normalize_property_name(property_name);
        if (!is_ascom_sensor_name(key)) throw AlpacaException("Unknown sensor property", AlpacaError::InvalidValue);
        require_connected();
        if (key != "cloudcover") throw AlpacaException("Sensor not implemented", AlpacaError::PropertyNotImplemented);
        return "Cloud cover from the allsky image's star count (" + std::to_string(config_.clear_sky_stars) +
               " stars = clear); night only";
    }

    void refresh() override {
        require_connected();
        store(measure_camera(config_, fetcher_, clock_));
    }

private:
    void require_connected() const {
        if (!connected_.load()) throw AlpacaException("Allsky camera not connected", AlpacaError::NotConnected);
    }
    double none(const char* name) const {
        require_connected();
        throw AlpacaException(std::string(name) + " not implemented: the allsky camera measures cloud cover only",
                              AlpacaError::PropertyNotImplemented);
    }
    void store(const CameraReading& r) {
        std::lock_guard<std::mutex> lock(mutex_);
        reading_ = r;
    }

    int device_number_;
    AllskyCameraConfig config_;
    AllskyFetcher fetcher_;
    AllskyClock clock_;
    std::atomic<bool> connected_{false};
    std::mutex transition_mutex_;
    mutable std::mutex mutex_;  // leaf: the reading only
    std::optional<CameraReading> reading_;
    Poller poller_;  // last: joins before the members its body touches go
};

// ---------------------------------------------------------------- SafetyMonitor

class WeatherSafetyMonitorDriver final : public SafetyMonitorDriver, protected alpacacore::AsyncConnectable {
public:
    ALPACA_EXPOSE_CONNECT_ERROR()

    WeatherSafetyMonitorDriver(int device_number, WeatherSafetyConfig config,
                               openweather::OpenWeatherFetcher weather_fetcher, AllskyFetcher camera_fetcher,
                               AllskyClock clock)
        : AsyncConnectable("WeatherSafety"),
          device_number_(device_number),
          config_(std::move(config)),
          weather_fetcher_(weather_fetcher ? std::move(weather_fetcher)
                                           : openweather::OpenWeatherFetcher(openweather::openweather_http_get)),
          camera_fetcher_(camera_fetcher ? std::move(camera_fetcher) : AllskyFetcher(allsky_http_get)),
          clock_(clock ? std::move(clock) : AllskyClock(system_now)) {
        // The camera shares the site with the weather.
        config_.camera.latitude = config_.weather.latitude;
        config_.camera.longitude = config_.weather.longitude;
    }

    ~WeatherSafetyMonitorDriver() override {
        shutdown_connection();
        poller_.stop();
    }

    int get_device_number() const override { return device_number_; }
    std::string get_name() const override { return "Weather Safety Monitor"; }
    DeviceType get_device_type() const override { return DeviceType::SafetyMonitor; }
    std::string fallback_unique_id() const override { return "WEATHER_SAFETY_" + std::to_string(device_number_); }
    std::string get_description() const override {
        return config_.camera.image_url.empty() ? "Safety from OpenWeather (rain, wind, humidity, clouds)"
                                                : "Safety from OpenWeather and an allsky camera";
    }
    std::string get_driver_info() const override { return "AlpacaCore Weather SafetyMonitor Driver"; }
    std::string get_driver_version() const override { return alpacacore::kVersion; }
    int get_interface_version() const override { return 3; }

    bool get_connected() const override { return connected_.load(); }
    void connect() override { start_connection_task(true); }
    void disconnect() override { start_connection_task(false); }
    bool get_connecting() const override { return connection_task_active(); }

    void set_connected(bool connected) override {
        std::lock_guard<std::mutex> transition(transition_mutex_);
        if (!connected && record_disconnect_if_connect_in_flight(connected_.load())) return;
        if (connected && consume_pending_disconnect(connected_.load())) return;
        if (connected == connected_.load()) return;
        if (!connected) {
            poller_.stop();
            connected_.store(false);
            std::lock_guard<std::mutex> lock(mutex_);
            weather_.reset();
            camera_.reset();
            ok_since_.reset();
            safe_ = false;
            reasons_ = "not connected";
            return;
        }
        if (config_.weather.api_key.empty()) {
            throw AlpacaException("Weather safety: set apiKey (a free key from openweathermap.org)",
                                  AlpacaError::NotConnected);
        }
        // Both sources must answer once: a wrong key or image URL is a failed connect, not a silent "unsafe".
        const auto weather = openweather::fetch_openweather_current(config_.weather, weather_fetcher_);
        std::optional<CameraReading> camera;
        if (!config_.camera.image_url.empty()) camera = measure_camera(config_.camera, camera_fetcher_, clock_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            weather_ = weather;
            weather_at_ = clock_();
            camera_ = camera;
            ok_since_.reset();
            safe_ = false;
            evaluate_locked();
        }
        last_weather_poll_ = std::chrono::steady_clock::now();
        connected_.store(true);
        const auto period = config_.camera.image_url.empty() ? config_.weather.poll_interval : config_.camera.poll_interval;
        poller_.start(period, [this] { poll(); });
        ALPACA_LOG_INFO("WeatherSafety", "Connected; " + std::string(config_.camera.image_url.empty()
                                                                         ? "OpenWeather only"
                                                                         : "OpenWeather and the allsky camera"));
    }

    // ISafetyMonitor: false, not NotConnected, while disconnected.
    bool get_is_safe() const override {
        if (!connected_.load()) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        evaluate_locked();  // ages the readings and the hold-off; no I/O
        return safe_;
    }

    // "Status": why IsSafe is what it is, for the operator.
    std::vector<std::string> get_supported_actions() const override { return {"status"}; }
    std::string action(std::string_view name, std::string_view) override {
        if (normalize_property_name(name) != "status") {
            throw AlpacaException("Action not supported: " + std::string(name), AlpacaError::ActionNotImplemented);
        }
        if (!connected_.load()) throw AlpacaException("Weather safety not connected", AlpacaError::NotConnected);
        std::lock_guard<std::mutex> lock(mutex_);
        evaluate_locked();
        return std::string(safe_ ? "safe" : "unsafe") + ": " + reasons_;
    }
    bool can_action(std::string_view name) const override { return normalize_property_name(name) == "status"; }
    ALLSKY_NO_COMMANDS

private:
    void poll() {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_weather_poll_ >= config_.weather.poll_interval) {
            last_weather_poll_ = now;
            try {
                const auto w = openweather::fetch_openweather_current(config_.weather, weather_fetcher_);
                std::lock_guard<std::mutex> lock(mutex_);
                weather_ = w;
                weather_at_ = clock_();
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("WeatherSafety", std::string("OpenWeather poll failed: ") + e.what());
            }
        }
        if (!config_.camera.image_url.empty()) {
            try {
                const auto c = measure_camera(config_.camera, camera_fetcher_, clock_);
                std::lock_guard<std::mutex> lock(mutex_);
                camera_ = c;
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("WeatherSafety", std::string("Allsky poll failed: ") + e.what());
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        evaluate_locked();
    }

    // Caller holds mutex_. Applies the rules to the held readings and moves the hold-off.
    void evaluate_locked() const {
        const auto now = clock_();
        std::vector<std::string> unsafe;
        if (!weather_ || now - weather_at_ > config_.max_weather_age) {
            unsafe.emplace_back("no recent OpenWeather reading");
        } else {
            const auto& w = *weather_;
            if (w.rain_rate_mmh > 0.0) unsafe.push_back("rain " + fmt(w.rain_rate_mmh, "%.2f") + " mm/h");
            if (std::isfinite(w.wind_speed_ms) && w.wind_speed_ms > config_.max_wind_speed) {
                unsafe.push_back("wind " + fmt(w.wind_speed_ms) + " m/s");
            }
            if (std::isfinite(w.wind_gust_ms) && w.wind_gust_ms > config_.max_wind_gust) {
                unsafe.push_back("gusts " + fmt(w.wind_gust_ms) + " m/s");
            }
            if (std::isfinite(w.humidity) && w.humidity > config_.max_humidity) {
                unsafe.push_back("humidity " + fmt(w.humidity, "%.0f") + " %");
            }
        }
        const bool night = sun_altitude_deg(now, config_.weather.latitude, config_.weather.longitude) <
                           kAllskyNightSunAltitude;
        std::string clouds;
        if (!config_.camera.image_url.empty() && night) {
            if (!camera_ || now - camera_->at > config_.max_camera_age) {
                unsafe.emplace_back("no recent allsky image");
            } else if (camera_->cloud_cover > config_.max_cloud_cover) {
                unsafe.push_back("allsky clouds " + fmt(camera_->cloud_cover, "%.0f") + " % (" +
                                 std::to_string(camera_->analysis.stars) + " stars)");
            } else {
                clouds = "allsky clouds " + fmt(camera_->cloud_cover, "%.0f") + " %";
            }
        } else if (weather_ && std::isfinite(weather_->cloud_cover_pct)) {
            if (weather_->cloud_cover_pct > config_.max_cloud_cover) {
                unsafe.push_back("OpenWeather clouds " + fmt(weather_->cloud_cover_pct, "%.0f") + " %");
            } else {
                clouds = "OpenWeather clouds " + fmt(weather_->cloud_cover_pct, "%.0f") + " %";
            }
        }

        const bool was_safe = safe_;
        if (!unsafe.empty()) {
            ok_since_.reset();
            safe_ = false;
            reasons_.clear();
            for (const auto& u : unsafe) reasons_ += (reasons_.empty() ? "" : ", ") + u;
        } else {
            if (!ok_since_) ok_since_ = now;
            const auto held = now - *ok_since_;
            safe_ = held >= config_.hold_off;
            reasons_ = (clouds.empty() ? std::string("conditions fine") : clouds) +
                       (safe_ ? std::string()
                              : ", hold-off " +
                                    std::to_string(std::chrono::duration_cast<std::chrono::seconds>(config_.hold_off - held)
                                                       .count()) +
                                    " s left");
        }
        if (was_safe != safe_) {
            ALPACA_LOG_INFO("WeatherSafety", std::string(safe_ ? "Safe: " : "Unsafe: ") + reasons_);
        }
    }

    int device_number_;
    WeatherSafetyConfig config_;
    openweather::OpenWeatherFetcher weather_fetcher_;
    AllskyFetcher camera_fetcher_;
    AllskyClock clock_;
    std::atomic<bool> connected_{false};
    std::mutex transition_mutex_;
    std::chrono::steady_clock::time_point last_weather_poll_{};  // the poll thread's own

    mutable std::mutex mutex_;  // leaf: readings and verdict; never held across a fetch
    std::optional<openweather::OpenWeatherCurrent> weather_;
    std::chrono::system_clock::time_point weather_at_{};
    std::optional<CameraReading> camera_;
    mutable std::optional<std::chrono::system_clock::time_point> ok_since_;
    mutable bool safe_ = false;
    mutable std::string reasons_ = "not connected";
    Poller poller_;  // last: joins before the members its body touches go
};

}  // namespace

std::string allsky_http_get(const std::string& url, std::chrono::milliseconds timeout) {
    static std::once_flag init_flag;
    std::call_once(init_flag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    CURL* curl = curl_easy_init();
    if (curl == nullptr) throw AlpacaException("Unable to initialize HTTP client", AlpacaError::DriverException);
    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout.count()));
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(kMaxImageBytes));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "AlpacaBridge-Allsky/1.0");
    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK) {
        throw AlpacaException(std::string("Allsky image request failed: ") + curl_easy_strerror(result),
                              AlpacaError::DriverException);
    }
    if (status >= 400) {
        throw AlpacaException("Allsky image request answered HTTP " + std::to_string(status),
                              AlpacaError::DriverException);
    }
    return body;
}

std::unique_ptr<ObservingConditionsDriver> create_allsky_observingconditions(int device_number,
                                                                             const AllskyCameraConfig& config,
                                                                             AllskyFetcher fetcher, AllskyClock clock) {
    return std::make_unique<AllskyObservingConditionsDriver>(device_number, config, std::move(fetcher),
                                                             std::move(clock));
}

std::unique_ptr<SafetyMonitorDriver> create_weather_safetymonitor(int device_number, const WeatherSafetyConfig& config,
                                                                  openweather::OpenWeatherFetcher weather_fetcher,
                                                                  AllskyFetcher camera_fetcher, AllskyClock clock) {
    return std::make_unique<WeatherSafetyMonitorDriver>(device_number, config, std::move(weather_fetcher),
                                                        std::move(camera_fetcher), std::move(clock));
}

}  // namespace alpacacore::vendor::allsky
