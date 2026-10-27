#pragma once

// Adapter identity and the config schema phi-ui renders. Data, not behaviour:
// nothing here talks to a router.

#include "phi/adapter/sdk/sidecar.h"

namespace phicore::fritz::ipc {

inline constexpr const char kPluginType[] = "fritz";

phicore::adapter::v1::Utf8String displayName();
phicore::adapter::v1::Utf8String description();
phicore::adapter::v1::Utf8String iconSvg();

phicore::adapter::v1::AdapterCapabilities capabilities();
phicore::adapter::v1::AdapterConfigSchema configSchema();

} // namespace phicore::fritz::ipc
