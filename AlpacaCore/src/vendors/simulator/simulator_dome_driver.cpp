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

#include <alpacacore/async_connectable.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/simulator/simulator_dome_driver.h>
#include <alpacacore/vendor/simulator/simulator_dome_model.h>
#include <alpacacore/version.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>

namespace alpacacore::vendor::simulator {

namespace {

// AtHome/AtPark: the dome is at the position when it is this close and still.
constexpr double kPositionToleranceDeg = 0.5;

double angular_distance(double a, double b) {
    double d = std::fabs(DomeModel::normalize(a) - DomeModel::normalize(b));
    return d > 180.0 ? 360.0 - d : d;
}

}  // namespace

class SimulatorDomeDriver : public DomeDriver, protected alpacacore::AsyncConnectable {
public:
    ALPACA_EXPOSE_CONNECT_ERROR()

    SimulatorDomeDriver(int device_number, const DomeSimulatorConfig& config, util::TaskClock& clock)
        : AsyncConnectable("Simulator"), clock_(clock), device_number_(device_number), model_(config) {}

    ~SimulatorDomeDriver() override {
        // Base contract: first, before members the connection task touches go away.
        shutdown_connection();
    }

    // --- Common Alpaca device interface ---

    int get_device_number() const override { return device_number_; }
    std::string get_name() const override { return "AlpacaBridge Dome Simulator"; }
    DeviceType get_device_type() const override { return DeviceType::Dome; }
    std::string fallback_unique_id() const override { return "SIMULATOR_DOME_" + std::to_string(device_number_); }
    std::string get_description() const override {
        return "Simulated observatory dome: rotation, shutter, park and home with no hardware";
    }
    std::string get_driver_info() const override { return "AlpacaCore Simulator Dome Driver"; }
    std::string get_driver_version() const override { return alpacacore::kVersion; }
    int get_interface_version() const override { return 3; }  // IDomeV3 (Platform 7)

    bool get_connected() const override { return connected_.load(); }

    void connect() override { start_connection_task(true); }
    void disconnect() override { start_connection_task(false); }
    bool get_connecting() const override { return connection_task_active(); }

    std::vector<std::string> get_supported_actions() const override { return {}; }
    std::string action(std::string_view action_name, std::string_view) override {
        throw AlpacaException("Action not supported: " + std::string(action_name), AlpacaError::ActionNotImplemented);
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

    void set_connected(bool connected) override {
        std::lock_guard<std::mutex> transition(transition_mutex_);
        if (!connected && record_disconnect_if_connect_in_flight(connected_.load())) {
            return;
        }
        if (connected && consume_pending_disconnect(connected_.load())) {
            return;
        }
        if (connected == connected_.load()) {
            return;
        }
        // The dome keeps its position and shutter across a disconnect, as a real one does.
        connected_.store(connected);
        ALPACA_LOG_INFO("Simulator", connected ? "Dome simulator connected" : "Dome simulator disconnected");
    }

    // --- Capabilities ---

    bool get_can_find_home() const override { return connected_flag(true); }
    bool get_can_park() const override { return connected_flag(true); }
    bool get_can_set_altitude() const override { return connected_flag(false); }
    bool get_can_set_azimuth() const override { return connected_flag(true); }
    bool get_can_set_park() const override { return connected_flag(true); }
    bool get_can_set_shutter() const override { return connected_flag(true); }
    bool get_can_slave() const override { return connected_flag(false); }
    bool get_can_sync_azimuth() const override { return connected_flag(true); }

    // --- State ---

    double get_altitude() const override {
        ensure_connected();
        throw AlpacaException("The simulated dome has no shutter altitude control", AlpacaError::NotImplemented);
    }

    double get_azimuth() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        return model_.azimuth(clock_.now());
    }

    bool get_at_home() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = clock_.now();
        return !model_.rotating(now) &&
               angular_distance(model_.azimuth(now), model_.config().home_azimuth) < kPositionToleranceDeg;
    }

    bool get_at_park() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        // True only once a Park has finished; any later movement clears it (IDomeV3).
        return parking_ && !model_.rotating(clock_.now());
    }

    bool get_slewing() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = clock_.now();
        // "True if any part of the dome is moving": the shutter counts.
        return model_.rotating(now) || model_.shutter_moving(now);
    }

    int get_shutter_status() const override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<int>(model_.shutter(clock_.now()));
    }

    bool get_slaved() const override {
        ensure_connected();
        return false;
    }

    void set_slaved(bool slaved) override {
        ensure_connected();
        if (slaved) {
            throw AlpacaException("The simulated dome cannot be slaved (CanSlave is false)",
                                  AlpacaError::NotImplemented);
        }
    }

    // --- Methods ---

    void abort_slew() override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        model_.stop(clock_.now());
        parking_ = false;
    }

    void open_shutter() override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        model_.open_shutter(clock_.now());
    }

    void close_shutter() override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        model_.close_shutter(clock_.now());
    }

    void find_home() override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        parking_ = false;
        model_.slew_to(model_.config().home_azimuth, clock_.now());
    }

    void park() override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        if (parking_) return;  // already parked or parking: a no-op
        parking_ = true;
        model_.slew_to(model_.config().park_azimuth, clock_.now());
    }

    void set_park() override {
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        model_.set_park_azimuth(model_.azimuth(clock_.now()));
    }

    void slew_to_azimuth(double azimuth) override {
        validate_azimuth(azimuth);
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        parking_ = false;
        model_.slew_to(azimuth, clock_.now());
    }

    void slew_to_altitude(double altitude) override {
        // CanSetAltitude is false: the member is not implemented whatever the argument.
        (void)altitude;
        ensure_connected();
        throw AlpacaException("The simulated dome has no shutter altitude control (CanSetAltitude is false)",
                              AlpacaError::MethodNotImplemented);
    }

    void sync_to_azimuth(double azimuth) override {
        validate_azimuth(azimuth);
        ensure_connected();
        std::lock_guard<std::mutex> lock(mutex_);
        parking_ = false;
        model_.sync(azimuth, clock_.now());
    }

private:
    static void validate_azimuth(double azimuth) {
        if (!std::isfinite(azimuth) || azimuth < 0.0 || azimuth >= 360.0) {
            throw AlpacaException("Azimuth " + std::to_string(azimuth) + " out of range [0, 360)",
                                  AlpacaError::InvalidValue);
        }
    }

    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Dome simulator not connected", AlpacaError::NotConnected);
        }
    }

    bool connected_flag(bool value) const {
        ensure_connected();
        return value;
    }

    util::TaskClock& clock_;
    int device_number_;
    std::atomic<bool> connected_{false};
    std::mutex transition_mutex_;
    mutable std::mutex mutex_;  // the model and parking_; never held across anything but arithmetic
    DomeModel model_;
    bool parking_ = false;
};

std::unique_ptr<DomeDriver> create_simulator_dome(int device_number, const DomeSimulatorConfig& config,
                                                  util::TaskClock& clock) {
    return std::make_unique<SimulatorDomeDriver>(device_number, config, clock);
}

}  // namespace alpacacore::vendor::simulator
