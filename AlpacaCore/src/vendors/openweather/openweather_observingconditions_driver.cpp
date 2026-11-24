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
#include <alpacacore/vendor/openweather/openweather_observingconditions_driver.h>
#include <alpacacore/version.h>
#include <curl/curl.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace alpacacore::vendor::openweather {

namespace {

constexpr const char* kTag = "OpenWeather";
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

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
    static const char* const kNames[] = {"cloudcover",     "dewpoint", "humidity",    "pressure",      "rainrate",
                                         "skybrightness",  "skyquality", "skytemperature", "starfwhm", "temperature",
                                         "winddirection", "windgust", "windspeed"};
    for (const char* k : kNames) {
        if (n == k) return true;
    }
    return false;
}

// The body of the object value of "key" (between its braces), or nullopt.
std::optional<std::string_view> object_block(std::string_view payload, std::string_view key) {
    // The first occurrence whose value is an object: "weather":[{"main":"Clouds"}] comes before "main":{...}.
    const std::string target = "\"" + std::string(key) + "\"";
    for (auto key_pos = payload.find(target); key_pos != std::string_view::npos;
         key_pos = payload.find(target, key_pos + target.size())) {
        auto i = key_pos + target.size();
        while (i < payload.size() && (std::isspace(static_cast<unsigned char>(payload[i])) || payload[i] == ':')) ++i;
        if (i >= payload.size() || payload[i] != '{') continue;
        int depth = 0;
        for (auto j = i; j < payload.size(); ++j) {
            if (payload[j] == '{') ++depth;
            if (payload[j] == '}' && --depth == 0) return payload.substr(i + 1, j - i - 1);
        }
        return std::nullopt;
    }
    return std::nullopt;
}

// The number value of "key" at the top level of @p block, or NaN.
double number_field(std::string_view block, std::string_view key) {
    const std::string target = "\"" + std::string(key) + "\"";
    std::size_t pos = 0;
    while ((pos = block.find(target, pos)) != std::string_view::npos) {
        auto i = pos + target.size();
        while (i < block.size() && std::isspace(static_cast<unsigned char>(block[i]))) ++i;
        if (i < block.size() && block[i] == ':') {
            ++i;
            while (i < block.size() && std::isspace(static_cast<unsigned char>(block[i]))) ++i;
            const std::string rest(block.substr(i, 32));
            char* end = nullptr;
            const double v = std::strtod(rest.c_str(), &end);
            return end == rest.c_str() ? kNaN : v;
        }
        pos += target.size();
    }
    return kNaN;
}

// "api.openweathermap.org/...?appid=SECRET" with the key masked, for the log.
std::string redact(const std::string& url) {
    const auto p = url.find("appid=");
    if (p == std::string::npos) return url;
    auto e = url.find('&', p);
    return url.substr(0, p + 6) + "***" + (e == std::string::npos ? std::string() : url.substr(e));
}

constexpr std::size_t kMaxResponseBytes = std::size_t{256} * 1024;

size_t curl_write(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buffer = static_cast<std::string*>(userdata);
    if (buffer == nullptr || size * nmemb > kMaxResponseBytes - buffer->size()) return 0;
    buffer->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string http_get_impl(const std::string& url, std::chrono::milliseconds timeout) {
    static std::once_flag init_flag;
    std::call_once(init_flag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        throw AlpacaException("Unable to initialize HTTP client", AlpacaError::DriverException);
    }
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout.count()));
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(kMaxResponseBytes));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "AlpacaBridge-OpenWeather/1.0");
    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK) {
        throw AlpacaException(std::string("OpenWeather request failed: ") + curl_easy_strerror(result),
                              AlpacaError::DriverException);
    }
    if (status == 401) {
        throw AlpacaException("OpenWeather refused the API key (HTTP 401): check apiKey; a new key takes up to "
                              "two hours to activate",
                              AlpacaError::DriverException);
    }
    if (status == 429) {
        throw AlpacaException("OpenWeather rate limit reached (HTTP 429): raise pollIntervalSeconds",
                              AlpacaError::DriverException);
    }
    if (status >= 400) {
        throw AlpacaException("OpenWeather answered HTTP " + std::to_string(status), AlpacaError::DriverException);
    }
    return response;
}

struct Sensor {
    const char* name;
    double OpenWeatherCurrent::*member;
    const char* description;
};

const Sensor kSensors[] = {
    {"temperature", &OpenWeatherCurrent::temperature_c, "Air temperature (OpenWeather main.temp)"},
    {"humidity", &OpenWeatherCurrent::humidity, "Relative humidity (OpenWeather main.humidity)"},
    {"dewpoint", &OpenWeatherCurrent::dewpoint_c, "Dew point, computed from temperature and humidity (Magnus)"},
    {"pressure", &OpenWeatherCurrent::pressure_hpa, "Sea-level pressure (OpenWeather main.pressure)"},
    {"windspeed", &OpenWeatherCurrent::wind_speed_ms, "Wind speed (OpenWeather wind.speed)"},
    {"winddirection", &OpenWeatherCurrent::wind_direction_deg, "Wind direction (OpenWeather wind.deg)"},
    {"windgust", &OpenWeatherCurrent::wind_gust_ms, "Wind gust (OpenWeather wind.gust)"},
    {"cloudcover", &OpenWeatherCurrent::cloud_cover_pct, "Cloud cover from the forecast model (OpenWeather clouds.all)"},
    {"rainrate", &OpenWeatherCurrent::rain_rate_mmh, "Rain in the last hour (OpenWeather rain.1h)"},
};

class OpenWeatherObservingConditionsDriver final : public ObservingConditionsDriver,
                                                   protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    OpenWeatherObservingConditionsDriver(int device_number, OpenWeatherConfig config, OpenWeatherFetcher fetcher)
        : AsyncConnectable(kTag), device_number_(device_number), config_(std::move(config)), fetcher_(std::move(fetcher)) {
        if (!fetcher_) {
            throw AlpacaException("OpenWeather fetcher is required", AlpacaError::InvalidValue);
        }
    }

    ~OpenWeatherObservingConditionsDriver() override {
        shutdown_connection();  // first, before members the connection task touches go away
        stop_polling();
    }

    int get_device_number() const override { return device_number_; }
    std::string get_name() const override { return "OpenWeather"; }
    DeviceType get_device_type() const override { return DeviceType::ObservingConditions; }
    std::string fallback_unique_id() const override { return "OPENWEATHER_OC_" + std::to_string(device_number_); }
    std::string get_description() const override {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "OpenWeather current weather at %.4f, %.4f", config_.latitude,
                      config_.longitude);
        return buf;
    }
    std::string get_driver_info() const override { return "AlpacaCore OpenWeather ObservingConditions Driver"; }
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
        if (connected) {
            if (config_.api_key.empty()) {
                throw AlpacaException("OpenWeather: set apiKey (a free key from openweathermap.org)",
                                      AlpacaError::NotConnected);
            }
            store(fetch());  // a bad key or no network fails the connect with OpenWeather's reason
            connected_.store(true);
            start_polling();
            ALPACA_LOG_INFO(kTag, "Connected; polling every " + std::to_string(config_.poll_interval.count()) + " s");
        } else {
            stop_polling();
            connected_.store(false);
            std::lock_guard<std::mutex> lock(data_mutex_);
            have_reading_ = false;
            last_error_.clear();
        }
    }

    std::vector<std::string> get_supported_actions() const override { return {}; }
    std::string action(std::string_view name, std::string_view) override {
        throw AlpacaException("Action not supported: " + std::string(name), AlpacaError::ActionNotImplemented);
    }
    bool can_action(std::string_view) const override { return false; }
    std::string command_blind(std::string_view, bool) override {
        throw AlpacaException("Command not supported", AlpacaError::MethodNotImplemented);
    }
    bool command_bool(std::string_view, bool) override {
        throw AlpacaException("Command not supported", AlpacaError::MethodNotImplemented);
    }
    std::string command_string(std::string_view, bool) override {
        throw AlpacaException("Command not supported", AlpacaError::MethodNotImplemented);
    }

    // OpenWeather serves one current value per field: the only valid AveragePeriod is 0.
    double get_average_period() const override {
        require_connected();
        return 0.0;
    }
    void set_average_period(double period) override {
        if (!std::isfinite(period) || period != 0.0) {
            throw AlpacaException("AveragePeriod must be 0: OpenWeather readings are instantaneous",
                                  AlpacaError::InvalidValue);
        }
        require_connected();
    }

    double get_cloud_cover() const override { return value("cloudcover"); }
    double get_dew_point() const override { return value("dewpoint"); }
    double get_humidity() const override { return value("humidity"); }
    double get_pressure() const override { return value("pressure"); }
    double get_rain_rate() const override { return value("rainrate"); }
    double get_sky_brightness() const override { return not_implemented("SkyBrightness"); }
    double get_sky_quality() const override { return not_implemented("SkyQuality"); }
    double get_sky_temperature() const override { return not_implemented("SkyTemperature"); }
    double get_star_fwhm() const override { return not_implemented("StarFWHM"); }
    double get_temperature() const override { return value("temperature"); }
    double get_wind_direction() const override { return value("winddirection"); }
    double get_wind_gust() const override { return value("windgust"); }
    double get_wind_speed() const override { return value("windspeed"); }

    double get_time_since_last_update(std::string_view property_name) const override {
        const std::string key = normalize_property_name(property_name);
        if (!key.empty() && !is_ascom_sensor_name(key)) {
            throw AlpacaException("Unknown sensor property", AlpacaError::InvalidValue);
        }
        require_connected();
        if (!key.empty() && find(key) == nullptr) {
            throw AlpacaException("Sensor not implemented", AlpacaError::PropertyNotImplemented);
        }
        std::lock_guard<std::mutex> lock(data_mutex_);
        if (!have_reading_) return -1.0;
        const auto observed = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(static_cast<long long>(current_.epoch_s * 1000.0)));
        return std::chrono::duration<double>(std::chrono::system_clock::now() - observed).count();
    }

    std::string get_sensor_description(std::string_view property_name) const override {
        const std::string key = normalize_property_name(property_name);
        if (!is_ascom_sensor_name(key)) {
            throw AlpacaException("Unknown sensor property", AlpacaError::InvalidValue);
        }
        require_connected();
        const Sensor* s = find(key);
        if (s == nullptr) {
            throw AlpacaException("Sensor not implemented", AlpacaError::PropertyNotImplemented);
        }
        return s->description;
    }

    void refresh() override {
        require_connected();
        store(fetch());
    }

private:
    static const Sensor* find(const std::string& key) {
        for (const auto& s : kSensors) {
            if (key == s.name) return &s;
        }
        return nullptr;
    }

    void require_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("OpenWeather not connected", AlpacaError::NotConnected);
        }
    }

    double not_implemented(const char* name) const {
        require_connected();
        throw AlpacaException(std::string(name) + " not implemented: OpenWeather does not provide it",
                              AlpacaError::PropertyNotImplemented);
    }

    double value(const char* key) const {
        require_connected();
        const Sensor* s = find(key);
        std::lock_guard<std::mutex> lock(data_mutex_);
        const double v = have_reading_ ? current_.*(s->member) : kNaN;
        if (!std::isfinite(v)) {
            throw AlpacaException(std::string(key) + " not in the last OpenWeather reply" +
                                      (last_error_.empty() ? std::string() : " (" + last_error_ + ")"),
                                  AlpacaError::ValueNotSet);
        }
        return v;
    }

    OpenWeatherCurrent fetch() const { return fetch_openweather_current(config_, fetcher_); }

    void store(const OpenWeatherCurrent& reading) {
        std::lock_guard<std::mutex> lock(data_mutex_);
        current_ = reading;
        have_reading_ = true;
        last_error_.clear();
    }

    void start_polling() {
        std::lock_guard<std::mutex> lock(poll_mutex_);
        if (poll_running_) return;
        poll_running_ = true;
        poll_thread_ = std::thread([this] { poll_loop(); });
    }

    void stop_polling() {
        {
            std::lock_guard<std::mutex> lock(poll_mutex_);
            if (!poll_running_) return;
            poll_running_ = false;
        }
        poll_cv_.notify_all();
        if (poll_thread_.joinable()) poll_thread_.join();
    }

    void poll_loop() {
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(poll_mutex_);
                if (poll_cv_.wait_for(lock, config_.poll_interval, [this] { return !poll_running_; })) return;
            }
            try {
                store(fetch());
            } catch (const std::exception& e) {
                // Keep the last reading: TimeSinceLastUpdate shows how old it is.
                ALPACA_LOG_WARN(kTag, std::string("Poll failed: ") + e.what());
                std::lock_guard<std::mutex> lock(data_mutex_);
                last_error_ = e.what();
            }
        }
    }

    int device_number_;
    OpenWeatherConfig config_;
    OpenWeatherFetcher fetcher_;
    std::atomic<bool> connected_{false};
    std::mutex transition_mutex_;  // set_connected() against itself (#528)

    mutable std::mutex data_mutex_;  // leaf: the reading only
    OpenWeatherCurrent current_{};
    bool have_reading_ = false;
    std::string last_error_;

    std::mutex poll_mutex_;
    std::condition_variable poll_cv_;
    bool poll_running_ = false;
    std::thread poll_thread_;
};

}  // namespace

std::string openweather_http_get(const std::string& url, std::chrono::milliseconds timeout) {
    return http_get_impl(url, timeout);
}

OpenWeatherCurrent fetch_openweather_current(const OpenWeatherConfig& config, const OpenWeatherFetcher& fetcher) {
    const std::string url = openweather_request_url(config);
    ALPACA_LOG_DEBUG(kTag, "GET " + redact(url));
    const std::string body = (fetcher ? fetcher : OpenWeatherFetcher(http_get_impl))(url, config.timeout);
    const auto parsed = parse_openweather_current(body);
    if (!parsed) {
        throw AlpacaException("OpenWeather sent a reply that is not current weather", AlpacaError::DriverException);
    }
    return *parsed;
}

double openweather_dew_point(double temperature_c, double humidity_pct) {
    if (!std::isfinite(temperature_c) || !std::isfinite(humidity_pct) || humidity_pct <= 0.0) return kNaN;
    constexpr double a = 17.62;
    constexpr double b = 243.12;
    const double gamma = std::log(humidity_pct / 100.0) + a * temperature_c / (b + temperature_c);
    return b * gamma / (a - gamma);
}

std::optional<OpenWeatherCurrent> parse_openweather_current(std::string_view payload) {
    const auto main = object_block(payload, "main");
    if (!main) return std::nullopt;
    OpenWeatherCurrent c{};
    c.temperature_c = number_field(*main, "temp");
    c.humidity = number_field(*main, "humidity");
    c.pressure_hpa = number_field(*main, "pressure");
    c.dewpoint_c = openweather_dew_point(c.temperature_c, c.humidity);
    c.wind_speed_ms = kNaN;
    c.wind_direction_deg = kNaN;
    c.wind_gust_ms = kNaN;
    if (const auto wind = object_block(payload, "wind")) {
        c.wind_speed_ms = number_field(*wind, "speed");
        c.wind_direction_deg = number_field(*wind, "deg");
        c.wind_gust_ms = number_field(*wind, "gust");
    }
    c.cloud_cover_pct = kNaN;
    if (const auto clouds = object_block(payload, "clouds")) c.cloud_cover_pct = number_field(*clouds, "all");
    c.rain_rate_mmh = 0.0;  // the reply has a rain block only while it rains
    if (const auto rain = object_block(payload, "rain")) {
        const double r = number_field(*rain, "1h");
        if (std::isfinite(r)) c.rain_rate_mmh = r;
    }
    c.epoch_s = number_field(payload.substr(main->data() + main->size() - payload.data()), "dt");
    if (!std::isfinite(c.epoch_s)) c.epoch_s = number_field(payload, "dt");
    if (!std::isfinite(c.epoch_s) || c.epoch_s <= 0.0) {
        c.epoch_s = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    return c;
}

std::string openweather_request_url(const OpenWeatherConfig& config) {
    char coords[96];
    std::snprintf(coords, sizeof(coords), "?lat=%.5f&lon=%.5f", config.latitude, config.longitude);
    std::string key;
    for (unsigned char ch : config.api_key) {
        if (std::isalnum(ch)) key.push_back(static_cast<char>(ch));  // keys are hex; nothing else reaches the URL
    }
    return config.base_url + coords + "&units=metric&appid=" + key;
}

std::unique_ptr<ObservingConditionsDriver> create_openweather_observingconditions(int device_number,
                                                                                  const OpenWeatherConfig& config) {
    return create_openweather_observingconditions(device_number, config, http_get_impl);
}

std::unique_ptr<ObservingConditionsDriver> create_openweather_observingconditions(int device_number,
                                                                                  const OpenWeatherConfig& config,
                                                                                  OpenWeatherFetcher fetcher) {
    return std::make_unique<OpenWeatherObservingConditionsDriver>(device_number, config, std::move(fetcher));
}

}  // namespace alpacacore::vendor::openweather
