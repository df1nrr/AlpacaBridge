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

#include <alpacacore/vendor/simulator/simulator_dome_model.h>

#include <algorithm>
#include <cmath>

namespace alpacacore::vendor::simulator {

namespace {

double seconds_between(DomeModel::time_point from, DomeModel::time_point to) {
    return std::chrono::duration<double>(to - from).count();
}

}  // namespace

double DomeModel::normalize(double azimuth) {
    double a = std::fmod(azimuth, 360.0);
    if (a < 0.0) a += 360.0;
    if (a >= 360.0) a = 0.0;  // fmod of a value just below a multiple of 360 can round up
    return a;
}

DomeModel::DomeModel(const DomeSimulatorConfig& config)
    : config_(config),
      start_az_(normalize(config.start_azimuth)),
      shutter_from_(config.start_shutter_open ? 1.0 : 0.0),
      shutter_to_(shutter_from_) {}

double DomeModel::azimuth(time_point now) const {
    if (delta_ == 0.0) return start_az_;
    const double travelled = std::min(std::fabs(delta_), config_.slew_rate_deg_per_s * seconds_between(rot_t0_, now));
    return normalize(start_az_ + (delta_ > 0.0 ? travelled : -travelled));
}

bool DomeModel::rotating(time_point now) const {
    return delta_ != 0.0 && config_.slew_rate_deg_per_s * seconds_between(rot_t0_, now) < std::fabs(delta_);
}

double DomeModel::shutter_fraction(time_point now) const {
    if (shutter_from_ == shutter_to_) return shutter_to_;
    const double step = seconds_between(shut_t0_, now) / std::max(config_.shutter_time_s, 0.001);
    return shutter_to_ > shutter_from_ ? std::min(shutter_to_, shutter_from_ + step)
                                       : std::max(shutter_to_, shutter_from_ - step);
}

bool DomeModel::shutter_moving(time_point now) const {
    const double f = shutter_fraction(now);
    return shutter_from_ != shutter_to_ && f != shutter_to_;
}

ShutterState DomeModel::shutter(time_point now) const {
    if (shutter_error_) return ShutterState::Error;
    if (shutter_moving(now)) return shutter_to_ > shutter_from_ ? ShutterState::Opening : ShutterState::Closing;
    return shutter_fraction(now) >= 1.0 ? ShutterState::Open : ShutterState::Closed;
}

void DomeModel::slew_to(double target, time_point now) {
    const double from = azimuth(now);
    double d = normalize(target) - from;
    if (d > 180.0) d -= 360.0;
    if (d < -180.0) d += 360.0;
    start_az_ = from;
    delta_ = d;
    rot_t0_ = now;
}

void DomeModel::stop(time_point now) {
    start_az_ = azimuth(now);
    delta_ = 0.0;
    if (shutter_moving(now)) {
        const double f = shutter_fraction(now);
        shutter_from_ = f;
        shutter_to_ = f;
        shutter_error_ = true;  // stopped part-way: neither open nor closed
    }
}

void DomeModel::sync(double azimuth, time_point now) {
    (void)now;
    start_az_ = normalize(azimuth);
    delta_ = 0.0;
}

void DomeModel::open_shutter(time_point now) {
    shutter_from_ = shutter_fraction(now);
    shutter_to_ = 1.0;
    shut_t0_ = now;
    shutter_error_ = false;
}

void DomeModel::close_shutter(time_point now) {
    shutter_from_ = shutter_fraction(now);
    shutter_to_ = 0.0;
    shut_t0_ = now;
    shutter_error_ = false;
}

}  // namespace alpacacore::vendor::simulator
