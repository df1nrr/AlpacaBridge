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
#include <alpacacore/util/async_operation.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/polled_state.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/scopedome/scopedome_dome_driver.h>
#include <alpacacore/vendor/scopedome/scopedome_usb21_protocol_wrapper.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>

namespace alpacacore::vendor::scopedome {

namespace {

// Three exchanges per poll (GetStatus, GetCounter, GetAllDigitalExt).
constexpr auto kStatePollPeriod = std::chrono::milliseconds(500);
constexpr int kFaultThreshold = 3;
// After a rotation command the card may take a moment to report MOVING: no move
// is judged finished until this long after it was sent.
constexpr auto kStartGrace = std::chrono::seconds(2);
// A rotation (all its refinements included) that has not ended by then failed.
// TODO(scopedome): a full turn of a large dome may take minutes; INDI sets no bound.
constexpr auto kRotationTimeout = std::chrono::minutes(5);
// Sends per slew: the first move and up to three refinements (INDI refines until in tolerance).
constexpr int kMaxMoveSends = 4;
constexpr const char* kTag = "ScopeDome";

double normalize(double az) {
    double a = std::fmod(az, 360.0);
    if (a < 0.0) a += 360.0;
    return a >= 360.0 ? 0.0 : a;
}

// Signed shortest way from @p from to @p to, in (-180, 180].
double shortest(double from, double to) {
    double d = std::fmod(to - from, 360.0);
    if (d > 180.0) d -= 360.0;
    if (d <= -180.0) d += 360.0;
    return d;
}

std::string degrees(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", v);
    return buf;
}

}  // namespace

class ScopeDomeDomeDriver : public DomeDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    ScopeDomeDomeDriver(int device_number, ScopeDomeDomeConfig config, util::TaskClock& clock)
        : AsyncConnectable(kTag),
          clock_(clock),
          device_number_(device_number),
          config_(std::move(config)),
          park_azimuth_(normalize(config_.park_azimuth)),
          state_(
              clock_, state_poll_generation_, {kStatePollPeriod, kFaultThreshold, 2 * kStatePollPeriod, "ScopeDome card"},
              [this] { return probe(); },
              // A port the wrapper saw gone is already released; its disconnect() would clear the
              // loss reason get_link_fault() keeps until the next Connect or Disconnect (decision 0009 rule 5).
              [this] {
                  if (protocol_.link_alive()) protocol_.disconnect();
              }) {}

    ~ScopeDomeDomeDriver() override {
        // Base contract: first, before members the connection task touches go away.
        shutdown_connection();
        state_.stop_and_join();
        if (connected_.load()) {
            try {
                ScopeDomeDomeDriver::set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN(kTag, "Error during dome destruction: " + std::string(e.what()));
            }
        }
    }

    // --- Common Alpaca device interface ---

    int get_device_number() const override { return device_number_; }
    std::string get_name() const override { return "ScopeDome USB Card 2.1"; }
    DeviceType get_device_type() const override { return DeviceType::Dome; }
    std::string fallback_unique_id() const override { return "SCOPEDOME_DOME_" + std::to_string(device_number_); }
    std::string get_description() const override { return "ScopeDome observatory dome (USB Card 2.1)"; }
    std::string get_driver_info() const override { return "AlpacaCore ScopeDome Dome Driver"; }
    std::string get_driver_version() const override { return alpacacore::kVersion; }
    int get_interface_version() const override { return 3; }  // IDomeV3 (Platform 7)

    std::optional<std::string> get_device_firmware() const override {
        std::lock_guard<std::mutex> lock(firmware_mutex_);
        if (firmware_.empty()) return std::nullopt;
        return firmware_;
    }

    bool get_connected() const override { return connected_.load() && link_up(); }

    std::string get_link_fault() const override {
        if (auto lost = protocol_.link_fault(); !lost.empty()) return lost;
        if (auto fault = state_.fault(); !fault.empty()) return fault;
        return state_.link_lost() ? "no answer to the state poll" : std::string();
    }

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
        // A connect is a multi-second handshake: serialise set_connected() against itself (#528).
        std::lock_guard<std::mutex> transition(transition_mutex_);
        const bool live = connected_.load() && link_up();
        if (!connected && record_disconnect_if_connect_in_flight(connected_.load())) {
            return;
        }
        if (connected && consume_pending_disconnect(live)) {
            return;
        }
        if (connected ? live : !connected_.load()) {
            return;
        }
        if (!connected) {
            teardown_locked(true);
            ALPACA_LOG_INFO(kTag, "Dome disconnected");
            return;
        }
        if (connected_.load()) {
            teardown_locked(false);  // a lost link: start from a clean driver
        }
        if (config_.serial_port.empty()) {
            throw AlpacaException("ScopeDome: set portPath to the card's serial port", AlpacaError::NotConnected);
        }
        ScopeDomeConnectionConfig cfg;
        cfg.serial_port = config_.serial_port;
        const ScopeDomeCardInfo info = protocol_.connect(cfg);
        try {
            // A shutter output left on by an earlier session has nobody watching its limit switch.
            protocol_.set_output(ScopeDomeChannel::OutOpen1, false);
            protocol_.set_output(ScopeDomeChannel::OutClose1, false);
        } catch (...) {
            protocol_.disconnect();
            throw;
        }
        steps_per_turn_.store(info.steps_per_turn);
        {
            char fw[96];
            std::snprintf(fw, sizeof(fw), "main %.2f, rotary %.1f", info.firmware_main, info.firmware_rotary);
            std::lock_guard<std::mutex> lock(firmware_mutex_);
            firmware_ = fw;
        }
        if (info.steps_per_turn == 0) {
            ALPACA_LOG_WARN(kTag, "The card reports 0 steps per turn: run the full system calibration in the "
                                  "ScopeDome software; slews are refused until then");
        }
        if (info.calibration_needed) {
            ALPACA_LOG_WARN(kTag, "The card asks for a full system calibration");
        }
        state_.reset();
        if (!state_.start()) {
            const bool lost = !protocol_.link_alive();
            state_.stop_and_join();
            state_.reset();
            {
                std::lock_guard<std::mutex> lock(firmware_mutex_);
                firmware_.clear();
            }
            protocol_.disconnect();
            throw AlpacaException(lost ? "ScopeDome card link lost during connect"
                                       : "ScopeDome card did not answer the first status poll",
                                  lost ? AlpacaError::NotConnected : AlpacaError::DriverException);
        }
        connected_.store(true);
        ALPACA_LOG_INFO(kTag, "Dome connected on " + config_.serial_port + " (" + std::to_string(info.steps_per_turn) +
                                  " steps per turn)");
    }

    // --- Capabilities ---

    bool get_can_find_home() const override { return flag(true); }
    bool get_can_park() const override { return flag(true); }
    bool get_can_set_altitude() const override { return flag(false); }
    bool get_can_set_azimuth() const override { return flag(true); }
    bool get_can_set_park() const override { return flag(true); }
    bool get_can_set_shutter() const override { return flag(true); }
    bool get_can_slave() const override { return flag(false); }
    bool get_can_sync_azimuth() const override { return flag(false); }

    // --- State ---

    double get_altitude() const override {
        ensure_connected();
        throw AlpacaException("The ScopeDome driver has no shutter altitude control", AlpacaError::NotImplemented);
    }

    double get_azimuth() const override {
        const auto reading = served();
        return azimuth_of(reading.value);
    }

    bool get_at_home() const override {
        const auto reading = served();
        std::lock_guard<std::mutex> lock(mutex_);
        return at_home_ && rotation_ == Rotation::None && !reading.value.moving() &&
               std::fabs(shortest(azimuth_or_home(reading.value), normalize(config_.home_azimuth))) <=
                   config_.azimuth_tolerance;
    }

    bool get_at_park() const override {
        const auto reading = served();
        std::lock_guard<std::mutex> lock(mutex_);
        return at_park_ && rotation_ == Rotation::None && !reading.value.moving();
    }

    bool get_slewing() const override {
        const auto reading = served();
        std::lock_guard<std::mutex> lock(mutex_);
        if (rotation_failure_) {
            // A failed rotation keeps its error until the next command or a reconnect (ASCOM async FAQ).
            throw AlpacaException("Dome rotation failed: " + *rotation_failure_, AlpacaError::DriverException);
        }
        const auto& s = reading.value;
        return rotation_ != Rotation::None || s.moving() || s.homing() || shutter_target_ != ShutterTarget::None;
    }

    int get_shutter_status() const override {
        const auto reading = served();
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutter_failure_) return kShutterError;
        if (shutter_target_ == ShutterTarget::Open) return kShutterOpening;
        if (shutter_target_ == ShutterTarget::Close) return kShutterClosing;
        if (reading.value.input(ScopeDomeChannel::InOpen1)) return kShutterOpen;
        if (reading.value.input(ScopeDomeChannel::InClosed1)) return kShutterClosed;
        return kShutterError;  // neither limit switch: part-way, which ASCOM can only call Error
    }

    bool get_slaved() const override {
        ensure_connected();
        return false;
    }

    void set_slaved(bool slaved) override {
        ensure_connected();
        if (slaved) {
            throw AlpacaException("The ScopeDome driver cannot slave the dome (CanSlave is false)",
                                  AlpacaError::NotImplemented);
        }
    }

    // --- Methods ---

    void abort_slew() override {
        (void)served();
        std::lock_guard<std::mutex> command(command_mutex_);
        bool shutter_moving = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shutter_moving = shutter_target_ != ShutterTarget::None;
        }
        protocol_.stop();
        if (shutter_moving) {
            stop_shutter_outputs();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        rotation_ = Rotation::None;
        rotation_failure_.reset();
        shutter_target_ = ShutterTarget::None;
        shutter_failure_.reset();
        at_park_ = false;
        at_home_ = false;
    }

    void open_shutter() override { start_shutter(ShutterTarget::Open); }
    void close_shutter() override { start_shutter(ShutterTarget::Close); }

    void find_home() override {
        (void)served();
        std::lock_guard<std::mutex> command(command_mutex_);
        stop_if_rotating();
        protocol_.find_home();
        std::lock_guard<std::mutex> lock(mutex_);
        begin_rotation_locked(Rotation::Home, normalize(config_.home_azimuth), 0);
    }

    void park() override {
        (void)served();
        std::lock_guard<std::mutex> command(command_mutex_);
        double target = 0.0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if ((at_park_ && rotation_ == Rotation::None) || rotation_ == Rotation::Park) {
                return;  // parked or parking: a no-op
            }
            target = park_azimuth_;
        }
        start_rotation(Rotation::Park, target);
    }

    void set_park() override {
        const auto reading = served();
        const double az = azimuth_of(reading.value);
        std::lock_guard<std::mutex> lock(mutex_);
        park_azimuth_ = az;  // until a restart: the configured parkAzimuth is what persists
        ALPACA_LOG_INFO(kTag, "Park position set to " + degrees(az) + " deg");
    }

    void slew_to_azimuth(double azimuth) override {
        validate_azimuth(azimuth);
        (void)served();
        std::lock_guard<std::mutex> command(command_mutex_);
        start_rotation(Rotation::Slew, azimuth);
    }

    void slew_to_altitude(double altitude) override {
        (void)altitude;  // CanSetAltitude is false: not implemented whatever the argument
        ensure_connected();
        throw AlpacaException("The ScopeDome driver has no shutter altitude control (CanSetAltitude is false)",
                              AlpacaError::MethodNotImplemented);
    }

    void sync_to_azimuth(double azimuth) override {
        validate_azimuth(azimuth);
        ensure_connected();
        throw AlpacaException("The ScopeDome driver cannot sync azimuth (CanSyncAzimuth is false); "
                              "set homeAzimuth to the home sensor's azimuth instead",
                              AlpacaError::MethodNotImplemented);
    }

private:
    enum class Rotation : std::uint8_t { None, Slew, Park, Home };
    enum class ShutterTarget : std::uint8_t { None, Open, Close };
    static constexpr int kShutterOpen = 0;
    static constexpr int kShutterClosed = 1;
    static constexpr int kShutterOpening = 2;
    static constexpr int kShutterClosing = 3;
    static constexpr int kShutterError = 4;
    using Reading = util::StateSnapshot<ScopeDomeCardState>::Reading;
    using TimePoint = util::TaskClock::clock::time_point;

    static void validate_azimuth(double azimuth) {
        if (!std::isfinite(azimuth) || azimuth < 0.0 || azimuth >= 360.0) {
            throw AlpacaException("Azimuth " + std::to_string(azimuth) + " out of range [0, 360)",
                                  AlpacaError::InvalidValue);
        }
    }

    // The card counts counter-clockwise from the home sensor (INDI UpdatePosition).
    double azimuth_of(const ScopeDomeCardState& s) const {
        const auto spt = steps_per_turn_.load();
        if (spt == 0) {
            throw AlpacaException("ScopeDome: the card reports 0 steps per turn; run its full system calibration",
                                  AlpacaError::DriverException);
        }
        return normalize(static_cast<double>(s.counter) * -360.0 / static_cast<double>(spt) + config_.home_azimuth);
    }

    double azimuth_or_home(const ScopeDomeCardState& s) const {
        return steps_per_turn_.load() == 0 ? normalize(config_.home_azimuth) : azimuth_of(s);
    }

    // Signed steps for a clockwise-positive move of @p diff degrees.
    int steps_for(double diff) const {
        const double spt = static_cast<double>(steps_per_turn_.load());
        return static_cast<int>(std::lround(diff * spt / 360.0));
    }

    // Caller holds command_mutex_ and mutex_.
    void begin_rotation_locked(Rotation kind, double target, int sends) {
        rotation_ = kind;
        rotation_target_ = target;
        rotation_sends_ = sends;
        rotation_started_at_ = clock_.now();
        rotation_command_at_ = rotation_started_at_;
        rotation_failure_.reset();
        at_park_ = false;
        at_home_ = false;
    }

    // Caller holds command_mutex_.
    void stop_if_rotating() {
        const auto frame = state_.read();
        if (frame && (frame->value.moving() || frame->value.homing())) {
            protocol_.stop();  // a new move into a running one answers "motion conflict"
        }
    }

    // Caller holds command_mutex_. A slew or park: stop a running move, then send the first one.
    void start_rotation(Rotation kind, double target) {
        if (steps_per_turn_.load() == 0) {
            throw AlpacaException("ScopeDome: the card reports 0 steps per turn; run its full system calibration",
                                  AlpacaError::DriverException);
        }
        const auto frame = state_.read();
        const bool running = frame && (frame->value.moving() || frame->value.homing());
        if (running) {
            protocol_.stop();
            // The supervisor sends the move once the card reports the stop.
            std::lock_guard<std::mutex> lock(mutex_);
            begin_rotation_locked(kind, target, 0);
            return;
        }
        const double diff = frame ? shortest(azimuth_of(frame->value), target) : 0.0;
        const int steps = steps_for(diff);
        if (frame && (std::fabs(diff) <= config_.azimuth_tolerance || steps == 0)) {
            std::lock_guard<std::mutex> lock(mutex_);
            begin_rotation_locked(Rotation::None, target, 0);
            rotation_ = Rotation::None;
            at_park_ = kind == Rotation::Park;
            return;
        }
        protocol_.rotate(steps);
        std::lock_guard<std::mutex> lock(mutex_);
        begin_rotation_locked(kind, target, 1);
    }

    void start_shutter(ShutterTarget target) {
        const auto reading = served();
        std::lock_guard<std::mutex> command(command_mutex_);
        const auto& s = reading.value;
        const auto limit = target == ShutterTarget::Open ? ScopeDomeChannel::InOpen1 : ScopeDomeChannel::InClosed1;
        if (s.input(limit)) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (shutter_target_ == ShutterTarget::None) {
                shutter_failure_.reset();
                return;  // already there
            }
        }
        if (!s.input(ScopeDomeChannel::InRotaryLink)) {
            throw AlpacaException("ScopeDome: no link to the shutter controller, the shutter cannot be moved",
                                  AlpacaError::DriverException);
        }
        if (target == ShutterTarget::Open) {
            protocol_.set_output(ScopeDomeChannel::OutClose1, false);
            protocol_.set_output(ScopeDomeChannel::OutOpen1, true);
        } else {
            protocol_.set_output(ScopeDomeChannel::OutOpen1, false);
            protocol_.set_output(ScopeDomeChannel::OutClose1, true);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        shutter_target_ = target;
        shutter_command_at_ = clock_.now();
        shutter_failure_.reset();
    }

    // Caller holds command_mutex_.
    void stop_shutter_outputs() {
        protocol_.set_output(ScopeDomeChannel::OutOpen1, false);
        protocol_.set_output(ScopeDomeChannel::OutClose1, false);
    }

    // The poll body: read the card, then finish or refine what is in progress. Holds
    // command_mutex_ throughout, so a command and a supervision step never interleave.
    ScopeDomeCardState probe() {
        try {
            std::lock_guard<std::mutex> command(command_mutex_);
            ScopeDomeCardState s = protocol_.read_state();
            supervise_shutter(s);
            supervise_rotation(s);
            return s;
        } catch (...) {
            if (!protocol_.link_alive()) state_.lose_link();
            throw;
        }
    }

    // Caller holds command_mutex_.
    void supervise_shutter(const ScopeDomeCardState& s) {
        ShutterTarget target;
        TimePoint since;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            target = shutter_target_;
            since = shutter_command_at_;
        }
        if (target == ShutterTarget::None) return;
        const bool reached = target == ShutterTarget::Open ? s.input(ScopeDomeChannel::InOpen1)
                                                           : s.input(ScopeDomeChannel::InClosed1);
        const auto timeout = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::duration<double>(config_.shutter_timeout_s));
        if (!reached && clock_.now() - since < timeout) return;
        if (!reached) {
            std::lock_guard<std::mutex> lock(mutex_);
            shutter_failure_ = std::string("the shutter did not reach its ") +
                               (target == ShutterTarget::Open ? "open" : "closed") + " limit within " +
                               degrees(config_.shutter_timeout_s) + " s";
            shutter_target_ = ShutterTarget::None;
            ALPACA_LOG_WARN(kTag, "Shutter move failed: " + *shutter_failure_);
        } else {
            std::lock_guard<std::mutex> lock(mutex_);
            shutter_target_ = ShutterTarget::None;
            ALPACA_LOG_INFO(kTag, target == ShutterTarget::Open ? "Shutter open" : "Shutter closed");
        }
        stop_shutter_outputs();  // the card does not stop at the limit switch on its own (INDI)
    }

    // Caller holds command_mutex_.
    void supervise_rotation(const ScopeDomeCardState& s) {
        Rotation kind;
        double target;
        int sends;
        TimePoint started;
        TimePoint commanded;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            kind = rotation_;
            target = rotation_target_;
            sends = rotation_sends_;
            started = rotation_started_at_;
            commanded = rotation_command_at_;
        }
        if (kind == Rotation::None) return;
        const auto now = clock_.now();
        if (now - started > kRotationTimeout) {
            fail_rotation("the dome did not reach " + degrees(target) + " deg within 5 minutes");
            protocol_.stop();
            return;
        }
        if (s.moving() || s.homing() || now - commanded < kStartGrace) return;

        const double az = azimuth_of(s);
        const double diff = shortest(az, target);
        if (kind == Rotation::Home) {
            if (s.input(ScopeDomeChannel::InHome) || std::fabs(diff) <= config_.azimuth_tolerance) {
                protocol_.reset_counters();  // the counter now reads the home azimuth exactly
                std::lock_guard<std::mutex> lock(mutex_);
                rotation_ = Rotation::None;
                at_home_ = true;
                ALPACA_LOG_INFO(kTag, "Home sensor found");
                return;
            }
        } else if (std::fabs(diff) <= config_.azimuth_tolerance) {
            std::lock_guard<std::mutex> lock(mutex_);
            rotation_ = Rotation::None;
            at_park_ = kind == Rotation::Park;
            return;
        }
        const int steps = steps_for(diff);
        if (steps == 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            rotation_ = Rotation::None;
            at_park_ = kind == Rotation::Park;
            at_home_ = kind == Rotation::Home;
            return;
        }
        if (sends >= kMaxMoveSends) {
            fail_rotation(kind == Rotation::Home ? "the home sensor was not found"
                                                 : "the dome stopped " + degrees(std::fabs(diff)) + " deg from " +
                                                       degrees(target) + " deg");
            return;
        }
        protocol_.rotate(steps);
        std::lock_guard<std::mutex> lock(mutex_);
        rotation_sends_ = sends + 1;
        rotation_command_at_ = clock_.now();
    }

    void fail_rotation(const std::string& why) {
        std::lock_guard<std::mutex> lock(mutex_);
        rotation_ = Rotation::None;
        rotation_failure_ = why;
        ALPACA_LOG_WARN(kTag, "Dome rotation failed: " + why);
    }

    void teardown_locked(bool stop_shutter) {
        bool shutter_moving = false;
        {
            std::lock_guard<std::mutex> lock(firmware_mutex_);
            firmware_.clear();
        }
        connected_.store(false);
        state_.stop_and_join();
        state_.reset();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shutter_moving = shutter_target_ != ShutterTarget::None;
            rotation_ = Rotation::None;
            rotation_failure_.reset();
            shutter_target_ = ShutterTarget::None;
            shutter_failure_.reset();
            at_park_ = false;
            at_home_ = false;
        }
        if (stop_shutter && shutter_moving && protocol_.link_alive()) {
            // Nobody will watch the limit switch after this: stop the shutter motor.
            try {
                std::lock_guard<std::mutex> command(command_mutex_);
                stop_shutter_outputs();
                ALPACA_LOG_WARN(kTag, "Disconnected during a shutter move: shutter stopped");
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN(kTag, std::string("Could not stop the shutter at disconnect: ") + e.what());
            }
        }
        protocol_.disconnect();
    }

    bool link_up() const { return protocol_.link_alive() && !state_.link_lost(); }

    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("ScopeDome not connected", AlpacaError::NotConnected);
        }
        if (!link_up()) {
            throw AlpacaException("ScopeDome card link lost; set Connected false then true to reconnect",
                                  AlpacaError::NotConnected);
        }
    }

    bool flag(bool value) const {
        ensure_connected();
        return value;
    }

    // The polled frame; NotConnected while disconnected or lost, DriverException while faulted.
    Reading served() const {
        ensure_connected();
        return state_.served();
    }

    util::TaskClock& clock_;
    int device_number_;
    ScopeDomeDomeConfig config_;
    std::atomic<bool> connected_{false};
    std::atomic<std::uint32_t> steps_per_turn_{0};
    ScopeDomeUsb21ProtocolWrapper protocol_;

    // Lock order: transition_mutex_ -> command_mutex_ (an I/O-level lock: a command's or a
    // supervision step's wire exchanges and their state commit) -> mutex_ (short state leaf).
    // Getters take mutex_ only. The poll body takes command_mutex_; nothing holding
    // command_mutex_ waits on the poll.
    std::mutex transition_mutex_;
    std::mutex command_mutex_;
    mutable std::mutex mutex_;
    double park_azimuth_;
    Rotation rotation_ = Rotation::None;
    double rotation_target_ = 0.0;
    int rotation_sends_ = 0;
    TimePoint rotation_started_at_{};
    TimePoint rotation_command_at_{};
    std::optional<std::string> rotation_failure_;
    bool at_park_ = false;
    bool at_home_ = false;
    ShutterTarget shutter_target_ = ShutterTarget::None;
    TimePoint shutter_command_at_{};
    std::optional<std::string> shutter_failure_;

    mutable std::mutex firmware_mutex_;
    std::string firmware_;

    // Declared last: the poll body touches everything above, so the module (whose destructor joins) goes first.
    util::OperationGeneration state_poll_generation_;
    util::PolledState<ScopeDomeCardState> state_;
};

std::unique_ptr<DomeDriver> create_scopedome_dome(int device_number, const ScopeDomeDomeConfig& config,
                                                  util::TaskClock& clock) {
    return std::make_unique<ScopeDomeDomeDriver>(device_number, config, clock);
}

}  // namespace alpacacore::vendor::scopedome
