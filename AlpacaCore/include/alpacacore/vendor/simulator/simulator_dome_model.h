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

#pragma once

#include <chrono>
#include <cstdint>

namespace alpacacore::vendor::simulator {

/// Settings of a simulated dome. Rates are what a real observatory dome does in
/// round numbers: a full turn in a minute, a shutter in ten seconds.
struct DomeSimulatorConfig {
    double slew_rate_deg_per_s = 6.0;  // azimuth rotation speed
    double shutter_time_s = 10.0;      // fully closed to fully open
    double home_azimuth = 0.0;         // where FindHome ends
    double park_azimuth = 180.0;       // where Park ends; SetPark moves it
    double start_azimuth = 180.0;      // the azimuth when the simulator starts
    bool start_shutter_open = false;
};

/// ASCOM ShutterState values (IDomeV3).
enum class ShutterState : std::int32_t { Open = 0, Closed = 1, Opening = 2, Closing = 3, Error = 4 };

/**
 * @brief The motion of a simulated dome as a function of time: no thread, no I/O.
 *
 * Every member takes the time it is asked about, so the driver serves its
 * getters from the task clock and a test steps time with a FakeTaskClock. Not
 * thread-safe: the driver holds its state lock around every call.
 */
class DomeModel {
public:
    using time_point = std::chrono::steady_clock::time_point;

    explicit DomeModel(const DomeSimulatorConfig& config);

    double azimuth(time_point now) const;
    bool rotating(time_point now) const;
    ShutterState shutter(time_point now) const;
    bool shutter_moving(time_point now) const;

    /// Rotate the short way round to @p target (degrees, [0, 360)).
    void slew_to(double target, time_point now);
    /// Stop the rotation where it is now, and the shutter where it is (Error when part-way).
    void stop(time_point now);
    /// Declare the current position to be @p azimuth (stops a rotation first).
    void sync(double azimuth, time_point now);
    void open_shutter(time_point now);
    void close_shutter(time_point now);

    const DomeSimulatorConfig& config() const { return config_; }
    void set_park_azimuth(double azimuth) { config_.park_azimuth = azimuth; }

    /// [0, 360).
    static double normalize(double azimuth);

private:
    double shutter_fraction(time_point now) const;  // 0 closed .. 1 open

    DomeSimulatorConfig config_;
    // Rotation: from start_az_ at rot_t0_, by delta_ degrees (signed) at the configured rate.
    double start_az_;
    double delta_ = 0.0;
    time_point rot_t0_{};
    // Shutter: from shutter_from_ at shut_t0_ toward shutter_to_ (0 or 1); error_ after a stop part-way.
    double shutter_from_;
    double shutter_to_;
    time_point shut_t0_{};
    bool shutter_error_ = false;
};

}  // namespace alpacacore::vendor::simulator
