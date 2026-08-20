#pragma once

// The per-router runtime: TR-064 polling, the host table it turns into devices
// and the router's own channels. The class stays private to the .cpp - the
// factory only needs to create one - so the lifecycle is not part of any other
// translation unit.

#include <memory>

#include "phi/adapter/sdk/sidecar.h"

namespace phicore::fritz::ipc {

std::unique_ptr<phicore::adapter::sdk::AdapterInstance> makeInstance();

} // namespace phicore::fritz::ipc
