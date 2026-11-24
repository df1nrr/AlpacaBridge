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

// Deep Sky Dad catalog field declarations, shared by deepskydad_schema.cpp (no
// vendor header, compiled in every build) and deepskydad_catalog.cpp (the factory).

#include <alpacacore/catalog/device_catalog.h>

#include <cstdint>
#include <string>
#include <vector>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

namespace alpacacore::catalog {

inline const Field<std::string> kDeepSkyDadConnectionType{
    .key = "connectionType", .default_value = "auto", .role = Role::Discriminator};
inline const Field<std::string> kDeepSkyDadPortPath{.key = "portPath",
                                                    .default_value = "",
                                                    .role = Role::PortPath,
                                                    .applies_when = AppliesWhen{"connectionType", "serial"}};
inline const Field<std::int64_t> kDeepSkyDadPanelIndex{
    .key = "panelIndex", .default_value = 0, .role = Role::EnumerationIndex};

inline const std::vector<FieldRef>& deepskydad_covercalibrator_fields() {
    static const std::vector<FieldRef> fields{kDeepSkyDadConnectionType.ref(), kDeepSkyDadPortPath.ref(),
                                              kDeepSkyDadPanelIndex.ref()};
    return fields;
}

}  // namespace alpacacore::catalog

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
