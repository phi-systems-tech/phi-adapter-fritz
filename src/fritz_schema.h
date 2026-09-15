#pragma once

// Adapter identity and the config schema phi-ui renders. Data, not behaviour:
// nothing here talks to a router.

#include <string>
#include <vector>

#include "phi/adapter/sdk/sidecar.h"

#include "fritz_json.h"

namespace phicore::fritz::ipc {

inline constexpr const char kPluginType[] = "fritz";

phicore::adapter::v1::Utf8String displayName();
phicore::adapter::v1::Utf8String description();
phicore::adapter::v1::Utf8String iconSvg();

phicore::adapter::v1::AdapterCapabilities capabilities();
phicore::adapter::v1::AdapterConfigSchema configSchema();

/**
 * @brief Selectable hosts for the "tracked devices" field.
 *
 * The hosts the last WLAN probe listed, with every tracked address that is no
 * longer among them appended, so a selection never silently vanishes when a
 * device drops off the router.
 */
phicore::adapter::v1::AdapterConfigOptionList buildTrackedOptions(const Json &knownHosts,
                                                                  const std::vector<std::string> &trackedMacs);

} // namespace phicore::fritz::ipc
