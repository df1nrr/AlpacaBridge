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

// The ScopeDome schema. No vendor header: compiles in every build.

#include "../../catalog/builtin_descriptors.h"
#include "scopedome_fields.h"

namespace alpacacore::catalog {

void register_scopedome_schema(DeviceCatalog& catalog) {
    Schema schema;
    schema.key = DeviceKey{"scopedome", DeviceType::Dome};
    schema.display_name = "ScopeDome (USB Card 2.1)";
    schema.build_option = "ALPACACORE_ENABLE_SCOPEDOME";
    schema.fields = scopedome_dome_fields();
    catalog.add(std::move(schema));
}

}  // namespace alpacacore::catalog
