#include "fritz_schema.h"

#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "phi/runtime/str.h"

#include "fritz_tr064.h"

namespace phicore::fritz::ipc {

namespace str = phi::str;
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
    factory.setFields({host, port, user, password, poll, retry});

    // One list that can grow long: above its full width, the probe below it.
    v1::AdapterConfigSection &instance = schema.instance();
    instance.setTitle("FRITZ!Box");
    instance.setDescription("Connect via TR-064 to track network clients.");
    v1::AdapterConfigField tracked = field("trackedMacs", Type::Select, "Tracked devices", {});
    tracked.setParentActionId("settings");
    tracked.setFlags(Flag::Multi | Flag::InstanceOnly);
    tracked.addAction("browseHosts", "Probe WLAN");
    tracked.layout().setLabelPosition(v1::AdapterConfigLabelPosition::Top);
    tracked.layout().setActionPosition(v1::AdapterConfigActionPosition::Below);
    instance.setFields({tracked});
    return schema;
}

} // namespace

v1::AdapterConfigOptionList buildTrackedOptions(const Json &knownHosts, const std::vector<std::string> &trackedMacs)
{
    v1::AdapterConfigOptionList options;
    std::set<std::string> seen;

    const auto add = [&options, &seen](const std::string &mac, const std::string &label) {
        if (mac.empty() || seen.count(mac))
            return;
        seen.insert(mac);
        options.push_back({mac, label.empty() ? mac : label});
    };

    if (knownHosts.is_array()) {
        for (const Json &entry : knownHosts) {
            if (entry.is_string()) {
                const std::string mac = normalizeMac(entry.get<std::string>());
                add(mac, mac);
                continue;
            }
            if (!entry.is_object())
                continue;
            const std::string mac = normalizeMac(jsonString(entry, "mac"));
            const std::string name = str::trimmed(jsonString(entry, "name"));
            const std::string ip = str::trimmed(jsonString(entry, "ip"));
            std::string label;
            if (!ip.empty() && !name.empty())
                label = name + " (" + ip + ")";
            else if (!ip.empty())
                label = ip;
            else if (!name.empty())
                label = name;
            add(mac, label);
        }
    }

    // A tracked address the router no longer lists still has to be selectable,
    // or the selection appears to have cleared itself.
    for (const std::string &mac : trackedMacs)
        add(normalizeMac(mac), {});

    return options;
}

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
    caps.setFlags(v1::AdapterCapability::SupportsDiscovery
        | v1::AdapterCapability::SupportsProbe
        | v1::AdapterCapability::RequiresPolling);

    v1::AdapterActionDescriptor browse("browseHosts", "Probe WLAN", "Fetch current WLAN/LAN clients");
    browse.setPlacement(v1::AdapterActionPlacement::Field);
    caps.addInstanceAction(browse);

    v1::AdapterActionDescriptor settings("settings", "Settings", "Edit tracked devices.");
    settings.setHasForm(true);
    settings.setLoadFormOnOpen(true);
    caps.addInstanceAction(settings);

    v1::AdapterActionDescriptor probe("probe", "Test connection", "Reachability and credentials check");
    caps.addFactoryAction(probe);

    return caps;
}

v1::AdapterConfigSchema configSchema()
{
    return buildFritzConfigSchema();
}

} // namespace phicore::fritz::ipc
