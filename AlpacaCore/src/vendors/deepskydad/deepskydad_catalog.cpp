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

// The Deep Sky Dad factory, compiled only under ALPACACORE_ENABLE_DEEPSKYDAD.
// The auto-detect path scans at connect (create_*_by_index), never here (#659).

#include <alpacacore/vendor/deepskydad/deepskydad_covercalibrator_driver.h>
#include <alpacacore/vendor/deepskydad/deepskydad_flatpanel_protocol_wrapper.h>

#include <string>

#include "../../catalog/builtin_descriptors.h"
#include "deepskydad_fields.h"

namespace alpacacore::catalog {

void register_deepskydad_factory(DeviceCatalog& catalog) {
    Factory panel;
    panel.key = DeviceKey{"deepskydad", DeviceType::CoverCalibrator};
    panel.create = [](const DeviceConfig& config, int device_number) -> std::unique_ptr<AlpacaDriver> {
        if (config.get(kDeepSkyDadConnectionType) == "serial") {
            const std::string port_path = config.get(kDeepSkyDadPortPath);
            if (!port_path.empty()) {
                return vendor::deepskydad::create_deepskydad_covercalibrator(device_number, port_path);
            }
        }
        return vendor::deepskydad::create_deepskydad_covercalibrator_by_index(
            device_number, static_cast<int>(config.get(kDeepSkyDadPanelIndex)));
    };
    catalog.add(std::move(panel));

}

}  // namespace alpacacore::catalog
