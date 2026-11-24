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

// Connect/disconnect/operate concurrency stress for the ScopeDome dome (issue #101).
// The driver and its protocol wrapper run over FakeScopeDomeCard on a pty, so
// connects succeed and the storm reaches the handshake, the poll's supervision
// and the rotation and shutter commands, not just the connect-failure path.

#include <alpacacore/dome_driver.h>
#include <alpacacore/vendor/scopedome/scopedome_dome_driver.h>

#include <memory>

#include "catch2_compat.h"
#include "concurrency_stress.h"
#include "fake_scopedome_card.h"

using alpacacore::AlpacaDriver;
namespace sd = alpacacore::vendor::scopedome;

TEST_CASE("ScopeDome dome - concurrent connect/disconnect/operate stress", "[scopedome][dome][stress]") {
    alpacacore::test::FakeScopeDomeCard card;
    card.set_shutter_travel_time(std::chrono::milliseconds(50));
    auto driver = sd::create_scopedome_dome(0, sd::ScopeDomeDomeConfig{card.slave_path()});

    // NotConnected while a lifecycle thread has the driver down; DriverException when a command
    // lands while the poll is faulted by the churn or the card answers "motion conflict";
    // NotImplemented / MethodNotImplemented for Altitude, Slaved=true and the sync.
    alpacacore::test::StressCallGuard guard(
        {alpacacore::AlpacaError::NotConnected, alpacacore::AlpacaError::DriverException,
         alpacacore::AlpacaError::NotImplemented, alpacacore::AlpacaError::MethodNotImplemented});
    alpacacore::test::run_lifecycle_stress(*driver, [&guard](AlpacaDriver& d) {
        auto& dome = static_cast<alpacacore::DomeDriver&>(d);
        guard([&] { static_cast<void>(dome.get_azimuth()); });
        guard([&] { static_cast<void>(dome.get_slewing()); });
        guard([&] { static_cast<void>(dome.get_at_park()); });
        guard([&] { static_cast<void>(dome.get_at_home()); });
        guard([&] { static_cast<void>(dome.get_shutter_status()); });
        guard([&] { static_cast<void>(dome.get_altitude()); });
        guard([&] { static_cast<void>(dome.get_device_state()); });
        guard([&] { static_cast<void>(dome.get_device_firmware()); });
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
    CHECK(card.count(alpacacore::vendor::scopedome::ScopeDomeCommand::ConnectionTest) > 0);
    INFO(guard.report());
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.total_calls() > 0);
}

TEST_CASE("ScopeDome dome - destruction races an in-flight connect", "[scopedome][dome][stress]") {
    // A fresh fake per iteration: a shared pty would hand the next driver the replies queued for the last one.
    alpacacore::test::run_destruction_during_connect_stress_fresh_fake(
        []() { return std::make_unique<alpacacore::test::FakeScopeDomeCard>(); },
        [](alpacacore::test::FakeScopeDomeCard& card) {
            return sd::create_scopedome_dome(0, sd::ScopeDomeDomeConfig{card.slave_path()});
        },
        25);
}

TEST_CASE("ScopeDome dome - racing disconnect is never dropped", "[scopedome][dome][stress]") {
    alpacacore::test::FakeScopeDomeCard card;
    auto driver = sd::create_scopedome_dome(0, sd::ScopeDomeDomeConfig{card.slave_path()});
    CHECK(alpacacore::test::connect_then_disconnect_settles_disconnected(*driver) == false);
}
