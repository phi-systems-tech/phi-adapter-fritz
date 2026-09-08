#pragma once

// The "Test connection" action, shared by the factory and the instance.
//
// It lived only in the instance, while `capabilities()` declared it a factory
// action - and the factory never implemented the hook, so the SDK answered
// "Factory action handler not implemented". Factory scope is exactly when the
// button is pressed: while the router is being added, before an instance
// exists.

#include <cstdint>
#include <functional>
#include <string>

#include "fritz_json.h"
#include "fritz_session.h"

namespace phicore::fritz::ipc {

struct ProbeTarget {
    std::string host;
    std::uint16_t port = 0;
    std::string user;
    std::string password;
    bool useTls = false;

    [[nodiscard]] std::string endpoint() const;
};

/**
 * @brief Reads the target out of an action's params.
 *
 * Form values arrive at the top level and override anything the discovered
 * candidate under `factoryAdapter` carries, because the form is what the
 * person in front of the dialog just typed.
 */
ProbeTarget probeTargetFromParams(const Json &params);

struct ProbeOutcome {
    bool ok = false;
    /// Reached and refused. A different sentence to a person than "no route".
    bool badCredentials = false;
    std::string error;
};

/// One DeviceInfo GetInfo against the target. The session is reconfigured for
/// it, so a caller that shares one with a running poll must expect that.
void runProbe(Tr064Session &session,
              const ProbeTarget &target,
              std::function<void(ProbeOutcome)> done);

} // namespace phicore::fritz::ipc
