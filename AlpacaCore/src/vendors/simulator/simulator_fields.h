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

// Simulator catalog field declarations, shared by simulator_schema.cpp (no vendor
// header, compiled in every build) and simulator_catalog.cpp (the factory).

#include <alpacacore/catalog/device_catalog.h>

#include <vector>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

namespace alpacacore::catalog {

inline const Field<double> kSimulatorDomeSlewRate{
    .key = "slewRateDegPerSec", .default_value = 6.0, .min = 0.5, .max = 60.0};
inline const Field<double> kSimulatorDomeShutterTime{
    .key = "shutterTimeSec", .default_value = 10.0, .min = 0.5, .max = 300.0};
inline const Field<double> kSimulatorDomeHomeAzimuth{
    .key = "homeAzimuth", .default_value = 0.0, .min = 0.0, .max = 359.99};
inline const Field<double> kSimulatorDomeParkAzimuth{
    .key = "parkAzimuth", .default_value = 180.0, .min = 0.0, .max = 359.99};

inline const std::vector<FieldRef>& simulator_dome_fields() {
    static const std::vector<FieldRef> fields{kSimulatorDomeSlewRate.ref(), kSimulatorDomeShutterTime.ref(),
                                              kSimulatorDomeHomeAzimuth.ref(), kSimulatorDomeParkAzimuth.ref()};
    return fields;
}

}  // namespace alpacacore::catalog

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
