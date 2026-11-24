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

// ScopeDome USB Card 2.1 dome. The sweep entry (contract_sweep.h) covers the
// NotConnected and unsupported Action/Command codes (cases 3 and 4); this file
// carries cases 1, 2 and 5-8 over FakeScopeDomeCard on a FakeTaskClock. The fake
// card moves in real time (fast); the driver's poll and its timeouts run on the
// fake clock.

#include <alpacacore/util/error_handling.h>
#include <alpacacore/vendor/scopedome/scopedome_dome_driver.h>
#include <alpacacore/vendor/scopedome/scopedome_usb21_protocol_wrapper.h>
#include <alpacacore/version.h>

#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "driver_rig.h"
#include "fake_scopedome_card.h"
#include "fake_task_clock.h"

namespace {

namespace AlpacaError = alpacacore::AlpacaError;
using alpacacore::test::FakeScopeDomeCard;
using alpacacore::test::FakeTaskClock;
using alpacacore::test::require_alpaca_error;
namespace sd = alpacacore::vendor::scopedome;
using Cmd = sd::ScopeDomeCommand;
using Ch = sd::ScopeDomeChannel;

constexpr auto kPeriod = std::chrono::milliseconds(500);
constexpr auto kBound = std::chrono::milliseconds(5000);
constexpr int kOpen = 0;
constexpr int kClosed = 1;
constexpr int kOpening = 2;
constexpr int kClosing = 3;
constexpr int kError = 4;

struct Rig {
    explicit Rig(sd::ScopeDomeDomeConfig cfg = {}) {
        cfg.serial_port = card.slave_path();
        driver = sd::create_scopedome_dome(0, cfg, clock);
    }
    ~Rig() {
        try {
            if (driver && driver->get_connected()) driver->set_connected(false);
        } catch (...) {  // a destructor must not throw
        }
    }
    bool connect() { return alpacacore::test::connect_and_park(*driver, clock, kBound); }
    void tick(int n = 1) {
        for (int i = 0; i < n; ++i) alpacacore::test::tick(clock, kPeriod, kBound);
    }
    // Poll (letting the fake card move in real time between polls) until @p done, at most @p polls times.
    bool settle(const std::function<bool()>& done, int polls = 60) {
        for (int i = 0; i < polls; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            tick();
            if (done()) return true;
        }
        return false;
    }
    bool settle_slew() {
        return settle([this] { return !driver->get_slewing(); });
    }
    FakeTaskClock clock;
    FakeScopeDomeCard card;
    std::unique_ptr<alpacacore::DomeDriver> driver;
};

double az_distance(double a, double b) {
    double d = std::fmod(std::fabs(a - b), 360.0);
    return d > 180.0 ? 360.0 - d : d;
}

}  // namespace

TEST_CASE("ScopeDome Dome Driver - Defaults", "[scopedome][dome][unit]") {
    auto driver = sd::create_scopedome_dome(0, sd::ScopeDomeDomeConfig{"/dev/ttyUSB0"});
    REQUIRE(driver->get_device_type() == alpacacore::DeviceType::Dome);
    REQUIRE(driver->get_device_number() == 0);
    CHECK(driver->get_connected() == false);
    CHECK(driver->get_connecting() == false);
    CHECK(driver->get_name() == "ScopeDome USB Card 2.1");
    CHECK(driver->get_device_firmware() == std::nullopt);
    CHECK(driver->get_device_state().empty());
}

TEST_CASE("ScopeDome Dome Driver - Device metadata", "[scopedome][dome][unit]") {
    auto driver = sd::create_scopedome_dome(4, sd::ScopeDomeDomeConfig{"/dev/ttyUSB0"});
    CHECK(driver->get_device_number() == 4);
    CHECK(driver->get_description() == "ScopeDome observatory dome (USB Card 2.1)");
    CHECK(driver->get_driver_info() == "AlpacaCore ScopeDome Dome Driver");
    CHECK(driver->get_driver_version() == alpacacore::kVersion);
    CHECK(driver->get_interface_version() == 3);
    CHECK(driver->get_unique_id() == "SCOPEDOME_DOME_4");
}

TEST_CASE("ScopeDome Dome Driver - Frame encoding and CRC", "[scopedome][dome][unit]") {
    // Dallas/Maxim CRC-8 check value for "123456789" is 0xA1.
    std::uint8_t crc = 0;
    for (char c : std::string("123456789")) crc = sd::scopedome_crc8(crc, static_cast<std::uint8_t>(c));
    CHECK(crc == 0xA1);

    const auto frame = sd::encode_scopedome_frame(Cmd::CwRotation, std::string("\x10\x02", 2));
    REQUIRE(frame.size() == 6);
    CHECK(static_cast<std::uint8_t>(frame[0]) == 0xAA);
    CHECK(static_cast<std::uint8_t>(frame[1]) == 2);
    CHECK(static_cast<std::uint8_t>(frame[2]) == 30);
    sd::ScopeDomeFrame decoded;
    REQUIRE(sd::decode_scopedome_frame(frame, decoded));
    CHECK(decoded.command == 30);
    CHECK(decoded.payload == std::string("\x10\x02", 2));

    std::string bad = frame;
    bad[3] = static_cast<char>(bad[3] ^ 1);
    CHECK_FALSE(sd::decode_scopedome_frame(bad, decoded));
    CHECK_FALSE(sd::decode_scopedome_frame(frame.substr(0, 5), decoded));

    sd::ScopeDomeCardState s;
    s.digital[1] = 0x20;  // bit 13
    CHECK(s.input(Ch::InHome));
    CHECK_FALSE(s.input(Ch::InOpen1));
    s.digital[4] = 0x08;  // bit 35
    CHECK(s.input(Ch::InRotaryLink));
}

TEST_CASE("ScopeDome Dome Driver - connect reads the card and stops the shutter outputs",
          "[scopedome][dome][unit]") {
    Rig rig;
    REQUIRE(rig.connect());
    CHECK(rig.driver->get_connected());
    CHECK(rig.card.count(Cmd::ConnectionTest) >= 1);
    CHECK(rig.card.count(Cmd::StopSafeCommunication) == 1);
    CHECK(rig.card.count(Cmd::GetImpPerTurn) == 1);
    CHECK(rig.card.count(Cmd::ClearDigitalChannel) >= 2);  // OPEN1 and CLOSE1 off
    CHECK(rig.driver->get_device_firmware() == std::optional<std::string>("main 2.13, rotary 2.0"));
    CHECK(rig.driver->get_azimuth() == Approx(0.0));
    CHECK(rig.driver->get_shutter_status() == kClosed);
    CHECK_FALSE(rig.driver->get_slewing());
    rig.driver->set_connected(false);
    CHECK_FALSE(rig.driver->get_connected());
    CHECK(rig.driver->get_device_firmware() == std::nullopt);
}

TEST_CASE("ScopeDome Dome Driver - a port with no card fails the connect", "[scopedome][dome][unit]") {
    FakeScopeDomeCard card;
    card.set_muted(true);
    FakeTaskClock clock;
    auto driver = sd::create_scopedome_dome(0, sd::ScopeDomeDomeConfig{card.slave_path()}, clock);
    require_alpaca_error([&]() { driver->set_connected(true); }, AlpacaError::NotConnected);
    CHECK_FALSE(driver->get_connected());
    auto no_port = sd::create_scopedome_dome(0, sd::ScopeDomeDomeConfig{});
    require_alpaca_error([&]() { no_port->set_connected(true); }, AlpacaError::NotConnected);
}

TEST_CASE("ScopeDome Dome Driver - azimuth follows the counter and the home azimuth", "[scopedome][dome][unit]") {
    sd::ScopeDomeDomeConfig cfg;
    cfg.home_azimuth = 90.0;
    Rig rig(cfg);
    rig.card.set_counter(450);  // 45 deg counter-clockwise of home
    REQUIRE(rig.connect());
    CHECK(rig.driver->get_azimuth() == Approx(45.0));
    rig.card.set_counter(-900);  // 90 deg clockwise of home
    rig.tick();
    CHECK(rig.driver->get_azimuth() == Approx(180.0));
}

TEST_CASE("ScopeDome Dome Driver - SlewToAzimuth goes the short way and ends on target", "[scopedome][dome][unit]") {
    Rig rig;
    REQUIRE(rig.connect());
    rig.driver->slew_to_azimuth(300.0);  // 60 deg counter-clockwise
    CHECK(rig.driver->get_slewing());
    CHECK(rig.card.count(Cmd::CcwRotation) == 1);
    CHECK(rig.card.count(Cmd::CwRotation) == 0);
    REQUIRE(rig.settle_slew());
    CHECK(az_distance(rig.driver->get_azimuth(), 300.0) <= 1.0);
    CHECK_FALSE(rig.driver->get_at_park());
    CHECK_FALSE(rig.driver->get_at_home());

    rig.driver->slew_to_azimuth(20.0);  // 80 deg clockwise through north
    CHECK(rig.card.count(Cmd::CwRotation) == 1);
    REQUIRE(rig.settle_slew());
    CHECK(az_distance(rig.driver->get_azimuth(), 20.0) <= 1.0);
}

TEST_CASE("ScopeDome Dome Driver - an overshooting dome is refined into tolerance", "[scopedome][dome][unit]") {
    Rig rig;
    rig.card.set_overshoot(30, 1);  // the first move ends 3 deg past its target
    REQUIRE(rig.connect());
    rig.driver->slew_to_azimuth(90.0);
    REQUIRE(rig.settle_slew());
    CHECK(rig.card.count(Cmd::CwRotation) == 1);
    CHECK(rig.card.count(Cmd::CcwRotation) == 1);  // the refinement comes back
    CHECK(az_distance(rig.driver->get_azimuth(), 90.0) <= 1.0);
}

TEST_CASE("ScopeDome Dome Driver - a slew that cannot reach tolerance keeps its error", "[scopedome][dome][unit]") {
    Rig rig;
    rig.card.set_overshoot(30);  // every send, refinements included, lands 3 deg past
    REQUIRE(rig.connect());
    rig.driver->slew_to_azimuth(90.0);
    bool failed = false;
    for (int i = 0; i < 80 && !failed; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        rig.tick();
        try {
            (void)rig.driver->get_slewing();
        } catch (const alpacacore::AlpacaException& e) {
            failed = e.error_code() == AlpacaError::DriverException;
        }
    }
    REQUIRE(failed);
    CHECK(rig.card.count(Cmd::CwRotation) + rig.card.count(Cmd::CcwRotation) == 4);
    // Kept until the next command: a second read still raises it.
    require_alpaca_error([&]() { (void)rig.driver->get_slewing(); }, AlpacaError::DriverException);
    rig.card.set_overshoot(0);
    rig.driver->slew_to_azimuth(100.0);
    REQUIRE(rig.settle_slew());
}

TEST_CASE("ScopeDome Dome Driver - a slew into a running move stops it first", "[scopedome][dome][unit]") {
    Rig rig;
    rig.card.set_steps_per_second(600.0);  // slow: 60 deg/s
    REQUIRE(rig.connect());
    rig.driver->slew_to_azimuth(180.0);
    rig.tick();
    REQUIRE(rig.card.rotating());
    rig.driver->slew_to_azimuth(10.0);
    CHECK(rig.card.count(Cmd::Stop) == 1);
    REQUIRE(rig.settle_slew());
    CHECK(az_distance(rig.driver->get_azimuth(), 10.0) <= 1.0);
}

TEST_CASE("ScopeDome Dome Driver - FindHome resets the counter at the home sensor", "[scopedome][dome][unit]") {
    sd::ScopeDomeDomeConfig cfg;
    cfg.home_azimuth = 45.0;
    Rig rig(cfg);
    rig.card.set_counter(1200);
    REQUIRE(rig.connect());
    rig.driver->find_home();
    CHECK(rig.card.count(Cmd::FindHome) == 1);
    CHECK(rig.driver->get_slewing());
    REQUIRE(rig.settle([&] { return rig.driver->get_at_home(); }));
    CHECK(rig.card.count(Cmd::ResetCounter) == 1);
    CHECK(rig.card.counter() == 0);
    CHECK(rig.driver->get_azimuth() == Approx(45.0));
    CHECK_FALSE(rig.driver->get_slewing());
    rig.driver->slew_to_azimuth(100.0);
    CHECK_FALSE(rig.driver->get_at_home());  // any later movement clears AtHome
}

TEST_CASE("ScopeDome Dome Driver - Park, AtPark and SetPark", "[scopedome][dome][unit]") {
    sd::ScopeDomeDomeConfig cfg;
    cfg.park_azimuth = 270.0;
    Rig rig(cfg);
    REQUIRE(rig.connect());
    CHECK_FALSE(rig.driver->get_at_park());
    rig.driver->park();
    REQUIRE(rig.settle([&] { return rig.driver->get_at_park(); }));
    CHECK(az_distance(rig.driver->get_azimuth(), 270.0) <= 1.0);
    const int sends = rig.card.count(Cmd::CwRotation) + rig.card.count(Cmd::CcwRotation);
    rig.driver->park();  // parked: a no-op
    CHECK(rig.card.count(Cmd::CwRotation) + rig.card.count(Cmd::CcwRotation) == sends);

    rig.driver->slew_to_azimuth(120.0);
    CHECK_FALSE(rig.driver->get_at_park());
    REQUIRE(rig.settle_slew());
    rig.driver->set_park();
    rig.driver->slew_to_azimuth(200.0);
    REQUIRE(rig.settle_slew());
    rig.driver->park();
    REQUIRE(rig.settle([&] { return rig.driver->get_at_park(); }));
    CHECK(az_distance(rig.driver->get_azimuth(), 120.0) <= 1.0);
}

TEST_CASE("ScopeDome Dome Driver - the shutter opens and closes on its limit switches", "[scopedome][dome][unit]") {
    Rig rig;
    REQUIRE(rig.connect());
    rig.driver->open_shutter();
    CHECK(rig.card.output(Ch::OutOpen1));
    CHECK_FALSE(rig.card.output(Ch::OutClose1));
    CHECK(rig.driver->get_shutter_status() == kOpening);
    CHECK(rig.driver->get_slewing());  // "any part of the dome is moving"
    REQUIRE(rig.settle([&] { return rig.driver->get_shutter_status() == kOpen; }));
    CHECK_FALSE(rig.card.output(Ch::OutOpen1));  // the driver switched the motor off at the limit
    CHECK_FALSE(rig.driver->get_slewing());

    rig.driver->open_shutter();  // already open: a no-op
    CHECK_FALSE(rig.card.output(Ch::OutOpen1));

    rig.driver->close_shutter();
    CHECK(rig.card.output(Ch::OutClose1));
    CHECK(rig.driver->get_shutter_status() == kClosing);
    REQUIRE(rig.settle([&] { return rig.driver->get_shutter_status() == kClosed; }));
    CHECK_FALSE(rig.card.output(Ch::OutClose1));
}

TEST_CASE("ScopeDome Dome Driver - a jammed shutter times out to Error and stops its motor",
          "[scopedome][dome][unit]") {
    sd::ScopeDomeDomeConfig cfg;
    cfg.shutter_timeout_s = 3.0;
    Rig rig(cfg);
    rig.card.set_shutter_jammed(true);
    REQUIRE(rig.connect());
    rig.driver->open_shutter();
    REQUIRE(rig.settle([&] { return rig.driver->get_shutter_status() == kError; }, 20));
    CHECK_FALSE(rig.card.output(Ch::OutOpen1));
    rig.tick(2);
    CHECK(rig.driver->get_shutter_status() == kError);  // kept until the next command
    rig.card.set_shutter_jammed(false);
    rig.driver->close_shutter();  // the shutter is still at its closed limit: nothing to do
    CHECK(rig.driver->get_shutter_status() == kClosed);
}

TEST_CASE("ScopeDome Dome Driver - no shutter link refuses the shutter", "[scopedome][dome][unit]") {
    Rig rig;
    rig.card.set_rotary_link(false);
    REQUIRE(rig.connect());
    require_alpaca_error([&]() { rig.driver->open_shutter(); }, AlpacaError::DriverException);
    CHECK_FALSE(rig.card.output(Ch::OutOpen1));
    CHECK(rig.driver->get_shutter_status() == kClosed);
}

TEST_CASE("ScopeDome Dome Driver - AbortSlew stops rotation and the shutter", "[scopedome][dome][unit]") {
    Rig rig;
    rig.card.set_steps_per_second(300.0);
    rig.card.set_shutter_travel_time(std::chrono::milliseconds(5000));
    REQUIRE(rig.connect());
    rig.driver->slew_to_azimuth(180.0);
    rig.driver->open_shutter();
    rig.tick();
    rig.driver->abort_slew();
    CHECK(rig.card.count(Cmd::Stop) == 1);
    CHECK_FALSE(rig.card.rotating());
    CHECK_FALSE(rig.card.output(Ch::OutOpen1));
    rig.tick();
    CHECK_FALSE(rig.driver->get_slewing());
    CHECK(rig.driver->get_shutter_status() == kError);  // part-way: neither limit switch
}

TEST_CASE("ScopeDome Dome Driver - a disconnect during a shutter move stops the motor", "[scopedome][dome][unit]") {
    Rig rig;
    rig.card.set_shutter_travel_time(std::chrono::milliseconds(5000));
    REQUIRE(rig.connect());
    rig.driver->open_shutter();
    REQUIRE(rig.card.output(Ch::OutOpen1));
    rig.driver->set_connected(false);
    CHECK_FALSE(rig.card.output(Ch::OutOpen1));
}

TEST_CASE("ScopeDome Dome Driver - a noisy line is retried, an unplug is a lost link", "[scopedome][dome][unit]") {
    Rig rig;
    REQUIRE(rig.connect());
    rig.card.set_corrupt_replies(true);
    rig.tick();
    CHECK(rig.driver->get_azimuth() == Approx(0.0));  // one failed poll serves the last frame
    rig.tick(2);                                       // three in a row fault the poll
    require_alpaca_error([&]() { (void)rig.driver->get_azimuth(); }, AlpacaError::DriverException);
    rig.card.set_corrupt_replies(false);
    rig.tick();
    CHECK(rig.driver->get_azimuth() == Approx(0.0));

    rig.card.unplug();
    // The poll ends once it sees the loss: advance the clock without waiting for it to park again.
    for (int i = 0; i < 100 && rig.driver->get_connected(); ++i) {
        rig.clock.advance(kPeriod);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK_FALSE(rig.driver->get_connected());
    CHECK_FALSE(rig.driver->get_link_fault().empty());
    require_alpaca_error([&]() { (void)rig.driver->get_azimuth(); }, AlpacaError::NotConnected);
}

TEST_CASE("ScopeDome Dome Driver - value range validation", "[scopedome][dome][unit]") {
    Rig rig;
    // InvalidValue precedes the connection check.
    require_alpaca_error([&]() { rig.driver->slew_to_azimuth(-1.0); }, AlpacaError::InvalidValue);
    require_alpaca_error([&]() { rig.driver->slew_to_azimuth(360.0); }, AlpacaError::InvalidValue);
    require_alpaca_error([&]() { rig.driver->slew_to_azimuth(std::nan("")); }, AlpacaError::InvalidValue);
    require_alpaca_error([&]() { rig.driver->sync_to_azimuth(400.0); }, AlpacaError::InvalidValue);
    REQUIRE(rig.connect());
    require_alpaca_error([&]() { rig.driver->slew_to_azimuth(360.5); }, AlpacaError::InvalidValue);
    CHECK(rig.card.count(Cmd::CwRotation) + rig.card.count(Cmd::CcwRotation) == 0);
}

TEST_CASE("ScopeDome Dome Driver - state machine contracts", "[scopedome][dome][unit]") {
    Rig rig;
    require_alpaca_error([&]() { (void)rig.driver->get_slewing(); }, AlpacaError::NotConnected);
    require_alpaca_error([&]() { (void)rig.driver->get_shutter_status(); }, AlpacaError::NotConnected);
    REQUIRE(rig.connect());
    CHECK_FALSE(rig.driver->get_slewing());
    CHECK_FALSE(rig.driver->get_at_park());
    CHECK_FALSE(rig.driver->get_at_home());  // not found since the connect
    CHECK_FALSE(rig.driver->get_slaved());
    const auto state = rig.driver->get_device_state();
    CHECK_FALSE(state.empty());
    bool has_azimuth = false;
    for (const auto& s : state) has_azimuth = has_azimuth || s.name == "Azimuth";
    CHECK(has_azimuth);
}

TEST_CASE("ScopeDome Dome Driver - unsupported members throw not-implemented", "[scopedome][dome][unit]") {
    Rig rig;
    REQUIRE(rig.connect());
    CHECK_FALSE(rig.driver->get_can_set_altitude());
    CHECK_FALSE(rig.driver->get_can_slave());
    CHECK_FALSE(rig.driver->get_can_sync_azimuth());
    CHECK(rig.driver->get_can_set_azimuth());
    CHECK(rig.driver->get_can_set_shutter());
    CHECK(rig.driver->get_can_park());
    CHECK(rig.driver->get_can_find_home());
    require_alpaca_error([&]() { (void)rig.driver->get_altitude(); }, AlpacaError::NotImplemented);
    require_alpaca_error([&]() { rig.driver->slew_to_altitude(30.0); }, AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { rig.driver->sync_to_azimuth(30.0); }, AlpacaError::MethodNotImplemented);
    require_alpaca_error([&]() { rig.driver->set_slaved(true); }, AlpacaError::NotImplemented);
    rig.driver->set_slaved(false);
    require_alpaca_error([&]() { (void)rig.driver->action("Foo", ""); }, AlpacaError::ActionNotImplemented);
}
