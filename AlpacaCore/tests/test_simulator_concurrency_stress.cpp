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

// Connect/disconnect/operate concurrency stress for the Dome simulator (issue #101).
// The simulator has no hardware and every connect succeeds, so the storm reaches
// every operation; the motion model runs on the real clock here.

#include <alpacacore/dome_driver.h>
#include <alpacacore/vendor/simulator/simulator_dome_driver.h>

#include <memory>

#include "catch2_compat.h"
#include "concurrency_stress.h"

using alpacacore::AlpacaDriver;

TEST_CASE("Simulator dome - concurrent connect/disconnect/operate stress", "[simulator][dome][stress]") {
    alpacacore::vendor::simulator::DomeSimulatorConfig config;
    config.slew_rate_deg_per_s = 60.0;
    config.shutter_time_s = 0.5;
    auto driver = alpacacore::vendor::simulator::create_simulator_dome(0, config);

    // NotConnected while a lifecycle thread has the driver down; NotImplemented for Altitude and Slaved=true.
    alpacacore::test::StressCallGuard guard({alpacacore::AlpacaError::NotConnected,
                                             alpacacore::AlpacaError::NotImplemented,
                                             alpacacore::AlpacaError::MethodNotImplemented});
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& dome = static_cast<alpacacore::DomeDriver&>(d);
        guard([&] { static_cast<void>(dome.get_azimuth()); });
        guard([&] { static_cast<void>(dome.get_slewing()); });
        guard([&] { static_cast<void>(dome.get_at_park()); });
        guard([&] { static_cast<void>(dome.get_at_home()); });
        guard([&] { static_cast<void>(dome.get_shutter_status()); });
        guard([&] { static_cast<void>(dome.get_altitude()); });
        guard([&] { static_cast<void>(dome.get_device_state()); });
        guard([&] { dome.slew_to_azimuth(123.0); });
        guard([&] { dome.open_shutter(); });
        guard([&] { dome.park(); });
        guard([&] { dome.abort_slew(); });
        guard([&] { dome.find_home(); });
        guard([&] { dome.close_shutter(); });
        guard([&] { dome.sync_to_azimuth(10.0); });
        guard([&] { dome.set_park(); });
        guard([&] { dome.slew_to_altitude(10.0); });
    });

    CHECK(alpacacore::test::settle_connected(*driver, false));
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Simulator dome - destruction races an in-flight connect", "[simulator][dome][stress]") {
    alpacacore::test::run_destruction_during_connect_stress(
        []() { return alpacacore::vendor::simulator::create_simulator_dome(0); });
}

TEST_CASE("Simulator dome - racing disconnect is never dropped", "[simulator][dome][stress]") {
    auto driver = alpacacore::vendor::simulator::create_simulator_dome(0);
    CHECK(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver) == false);
}
