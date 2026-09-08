#include "fritz_schema.h"

#include <algorithm>
#include <set>
#include <string>
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

struct FieldSpec {
    std::string key;
    std::string type;
    std::string label;
    Json defaultValue = Json();
    std::string actionId;
    std::string actionLabel;
    std::string parentActionId;
    Json flags = Json::array();
    Json choices = Json::array();
    Json layout = Json::object();
};

Json field(const FieldSpec &spec)
{
    Json obj = Json::object();
    obj["key"] = spec.key;
    obj["type"] = spec.type;
    obj["label"] = spec.label;
    if (!spec.defaultValue.is_null())
        obj["default"] = spec.defaultValue;
    if (!spec.actionId.empty())
        obj["actionId"] = spec.actionId;
    if (!spec.actionLabel.empty())
        obj["actionLabel"] = spec.actionLabel;
    if (!spec.parentActionId.empty())
        obj["parentActionId"] = spec.parentActionId;
    if (!spec.flags.empty())
        obj["flags"] = spec.flags;
    if (!spec.choices.empty())
        obj["choices"] = spec.choices;
    if (!spec.layout.empty())
        obj["layout"] = spec.layout;
    return obj;
}

Json buildFritzConfigSchemaObject()
{
    Json factoryFields = Json::array();
    factoryFields.push_back(field({.key = "host", .type = "Hostname", .label = "Host",
                                   .flags = Json::array({"Required"})}));
    factoryFields.push_back(field({.key = "tr064Port", .type = "Integer",
                                   .label = "TR-064 port",
                                   .defaultValue = static_cast<int>(kDefaultTr064Port)}));
    factoryFields.push_back(field({.key = "user", .type = "String", .label = "Username",
                                   .flags = Json::array({"Required"})}));
    factoryFields.push_back(field({.key = "password", .type = "Password", .label = "Password",
                                   .flags = Json::array({"Required", "Secret"})}));
    factoryFields.push_back(field({.key = "pollIntervalMs", .type = "Integer",
                                   .label = "Poll interval", .defaultValue = 5000}));
    factoryFields.push_back(field({.key = "retryIntervalMs", .type = "Integer",
                                   .label = "Retry interval", .defaultValue = 10000}));

    Json instanceFields = Json::array();
    instanceFields.push_back(field({.key = "trackedMacs", .type = "Select",
                                    .label = "Tracked devices",
                                    .defaultValue = Json::array(),
                                    .actionId = "browseHosts",
                                    .actionLabel = "Probe WLAN",
                                    .parentActionId = "settings",
                                    .flags = Json::array({"Multi", "InstanceOnly"}),
                                    .layout = Json{{"labelPosition", "top"},
                                                   {"actionPosition", "below"}}}));

    Json factorySection = Json::object();
    factorySection["title"] = "FRITZ!Box";
    factorySection["description"] = "Connect via TR-064 to track network clients.";
    factorySection["fields"] = factoryFields;

    Json instanceSection = Json::object();
    instanceSection["title"] = "FRITZ!Box";
    instanceSection["description"] = "Connect via TR-064 to track network clients.";
    instanceSection["fields"] = instanceFields;

    Json schema = Json::object();
    schema["factory"] = factorySection;
    schema["instance"] = instanceSection;
    return schema;
}

} // namespace

v1::AdapterConfigOptionList buildTrackedOptions(const Json &meta)
{
    v1::AdapterConfigOptionList options;
    std::set<std::string> seen;

    const auto add = [&options, &seen](const std::string &mac, const std::string &label) {
        if (mac.empty() || seen.count(mac))
            return;
        seen.insert(mac);
        options.push_back({mac, label.empty() ? mac : label});
    };

    if (meta.is_object() && meta.contains("knownHosts") && meta.at("knownHosts").is_array()) {
        for (const Json &entry : meta.at("knownHosts")) {
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
    if (meta.is_object() && meta.contains("trackedMacs")) {
        const Json &tracked = meta.at("trackedMacs");
        if (tracked.is_array()) {
            for (const Json &entry : tracked) {
                if (entry.is_string())
                    add(normalizeMac(entry.get<std::string>()), {});
                else if (entry.is_object())
                    add(normalizeMac(jsonString(entry, "mac")), {});
            }
        } else if (tracked.is_string()) {
            add(normalizeMac(tracked.get<std::string>()), {});
        }
    }

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
    caps.required = v1::AdapterRequirement::UsesRetryInterval;
    caps.flags = v1::AdapterFlag::SupportsDiscovery
        | v1::AdapterFlag::SupportsProbe
        | v1::AdapterFlag::RequiresPolling;
    caps.defaultsJson = R"({"tr064Port":49000,"pollIntervalMs":5000,"retryIntervalMs":10000})";

    v1::AdapterActionDescriptor browse;
    browse.id = "browseHosts";
    browse.label = "Probe WLAN";
    browse.description = "Fetch current WLAN/LAN clients";
    browse.metaJson = R"({"placement":"form_field","kind":"command","requiresAck":true})";
    caps.instanceActions.push_back(browse);

    v1::AdapterActionDescriptor settings;
    settings.id = "settings";
    settings.label = "Settings";
    settings.description = "Edit tracked devices.";
    settings.hasForm = true;
    settings.metaJson = R"({"placement":"card","kind":"open_dialog","requiresAck":true})";
    caps.instanceActions.push_back(settings);

    v1::AdapterActionDescriptor probe;
    probe.id = "probe";
    probe.label = "Test connection";
    probe.description = "Reachability and credentials check";
    probe.metaJson = R"({"placement":"card","kind":"command","requiresAck":true})";
    caps.factoryActions.push_back(probe);

    return caps;
}

v1::JsonText configSchemaJson()
{
    return dump(buildFritzConfigSchemaObject());
}

} // namespace phicore::fritz::ipc
