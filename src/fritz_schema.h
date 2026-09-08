#pragma once

// Adapter identity and the config schema phi-ui renders. Data, not behaviour:
// nothing here talks to a router.

#include <string>

#include "phi/adapter/sdk/sidecar.h"

#include "fritz_json.h"

namespace phicore::fritz::ipc {

inline constexpr const char kPluginType[] = "fritz";

phicore::adapter::v1::Utf8String displayName();
phicore::adapter::v1::Utf8String description();
phicore::adapter::v1::Utf8String iconSvg();

phicore::adapter::v1::AdapterCapabilities capabilities();
phicore::adapter::v1::JsonText configSchemaJson();

/**
 * @brief Selectable hosts for the "tracked devices" field.
 *
 * Built from the instance meta's `knownHosts`, with any `trackedMacs` that are
 * no longer in the host list appended, so a selection never silently vanishes
 * when a device drops off the router.
 */
phicore::adapter::v1::AdapterConfigOptionList buildTrackedOptions(const Json &meta);

} // namespace phicore::fritz::ipc
