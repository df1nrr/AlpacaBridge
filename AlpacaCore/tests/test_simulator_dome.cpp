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

// The Dome simulator (IDomeV3). The sweep entry (contract_sweep.h) covers the
// NotConnected and unsupported Action/Command codes; this file carries the rest
// on a FakeTaskClock, which the motion model reads.

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/simulator/simulator_dome_driver.h>
#include <alpacacore/vendor/simulator/simulator_dome_model.h>
#include <alpacacore/version.h>

#include <chrono>
#include <cmath>
#include <limits>
#include <memory>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_task_clock.h"

namespace {

namespace AlpacaError = alpacacore::AlpacaError;
using alpacacore::test::FakeTaskClock;
using alpacacore::test::require_alpaca_error;
using Catch::Approx;
namespace sim = alpacacore::vendor::simulator;
using std::chrono::milliseconds;
using std::chrono::seconds;

constexpr int kOpen = 0;
constexpr int kClosed = 1;
constexpr int kOpening = 2;
constexpr int kClosing = 3;
constexpr int kError = 4;

struct Rig {
    explicit Rig(sim::DomeSimulatorConfig config = {}) : driver(sim::create_simulator_dome(0, config, clock)) {
        driver->set_connected(true);
    }
    FakeTaskClock clock;
    std::unique_ptr<alpacacore::DomeDriver> driver;
};

}  // namespace

TEST_CASE("Simulator Dome Driver - Defaults", "[simulator][dome][unit]") {
    auto driver = sim::create_simulator_dome(0);
    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Dome);
    REQUIRE(driver->get_device_number() == 0);
    REQUIRE(driver->get_connected() == false);
    CHECK(driver->get_name() == "AlpacaBridge Dome Simulator");
    CHECK(driver->get_connecting() == false);
    require_alpaca_error([&]() { (void)driver->get_can_park(); }, AlpacaError::NotConnected);
}

TEST_CASE("Simulator Dome Driver - Device metadata", "[simulator][dome][unit]") {
    auto driver = sim::create_simulator_dome(3);
    CHECK(driver->get_device_number() == 3);
    CHECK(driver->get_description() == "Simulated observatory dome: rotation, shutter, park and home with no hardware");
    CHECK(driver->get_driver_info() == "AlpacaCore Simulator Dome Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 3);
    CHECK(driver->get_unique_id() == "SIMULATOR_DOME_3");
}

TEST_CASE("Simulator Dome Driver - Capabilities", "[simulator][dome][unit]") {
    Rig rig;
    CHECK(rig.driver->get_can_find_home());
    CHECK(rig.driver->get_can_park());
    CHECK(rig.driver->get_can_set_azimuth());
    CHECK(rig.driver->get_can_set_park());
    CHECK(rig.driver->get_can_set_shutter());
    CHECK(rig.driver->get_can_sync_azimuth());
    CHECK_FALSE(rig.driver->get_can_set_altitude());
    CHECK_FALSE(rig.driver->get_can_slave());
    CHECK_FALSE(rig.driver->get_slaved());
    CHECK_FALSE(rig.driver->get_device_state().empty());
}

TEST_CASE("Simulator Dome Driver - Value range validation", "[simulator][dome][unit]") {
    // InvalidValue precedes the connection check.
    auto driver = sim::create_simulator_dome(0);
    require_alpaca_error([&]() { driver->slew_to_azimuth(-0.1); }, AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->slew_to_azimuth(360.0); }, AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->sync_to_azimuth(400.0); }, AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->slew_to_azimuth(std::numeric_limits<double>::quiet_NaN()); },
                         AlpacaError::InvalidValue);
    require_alpaca_error([&]() { driver->slew_to_azimuth(90.0); }, AlpacaError::NotConnected);

    Rig rig;
    require_alpaca_error([&]() { rig.driver->slew_to_azimuth(361.0); }, AlpacaError::InvalidValue);
    require_alpaca_error([&]() { rig.driver->sync_to_azimuth(-1.0); }, AlpacaError::InvalidValue);
    CHECK_FALSE(rig.driver->get_slewing());  // a refused slew starts nothing
}

TEST_CASE("Simulator Dome Driver - Unsupported methods", "[simulator][dome][unit]") {
    Rig rig;
    require_alpaca_error([&]() { (void)rig.driver->get_altitude(); }, AlpacaError::NotImplemented);
    require_alpaca_error([&]() { rig.driver->slew_to_altitude(30.0); }, AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { rig.driver->set_slaved(true); }, AlpacaError::NotImplemented);
    rig.driver->set_slaved(false);  // false is always accepted
    CHECK(rig.driver->get_supported_actions().empty());
    require_alpaca_error([&]() { rig.driver->action("test", ""); }, AlpacaError::ActionNotImplemented);
    require_alpaca_error([&]() { rig.driver->command_blind("x", false); }, AlpacaError::MethodNotImplemented);
}

TEST_CASE("Simulator Dome Driver - State machine", "[simulator][dome][unit]") {
    auto driver = sim::create_simulator_dome(0);
    require_alpaca_error([&]() { (void)driver->get_azimuth(); }, AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_slewing(); }, AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)driver->get_shutter_status(); }, AlpacaError::NotConnected);
    require_alpaca_error([&]() { driver->park(); }, AlpacaError::NotConnected);
    CHECK(driver->get_device_state().empty());

    Rig rig;
    CHECK(rig.driver->get_azimuth() == Approx(180.0));
    CHECK_FALSE(rig.driver->get_slewing());
    CHECK(rig.driver->get_shutter_status() == kClosed);
    CHECK_FALSE(rig.driver->get_at_park());  // AtPark only after a Park
    CHECK_FALSE(rig.driver->get_at_home());
    rig.driver->set_connected(false);
    CHECK_FALSE(rig.driver->get_connected());
}

TEST_CASE("Simulator Dome Driver - Slew the short way round at the configured rate", "[simulator][dome][unit]") {
    sim::DomeSimulatorConfig config;
    config.start_azimuth = 350.0;
    config.slew_rate_deg_per_s = 10.0;
    Rig rig(config);
    rig.driver->slew_to_azimuth(20.0);  // 30 degrees clockwise through north, not 330 the other way
    CHECK(rig.driver->get_slewing());
    rig.clock.advance(seconds(1));
    CHECK(rig.driver->get_azimuth() == Approx(0.0).margin(1e-9));
    rig.clock.advance(seconds(1));
    CHECK(rig.driver->get_azimuth() == Approx(10.0));
    CHECK(rig.driver->get_slewing());
    rig.clock.advance(seconds(2));
    CHECK(rig.driver->get_azimuth() == Approx(20.0));
    CHECK_FALSE(rig.driver->get_slewing());

    rig.driver->slew_to_azimuth(350.0);  // and back counter-clockwise
    rig.clock.advance(milliseconds(1500));
    CHECK(rig.driver->get_azimuth() == Approx(5.0));
}

TEST_CASE("Simulator Dome Driver - Shutter opens and closes in the configured time", "[simulator][dome][unit]") {
    sim::DomeSimulatorConfig config;
    config.shutter_time_s = 4.0;
    Rig rig(config);
    rig.driver->open_shutter();
    CHECK(rig.driver->get_shutter_status() == kOpening);
    CHECK(rig.driver->get_slewing());  // any part of the dome moving
    rig.clock.advance(seconds(3));
    CHECK(rig.driver->get_shutter_status() == kOpening);
    rig.clock.advance(seconds(1));
    CHECK(rig.driver->get_shutter_status() == kOpen);
    CHECK_FALSE(rig.driver->get_slewing());

    rig.driver->open_shutter();  // already open: nothing moves
    CHECK(rig.driver->get_shutter_status() == kOpen);

    rig.driver->close_shutter();
    CHECK(rig.driver->get_shutter_status() == kClosing);
    rig.clock.advance(seconds(1));
    rig.driver->open_shutter();  // reverse part-way: opens from where it is
    rig.clock.advance(milliseconds(900));
    CHECK(rig.driver->get_shutter_status() == kOpening);
    rig.clock.advance(milliseconds(200));
    CHECK(rig.driver->get_shutter_status() == kOpen);
}

TEST_CASE("Simulator Dome Driver - Park, AtPark and SetPark", "[simulator][dome][unit]") {
    sim::DomeSimulatorConfig config;
    config.start_azimuth = 90.0;
    config.park_azimuth = 120.0;
    config.slew_rate_deg_per_s = 10.0;
    Rig rig(config);
    rig.driver->park();
    CHECK_FALSE(rig.driver->get_at_park());  // still rotating
    CHECK(rig.driver->get_slewing());
    rig.clock.advance(seconds(3));
    CHECK(rig.driver->get_at_park());
    CHECK(rig.driver->get_azimuth() == Approx(120.0));
    rig.driver->park();  // parked: a no-op
    CHECK(rig.driver->get_at_park());

    rig.driver->slew_to_azimuth(200.0);  // any movement clears AtPark
    CHECK_FALSE(rig.driver->get_at_park());
    rig.clock.advance(seconds(8));
    rig.driver->set_park();
    rig.driver->park();
    CHECK(rig.driver->get_at_park());  // the new park position is where it stands
    CHECK(rig.driver->get_azimuth() == Approx(200.0));
}

TEST_CASE("Simulator Dome Driver - FindHome and AtHome", "[simulator][dome][unit]") {
    sim::DomeSimulatorConfig config;
    config.start_azimuth = 30.0;
    config.home_azimuth = 0.0;
    config.slew_rate_deg_per_s = 10.0;
    Rig rig(config);
    CHECK_FALSE(rig.driver->get_at_home());
    rig.driver->find_home();
    rig.clock.advance(seconds(1));
    CHECK_FALSE(rig.driver->get_at_home());
    rig.clock.advance(seconds(2));
    CHECK(rig.driver->get_at_home());
    CHECK(rig.driver->get_azimuth() == Approx(0.0).margin(1e-9));
}

TEST_CASE("Simulator Dome Driver - AbortSlew stops rotation and shutter", "[simulator][dome][unit]") {
    sim::DomeSimulatorConfig config;
    config.start_azimuth = 0.0;
    config.slew_rate_deg_per_s = 10.0;
    config.shutter_time_s = 10.0;
    Rig rig(config);
    rig.driver->slew_to_azimuth(90.0);
    rig.driver->open_shutter();
    rig.clock.advance(seconds(3));
    rig.driver->abort_slew();
    CHECK_FALSE(rig.driver->get_slewing());
    CHECK(rig.driver->get_azimuth() == Approx(30.0));
    CHECK(rig.driver->get_shutter_status() == kError);  // stopped part-way
    rig.clock.advance(seconds(10));
    CHECK(rig.driver->get_azimuth() == Approx(30.0));
    rig.driver->close_shutter();  // a new command clears the error
    rig.clock.advance(seconds(5));
    CHECK(rig.driver->get_shutter_status() == kClosed);
}

TEST_CASE("Simulator Dome Driver - SyncToAzimuth and disconnect keep the position", "[simulator][dome][unit]") {
    Rig rig;
    rig.driver->sync_to_azimuth(45.0);
    CHECK(rig.driver->get_azimuth() == Approx(45.0));
    CHECK_FALSE(rig.driver->get_slewing());
    rig.driver->set_connected(false);
    rig.driver->set_connected(true);
    CHECK(rig.driver->get_azimuth() == Approx(45.0));
}

TEST_CASE("Simulator Dome Driver - Model normalizes azimuth", "[simulator][dome][unit]") {
    CHECK(sim::DomeModel::normalize(-10.0) == Approx(350.0));
    CHECK(sim::DomeModel::normalize(370.0) == Approx(10.0));
    CHECK(sim::DomeModel::normalize(360.0) == Approx(0.0));
    CHECK(sim::DomeModel::normalize(0.0) == Approx(0.0));
}
