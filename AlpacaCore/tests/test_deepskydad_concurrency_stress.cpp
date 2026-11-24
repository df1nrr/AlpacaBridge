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

// Connect/disconnect/operate concurrency stress for the Deep Sky Dad flat panel
// (issue #101). The driver and its protocol wrapper run over FakeDeepSkyDadFlatPanel
// on a pty, so connects succeed and the storm reaches the handshake, the state poll
// and the cover and calibrator commands, not just the connect-failure path.

#include <alpacacore/covercalibrator_driver.h>
#include <alpacacore/vendor/deepskydad/deepskydad_covercalibrator_driver.h>

#include <chrono>
#include <memory>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_deepskydad_flatpanel.h"

using alpacacore::AlpacaDriver;

TEST_CASE("Deep Sky Dad flat panel - concurrent connect/disconnect/operate stress",
          "[deepskydad][covercalibrator][stress]") {
    alpacacore::test::FakeDeepSkyDadFlatPanel panel;
    panel.set_move_time(std::chrono::milliseconds(50));
    auto driver = alpacacore::vendor::deepskydad::create_deepskydad_covercalibrator(0, panel.slave_path());

    // NotConnected while a lifecycle thread has the driver down; DriverException when a command
    // lands while the poll is faulted by the churn (a reply cut off by a disconnect).
    alpacacore::test::StressCallGuard guard(
        {alpacacore::AlpacaError::NotConnected, alpacacore::AlpacaError::DriverException});
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& panel_driver = static_cast<alpacacore::CoverCalibratorDriver&>(d);
        guard([&] { static_cast<void>(panel_driver.get_cover_state()); });
        guard([&] { static_cast<void>(panel_driver.get_cover_moving()); });
        guard([&] { static_cast<void>(panel_driver.get_calibrator_state()); });
        guard([&] { static_cast<void>(panel_driver.get_calibrator_changing()); });
        guard([&] { static_cast<void>(panel_driver.get_brightness()); });
        guard([&] { static_cast<void>(panel_driver.get_max_brightness()); });
        guard([&] { static_cast<void>(panel_driver.get_device_firmware()); });
        guard([&] { static_cast<void>(panel_driver.get_device_state()); });
        guard([&] { panel_driver.calibrator_on(1200); });
        guard([&] { panel_driver.calibrator_off(); });
        guard([&] { panel_driver.open_cover(); });
        guard([&] { panel_driver.halt_cover(); });
        guard([&] { panel_driver.close_cover(); });
    });

    CHECK(alpacacore::test::settle_connected(*driver, false));
    CHECK(panel.count("[GFRM]") > 0);  // the storm reached the handshake, not only the failure path

    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("Deep Sky Dad flat panel - destruction races an in-flight connect", "[deepskydad][covercalibrator][stress]") {
    // A fresh fake per iteration: a shared pty would hand the next driver the replies queued for the last one.
    alpacacore::test::run_destruction_during_connect_stress_fresh_fake(
        []() { return std::make_unique<alpacacore::test::FakeDeepSkyDadFlatPanel>(); },
        [](alpacacore::test::FakeDeepSkyDadFlatPanel& panel) {
            return alpacacore::vendor::deepskydad::create_deepskydad_covercalibrator(0, panel.slave_path());
        },
        25);
}

TEST_CASE("Deep Sky Dad flat panel - racing disconnect is never dropped", "[deepskydad][covercalibrator][stress]") {
    alpacacore::test::FakeDeepSkyDadFlatPanel panel;
    auto driver = alpacacore::vendor::deepskydad::create_deepskydad_covercalibrator(0, panel.slave_path());
    CHECK(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver) == false);
}
