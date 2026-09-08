#include "fritz_probe.h"

#include <utility>

#include "phi/runtime/str.h"

#include "fritz_soap.h"
#include "fritz_tr064.h"

namespace phicore::fritz::ipc {

namespace str = phi::str;

namespace {

/// The first non-empty of `keys` in `obj`.
std::string firstOf(const Json &obj, std::initializer_list<const char *> keys)
{
    for (const char *key : keys) {
        const std::string value = str::trimmed(jsonString(obj, key));
        if (!value.empty())
            return value;
    }
    return {};
}

} // namespace

std::string ProbeTarget::endpoint() const
{
    if (host.empty())
        return {};
    const std::uint16_t effective = port > 0 ? port : kDefaultTr064Port;
    return std::string(useTls ? "https://" : "http://") + host + ":" + str::number(effective);
}

ProbeTarget probeTargetFromParams(const Json &params)
{
    const Json form = params.is_object() ? params : Json::object();
    const Json candidate = form.contains("factoryAdapter") && form.at("factoryAdapter").is_object()
        ? form.at("factoryAdapter")
        : Json::object();
    const Json meta = candidate.contains("meta") && candidate.at("meta").is_object()
        ? candidate.at("meta")
        : Json::object();

    /**
     * The form wins as a whole, not field by field. Somebody typing a name into
     * a dialog that still carries a discovered address means the address; a
     * merge that prefers `ip` over `host` would take the discovered one and
     * probe the machine the router used to be.
     */
    const auto pick = [&](std::initializer_list<const char *> keys) {
        std::string value = firstOf(form, keys);
        if (value.empty())
            value = firstOf(candidate, keys);
        if (value.empty())
            value = firstOf(meta, keys);
        return value;
    };

    ProbeTarget target;
    target.host = pick({"ip", "host"});
    target.user = pick({"user", "username"});
    target.password = pick({"password", "pw", "token"});

    target.port = normalizedPort(jsonInt(form, "tr064Port", 0));
    if (target.port == 0)
        target.port = normalizedPort(jsonInt(candidate, "tr064Port", 0));
    if (target.port == 0)
        target.port = normalizedPort(jsonInt(meta, "tr064Port", 0));

    // 0x1 is AdapterFlag::UseTls.
    const int flags = form.contains("flags") ? jsonInt(form, "flags", 0)
                                             : jsonInt(candidate, "flags", 0);
    target.useTls = (flags & 0x1) != 0;
    return target;
}

void runProbe(Tr064Session &session, const ProbeTarget &target,
              std::function<void(ProbeOutcome)> done)
{
    const std::string endpoint = target.endpoint();
    if (endpoint.empty()) {
        if (done)
            done({false, false, "Probe requires host or ip"});
        return;
    }

    session.setEndpoint(endpoint);
    session.setCredentials(target.user, target.password);

    const bool issued = session.call(kDeviceInfo, "GetInfo", {},
                                     [done = std::move(done), endpoint](Tr064Session::Reply reply) {
                                         ProbeOutcome outcome;
                                         outcome.ok = reply.ok;
                                         outcome.badCredentials = reply.unauthorized;
                                         if (!reply.ok) {
                                             outcome.error = reply.unauthorized
                                                 ? "Invalid credentials"
                                                 : (reply.error.empty()
                                                        ? "Probe failed (" + endpoint + ")"
                                                        : reply.error + " (" + endpoint + ")");
                                         }
                                         if (done)
                                             done(std::move(outcome));
                                     });
    if (!issued && done)
        done({false, false, "Another request is already in flight"});
}

} // namespace phicore::fritz::ipc
