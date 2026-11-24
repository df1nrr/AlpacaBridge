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

// ScopeDome catalog field declarations, shared by scopedome_schema.cpp (no vendor
// header, compiled in every build) and scopedome_catalog.cpp (the factory).

#include <alpacacore/catalog/device_catalog.h>

#include <cstdint>
#include <string>
#include <vector>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

namespace alpacacore::catalog {

inline const Field<std::string> kScopeDomePortPath{.key = "portPath", .default_value = "", .role = Role::PortPath};
inline const Field<double> kScopeDomeHomeAzimuth{
    .key = "homeAzimuth", .default_value = 0.0, .min = 0.0, .max = 359.99};
inline const Field<double> kScopeDomeParkAzimuth{
    .key = "parkAzimuth", .default_value = 0.0, .min = 0.0, .max = 359.99};
inline const Field<double> kScopeDomeAzimuthTolerance{
    .key = "azimuthTolerance", .default_value = 1.0, .min = 0.1, .max = 10.0};
inline const Field<double> kScopeDomeShutterTimeout{
    .key = "shutterTimeoutSec", .default_value = 180.0, .min = 10.0, .max = 600.0};

inline const std::vector<FieldRef>& scopedome_dome_fields() {
    static const std::vector<FieldRef> fields{kScopeDomePortPath.ref(), kScopeDomeHomeAzimuth.ref(),
                                              kScopeDomeParkAzimuth.ref(), kScopeDomeAzimuthTolerance.ref(),
                                              kScopeDomeShutterTimeout.ref()};
    return fields;
}

}  // namespace alpacacore::catalog

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
