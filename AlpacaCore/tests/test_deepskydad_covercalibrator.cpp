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

// Deep Sky Dad FP1/FP2 flat panel. The sweep entry (contract_sweep.h) covers the
// NotConnected and unsupported Action/Command codes (cases 3 and 4); this file
// carries cases 1, 2 and 5-8 over FakeDeepSkyDadFlatPanel on a FakeTaskClock.

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/deepskydad/deepskydad_covercalibrator_driver.h>
#include <alpacacore/vendor/deepskydad/deepskydad_flatpanel_protocol_wrapper.h>
#include <alpacacore/version.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_deepskydad_flatpanel.h"
#include "fake_task_clock.h"

namespace {

namespace AlpacaError = alpacacore::AlpacaError;
using alpacacore::CalibratorState;
using alpacacore::CoverState;
using alpacacore::test::FakeDeepSkyDadFlatPanel;
using alpacacore::test::FakeTaskClock;
using alpacacore::test::require_alpaca_error;
namespace dsd = alpacacore::vendor::deepskydad;

constexpr auto kPeriod = std::chrono::milliseconds(500);
constexpr auto kBound = std::chrono::milliseconds(5000);

// A fake panel and a driver on a fake clock, connected and with its poll parked.
struct Rig {
    explicit Rig(bool fp2 = true) : panel(fp2) {
        panel.set_manual_moves(true);
        driver = dsd::create_deepskydad_covercalibrator(0, panel.slave_path(), clock);
    }
    ~Rig() {
        try {
            if (driver && driver->get_connected()) driver->set_connected(false);
        } catch (...) {  // a destructor must not throw
        }
    }
    bool connect() { return alpacacore::test::connect_and_park(*driver, clock, kBound); }
    void tick(int n = 1, std::chrono::milliseconds bound = kBound) {
        for (int i = 0; i < n; ++i) alpacacore::test::tick(clock, kPeriod, bound);
    }
    FakeTaskClock clock;
    FakeDeepSkyDadFlatPanel panel;
    std::unique_ptr<alpacacore::CoverCalibratorDriver> driver;
};

}  // namespace

TEST_CASE("Deep Sky Dad CoverCalibrator Driver - Defaults", "[deepskydad][covercalibrator][unit]") {
    auto driver = dsd::create_deepskydad_covercalibrator(0, "/dev/ttyACM0");
    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::CoverCalibrator);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_name() == "Deep Sky Dad FP2");
    CHECK(driver->get_connecting() == false);
    CHECK(driver->get_device_firmware() == std::nullopt);
    // MaxBrightness is a property: disconnected it refuses, the value is read connected.
    require_alpaca_error([&]() { (void)driver->get_max_brightness(); }, AlpacaError::NotConnected);
}

TEST_CASE("Deep Sky Dad CoverCalibrator Driver - Device metadata", "[deepskydad][covercalibrator][unit]") {
    auto driver = dsd::create_deepskydad_covercalibrator(3, "/dev/ttyACM0");
    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "Deep Sky Dad flat panel (motorized flap and LED panel)");
    CHECK(driver->get_driver_info() == "AlpacaCore Deep Sky Dad CoverCalibrator Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 2);
    CHECK(driver->get_unique_id() == "DEEPSKYDAD_COVERCALIBRATOR_3");
}

TEST_CASE("Deep Sky Dad CoverCalibrator Driver - Protocol parsing", "[deepskydad][covercalibrator][unit]") {
    std::string payload;
    CHECK(dsd::parse_deepskydad_reply("(OK)", payload));
    CHECK(payload == "OK");
    CHECK(dsd::parse_deepskydad_reply("\r(270)", payload));
    CHECK(payload == "270");
    CHECK_FALSE(dsd::parse_deepskydad_reply("OK", payload));
    CHECK_FALSE(dsd::parse_deepskydad_reply("(OK", payload));

    dsd::FlatPanelModel model{};
    REQUIRE(dsd::parse_deepskydad_firmware("Board=DeepSkyDad.FP2, Version=2.1.0", model));
    CHECK(model == dsd::FlatPanelModel::FP2);
    REQUIRE(dsd::parse_deepskydad_firmware("Board=DeepSkyDad.FP1, Version=1.3.0", model));
    CHECK(model == dsd::FlatPanelModel::FP1);
    CHECK_FALSE(dsd::parse_deepskydad_firmware("Board=DeepSkyDad.AF3", model));

    CHECK(dsd::cover_end_for(dsd::FlatPanelModel::FP2, 0) == dsd::FlatPanelCoverEnd::Closed);
    CHECK(dsd::cover_end_for(dsd::FlatPanelModel::FP2, 1) == dsd::FlatPanelCoverEnd::Open);
    CHECK(dsd::cover_end_for(dsd::FlatPanelModel::FP2, 2) == dsd::FlatPanelCoverEnd::Neither);
    CHECK(dsd::cover_end_for(dsd::FlatPanelModel::FP1, 270) == dsd::FlatPanelCoverEnd::Closed);
    CHECK(dsd::cover_end_for(dsd::FlatPanelModel::FP1, 0) == dsd::FlatPanelCoverEnd::Open);
    CHECK(dsd::cover_end_for(dsd::FlatPanelModel::FP1, 135) == dsd::FlatPanelCoverEnd::Neither);
}

TEST_CASE("Deep Sky Dad CoverCalibrator Driver - Value range validation", "[deepskydad][covercalibrator][unit]") {
    // InvalidValue precedes the connection check.
    auto driver = dsd::create_deepskydad_covercalibrator(0, "/dev/ttyACM0");
    require_alpaca_error([&]() { driver->calibrator_on(-1); }, AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->calibrator_on(dsd::kDeepSkyDadMaxBrightness + 1); },
                         AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->calibrator_on(0); }, AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->calibrator_on(dsd::kDeepSkyDadMaxBrightness); }, AlpacaError::NotConnected);

    Rig rig;
    REQUIRE(rig.connect());
    require_alpaca_error([&]() { rig.driver->calibrator_on(-1); }, AlpacaError::InvalidValue);
    require_alpaca_error([&]() { rig.driver->calibrator_on(4097); }, AlpacaError::InvalidValue);
    CHECK(rig.panel.count("[SLBR-001]") == 0);
    CHECK(rig.driver->get_max_brightness() == 4096);
}

TEST_CASE("Deep Sky Dad CoverCalibrator Driver - Unsupported methods", "[deepskydad][covercalibrator][unit]") {
    auto driver = dsd::create_deepskydad_covercalibrator(0, "/dev/ttyACM0");
    CHECK(driver->get_supported_actions().empty());
    CHECK(driver->can_action("anything") == false);
    require_alpaca_error([&]() { driver->action("test", ""); }, AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { driver->command_blind("test", false); }, AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_bool("test", false); }, AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { driver->command_string("test", false); }, AlpacaError::MethodNotImplemented);
}

TEST_CASE("Deep Sky Dad CoverCalibrator Driver - State machine", "[deepskydad][covercalibrator][unit]") {
    auto driver = dsd::create_deepskydad_covercalibrator(0, "/dev/ttyACM0");
    require_alpaca_error([&]() { (void)driver->get_cover_state(); }, AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_calibrator_state(); }, AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_cover_moving(); }, AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_brightness(); }, AlpacaError::NotConnected);
    CHECK(driver->get_device_state().empty());

    Rig rig;
    REQUIRE(rig.connect());
    CHECK(rig.driver->get_connected());
    CHECK(rig.driver->get_cover_state() == CoverState::Closed);
    CHECK_FALSE(rig.driver->get_cover_moving());
    CHECK(rig.driver->get_calibrator_state() == CalibratorState::Off);
    CHECK_FALSE(rig.driver->get_calibrator_changing());
    CHECK(rig.driver->get_brightness() == 0);
    CHECK_FALSE(rig.driver->get_device_state().empty());
    REQUIRE(rig.driver->get_device_firmware().has_value());
    CHECK(rig.driver->get_device_firmware()->find("DeepSkyDad.FP2") != std::string::npos);

    rig.driver->set_connected(false);
    CHECK_FALSE(rig.driver->get_connected());
    CHECK(rig.driver->get_device_firmware() == std::nullopt);
}

TEST_CASE("Deep Sky Dad CoverCalibrator Driver - Open and close the flap", "[deepskydad][covercalibrator][fake]") {
    Rig rig;
    REQUIRE(rig.connect());
    rig.panel.clear_commands();

    rig.driver->open_cover();
    const auto sent = rig.panel.commands();
    REQUIRE(sent.size() >= 2);
    CHECK(sent[0] == "[STRG0]");
    CHECK(sent[1] == "[SMOV]");
    // Moving at once, before any poll has seen the motor.
    CHECK(rig.driver->get_cover_state() == CoverState::Moving);
    CHECK(rig.driver->get_cover_moving());
    rig.tick(2);
    CHECK(rig.driver->get_cover_state() == CoverState::Moving);

    rig.panel.finish_move();
    rig.tick();
    CHECK(rig.driver->get_cover_state() == CoverState::Open);
    CHECK_FALSE(rig.driver->get_cover_moving());

    rig.panel.clear_commands();
    rig.driver->close_cover();
    CHECK(rig.panel.count("[STRG270]") == 1);
    CHECK(rig.panel.count("[SMOV]") == 1);
    CHECK(rig.driver->get_cover_state() == CoverState::Moving);
    rig.panel.finish_move();
    rig.tick();
    CHECK(rig.driver->get_cover_state() == CoverState::Closed);
}

TEST_CASE("Deep Sky Dad CoverCalibrator Driver - A flap that stops short is an Error until the next command",
          "[deepskydad][covercalibrator][fake]") {
    Rig rig;
    REQUIRE(rig.connect());
    rig.panel.set_jam(true);
    rig.driver->open_cover();
    rig.panel.finish_move();  // stops part-way
    rig.tick();
    // Inside the start grace the stop can still be the motor not yet reporting.
    CHECK(rig.driver->get_cover_state() == CoverState::Moving);
    rig.tick(7);  // past the 3 s grace
    CHECK(rig.driver->get_cover_state() == CoverState::Error);
    rig.tick(2);
    CHECK(rig.driver->get_cover_state() == CoverState::Error);  // kept, not re-derived

    rig.panel.set_jam(false);
    rig.driver->close_cover();  // a new command clears it
    CHECK(rig.driver->get_cover_state() == CoverState::Moving);
    rig.panel.finish_move();
    rig.tick();
    CHECK(rig.driver->get_cover_state() == CoverState::Closed);
}

TEST_CASE("Deep Sky Dad CoverCalibrator Driver - A move past its time limit is an Error",
          "[deepskydad][covercalibrator][fake]") {
    Rig rig;
    REQUIRE(rig.connect());
    rig.driver->open_cover();
    rig.tick(178);  // 89 s: still moving
    CHECK(rig.driver->get_cover_state() == CoverState::Moving);
    rig.tick(4);  // past 90 s
    CHECK(rig.driver->get_cover_state() == CoverState::Error);
}

TEST_CASE("Deep Sky Dad CoverCalibrator Driver - HaltCover stops reporting the move",
          "[deepskydad][covercalibrator][fake]") {
    Rig rig;
    REQUIRE(rig.connect());
    rig.driver->halt_cover();  // idle: no effect
    CHECK(rig.driver->get_cover_state() == CoverState::Closed);

    rig.driver->open_cover();
    rig.tick();
    REQUIRE(rig.driver->get_cover_state() == CoverState::Moving);
    rig.driver->halt_cover();
    CHECK(rig.driver->get_cover_state() == CoverState::Unknown);  // part-way, no longer Moving
    CHECK_FALSE(rig.driver->get_cover_moving());
    rig.panel.finish_move();
    rig.tick();
    CHECK(rig.driver->get_cover_state() == CoverState::Open);  // the panel's own end once still
}

TEST_CASE("Deep Sky Dad CoverCalibrator Driver - Calibrator on, zero and off", "[deepskydad][covercalibrator][fake]") {
    Rig rig;
    REQUIRE(rig.connect());

    rig.driver->calibrator_on(1500);
    CHECK(rig.panel.count("[SLBR1500]") == 1);
    CHECK(rig.panel.count("[SLON1]") == 1);
    CHECK(rig.panel.light_on());
    CHECK(rig.driver->get_calibrator_state() == CalibratorState::Ready);
    CHECK(rig.driver->get_brightness() == 1500);

    // CalibratorOn(0) is Ready at zero; the panel is dark.
    rig.driver->calibrator_on(0);
    CHECK(rig.driver->get_calibrator_state() == CalibratorState::Ready);
    CHECK(rig.driver->get_brightness() == 0);
    CHECK_FALSE(rig.panel.light_on());

    rig.driver->calibrator_on(4096);
    CHECK(rig.panel.count("[SLBR4096]") == 1);
    rig.tick();
    CHECK(rig.driver->get_brightness() == 4096);

    rig.driver->calibrator_off();
    CHECK(rig.panel.count("[SLON0]") >= 1);
    CHECK_FALSE(rig.panel.light_on());
    CHECK(rig.driver->get_calibrator_state() == CalibratorState::Off);
    CHECK(rig.driver->get_brightness() == 0);
}

TEST_CASE("Deep Sky Dad CoverCalibrator Driver - A refused brightness throws and keeps the state",
          "[deepskydad][covercalibrator][fake]") {
    Rig rig;
    REQUIRE(rig.connect());
    rig.panel.set_brightness_reply("ERR");
    require_alpaca_error([&]() { rig.driver->calibrator_on(800); }, AlpacaError::DriverException);
    CHECK(rig.driver->get_calibrator_state() == CalibratorState::Off);
    CHECK(rig.driver->get_brightness() == 0);
}

TEST_CASE("Deep Sky Dad CoverCalibrator Driver - Connect seeds a lit panel", "[deepskydad][covercalibrator][fake]") {
    Rig rig;
    rig.panel.set_light(true, 2000);
    rig.panel.set_cover_open(true);
    REQUIRE(rig.connect());
    CHECK(rig.driver->get_calibrator_state() == CalibratorState::Ready);
    CHECK(rig.driver->get_brightness() == 2000);
    CHECK(rig.driver->get_cover_state() == CoverState::Open);
}

TEST_CASE("Deep Sky Dad CoverCalibrator Driver - FP1 reads the flap angle", "[deepskydad][covercalibrator][fake]") {
    Rig rig(/*fp2=*/false);
    REQUIRE(rig.connect());
    CHECK(rig.driver->get_name() == "Deep Sky Dad FP1");
    CHECK(rig.driver->get_cover_state() == CoverState::Closed);
    CHECK(rig.panel.count("[GPOS]") >= 1);
    CHECK(rig.panel.count("[GOPS]") == 0);
    rig.driver->open_cover();
    rig.panel.finish_move();
    rig.tick();
    CHECK(rig.driver->get_cover_state() == CoverState::Open);
}

TEST_CASE("Deep Sky Dad CoverCalibrator Driver - A silent panel faults, then recovers",
          "[deepskydad][covercalibrator][fake]") {
    Rig rig;
    REQUIRE(rig.connect());
    rig.panel.set_muted(true);
    // Each muted poll waits out the 3 s reply timeout; three in a row latch the fault.
    rig.tick(3, std::chrono::milliseconds(10000));
    CHECK(rig.driver->get_connected());  // a young fault keeps Connected
    CHECK(rig.driver->get_cover_state() == CoverState::Unknown);
    CHECK(rig.driver->get_calibrator_state() == CalibratorState::Unknown);
    require_alpaca_error([&]() { (void)rig.driver->get_brightness(); }, AlpacaError::DriverException);
    require_alpaca_error([&]() { rig.driver->open_cover(); }, AlpacaError::DriverException);
    CHECK(rig.driver->get_max_brightness() == 4096);  // static metadata still answers

    rig.panel.set_muted(false);
    rig.tick(1, std::chrono::milliseconds(10000));
    CHECK(rig.driver->get_cover_state() == CoverState::Closed);
    CHECK(rig.driver->get_calibrator_state() == CalibratorState::Off);
}

TEST_CASE("Deep Sky Dad CoverCalibrator Driver - An unplugged panel loses the link",
          "[deepskydad][covercalibrator][fake]") {
    Rig rig;
    REQUIRE(rig.connect());
    rig.panel.unplug();
    rig.clock.advance(kPeriod);
    REQUIRE(rig.clock.wait_for_woken_settled(kBound));
    CHECK_FALSE(rig.driver->get_connected());
    CHECK_FALSE(rig.driver->get_link_fault().empty());
    require_alpaca_error([&]() { (void)rig.driver->get_cover_state(); }, AlpacaError::NotConnected);
    require_alpaca_error([&]() { rig.driver->calibrator_off(); }, AlpacaError::NotConnected);
    rig.driver->set_connected(false);  // the teardown over a lost link does not throw
    CHECK_FALSE(rig.driver->get_connected());
}

TEST_CASE("Deep Sky Dad CoverCalibrator Driver - A port with no panel refuses the connect",
          "[deepskydad][covercalibrator][fake]") {
    auto driver = dsd::create_deepskydad_covercalibrator(0, "/dev/alpacacore-deepskydad-absent");
    require_alpaca_error([&]() { driver->set_connected(true); }, AlpacaError::NotConnected);
    CHECK_FALSE(driver->get_connected());
}

TEST_CASE("Deep Sky Dad CoverCalibrator Driver - Deferred connect resolves the port at connect",
          "[deepskydad][covercalibrator][fake]") {
    FakeDeepSkyDadFlatPanel panel;
    int scans = 0;
    auto driver = dsd::create_deepskydad_covercalibrator_deferred(0, [&] {
        ++scans;
        dsd::FlatPanelConnectionConfig cfg;
        cfg.serial_port = panel.slave_path();
        cfg.auto_detect_index = 0;
        return cfg;
    });
    CHECK(scans == 0);  // construction touches no port
    driver->set_connected(true);
    CHECK(driver->get_connected());
    driver->set_connected(false);
    driver->set_connected(true);
    CHECK(scans == 1);  // the resolved port is reused across a reconnect
    driver->set_connected(false);

    auto refused = dsd::create_deepskydad_covercalibrator_deferred(0, []() -> dsd::FlatPanelConnectionConfig {
        throw alpacacore::AlpacaException("no panel found", AlpacaError::NotConnected);
    });
    std::string message;
    CHECK(alpacacore::test::error_code_of([&]() { refused->set_connected(true); }, &message) ==
          static_cast<int>(AlpacaError::NotConnected));
    CHECK(message == "no panel found");
}
