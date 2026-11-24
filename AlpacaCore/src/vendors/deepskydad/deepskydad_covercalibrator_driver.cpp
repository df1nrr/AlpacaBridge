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
#include <alpacacore/util/auto_detect.h>
#include <alpacacore/util/connection_resolver.h>
#include <alpacacore/util/error_handling.h>
#include <alpacacore/util/logging.h>
#include <alpacacore/util/polled_state.h>
#include <alpacacore/util/task_clock.h>
#include <alpacacore/vendor/deepskydad/deepskydad_covercalibrator_driver.h>
#include <alpacacore/vendor/deepskydad/deepskydad_flatpanel_protocol_wrapper.h>
#include <alpacacore/version.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>

namespace alpacacore::vendor::deepskydad {

namespace {

// Four exchanges per poll ([GMOV] [GLON] [GOPS] [GLBR]); 500 ms keeps the panel's
// MCU mostly idle and still shows a flap move within a second.
constexpr auto kStatePollPeriod = std::chrono::milliseconds(500);
constexpr int kFaultThreshold = 3;
// After [SMOV] the motor may take a moment to report running: until a poll measured
// this long after the command says otherwise, an unfinished move reads Moving.
constexpr auto kMoveStartGrace = std::chrono::seconds(3);
// A flap move that has not reached its end by then is a failed move.
// TODO(deepskydad): measured travel time unknown; INDI sets no bound.
constexpr auto kMoveTimeout = std::chrono::seconds(90);

}  // namespace

class DeepSkyDadCoverCalibratorDriver : public CoverCalibratorDriver, protected alpacacore::AsyncConnectable {
public:
    // Issue #358: hand the connect-failure reason to the router.
    ALPACA_EXPOSE_CONNECT_ERROR()

    DeepSkyDadCoverCalibratorDriver(int device_number, FlatPanelConnectionConfig config,
                                    util::ConnectionResolver<FlatPanelConnectionConfig> resolver = {},
                                    util::TaskClock& clock = util::default_task_clock())
        : AsyncConnectable("DeepSkyDad"),
          clock_(clock),
          device_number_(device_number),
          config_(std::move(config)),
          resolver_(std::move(resolver)),
          state_(
              clock_, state_poll_generation_,
              {kStatePollPeriod, kFaultThreshold, 2 * kStatePollPeriod, "Deep Sky Dad flat panel"},
              [this] { return probe(); },
              // A port the wrapper saw gone is already released; its disconnect() would clear the
              // loss reason get_link_fault() keeps until the next Connect or Disconnect (decision 0009 rule 5).
              [this] {
                  if (protocol_.link_alive()) protocol_.disconnect();
              }) {}

    ~DeepSkyDadCoverCalibratorDriver() override {
        // Base contract: first, before members the connection task touches go away.
        shutdown_connection();
        state_.stop_and_join();
        if (connected_.load()) {
            try {
                DeepSkyDadCoverCalibratorDriver::set_connected(false);
            } catch (const std::exception& e) {
                ALPACA_LOG_WARN("DeepSkyDad", "Error during flat panel destruction: " + std::string(e.what()));
            }
        }
    }

    // --- Common Alpaca device interface ---

    int get_device_number() const override { return device_number_; }

    std::string get_name() const override {
        return protocol_.model() == FlatPanelModel::FP1 ? "Deep Sky Dad FP1" : "Deep Sky Dad FP2";
    }

    DeviceType get_device_type() const override { return DeviceType::CoverCalibrator; }

    std::string fallback_unique_id() const override {
        return "DEEPSKYDAD_COVERCALIBRATOR_" + std::to_string(device_number_);
    }

    std::string get_description() const override { return "Deep Sky Dad flat panel (motorized flap and LED panel)"; }

    std::string get_driver_info() const override { return "AlpacaCore Deep Sky Dad CoverCalibrator Driver"; }

    std::string get_driver_version() const override { return alpacacore::kVersion; }

    // The [GFRM] banner captured at connect; web UI only, never DriverInfo.
    std::optional<std::string> get_device_firmware() const override {
        std::lock_guard<std::mutex> lock(firmware_mutex_);
        if (firmware_.empty()) return std::nullopt;
        return firmware_;
    }

    // ICoverCalibratorV2: CoverMoving, CalibratorChanging, Connecting, DeviceState.
    int get_interface_version() const override { return 2; }

    bool get_connected() const override { return connected_.load() && link_up(); }

    std::string get_link_fault() const override {
        if (auto lost = protocol_.link_fault(); !lost.empty()) return lost;
        if (auto fault = state_.fault(); !fault.empty()) return fault;
        return state_.link_lost() ? "no answer to the state poll" : std::string();
    }

    ALPACA_ASYNC_CONNECT_FORWARDERS()

    void set_connected(bool connected) override {
        // A connect is a multi-second handshake: serialise set_connected() against itself (#528).
        std::lock_guard<std::mutex> transition(transition_mutex_);
        // Raw flag for disconnect, link-aware value for connect (decision 0009).
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
            teardown_locked();
            ALPACA_LOG_INFO("DeepSkyDad", "Flat panel disconnected");
            return;
        }
        if (connected_.load()) {
            teardown_locked();  // a lost link: start from a clean driver
        }
        std::string firmware;
        util::connect_resolved(
            config_, resolved_, resolver_,
            [this, &firmware](const FlatPanelConnectionConfig& cfg) {
                if (cfg.serial_port.empty()) {
                    throw AlpacaException(
                        "Deep Sky Dad flat panel: set portPath, or choose auto-detect with a panel index",
                        AlpacaError::NotConnected);
                }
                try {
                    firmware = protocol_.connect(cfg);
                } catch (const AlpacaException& e) {
                    // Only a vanished node or a port another connected device now holds re-scans.
                    if (util::serial_endpoint_stale(cfg.serial_port)) {
                        throw util::StaleEndpoint(e.what(), e.error_code());
                    }
                    throw;
                }
            },
            "DeepSkyDad");
        {
            std::lock_guard<std::mutex> lock(firmware_mutex_);
            firmware_ = firmware;
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
            throw AlpacaException(lost ? "Deep Sky Dad flat panel link lost during connect"
                                       : "Deep Sky Dad flat panel did not answer the first status poll",
                                  lost ? AlpacaError::NotConnected : AlpacaError::DriverException);
        }
        // Seed the calibrator from the panel: a light left on from a previous session reads Ready.
        if (const auto first = state_.read()) {
            std::lock_guard<std::mutex> lock(mutex_);
            calibrator_engaged_ = first->value.light_on;
            commanded_brightness_ = first->value.light_on ? first->value.brightness : 0;
            clear_cover_command_locked();
        }
        connected_.store(true);
        ALPACA_LOG_INFO("DeepSkyDad", "Flat panel connected");
    }

    // --- Calibrator ---

    int get_max_brightness() const override {
        ensure_connected();
        return kDeepSkyDadMaxBrightness;
    }

    int get_brightness() const override {
        (void)served();  // a number cannot say "unknown": a faulted link throws (issue #237)
        std::lock_guard<std::mutex> lock(mutex_);
        return calibrator_engaged_ ? commanded_brightness_ : 0;
    }

    CalibratorState get_calibrator_state() const override {
        if (!served_or_unknown()) return CalibratorState::Unknown;
        std::lock_guard<std::mutex> lock(mutex_);
        return calibrator_engaged_ ? CalibratorState::Ready : CalibratorState::Off;
    }

    bool get_calibrator_changing() const override {
        // [SLBR]/[SLON] take effect within their own exchange; the light never warms up.
        ensure_connected();
        return false;
    }

    void calibrator_on(int brightness) override {
        if (brightness < 0 || brightness > kDeepSkyDadMaxBrightness) {
            throw AlpacaException("Brightness " + std::to_string(brightness) + " out of range [0, " +
                                      std::to_string(kDeepSkyDadMaxBrightness) + "]",
                                  AlpacaError::InvalidValue);
        }
        (void)served();
        std::lock_guard<std::mutex> command(command_mutex_);
        // CalibratorOn(0) is Ready at zero (ICoverCalibratorV2, ConformU): the panel is dark,
        // so send the light off rather than an [SLBR0000] the firmware may not accept.
        protocol_.set_light(brightness > 0, brightness);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            calibrator_engaged_ = true;
            commanded_brightness_ = brightness;
        }
        state_.write([brightness](FlatPanelStatus& s) {
            s.light_on = brightness > 0;
            if (brightness > 0) s.brightness = brightness;
        });
    }

    void calibrator_off() override {
        (void)served();
        std::lock_guard<std::mutex> command(command_mutex_);
        protocol_.set_light(false, 0);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            calibrator_engaged_ = false;
            commanded_brightness_ = 0;
        }
        state_.write([](FlatPanelStatus& s) { s.light_on = false; });
    }

    void set_brightness(int brightness) override { calibrator_on(brightness); }

    // --- Cover ---

    CoverState get_cover_state() const override {
        std::optional<util::StateSnapshot<FlatPanelStatus>::Reading> reading;
        try {
            ensure_connected();
            reading = state_.served();
        } catch (const AlpacaException& e) {
            if (e.error_code() != AlpacaError::DriverException) throw;
            ensure_connected();          // a fault that became a loss is NotConnected
            return CoverState::Unknown;  // faulted: ASCOM's word for "cannot tell" (issue #237)
        }
        const auto end = cover_end_for(protocol_.model(), reading->value.cover_raw);
        std::lock_guard<std::mutex> lock(mutex_);
        return cover_state_locked(*reading, end);
    }

    bool get_cover_moving() const override { return get_cover_state() == CoverState::Moving; }

    void open_cover() override { start_cover_move(true); }

    void close_cover() override { start_cover_move(false); }

    void halt_cover() override {
        (void)served();
        // TODO(deepskydad): INDI documents no stop command for the flap. Like the Gemini Rev2
        // and WandererCover drivers, stop reporting the move: CoverState reads the panel's own
        // end (or Unknown part-way) from now on, while the motor finishes its travel.
        std::lock_guard<std::mutex> lock(mutex_);
        if (cover_target_ != CoverTarget::None) {
            cover_target_ = CoverTarget::None;
            cover_halted_ = true;
        }
    }

    // No per-vendor get_device_state(): the CoverCalibratorDriver base builds it from these getters.

private:
    enum class CoverTarget : std::uint8_t { None, Open, Closed };
    using Reading = util::StateSnapshot<FlatPanelStatus>::Reading;

    void start_cover_move(bool open) {
        (void)served();
        std::lock_guard<std::mutex> command(command_mutex_);
        protocol_.move_cover(open);  // throws when the panel refused or did not answer: nothing changed
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cover_target_ = open ? CoverTarget::Open : CoverTarget::Closed;
            cover_command_at_ = clock_.now();
            cover_halted_ = false;
            cover_failure_.reset();  // a new command clears the previous failure
        }
        state_.write([](FlatPanelStatus& s) { s.motor_moving = true; });
    }

    // Caller holds mutex_. Records a failed move (kept until the next command or reconnect, as the
    // ASCOM async FAQ requires) the first time a read sees it.
    CoverState cover_state_locked(const Reading& reading, FlatPanelCoverEnd end) const {
        if (cover_failure_) {
            return CoverState::Error;
        }
        const auto& s = reading.value;
        if (cover_target_ == CoverTarget::None) {
            if (s.motor_moving && !cover_halted_) return CoverState::Moving;
            if (end == FlatPanelCoverEnd::Closed) return CoverState::Closed;
            if (end == FlatPanelCoverEnd::Open) return CoverState::Open;
            // A halted move is no longer reported, even while the motor finishes its travel.
            return s.motor_moving && !cover_halted_ ? CoverState::Moving : CoverState::Unknown;
        }
        const auto now = clock_.now();
        const bool fresh = reading.measured_at > cover_command_at_;  // polled after the command
        const bool wanted_end = (cover_target_ == CoverTarget::Open && end == FlatPanelCoverEnd::Open) ||
                                (cover_target_ == CoverTarget::Closed && end == FlatPanelCoverEnd::Closed);
        if (fresh && !s.motor_moving && wanted_end) {
            return cover_target_ == CoverTarget::Open ? CoverState::Open : CoverState::Closed;
        }
        if (now - cover_command_at_ > kMoveTimeout) {
            cover_failure_ = "the flap did not reach its end within " +
                             std::to_string(std::chrono::duration_cast<std::chrono::seconds>(kMoveTimeout).count()) +
                             " s";
            ALPACA_LOG_WARN("DeepSkyDad", "Flat panel cover move failed: " + *cover_failure_);
            return CoverState::Error;
        }
        if (!fresh || s.motor_moving || now - cover_command_at_ < kMoveStartGrace) {
            return CoverState::Moving;
        }
        cover_failure_ = "the flap stopped before reaching its end";
        ALPACA_LOG_WARN("DeepSkyDad", "Flat panel cover move failed: " + *cover_failure_);
        return CoverState::Error;
    }

    void clear_cover_command_locked() {
        cover_target_ = CoverTarget::None;
        cover_halted_ = false;
        cover_failure_.reset();
    }

    void teardown_locked() {
        {
            std::lock_guard<std::mutex> lock(firmware_mutex_);
            firmware_.clear();
        }
        connected_.store(false);
        state_.stop_and_join();
        state_.reset();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            clear_cover_command_locked();
        }
        protocol_.disconnect();
    }

    FlatPanelStatus probe() {
        try {
            FlatPanelStatus s = protocol_.get_status();
            if (!protocol_.link_alive()) {
                throw AlpacaException("Deep Sky Dad flat panel link lost during the poll", AlpacaError::NotConnected);
            }
            return s;
        } catch (...) {
            if (!protocol_.link_alive()) state_.lose_link();
            throw;
        }
    }

    bool link_up() const { return protocol_.link_alive() && !state_.link_lost(); }

    void ensure_connected() const {
        if (!connected_.load()) {
            throw AlpacaException("Deep Sky Dad flat panel not connected", AlpacaError::NotConnected);
        }
        if (!link_up()) {
            throw AlpacaException("Deep Sky Dad flat panel link lost; set Connected false then true to reconnect",
                                  AlpacaError::NotConnected);
        }
    }

    // The polled frame; NotConnected while disconnected or lost, DriverException while faulted.
    Reading served() const {
        ensure_connected();
        return state_.served();
    }

    // False when the poll is faulted (a state read answers Unknown then); NotConnected propagates.
    bool served_or_unknown() const {
        try {
            (void)served();
            return true;
        } catch (const AlpacaException& e) {
            if (e.error_code() != AlpacaError::DriverException) throw;
            ensure_connected();
            return false;
        }
    }

    util::TaskClock& clock_;
    int device_number_;
    FlatPanelConnectionConfig config_;
    util::ConnectionResolver<FlatPanelConnectionConfig> resolver_;
    bool resolved_ = false;
    std::atomic<bool> connected_{false};
    DeepSkyDadFlatPanelProtocolWrapper protocol_;

    // Lock order: transition_mutex_ -> command_mutex_ (an I/O-level lock: the command's wire
    // exchange and its state commit) -> mutex_ (short state leaf). Getters take mutex_ only.
    std::mutex transition_mutex_;
    std::mutex command_mutex_;
    mutable std::mutex mutex_;
    bool calibrator_engaged_ = false;
    int commanded_brightness_ = 0;
    CoverTarget cover_target_ = CoverTarget::None;
    util::TaskClock::clock::time_point cover_command_at_{};
    bool cover_halted_ = false;
    mutable std::optional<std::string> cover_failure_;  // set by the first read that sees the failure

    mutable std::mutex firmware_mutex_;
    std::string firmware_;

    // Declared last: the poll body touches everything above, so the module (whose destructor joins) goes first.
    util::OperationGeneration state_poll_generation_;
    util::PolledState<FlatPanelStatus> state_;
};

std::unique_ptr<CoverCalibratorDriver> create_deepskydad_covercalibrator(int device_number,
                                                                         const std::string& serial_port,
                                                                         util::TaskClock& clock) {
    FlatPanelConnectionConfig config;
    config.serial_port = serial_port;
    return std::make_unique<DeepSkyDadCoverCalibratorDriver>(
        device_number, std::move(config), util::ConnectionResolver<FlatPanelConnectionConfig>{}, clock);
}

std::unique_ptr<CoverCalibratorDriver> create_deepskydad_covercalibrator_deferred(
    int device_number, util::ConnectionResolver<FlatPanelConnectionConfig> resolver, util::TaskClock& clock) {
    if (!resolver) {
        throw AlpacaException("Deep Sky Dad flat panel: a connection resolver is required", AlpacaError::InvalidValue);
    }
    return std::make_unique<DeepSkyDadCoverCalibratorDriver>(device_number, FlatPanelConnectionConfig{},
                                                             std::move(resolver), clock);
}

FlatPanelConnectionConfig resolve_deepskydad_flatpanel_by_index(int panel_index) {
    const auto ports = enumerate_deepskydad_flatpanel_ports();
    if (ports.empty()) {
        throw AlpacaException(util::serial_auto_detect_failed_message("Deep Sky Dad flat panel"),
                              AlpacaError::NotConnected);
    }
    if (panel_index < 0 || panel_index >= static_cast<int>(ports.size())) {
        throw AlpacaException("Flat panel index " + std::to_string(panel_index) + " out of range (detected " +
                                  std::to_string(ports.size()) + ")",
                              AlpacaError::NotConnected);
    }
    FlatPanelConnectionConfig config;
    config.serial_port = ports[static_cast<std::size_t>(panel_index)].port_path;
    config.auto_detect_index = panel_index;
    ALPACA_LOG_INFO("DeepSkyDad", "Auto-detected flat panel at " + config.serial_port);
    return config;
}

std::unique_ptr<CoverCalibratorDriver> create_deepskydad_covercalibrator_by_index(int device_number, int panel_index) {
    // The scan runs at connect, never here (serial-autodetect rules, #659).
    return create_deepskydad_covercalibrator_deferred(
        device_number, [panel_index] { return resolve_deepskydad_flatpanel_by_index(panel_index); });
}

}  // namespace alpacacore::vendor::deepskydad
