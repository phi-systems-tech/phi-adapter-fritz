#include "fritz_schema.h"

#include <utility>

#include "phi/adapter/sdk/network_clients.h"

#include "fritz_tr064.h"

namespace phicore::fritz::ipc {

namespace sdk = phicore::adapter::sdk;
namespace v1 = phicore::adapter::v1;

namespace {

constexpr const char kFritzIconSvg[] =
    "<svg width=\"24\" height=\"24\" viewBox=\"0 0 24 24\" xmlns=\"http://www.w3.org/2000/svg\" role=\"img\" aria-label=\"FRITZ!Box logo\">"
    "<rect x=\"4\" y=\"4\" width=\"16\" height=\"16\" rx=\"2\" fill=\"#FFD84D\" transform=\"rotate(45 12 12)\"/>"
    "<text x=\"12\" y=\"15\" text-anchor=\"middle\" font-family=\"'Geist', 'Inter', 'Arial', sans-serif\" font-weight=\"700\" font-size=\"8.5\" fill=\"#D94A4A\">FRITZ!</text>"
    "</svg>";

v1::AdapterConfigField field(const char *key, v1::AdapterConfigFieldType type, const char *label,
                             v1::ScalarValue defaultValue = {})
{
    v1::AdapterConfigField out(key, type, label);
    out.setDefaultValue(std::move(defaultValue));
    return out;
}

v1::AdapterConfigSchema buildFritzConfigSchema()
{
    using Type = v1::AdapterConfigFieldType;
    using Flag = v1::AdapterConfigFieldFlag;
    v1::AdapterConfigSchema schema;

    // Two columns: the address beside its port, the account beside its
    // password, the two intervals short.
    v1::AdapterConfigSection &factory = schema.factory();
    factory.setTitle("FRITZ!Box");
    factory.setDescription("Connect via TR-064 to track network clients.");
    factory.layout().setColumns(2);
    v1::AdapterConfigField host = field("host", Type::Hostname, "Host");
    host.setFlags(Flag::Required);
    v1::AdapterConfigField port = field("tr064Port", Type::Integer, "TR-064 port", std::int64_t{kDefaultTr064Port});
    port.setFlags(Flag::Required);
    port.layout().setControlWidth(v1::AdapterConfigSize::Narrow);
    v1::AdapterConfigField user = field("user", Type::String, "Username");
    user.setFlags(Flag::Required);
    v1::AdapterConfigField password = field("password", Type::Password, "Password");
    password.setFlags(Flag::Required | Flag::Secret);
    v1::AdapterConfigField poll = field("pollIntervalMs", Type::Integer, "Poll interval", std::int64_t{5000});
    poll.layout().setControlWidth(v1::AdapterConfigSize::Narrow);
    v1::AdapterConfigField retry = field("retryIntervalMs", Type::Integer, "Retry interval", std::int64_t{10000});
    retry.layout().setControlWidth(v1::AdapterConfigSize::Narrow);
    v1::AdapterConfigFieldList factoryFields{host, port, user, password, poll, retry};
    for (v1::AdapterConfigField &clientField : sdk::ClientTracker::configFields())
        factoryFields.push_back(std::move(clientField));
    factory.setFields(std::move(factoryFields));

    // Which clients are tracked is picked under Add device (pairing style
    // Listing), not here; what is left is how they are tracked.
    v1::AdapterConfigSection &instance = schema.instance();
    instance.setTitle("FRITZ!Box");
    instance.setDescription("Connect via TR-064 to track network clients. Clients are added under "
                            "Add device, Network clients.");
    instance.setFields(sdk::ClientTracker::configFields());
    return schema;
}

} // namespace

v1::Utf8String displayName()
{
    return "FRITZ!Box";
}

v1::Utf8String description()
{
    return "AVM FRITZ!Box via TR-064 (IPC sidecar)";
}

v1::Utf8String iconSvg()
{
    return kFritzIconSvg;
}

v1::AdapterCapabilities capabilities()
{
    v1::AdapterCapabilities caps;
    caps.setRequired(v1::AdapterRequirement::UsesRetryInterval);
    caps.setTopologies(v1::Topology::Local);
    caps.setFlags(v1::AdapterCapability::SupportsDiscovery
        | v1::AdapterCapability::SupportsProbe
        | v1::AdapterCapability::RequiresPolling
        | v1::AdapterCapability::NetworkClients);
    // The router's host table is the candidate list; nothing opens.
    caps.setPairingStyles(v1::PairingStyle::Listing);

    v1::AdapterActionDescriptor probe("probe", "Test connection", "Reachability and credentials check");
    caps.addFactoryAction(probe);

    return caps;
}

v1::AdapterConfigSchema configSchema()
{
    return buildFritzConfigSchema();
}

} // namespace phicore::fritz::ipc
