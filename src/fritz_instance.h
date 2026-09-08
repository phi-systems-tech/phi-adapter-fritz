#pragma once

// The per-router runtime: what phi-core asks for, turned into TR-064 calls and
// answers back. The class stays private to the .cpp - the factory only needs to
// create one.

#include <memory>

#include "phi/adapter/sdk/sidecar.h"

namespace phicore::fritz::ipc {

std::unique_ptr<phicore::adapter::sdk::AdapterInstance> makeInstance();

} // namespace phicore::fritz::ipc
