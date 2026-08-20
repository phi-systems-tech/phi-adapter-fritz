#include "fritz_schema.h"

#include <QJsonValue>
#include <QSet>
#include <QString>

#include "fritz_runtime_convert.h"
#include "fritz_tr064.h"

namespace phicore::fritz::ipc {

namespace v1 = phicore::adapter::v1;

namespace {

constexpr const char kFritzIconSvg[] =
    "<svg width=\"24\" height=\"24\" viewBox=\"0 0 24 24\" xmlns=\"http://www.w3.org/2000/svg\" role=\"img\" aria-label=\"FRITZ!Box logo\">"
    "<rect x=\"4\" y=\"4\" width=\"16\" height=\"16\" rx=\"2\" fill=\"#FFD84D\" transform=\"rotate(45 12 12)\"/>"
    "<text x=\"12\" y=\"15\" text-anchor=\"middle\" font-family=\"'Geist', 'Inter', 'Arial', sans-serif\" font-weight=\"700\" font-size=\"8.5\" fill=\"#D94A4A\">FRITZ!</text>"
    "</svg>";

QJsonObject buildFritzConfigSchemaObject()
{
    auto field = [](const QString &key,
                    const QString &type,
                    const QString &label,
                    const QJsonValue &defaultValue = QJsonValue(),
                    const QString &actionId = QString(),
                    const QString &actionLabel = QString(),
                    const QString &parentActionId = QString(),
                    const QJsonArray &flags = QJsonArray(),
                    const QJsonArray &choices = QJsonArray(),
                    const QJsonObject &layout = QJsonObject()) {
        QJsonObject obj;
        obj.insert(QStringLiteral("key"), key);
        obj.insert(QStringLiteral("type"), type);
        obj.insert(QStringLiteral("label"), label);
        if (!defaultValue.isUndefined() && !defaultValue.isNull())
            obj.insert(QStringLiteral("default"), defaultValue);
        if (!actionId.isEmpty())
            obj.insert(QStringLiteral("actionId"), actionId);
        if (!actionLabel.isEmpty())
            obj.insert(QStringLiteral("actionLabel"), actionLabel);
        if (!parentActionId.isEmpty())
            obj.insert(QStringLiteral("parentActionId"), parentActionId);
        if (!flags.isEmpty())
            obj.insert(QStringLiteral("flags"), flags);
        if (!choices.isEmpty())
            obj.insert(QStringLiteral("choices"), choices);
        if (!layout.isEmpty())
            obj.insert(QStringLiteral("layout"), layout);
        return obj;
    };

    QJsonArray factoryFields;
    factoryFields.append(field(QStringLiteral("host"),
                              QStringLiteral("Hostname"),
                              QStringLiteral("Host"),
                              QJsonValue(),
                              QString(),
                              QString(),
                              QString(),
                              QJsonArray{QStringLiteral("Required")}));
    factoryFields.append(field(QStringLiteral("tr064Port"),
                              QStringLiteral("Integer"),
                              QStringLiteral("TR-064 port"),
                              static_cast<int>(kDefaultTr064Port)));
    factoryFields.append(field(QStringLiteral("user"),
                              QStringLiteral("String"),
                              QStringLiteral("Username"),
                              QJsonValue(),
                              QString(),
                              QString(),
                              QString(),
                              QJsonArray{QStringLiteral("Required")}));
    factoryFields.append(field(QStringLiteral("password"),
                              QStringLiteral("Password"),
                              QStringLiteral("Password"),
                              QJsonValue(),
                              QString(),
                              QString(),
                              QString(),
                              QJsonArray{QStringLiteral("Required"), QStringLiteral("Secret")}));
    factoryFields.append(field(QStringLiteral("pollIntervalMs"),
                              QStringLiteral("Integer"),
                              QStringLiteral("Poll interval"),
                              5000));
    factoryFields.append(field(QStringLiteral("retryIntervalMs"),
                              QStringLiteral("Integer"),
                              QStringLiteral("Retry interval"),
                              10000));

    QJsonArray instanceFields;
    instanceFields.append(field(QStringLiteral("trackedMacs"),
                               QStringLiteral("Select"),
                               QStringLiteral("Tracked devices"),
                               QJsonArray(),
                               QStringLiteral("browseHosts"),
                               QStringLiteral("Probe WLAN"),
                               QStringLiteral("settings"),
                               QJsonArray{QStringLiteral("Multi"), QStringLiteral("InstanceOnly")},
                               QJsonArray(),
                               QJsonObject{
                                   {QStringLiteral("labelPosition"), QStringLiteral("top")},
                                   {QStringLiteral("actionPosition"), QStringLiteral("below")},
                               }));

    QJsonObject factorySection;
    factorySection.insert(QStringLiteral("title"), QStringLiteral("FRITZ!Box"));
    factorySection.insert(QStringLiteral("description"),
                         QStringLiteral("Connect via TR-064 to track network clients."));
    factorySection.insert(QStringLiteral("fields"), factoryFields);

    QJsonObject instanceSection;
    instanceSection.insert(QStringLiteral("title"), QStringLiteral("FRITZ!Box"));
    instanceSection.insert(QStringLiteral("description"),
                          QStringLiteral("Connect via TR-064 to track network clients."));
    instanceSection.insert(QStringLiteral("fields"), instanceFields);

    QJsonObject schema;
    schema.insert(QStringLiteral("factory"), factorySection);
    schema.insert(QStringLiteral("instance"), instanceSection);
    return schema;
}

} // namespace

v1::AdapterConfigOptionList buildTrackedOptions(const QJsonObject &meta)
{
    v1::AdapterConfigOptionList options;
    QSet<QString> seen;

    const QJsonValue knownValue = meta.value(QStringLiteral("knownHosts"));
    if (knownValue.isArray()) {
        const QJsonArray arr = knownValue.toArray();
        for (const QJsonValue &entry : arr) {
            if (entry.isString()) {
                const QString mac = normalizeMac(entry.toString());
                if (mac.isEmpty() || seen.contains(mac))
                    continue;
                seen.insert(mac);
                options.push_back({mac.toStdString(), mac.toStdString()});
                continue;
            }
            if (!entry.isObject())
                continue;
            const QJsonObject obj = entry.toObject();
            const QString mac = normalizeMac(obj.value(QStringLiteral("mac")).toString());
            if (mac.isEmpty() || seen.contains(mac))
                continue;
            seen.insert(mac);

            const QString name = obj.value(QStringLiteral("name")).toString().trimmed();
            const QString ip = obj.value(QStringLiteral("ip")).toString().trimmed();

            QString label;
            if (!ip.isEmpty() && !name.isEmpty()) {
                label = QStringLiteral("%1 (%2)").arg(name, ip);
            } else if (!ip.isEmpty()) {
                label = ip;
            } else if (!name.isEmpty()) {
                label = name;
            } else {
                label = mac;
            }
            options.push_back({mac.toStdString(), label.toStdString()});
        }
    }

    const QSet<QString> trackedMacs = parseTrackedMacSelection(meta.value(QStringLiteral("trackedMacs")));
    for (const QString &mac : trackedMacs) {
        if (mac.isEmpty() || seen.contains(mac))
            continue;
        seen.insert(mac);
        options.push_back({mac.toStdString(), mac.toStdString()});
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
    return toJson(buildFritzConfigSchemaObject());
}

} // namespace phicore::fritz::ipc
