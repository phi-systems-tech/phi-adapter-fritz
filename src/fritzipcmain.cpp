#include <atomic>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <QAuthenticator>
#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QXmlStreamReader>

#include "phi/adapter/sdk/sidecar.h"

namespace v1 = phicore::adapter::v1;
namespace sdk = phicore::adapter::sdk;

namespace {

constexpr const char kPluginType[] = "fritz!";
constexpr const char kRouterDeviceId[] = "router";
constexpr const char kDeviceSoftwareUpdateChannelId[] = "device_software_update";
constexpr const char kHostsServiceType[] = "urn:dslforum-org:service:Hosts:1";
constexpr const char kHostsControlPath[] = "/upnp/control/hosts";
constexpr const char kDeviceInfoServiceType[] = "urn:dslforum-org:service:DeviceInfo:1";
constexpr const char kDeviceInfoControlPath[] = "/upnp/control/deviceinfo";
constexpr const char kWanCommonServiceType[] = "urn:dslforum-org:service:WANCommonInterfaceConfig:1";
constexpr const char kWanCommonControlPath[] = "/upnp/control/wancommonifconfig";
constexpr const char kWlan24ServiceType[] = "urn:dslforum-org:service:WLANConfiguration:1";
constexpr const char kWlan24ControlPath[] = "/upnp/control/wlanconfig1";
constexpr const char kWlan5ServiceType[] = "urn:dslforum-org:service:WLANConfiguration:2";
constexpr const char kWlan5ControlPath[] = "/upnp/control/wlanconfig2";

constexpr const char kFritzIconSvg[] =
    "<svg width=\"24\" height=\"24\" viewBox=\"0 0 24 24\" xmlns=\"http://www.w3.org/2000/svg\" role=\"img\" aria-label=\"FRITZ!Box logo\">"
    "<rect x=\"4\" y=\"4\" width=\"16\" height=\"16\" rx=\"2\" fill=\"#FFD84D\" transform=\"rotate(45 12 12)\"/>"
    "<text x=\"12\" y=\"15\" text-anchor=\"middle\" font-family=\"'Geist', 'Inter', 'Arial', sans-serif\" font-weight=\"700\" font-size=\"8.5\" fill=\"#D94A4A\">FRITZ!</text>"
    "</svg>";

std::atomic_bool g_running{true};

void handleSignal(int)
{
    g_running.store(false);
}

std::int64_t nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

QJsonObject parseJsonObject(const std::string &text)
{
    if (text.empty())
        return {};
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(text), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return {};
    return doc.object();
}

std::string toJson(const QJsonObject &obj)
{
    return QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString();
}

QString normalizeMac(const QString &mac)
{
    return mac.trimmed().toLower();
}

QSet<QString> parseTrackedMacSelection(const QJsonValue &value)
{
    QSet<QString> out;
    auto addValue = [&out](const QJsonValue &entry) {
        QString mac;
        if (entry.isString()) {
            mac = normalizeMac(entry.toString());
        } else if (entry.isObject()) {
            const QJsonObject obj = entry.toObject();
            mac = normalizeMac(obj.value(QStringLiteral("value")).toString());
            if (mac.isEmpty())
                mac = normalizeMac(obj.value(QStringLiteral("mac")).toString());
        }
        if (!mac.isEmpty())
            out.insert(mac);
    };

    if (value.isArray()) {
        const QJsonArray arr = value.toArray();
        for (const QJsonValue &entry : arr)
            addValue(entry);
        return out;
    }

    addValue(value);
    return out;
}

QJsonArray sortedMacArray(const QSet<QString> &macs)
{
    QStringList sorted;
    sorted.reserve(macs.size());
    for (const QString &mac : macs)
        sorted.push_back(mac);
    std::sort(sorted.begin(), sorted.end(), [](const QString &a, const QString &b) {
        return a.localeAwareCompare(b) < 0;
    });
    QJsonArray out;
    for (const QString &mac : sorted)
        out.append(mac);
    return out;
}

bool isTruthy(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    return normalized == QLatin1String("1") || normalized == QLatin1String("true");
}

QString toSoapBoolean(bool enabled)
{
    return enabled ? QStringLiteral("1") : QStringLiteral("0");
}

std::optional<bool> scalarToBool(const v1::ScalarValue &value)
{
    if (const auto *v = std::get_if<bool>(&value))
        return *v;
    if (const auto *v = std::get_if<std::int64_t>(&value))
        return *v != 0;
    if (const auto *v = std::get_if<double>(&value))
        return *v != 0.0;
    if (const auto *v = std::get_if<v1::Utf8String>(&value)) {
        const QString text = QString::fromStdString(*v).trimmed().toLower();
        if (text == QLatin1String("true") || text == QLatin1String("1") || text == QLatin1String("on"))
            return true;
        if (text == QLatin1String("false") || text == QLatin1String("0") || text == QLatin1String("off"))
            return false;
    }
    return std::nullopt;
}

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

struct HostEntry {
    QString mac;
    QString name;
    QString ip;
    QString interfaceType;
    bool active = false;
    bool hasSignal = false;
    int signalDbm = 0;
};

struct RouterSnapshot {
    bool hasUptime = false;
    qint64 uptimeSec = 0;
    bool hasSoftwareVersion = false;
    QString softwareVersion;
    bool hasUpdateAvailable = false;
    bool updateAvailable = false;
    bool hasWlan24 = false;
    bool wlan24Enabled = false;
    bool hasWlan5 = false;
    bool wlan5Enabled = false;
    bool hasTxRate = false;
    double txRateKbit = 0.0;
    bool hasRxRate = false;
    double rxRateKbit = 0.0;
    QString friendlyName;
};

struct HttpResult {
    bool success = false;
    QByteArray payload;
    QNetworkReply::NetworkError networkError = QNetworkReply::NoError;
    int statusCode = 0;
    QString error;
};

class FritzIpcSidecar final : public sdk::AdapterSidecar
{
public:
    void onBootstrap(const sdk::BootstrapRequest &request) override
    {
        AdapterSidecar::onBootstrap(request);
        m_started = false;
        m_knownDevices.clear();
        m_routerEmitted = false;
        m_routerName.clear();
        m_routerFirmware.clear();
        m_pendingFullSync = true;
        m_lastPollError.clear();
        m_lastPollErrorLogMs = 0;
        m_nextPollDueMs = 0;
        setConnected(false);
    }

    void onConfigChanged(const sdk::ConfigChangedRequest &request) override
    {
        AdapterSidecar::onConfigChanged(request);
        m_info = request.adapter;
        m_meta = parseJsonObject(request.adapter.metaJson);
        refreshConfig();
        m_started = true;
        ensureNetworkManager();

        std::cerr << "fritz-ipc config.changed adapterId=" << request.adapterId
                  << " externalId=" << m_info.externalId
                  << " pluginType=" << m_info.pluginType
                  << " tracked=" << m_trackedMacs.size()
                  << " known=" << knownHostMapFromMeta(m_meta).size()
                  << '\n';

        v1::Utf8String descriptorError;
        if (!sendAdapterDescriptorUpdated(descriptor(), &descriptorError)) {
            std::cerr << "failed to send adapterDescriptorUpdated(config.changed): "
                      << descriptorError << '\n';
        }

        setConnected(false);
        pollOnce();
        scheduleNextPoll();
    }

    void onDisconnected() override
    {
        m_started = false;
        setConnected(false);
        m_knownDevices.clear();
        std::cerr << "fritz-ipc disconnected externalId=" << m_info.externalId << '\n';
    }

    v1::Utf8String displayName() const override
    {
        return "FRITZ!Box";
    }

    v1::Utf8String description() const override
    {
        return "AVM FRITZ!Box via TR-064 (IPC sidecar)";
    }

    v1::Utf8String apiVersion() const override
    {
        return v1::kProtocolLabel;
    }

    v1::Utf8String iconSvg() const override
    {
        return kFritzIconSvg;
    }

    int timeoutMs() const override
    {
        return 15000;
    }

    int maxInstances() const override
    {
        return 0;
    }

    v1::AdapterCapabilities capabilities() const override
    {
        v1::AdapterCapabilities caps;
        caps.required = v1::AdapterRequirement::UsesRetryInterval;
        caps.flags = v1::AdapterFlag::SupportsDiscovery
            | v1::AdapterFlag::SupportsProbe
            | v1::AdapterFlag::RequiresPolling;
        caps.defaultsJson = R"({"pollIntervalMs":5000,"retryIntervalMs":10000})";

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

    v1::JsonText configSchemaJson() const override
    {
        return toJson(buildConfigSchemaObject());
    }

    v1::CmdResponse onChannelInvoke(const sdk::ChannelInvokeRequest &request) override
    {
        v1::CmdResponse resp;
        resp.id = request.cmdId;
        resp.tsMs = nowMs();

        if (request.deviceExternalId != kRouterDeviceId) {
            resp.status = v1::CmdStatus::NotSupported;
            resp.error = "Channel only supported for router device";
            return resp;
        }

        const auto enabled = scalarToBool(request.value);
        if (!enabled.has_value()) {
            resp.status = v1::CmdStatus::InvalidArgument;
            resp.error = "Expected boolean value";
            return resp;
        }

        if (request.channelExternalId == "wlan_24_enabled") {
            QString error;
            if (setWlanEnabled(24, *enabled, &error)) {
                resp.status = v1::CmdStatus::Success;
                resp.finalValue = *enabled;
                sendChannelStateUpdated(kRouterDeviceId, "wlan_24_enabled", *enabled);
            } else {
                resp.status = v1::CmdStatus::Failure;
                resp.error = error.isEmpty() ? "WLAN 2.4 GHz update failed" : error.toStdString();
            }
            return resp;
        }

        if (request.channelExternalId == "wlan_5_enabled") {
            QString error;
            if (setWlanEnabled(5, *enabled, &error)) {
                resp.status = v1::CmdStatus::Success;
                resp.finalValue = *enabled;
                sendChannelStateUpdated(kRouterDeviceId, "wlan_5_enabled", *enabled);
            } else {
                resp.status = v1::CmdStatus::Failure;
                resp.error = error.isEmpty() ? "WLAN 5 GHz update failed" : error.toStdString();
            }
            return resp;
        }

        resp.status = v1::CmdStatus::NotSupported;
        resp.error = "Channel not supported";
        return resp;
    }

    v1::ActionResponse onAdapterActionInvoke(const sdk::AdapterActionInvokeRequest &request) override
    {
        v1::ActionResponse resp;
        resp.id = request.cmdId;
        resp.tsMs = nowMs();

        const QString actionId = QString::fromStdString(request.actionId).trimmed();
        if (actionId == QLatin1String("settings")) {
            const QJsonObject params = parseJsonObject(request.paramsJson);
            if (!params.isEmpty()) {
                QJsonObject patch;
                for (auto it = params.begin(); it != params.end(); ++it) {
                    if (it.key() == QLatin1String("trackedMacs")) {
                        const QSet<QString> normalized = parseTrackedMacSelection(it.value());
                        QJsonArray tracked;
                        for (const QString &mac : normalized)
                            tracked.append(mac);
                        patch.insert(it.key(), tracked);
                        continue;
                    }
                    patch.insert(it.key(), it.value());
                }
                for (auto it = patch.begin(); it != patch.end(); ++it)
                    m_meta.insert(it.key(), it.value());
                m_info.metaJson = toJson(m_meta);
                refreshConfig();

                v1::Utf8String error;
                if (!sendAdapterMetaUpdated(toJson(patch), &error))
                    std::cerr << "failed to send adapterMetaUpdated(settings): " << error << '\n';
                if (!sendAdapterDescriptorUpdated(descriptor(), &error))
                    std::cerr << "failed to send adapterDescriptorUpdated(settings): " << error << '\n';

                pollOnce();
                scheduleNextPoll();
            }
            resp.status = v1::CmdStatus::Success;
            resp.resultType = v1::ActionResultType::None;
            resp.reloadLayout = true;
            resp.formValuesJson = toJson(QJsonObject{
                {QStringLiteral("trackedMacs"), sortedMacArray(m_trackedMacs)},
            });
            QJsonArray trackedChoices;
            const v1::AdapterConfigOptionList trackedOptions = buildTrackedOptions(m_meta);
            for (const v1::AdapterConfigOption &option : trackedOptions) {
                QJsonObject choice;
                choice.insert(QStringLiteral("value"), QString::fromStdString(option.value));
                choice.insert(QStringLiteral("label"), QString::fromStdString(option.label));
                trackedChoices.append(choice);
            }
            resp.fieldChoicesJson = toJson(QJsonObject{
                {QStringLiteral("trackedMacs"), trackedChoices},
            });
            return resp;
        }

        if (actionId == QLatin1String("browseHosts")) {
            const QJsonObject params = parseJsonObject(request.paramsJson);
            QList<HostEntry> hosts;
            QString error;
            if (!fetchHostSnapshot(hosts, &error)) {
                std::cerr << "fritz-ipc browseHosts failed: "
                          << error.toStdString() << '\n';
                resp.status = v1::CmdStatus::Failure;
                resp.error = error.isEmpty() ? "Failed to fetch host snapshot" : error.toStdString();
                resp.errorContext = "instance.action";
                return resp;
            }

            QSet<QString> selectedMacs = parseTrackedMacSelection(params.value(QStringLiteral("trackedMacs")));
            selectedMacs.unite(parseTrackedMacSelection(m_meta.value(QStringLiteral("trackedMacs"))));
            selectedMacs.unite(m_trackedMacs);

            QMap<QString, QJsonObject> knownMap = knownHostMapFromMeta(m_meta);
            QMap<QString, QJsonObject> nextMap;
            for (const HostEntry &entry : std::as_const(hosts)) {
                const QString mac = normalizeMac(entry.mac);
                if (mac.isEmpty())
                    continue;

                const QString name = entry.name.trimmed();
                const QString ip = entry.ip.trimmed();
                QJsonObject merged = knownMap.value(mac);
                merged.insert(QStringLiteral("mac"), mac);
                if (!name.isEmpty())
                    merged.insert(QStringLiteral("name"), name);
                if (!ip.isEmpty())
                    merged.insert(QStringLiteral("ip"), ip);
                nextMap.insert(mac, merged);
            }

            for (const QString &mac : std::as_const(selectedMacs)) {
                if (nextMap.contains(mac))
                    continue;
                QJsonObject merged = knownMap.value(mac);
                merged.insert(QStringLiteral("mac"), mac);
                nextMap.insert(mac, merged);
            }

            QJsonArray knownHosts;
            for (auto it = nextMap.cbegin(); it != nextMap.cend(); ++it)
                knownHosts.append(it.value());

            QJsonObject patch;
            patch.insert(QStringLiteral("knownHosts"), knownHosts);
            QJsonArray trackedMacs;
            trackedMacs = sortedMacArray(selectedMacs);
            patch.insert(QStringLiteral("trackedMacs"), trackedMacs);
            for (auto it = patch.begin(); it != patch.end(); ++it)
                m_meta.insert(it.key(), it.value());
            m_info.metaJson = toJson(m_meta);
            refreshConfig();

            v1::Utf8String sendError;
            if (!sendAdapterMetaUpdated(toJson(patch), &sendError))
                std::cerr << "failed to send adapterMetaUpdated(browseHosts): " << sendError << '\n';
            if (!sendAdapterDescriptorUpdated(descriptor(), &sendError))
                std::cerr << "failed to send adapterDescriptorUpdated(browseHosts): " << sendError << '\n';

            resp.status = v1::CmdStatus::Success;
            resp.resultType = v1::ActionResultType::None;
            resp.reloadLayout = true;
            resp.formValuesJson = toJson(QJsonObject{
                {QStringLiteral("trackedMacs"), trackedMacs},
            });
            QJsonArray trackedChoices;
            const v1::AdapterConfigOptionList trackedOptions = buildTrackedOptions(m_meta);
            for (const v1::AdapterConfigOption &option : trackedOptions) {
                QJsonObject choice;
                choice.insert(QStringLiteral("value"), QString::fromStdString(option.value));
                choice.insert(QStringLiteral("label"), QString::fromStdString(option.label));
                trackedChoices.append(choice);
            }
            resp.fieldChoicesJson = toJson(QJsonObject{
                {QStringLiteral("trackedMacs"), trackedChoices},
            });
            std::cerr << "fritz-ipc browseHosts success hosts=" << hosts.size()
                      << " selected=" << selectedMacs.size()
                      << " choices=" << trackedChoices.size()
                      << '\n';
            return resp;
        }

        if (actionId == QLatin1String("probe")) {
            const QJsonObject params = parseJsonObject(request.paramsJson);
            const QJsonObject factoryAdapter = resolveFactoryAdapterFromParams(params);
            const QString host = resolveProbeHost(factoryAdapter);
            const quint16 port = resolveProbePort(factoryAdapter);
            const QString user = resolveProbeUser(factoryAdapter);
            const QString password = resolveProbePassword(factoryAdapter);
            const bool useTls = resolveProbeUseTls(factoryAdapter);

            if (host.isEmpty()) {
                resp.status = v1::CmdStatus::InvalidArgument;
                resp.error = "Probe requires host or ip";
                return resp;
            }

            QString error;
            if (probeConnection(host, port, user, password, useTls, &error)) {
                resp.status = v1::CmdStatus::Success;
                resp.resultType = v1::ActionResultType::String;
                resp.resultValue = QStringLiteral("%1:%2").arg(host).arg(port).toStdString();
            } else {
                resp.status = v1::CmdStatus::Failure;
                resp.error = error.isEmpty() ? "Probe failed" : error.toStdString();
                resp.errorContext = "factory.action";
            }
            return resp;
        }

        resp.status = v1::CmdStatus::NotSupported;
        resp.error = "Adapter action not supported";
        return resp;
    }

    v1::CmdResponse onDeviceNameUpdate(const sdk::DeviceNameUpdateRequest &request) override
    {
        v1::CmdResponse resp;
        resp.id = request.cmdId;
        resp.status = v1::CmdStatus::NotImplemented;
        resp.error = "Device rename not supported";
        resp.tsMs = nowMs();
        return resp;
    }

    v1::CmdResponse onDeviceEffectInvoke(const sdk::DeviceEffectInvokeRequest &request) override
    {
        v1::CmdResponse resp;
        resp.id = request.cmdId;
        resp.status = v1::CmdStatus::NotImplemented;
        resp.error = "Device effect not supported";
        resp.tsMs = nowMs();
        return resp;
    }

    v1::CmdResponse onSceneInvoke(const sdk::SceneInvokeRequest &request) override
    {
        v1::CmdResponse resp;
        resp.id = request.cmdId;
        resp.status = v1::CmdStatus::NotImplemented;
        resp.error = "Scene invocation not supported";
        resp.tsMs = nowMs();
        return resp;
    }

    void tick()
    {
        if (!m_started)
            return;

        const std::int64_t now = nowMs();
        if (now < m_nextPollDueMs)
            return;

        pollOnce();
        scheduleNextPoll();
    }

private:
    static QMap<QString, QJsonObject> knownHostMapFromMeta(const QJsonObject &meta)
    {
        QMap<QString, QJsonObject> map;
        const QJsonValue knownValue = meta.value(QStringLiteral("knownHosts"));
        if (!knownValue.isArray())
            return map;

        const QJsonArray arr = knownValue.toArray();
        for (const QJsonValue &entry : arr) {
            if (entry.isString()) {
                const QString mac = normalizeMac(entry.toString());
                if (mac.isEmpty() || map.contains(mac))
                    continue;
                QJsonObject obj;
                obj.insert(QStringLiteral("mac"), mac);
                map.insert(mac, obj);
                continue;
            }
            if (!entry.isObject())
                continue;
            const QJsonObject obj = entry.toObject();
            const QString mac = normalizeMac(obj.value(QStringLiteral("mac")).toString());
            if (mac.isEmpty())
                continue;
            QJsonObject merged = obj;
            merged.insert(QStringLiteral("mac"), mac);
            map.insert(mac, merged);
        }
        return map;
    }

    void ensureNetworkManager()
    {
        if (m_network)
            return;
        m_network = std::make_unique<QNetworkAccessManager>();
        QObject::connect(m_network.get(),
                         &QNetworkAccessManager::authenticationRequired,
                         [this](QNetworkReply *, QAuthenticator *auth) {
                             if (!auth)
                                 return;
                             const QString user = QString::fromStdString(m_info.user).trimmed();
                             if (!user.isEmpty())
                                 auth->setUser(user);
                             auth->setPassword(QString::fromStdString(m_info.password));
                         });
    }

    static QByteArray buildSoapEnvelope(const QString &serviceType,
                                        const QString &action,
                                        const QMap<QString, QString> &params)
    {
        QString body;
        body += QStringLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?>");
        body += QStringLiteral("<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" ");
        body += QStringLiteral("s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">");
        body += QStringLiteral("<s:Body>");
        body += QStringLiteral("<u:%1 xmlns:u=\"%2\">").arg(action, serviceType);
        for (auto it = params.cbegin(); it != params.cend(); ++it)
            body += QStringLiteral("<%1>%2</%1>").arg(it.key(), it.value());
        body += QStringLiteral("</u:%1>").arg(action);
        body += QStringLiteral("</s:Body>");
        body += QStringLiteral("</s:Envelope>");
        return body.toUtf8();
    }

    static HttpResult waitForReply(QNetworkReply *reply, int timeoutMs)
    {
        HttpResult result;
        if (!reply) {
            result.error = QStringLiteral("No reply object");
            return result;
        }

        QTimer timeout;
        timeout.setSingleShot(true);
        timeout.start(timeoutMs);

        QEventLoop loop;
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();

        if (!timeout.isActive()) {
            reply->abort();
            result.error = QStringLiteral("Connection timed out");
            reply->deleteLater();
            return result;
        }

        result.payload = reply->readAll();
        result.networkError = reply->error();
        result.statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        result.error = reply->errorString();
        result.success = (result.networkError == QNetworkReply::NoError);
        reply->deleteLater();
        return result;
    }

    static HttpResult sendSoapRequest(QNetworkAccessManager *manager,
                                      const QString &baseUrl,
                                      const QString &controlPath,
                                      const QString &serviceType,
                                      const QString &action,
                                      const QMap<QString, QString> &params = {},
                                      int timeoutMs = 3000)
    {
        HttpResult result;
        if (!manager) {
            result.error = QStringLiteral("Network manager not initialized");
            return result;
        }

        QNetworkRequest request(QUrl(baseUrl + controlPath));
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("text/xml; charset=\"utf-8\""));
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        const QString soapAction = QStringLiteral("\"%1#%2\"").arg(serviceType, action);
        request.setRawHeader("SOAPAction", soapAction.toUtf8());

        return waitForReply(manager->post(request, buildSoapEnvelope(serviceType, action, params)), timeoutMs);
    }

    static HttpResult sendGetRequest(QNetworkAccessManager *manager,
                                     const QString &url,
                                     int timeoutMs = 3000)
    {
        HttpResult result;
        if (!manager) {
            result.error = QStringLiteral("Network manager not initialized");
            return result;
        }

        QNetworkRequest request{QUrl(url)};
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        return waitForReply(manager->get(request), timeoutMs);
    }

    static bool parseSoapValue(const QByteArray &payload, const QString &key, QString *value)
    {
        if (!value)
            return false;

        QXmlStreamReader reader(payload);
        while (!reader.atEnd()) {
            reader.readNext();
            if (reader.isStartElement() && reader.name() == key) {
                *value = reader.readElementText().trimmed();
                return true;
            }
        }
        return false;
    }

    static bool parseHostListPath(const QByteArray &payload, QString *path, QString *error)
    {
        if (!path)
            return false;

        QXmlStreamReader reader(payload);
        while (!reader.atEnd()) {
            reader.readNext();
            if (reader.isStartElement() && reader.name() == QLatin1String("NewHostListPath")) {
                *path = reader.readElementText().trimmed();
                return true;
            }
        }

        if (error)
            *error = reader.hasError() ? reader.errorString() : QStringLiteral("Host list path missing");
        return false;
    }

    static bool parseHostEntryFromSoap(const QByteArray &payload, HostEntry *entry)
    {
        if (!entry)
            return false;

        QXmlStreamReader reader(payload);
        bool foundMac = false;
        while (!reader.atEnd()) {
            reader.readNext();
            if (!reader.isStartElement())
                continue;

            const QStringView name = reader.name();
            if (name == QLatin1String("NewMACAddress")) {
                entry->mac = reader.readElementText().trimmed();
                foundMac = true;
            } else if (name == QLatin1String("NewHostName")) {
                entry->name = reader.readElementText().trimmed();
            } else if (name == QLatin1String("NewIPAddress")) {
                entry->ip = reader.readElementText().trimmed();
            } else if (name == QLatin1String("NewActive")) {
                entry->active = isTruthy(reader.readElementText());
            } else if (name == QLatin1String("NewInterfaceType")) {
                entry->interfaceType = reader.readElementText().trimmed();
            } else if (name == QLatin1String("NewSignalStrength")) {
                const QString value = reader.readElementText().trimmed();
                bool ok = false;
                const int signal = value.toInt(&ok);
                if (ok) {
                    entry->hasSignal = true;
                    entry->signalDbm = signal;
                }
            }
        }
        return foundMac;
    }

    static QList<HostEntry> parseHostList(const QByteArray &payload)
    {
        QList<HostEntry> hosts;
        QXmlStreamReader reader(payload);
        HostEntry current;
        bool inHost = false;

        while (!reader.atEnd()) {
            reader.readNext();
            if (reader.isStartElement()) {
                const QStringView name = reader.name();
                if (name == QLatin1String("Host")) {
                    current = HostEntry();
                    inHost = true;
                } else if (inHost) {
                    if (name == QLatin1String("MACAddress")) {
                        current.mac = reader.readElementText().trimmed();
                    } else if (name == QLatin1String("HostName")) {
                        current.name = reader.readElementText().trimmed();
                    } else if (name == QLatin1String("IPAddress")) {
                        current.ip = reader.readElementText().trimmed();
                    } else if (name == QLatin1String("Active")) {
                        current.active = isTruthy(reader.readElementText());
                    } else if (name == QLatin1String("SignalStrength")) {
                        const QString value = reader.readElementText().trimmed();
                        bool ok = false;
                        const int signal = value.toInt(&ok);
                        if (ok) {
                            current.hasSignal = true;
                            current.signalDbm = signal;
                        }
                    } else if (name == QLatin1String("InterfaceType")) {
                        current.interfaceType = reader.readElementText().trimmed();
                    }
                }
            } else if (reader.isEndElement() && reader.name() == QLatin1String("Host")) {
                if (!current.mac.isEmpty()) {
                    current.mac = normalizeMac(current.mac);
                    hosts.push_back(current);
                }
                inHost = false;
            }
        }

        return hosts;
    }

    static bool isInvalidActionFault(const QByteArray &payload)
    {
        const QByteArray lower = payload.toLower();
        return lower.contains("invalid action") || lower.contains("<errorcode>401</errorcode>");
    }

    QString resolvedHost() const
    {
        const QString ip = QString::fromStdString(m_info.ip).trimmed();
        if (!ip.isEmpty())
            return ip;
        return QString::fromStdString(m_info.host).trimmed();
    }

    quint16 resolvedPort() const
    {
        return m_info.port > 0 ? m_info.port : 49000;
    }

    QString resolveBaseUrl(bool useTls = false) const
    {
        const QString host = resolvedHost();
        if (host.isEmpty())
            return {};
        const quint16 port = resolvedPort();
        const bool tls = useTls || v1::hasFlag(m_info.flags, v1::AdapterFlag::UseTls);
        const QString scheme = tls ? QStringLiteral("https") : QStringLiteral("http");
        return QStringLiteral("%1://%2:%3").arg(scheme, host).arg(port);
    }

    QString resolveProbeHost(const QJsonObject &factoryAdapter) const
    {
        auto fromObj = [](const QJsonObject &obj) -> QString {
            const QString ip = obj.value(QStringLiteral("ip")).toString().trimmed();
            if (!ip.isEmpty())
                return ip;
            return obj.value(QStringLiteral("host")).toString().trimmed();
        };

        QString host = fromObj(factoryAdapter);
        if (!host.isEmpty())
            return host;

        const QJsonObject meta = factoryAdapter.value(QStringLiteral("meta")).toObject();
        host = fromObj(meta);
        if (!host.isEmpty())
            return host;

        return resolvedHost();
    }

    quint16 resolveProbePort(const QJsonObject &factoryAdapter) const
    {
        auto parsePort = [](const QJsonValue &value) -> quint16 {
            if (!value.isDouble())
                return 0;
            const int parsed = value.toInt();
            if (parsed <= 0 || parsed > 65535)
                return 0;
            return static_cast<quint16>(parsed);
        };

        quint16 port = parsePort(factoryAdapter.value(QStringLiteral("port")));
        if (port == 0) {
            const QJsonObject meta = factoryAdapter.value(QStringLiteral("meta")).toObject();
            port = parsePort(meta.value(QStringLiteral("port")));
        }
        return port > 0 ? port : resolvedPort();
    }

    QString resolveProbeUser(const QJsonObject &factoryAdapter) const
    {
        auto fromObj = [](const QJsonObject &obj) -> QString {
            const QString user = obj.value(QStringLiteral("user")).toString().trimmed();
            if (!user.isEmpty())
                return user;
            return obj.value(QStringLiteral("username")).toString().trimmed();
        };

        QString user = fromObj(factoryAdapter);
        if (!user.isEmpty())
            return user;

        const QJsonObject meta = factoryAdapter.value(QStringLiteral("meta")).toObject();
        user = fromObj(meta);
        if (!user.isEmpty())
            return user;

        return QString::fromStdString(m_info.user).trimmed();
    }

    QString resolveProbePassword(const QJsonObject &factoryAdapter) const
    {
        auto fromObj = [](const QJsonObject &obj) -> QString {
            QString pw = obj.value(QStringLiteral("password")).toString();
            if (!pw.isEmpty())
                return pw;
            pw = obj.value(QStringLiteral("pw")).toString();
            if (!pw.isEmpty())
                return pw;
            return obj.value(QStringLiteral("token")).toString();
        };

        QString password = fromObj(factoryAdapter);
        if (!password.isEmpty())
            return password;

        const QJsonObject meta = factoryAdapter.value(QStringLiteral("meta")).toObject();
        password = fromObj(meta);
        if (!password.isEmpty())
            return password;

        return QString::fromStdString(m_info.password);
    }

    bool resolveProbeUseTls(const QJsonObject &factoryAdapter) const
    {
        if (factoryAdapter.contains(QStringLiteral("flags"))) {
            const int raw = factoryAdapter.value(QStringLiteral("flags")).toInt();
            const auto flags = static_cast<v1::AdapterFlags>(raw);
            return v1::hasFlag(flags, v1::AdapterFlag::UseTls);
        }
        return v1::hasFlag(m_info.flags, v1::AdapterFlag::UseTls);
    }

    static QJsonObject resolveFactoryAdapterFromParams(const QJsonObject &params)
    {
        const QJsonObject candidate = params.value(QStringLiteral("factoryAdapter")).toObject();
        if (!candidate.isEmpty())
            return candidate;
        return params;
    }

    bool probeConnection(const QString &host,
                         quint16 port,
                         const QString &user,
                         const QString &password,
                         bool useTls,
                         QString *error)
    {
        QNetworkAccessManager manager;
        QObject::connect(&manager,
                         &QNetworkAccessManager::authenticationRequired,
                         [&user, &password](QNetworkReply *, QAuthenticator *auth) {
                             if (!auth)
                                 return;
                             if (!user.isEmpty())
                                 auth->setUser(user);
                             auth->setPassword(password);
                         });

        const QString baseUrl = QStringLiteral("%1://%2:%3")
            .arg(useTls ? QStringLiteral("https") : QStringLiteral("http"), host)
            .arg(port > 0 ? port : static_cast<quint16>(49000));

        const HttpResult result = sendSoapRequest(&manager,
                                                  baseUrl,
                                                  QString::fromLatin1(kDeviceInfoControlPath),
                                                  QString::fromLatin1(kDeviceInfoServiceType),
                                                  QStringLiteral("GetInfo"));
        if (result.success)
            return true;

        const QString body = QString::fromUtf8(result.payload).trimmed();
        if (result.statusCode == 401 || body.contains(QStringLiteral("401 Unauthorized"), Qt::CaseInsensitive)) {
            if (error)
                *error = QStringLiteral("Invalid credentials");
            return false;
        }

        if (error) {
            if (!body.isEmpty())
                *error = QStringLiteral("HTTP %1: %2").arg(result.statusCode).arg(body);
            else if (!result.error.isEmpty())
                *error = result.error;
            else
                *error = QStringLiteral("Probe failed");
        }
        return false;
    }

    bool ensureHostAvailable(QString *error) const
    {
        if (!resolvedHost().isEmpty())
            return true;
        if (error)
            *error = QStringLiteral("Host/IP is required");
        return false;
    }

    bool fetchHostEntries(QList<HostEntry> &hosts, QString *error)
    {
        ensureNetworkManager();

        const QString baseUrl = resolveBaseUrl();
        if (baseUrl.isEmpty()) {
            if (error)
                *error = QStringLiteral("Host is required");
            return false;
        }

        const HttpResult countResult = sendSoapRequest(m_network.get(),
                                                       baseUrl,
                                                       QString::fromLatin1(kHostsControlPath),
                                                       QString::fromLatin1(kHostsServiceType),
                                                       QStringLiteral("GetHostNumberOfEntries"));
        if (!countResult.success) {
            if (error)
                *error = countResult.error;
            return false;
        }

        QString countValue;
        if (!parseSoapValue(countResult.payload, QStringLiteral("NewHostNumberOfEntries"), &countValue)) {
            if (error)
                *error = QStringLiteral("Host count missing");
            return false;
        }

        bool ok = false;
        const int total = countValue.toInt(&ok);
        if (!ok || total <= 0) {
            hosts.clear();
            return true;
        }

        hosts.clear();
        for (int index = 0; index < total; ++index) {
            const HttpResult entryResult = sendSoapRequest(m_network.get(),
                                                           baseUrl,
                                                           QString::fromLatin1(kHostsControlPath),
                                                           QString::fromLatin1(kHostsServiceType),
                                                           QStringLiteral("GetGenericHostEntry"),
                                                           {{QStringLiteral("NewIndex"), QString::number(index)}});
            if (!entryResult.success) {
                if (error)
                    *error = entryResult.error;
                return false;
            }

            HostEntry entry;
            if (parseHostEntryFromSoap(entryResult.payload, &entry) && !entry.mac.isEmpty()) {
                entry.mac = normalizeMac(entry.mac);
                hosts.push_back(entry);
            }
        }

        return true;
    }

    bool fetchHostSnapshot(QList<HostEntry> &hosts, QString *error)
    {
        ensureNetworkManager();

        const QString baseUrl = resolveBaseUrl();
        if (baseUrl.isEmpty()) {
            if (error)
                *error = QStringLiteral("Host is required");
            return false;
        }

        const HttpResult listPathResult = sendSoapRequest(m_network.get(),
                                                          baseUrl,
                                                          QString::fromLatin1(kHostsControlPath),
                                                          QString::fromLatin1(kHostsServiceType),
                                                          QStringLiteral("GetHostListPath"));
        if (!listPathResult.success) {
            if (isInvalidActionFault(listPathResult.payload))
                return fetchHostEntries(hosts, error);
            if (error)
                *error = listPathResult.error;
            return false;
        }

        QString listPath;
        QString parseError;
        if (!parseHostListPath(listPathResult.payload, &listPath, &parseError)) {
            if (isInvalidActionFault(listPathResult.payload))
                return fetchHostEntries(hosts, error);
            if (error)
                *error = parseError;
            return false;
        }

        if (listPath.isEmpty()) {
            if (error)
                *error = QStringLiteral("Host list path missing");
            return false;
        }

        const QString listUrl = listPath.startsWith(QLatin1Char('/'))
            ? (baseUrl + listPath)
            : (baseUrl + QLatin1Char('/') + listPath);
        const HttpResult listResult = sendGetRequest(m_network.get(), listUrl);
        if (!listResult.success) {
            if (error)
                *error = listResult.error;
            return false;
        }

        hosts = parseHostList(listResult.payload);
        if (hosts.isEmpty()) {
            QString fallbackError;
            if (fetchHostEntries(hosts, &fallbackError))
                return true;
            if (error && !fallbackError.isEmpty())
                *error = fallbackError;
            return false;
        }
        return true;
    }

    bool fetchRouterSnapshot(RouterSnapshot &snapshot, QString *error)
    {
        ensureNetworkManager();

        const QString baseUrl = resolveBaseUrl();
        if (baseUrl.isEmpty()) {
            if (error)
                *error = QStringLiteral("Host is required");
            return false;
        }

        HttpResult deviceInfo = sendSoapRequest(m_network.get(),
                                                baseUrl,
                                                QString::fromLatin1(kDeviceInfoControlPath),
                                                QString::fromLatin1(kDeviceInfoServiceType),
                                                QStringLiteral("GetInfo"));
        if (deviceInfo.success) {
            QString value;
            if (parseSoapValue(deviceInfo.payload, QStringLiteral("NewUpTime"), &value)) {
                bool ok = false;
                const qint64 uptime = value.toLongLong(&ok);
                if (ok) {
                    snapshot.hasUptime = true;
                    snapshot.uptimeSec = uptime;
                }
            }
            if (parseSoapValue(deviceInfo.payload, QStringLiteral("NewSoftwareVersion"), &value)) {
                const QString version = value.trimmed();
                if (!version.isEmpty()) {
                    snapshot.hasSoftwareVersion = true;
                    snapshot.softwareVersion = version;
                }
            }
            if (parseSoapValue(deviceInfo.payload, QStringLiteral("NewFriendlyName"), &value)) {
                const QString name = value.trimmed();
                if (!name.isEmpty())
                    snapshot.friendlyName = name;
            } else if (parseSoapValue(deviceInfo.payload, QStringLiteral("NewDeviceName"), &value)) {
                const QString name = value.trimmed();
                if (!name.isEmpty())
                    snapshot.friendlyName = name;
            }
        }

        HttpResult updateInfo = sendSoapRequest(m_network.get(),
                                                baseUrl,
                                                QString::fromLatin1(kDeviceInfoControlPath),
                                                QString::fromLatin1(kDeviceInfoServiceType),
                                                QStringLiteral("X_AVM-DE_GetAutoUpdateInfo"));
        if (updateInfo.success) {
            QString value;
            if (parseSoapValue(updateInfo.payload, QStringLiteral("NewUpdateAvailable"), &value)) {
                snapshot.hasUpdateAvailable = true;
                snapshot.updateAvailable = isTruthy(value);
            }
        }

        HttpResult wlan24 = sendSoapRequest(m_network.get(),
                                            baseUrl,
                                            QString::fromLatin1(kWlan24ControlPath),
                                            QString::fromLatin1(kWlan24ServiceType),
                                            QStringLiteral("GetInfo"));
        if (wlan24.success) {
            QString value;
            if (parseSoapValue(wlan24.payload, QStringLiteral("NewEnable"), &value)) {
                snapshot.hasWlan24 = true;
                snapshot.wlan24Enabled = isTruthy(value);
            }
        }

        HttpResult wlan5 = sendSoapRequest(m_network.get(),
                                           baseUrl,
                                           QString::fromLatin1(kWlan5ControlPath),
                                           QString::fromLatin1(kWlan5ServiceType),
                                           QStringLiteral("GetInfo"));
        if (wlan5.success) {
            QString value;
            if (parseSoapValue(wlan5.payload, QStringLiteral("NewEnable"), &value)) {
                snapshot.hasWlan5 = true;
                snapshot.wlan5Enabled = isTruthy(value);
            }
        }

        HttpResult wan = sendSoapRequest(m_network.get(),
                                         baseUrl,
                                         QString::fromLatin1(kWanCommonControlPath),
                                         QString::fromLatin1(kWanCommonServiceType),
                                         QStringLiteral("GetAddonInfos"));
        if (wan.success) {
            QString value;
            if (parseSoapValue(wan.payload, QStringLiteral("NewByteSendRate"), &value)) {
                bool ok = false;
                const double bytesPerSec = value.toDouble(&ok);
                if (ok) {
                    snapshot.hasTxRate = true;
                    snapshot.txRateKbit = (bytesPerSec * 8.0) / 1000.0;
                }
            }
            if (parseSoapValue(wan.payload, QStringLiteral("NewByteReceiveRate"), &value)) {
                bool ok = false;
                const double bytesPerSec = value.toDouble(&ok);
                if (ok) {
                    snapshot.hasRxRate = true;
                    snapshot.rxRateKbit = (bytesPerSec * 8.0) / 1000.0;
                }
            }
        }

        if (!deviceInfo.success && !updateInfo.success && !wlan24.success && !wlan5.success && !wan.success) {
            if (error)
                *error = QStringLiteral("Router snapshot unavailable");
            return false;
        }

        return true;
    }

    bool setWlanEnabled(int band, bool enabled, QString *error)
    {
        ensureNetworkManager();

        const QString baseUrl = resolveBaseUrl();
        if (baseUrl.isEmpty()) {
            if (error)
                *error = QStringLiteral("Host is required");
            return false;
        }

        QString controlPath;
        QString serviceType;
        if (band == 24) {
            controlPath = QString::fromLatin1(kWlan24ControlPath);
            serviceType = QString::fromLatin1(kWlan24ServiceType);
        } else if (band == 5) {
            controlPath = QString::fromLatin1(kWlan5ControlPath);
            serviceType = QString::fromLatin1(kWlan5ServiceType);
        } else {
            if (error)
                *error = QStringLiteral("Unsupported WLAN band");
            return false;
        }

        const HttpResult result = sendSoapRequest(m_network.get(),
                                                  baseUrl,
                                                  controlPath,
                                                  serviceType,
                                                  QStringLiteral("SetEnable"),
                                                  {{QStringLiteral("NewEnable"), toSoapBoolean(enabled)}});
        if (result.success)
            return true;

        if (isInvalidActionFault(result.payload)) {
            if (error)
                *error = QStringLiteral("WLAN configuration not available");
            return false;
        }

        if (error)
            *error = result.error.isEmpty() ? QStringLiteral("SetEnable failed") : result.error;
        return false;
    }

    void refreshConfig()
    {
        const int interval = m_meta.value(QStringLiteral("pollIntervalMs")).toInt(5000);
        if (interval >= 500)
            m_pollIntervalMs = interval;
        const int retry = m_meta.value(QStringLiteral("retryIntervalMs")).toInt(10000);
        if (retry >= 1000)
            m_retryIntervalMs = retry;

        m_trackedMacs.clear();
        m_trackedMacs = parseTrackedMacSelection(m_meta.value(QStringLiteral("trackedMacs")));
    }

    void setConnected(bool connected)
    {
        if (m_connected == connected)
            return;
        m_connected = connected;

        v1::Utf8String error;
        if (!sendConnectionStateChanged(m_connected, &error)) {
            std::cerr << "failed to send connectionStateChanged: " << error << '\n';
        }
    }

    void scheduleNextPoll()
    {
        const std::int64_t interval = m_connected ? m_pollIntervalMs : m_retryIntervalMs;
        m_nextPollDueMs = nowMs() + interval;
    }

    void logPollError(const QString &error)
    {
        const std::int64_t now = nowMs();
        if (error == m_lastPollError && (now - m_lastPollErrorLogMs) < m_retryIntervalMs)
            return;
        m_lastPollError = error;
        m_lastPollErrorLogMs = now;
        std::cerr << "fritz-ipc poll failed: " << error.toStdString() << '\n';
    }

    void pollOnce()
    {
        QString hostError;
        if (!ensureHostAvailable(&hostError)) {
            setConnected(false);
            if (m_pendingFullSync) {
                m_pendingFullSync = false;
                sendFullSyncCompleted();
            }
            logPollError(hostError);
            return;
        }

        QList<HostEntry> hosts;
        QString hostFetchError;
        if (!fetchHostSnapshot(hosts, &hostFetchError)) {
            setConnected(false);
            if (m_pendingFullSync) {
                m_pendingFullSync = false;
                sendFullSyncCompleted();
            }
            logPollError(hostFetchError);
            return;
        }

        setConnected(true);
        handleHostSnapshot(hosts);

        RouterSnapshot snapshot;
        QString snapshotError;
        if (fetchRouterSnapshot(snapshot, &snapshotError))
            applyRouterSnapshot(snapshot);

        if (m_pendingFullSync) {
            m_pendingFullSync = false;
            sendFullSyncCompleted();
        }
    }

    void handleHostSnapshot(const QList<HostEntry> &hosts)
    {
        QSet<QString> nextDeviceIds;
        nextDeviceIds.reserve(hosts.size());

        for (const HostEntry &host : hosts) {
            const QString mac = normalizeMac(host.mac);
            if (mac.isEmpty())
                continue;
            if (!m_trackedMacs.contains(mac))
                continue;

            nextDeviceIds.insert(mac);
            updateDeviceFromHost(host);
        }

        removeMissingDevices(nextDeviceIds);
        m_knownDevices = nextDeviceIds;
    }

    void updateDeviceFromHost(const HostEntry &host)
    {
        v1::Device device;
        device.externalId = normalizeMac(host.mac).toStdString();
        device.name = host.name.trimmed().isEmpty()
            ? device.externalId
            : host.name.trimmed().toStdString();
        device.deviceClass = v1::DeviceClass::Sensor;
        device.manufacturer = "AVM";

        if (host.interfaceType.contains(QLatin1String("802.11"), Qt::CaseInsensitive)
            || host.interfaceType.contains(QLatin1String("wlan"), Qt::CaseInsensitive)) {
            device.flags |= v1::DeviceFlag::Wireless;
        }

        QJsonObject meta;
        if (!host.ip.isEmpty())
            meta.insert(QStringLiteral("ip"), host.ip);
        meta.insert(QStringLiteral("mac"), QString::fromStdString(device.externalId));
        if (!host.interfaceType.isEmpty())
            meta.insert(QStringLiteral("interfaceType"), host.interfaceType);
        device.metaJson = toJson(meta);

        v1::ChannelList channels;

        v1::Channel online;
        online.externalId = "online";
        online.name = "Online";
        online.kind = v1::ChannelKind::ConnectivityStatus;
        online.dataType = v1::ChannelDataType::Enum;
        online.flags = v1::kChannelFlagDefaultRead;
        channels.push_back(online);

        if (host.hasSignal) {
            v1::Channel rssi;
            rssi.externalId = "rssi";
            rssi.name = "RSSI";
            rssi.kind = v1::ChannelKind::SignalStrength;
            rssi.dataType = v1::ChannelDataType::Float;
            rssi.flags = v1::kChannelFlagDefaultRead;
            rssi.unit = "dBm";
            rssi.minValue = -100.0;
            rssi.maxValue = 0.0;
            channels.push_back(rssi);
        }

        v1::Utf8String error;
        if (!sendDeviceUpdated(device, channels, &error))
            std::cerr << "failed to send deviceUpdated(host): " << error << '\n';

        const auto statusValue = host.active
            ? static_cast<std::int64_t>(v1::ConnectivityStatus::Connected)
            : static_cast<std::int64_t>(v1::ConnectivityStatus::Disconnected);
        if (!sendChannelStateUpdated(device.externalId, "online", statusValue, nowMs(), &error))
            std::cerr << "failed to send channelStateUpdated(online): " << error << '\n';

        if (host.hasSignal) {
            if (!sendChannelStateUpdated(device.externalId,
                                         "rssi",
                                         static_cast<double>(host.signalDbm),
                                         nowMs(),
                                         &error)) {
                std::cerr << "failed to send channelStateUpdated(rssi): " << error << '\n';
            }
        }
    }

    void removeMissingDevices(const QSet<QString> &deviceIds)
    {
        if (m_knownDevices.isEmpty())
            return;

        for (const QString &deviceId : std::as_const(m_knownDevices)) {
            if (deviceIds.contains(deviceId))
                continue;
            v1::Utf8String error;
            if (!sendDeviceRemoved(deviceId.toStdString(), &error))
                std::cerr << "failed to send deviceRemoved(" << deviceId.toStdString() << "): " << error << '\n';
        }
    }

    v1::ChannelList buildRouterChannels() const
    {
        v1::ChannelList channels;

        v1::Channel uptime;
        uptime.externalId = "uptime";
        uptime.name = "Uptime";
        uptime.kind = v1::ChannelKind::Unknown;
        uptime.dataType = v1::ChannelDataType::Int;
        uptime.flags = v1::kChannelFlagDefaultRead;
        uptime.unit = "s";
        channels.push_back(uptime);

        v1::Channel softwareUpdate;
        softwareUpdate.externalId = kDeviceSoftwareUpdateChannelId;
        softwareUpdate.name = "Software Update";
        softwareUpdate.kind = v1::ChannelKind::DeviceSoftwareUpdate;
        softwareUpdate.dataType = v1::ChannelDataType::Enum;
        softwareUpdate.flags = v1::kChannelFlagDefaultRead;
        channels.push_back(softwareUpdate);

        v1::Channel wlan24;
        wlan24.externalId = "wlan_24_enabled";
        wlan24.name = "WLAN 2.4 GHz";
        wlan24.kind = v1::ChannelKind::PowerOnOff;
        wlan24.dataType = v1::ChannelDataType::Bool;
        wlan24.flags = v1::kChannelFlagDefaultWrite;
        wlan24.metaJson = R"({"forceLabel":true})";
        channels.push_back(wlan24);

        v1::Channel wlan5;
        wlan5.externalId = "wlan_5_enabled";
        wlan5.name = "WLAN 5 GHz";
        wlan5.kind = v1::ChannelKind::PowerOnOff;
        wlan5.dataType = v1::ChannelDataType::Bool;
        wlan5.flags = v1::kChannelFlagDefaultWrite;
        wlan5.metaJson = R"({"forceLabel":true})";
        channels.push_back(wlan5);

        v1::Channel txRate;
        txRate.externalId = "tx_rate";
        txRate.name = "TX rate";
        txRate.kind = v1::ChannelKind::Unknown;
        txRate.dataType = v1::ChannelDataType::Float;
        txRate.flags = v1::kChannelFlagDefaultRead;
        txRate.unit = "kbit/s";
        channels.push_back(txRate);

        v1::Channel rxRate;
        rxRate.externalId = "rx_rate";
        rxRate.name = "RX rate";
        rxRate.kind = v1::ChannelKind::Unknown;
        rxRate.dataType = v1::ChannelDataType::Float;
        rxRate.flags = v1::kChannelFlagDefaultRead;
        rxRate.unit = "kbit/s";
        channels.push_back(rxRate);

        return channels;
    }

    void ensureRouterDevice()
    {
        if (m_routerEmitted)
            return;

        v1::Device device;
        device.externalId = kRouterDeviceId;
        device.name = m_routerName.isEmpty() ? "FRITZ!Box" : m_routerName.toStdString();
        device.deviceClass = v1::DeviceClass::Gateway;
        device.manufacturer = "AVM";
        if (!m_routerFirmware.isEmpty())
            device.firmware = m_routerFirmware.toStdString();

        v1::Utf8String error;
        if (!sendDeviceUpdated(device, buildRouterChannels(), &error))
            std::cerr << "failed to send deviceUpdated(router): " << error << '\n';
        else
            m_routerEmitted = true;
    }

    void applyRouterSnapshot(const RouterSnapshot &snapshot)
    {
        if (!snapshot.friendlyName.isEmpty() && snapshot.friendlyName != m_routerName) {
            m_routerName = snapshot.friendlyName;
            m_routerEmitted = false;
        }
        if (snapshot.hasSoftwareVersion && snapshot.softwareVersion != m_routerFirmware) {
            m_routerFirmware = snapshot.softwareVersion;
            m_routerEmitted = false;
        }

        ensureRouterDevice();

        const std::int64_t ts = nowMs();
        v1::Utf8String error;
        if (snapshot.hasUptime) {
            if (!sendChannelStateUpdated(kRouterDeviceId,
                                         "uptime",
                                         static_cast<std::int64_t>(snapshot.uptimeSec),
                                         ts,
                                         &error)) {
                std::cerr << "failed to send channelStateUpdated(uptime): " << error << '\n';
            }
        }

        if (snapshot.hasUpdateAvailable || snapshot.hasSoftwareVersion) {
            const std::string status = snapshot.hasUpdateAvailable
                ? (snapshot.updateAvailable ? "UpdateAvailable" : "UpToDate")
                : "Unknown";
            if (!sendChannelStateUpdated(kRouterDeviceId,
                                         kDeviceSoftwareUpdateChannelId,
                                         status,
                                         ts,
                                         &error)) {
                std::cerr << "failed to send channelStateUpdated(software_update): " << error << '\n';
            }
        }

        if (snapshot.hasWlan24) {
            if (!sendChannelStateUpdated(kRouterDeviceId, "wlan_24_enabled", snapshot.wlan24Enabled, ts, &error))
                std::cerr << "failed to send channelStateUpdated(wlan_24_enabled): " << error << '\n';
        }

        if (snapshot.hasWlan5) {
            if (!sendChannelStateUpdated(kRouterDeviceId, "wlan_5_enabled", snapshot.wlan5Enabled, ts, &error))
                std::cerr << "failed to send channelStateUpdated(wlan_5_enabled): " << error << '\n';
        }

        if (snapshot.hasTxRate) {
            if (!sendChannelStateUpdated(kRouterDeviceId, "tx_rate", snapshot.txRateKbit, ts, &error))
                std::cerr << "failed to send channelStateUpdated(tx_rate): " << error << '\n';
        }

        if (snapshot.hasRxRate) {
            if (!sendChannelStateUpdated(kRouterDeviceId, "rx_rate", snapshot.rxRateKbit, ts, &error))
                std::cerr << "failed to send channelStateUpdated(rx_rate): " << error << '\n';
        }
    }

    QJsonObject buildConfigSchemaObject() const
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
        const QString hostDefault = resolvedHost();
        factoryFields.append(field(QStringLiteral("host"),
                                   QStringLiteral("Hostname"),
                                   QStringLiteral("Host"),
                                   hostDefault.isEmpty() ? QJsonValue() : QJsonValue(hostDefault),
                                   QString(),
                                   QString(),
                                   QString(),
                                   QJsonArray{QStringLiteral("Required")}));
        factoryFields.append(field(QStringLiteral("port"),
                                   QStringLiteral("Port"),
                                   QStringLiteral("Port"),
                                   static_cast<int>(resolvedPort())));
        factoryFields.append(field(QStringLiteral("user"),
                                   QStringLiteral("String"),
                                   QStringLiteral("Username"),
                                   QString::fromStdString(m_info.user),
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
                                   m_pollIntervalMs));
        factoryFields.append(field(QStringLiteral("retryIntervalMs"),
                                   QStringLiteral("Integer"),
                                   QStringLiteral("Retry interval"),
                                   m_retryIntervalMs));

        QJsonArray trackedChoices;
        const v1::AdapterConfigOptionList trackedOptions = buildTrackedOptions(m_meta);
        for (const v1::AdapterConfigOption &option : trackedOptions) {
            QJsonObject choice;
            choice.insert(QStringLiteral("value"), QString::fromStdString(option.value));
            choice.insert(QStringLiteral("label"), QString::fromStdString(option.label));
            trackedChoices.append(choice);
        }

        QJsonArray trackedDefaults;
        trackedDefaults = sortedMacArray(m_trackedMacs);

        QJsonArray instanceFields;
        instanceFields.append(field(QStringLiteral("trackedMacs"),
                                    QStringLiteral("Select"),
                                    QStringLiteral("Tracked devices"),
                                    trackedDefaults,
                                    QStringLiteral("browseHosts"),
                                    QStringLiteral("Probe WLAN"),
                                    QStringLiteral("settings"),
                                    QJsonArray{QStringLiteral("Multi"), QStringLiteral("InstanceOnly")},
                                    trackedChoices,
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

private:
    std::unique_ptr<QNetworkAccessManager> m_network;
    v1::Adapter m_info;
    QJsonObject m_meta;
    QSet<QString> m_trackedMacs;
    QSet<QString> m_knownDevices;

    bool m_connected = false;
    bool m_started = false;
    bool m_pendingFullSync = false;
    bool m_routerEmitted = false;

    int m_pollIntervalMs = 5000;
    int m_retryIntervalMs = 10000;
    std::int64_t m_nextPollDueMs = 0;
    std::int64_t m_lastPollErrorLogMs = 0;

    QString m_routerName;
    QString m_routerFirmware;
    QString m_lastPollError;
};

class FritzIpcFactory final : public sdk::AdapterFactory
{
public:
    v1::Utf8String pluginType() const override
    {
        return kPluginType;
    }

    std::unique_ptr<sdk::AdapterSidecar> create() const override
    {
        return std::make_unique<FritzIpcSidecar>();
    }
};

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    const char *envSocketPath = std::getenv("PHI_ADAPTER_SOCKET_PATH");
    const v1::Utf8String socketPath = (argc > 1)
        ? argv[1]
        : (envSocketPath ? envSocketPath : v1::Utf8String("/tmp/phi-adapter-fritz-ipc.sock"));

    std::cerr << "starting " << (argc > 0 && argv && argv[0] ? argv[0] : "phi_adapter_fritz")
              << " for pluginType=" << kPluginType
              << " socket=" << socketPath << '\n';

    FritzIpcFactory factory;
    sdk::SidecarHost host(socketPath, factory);

    v1::Utf8String error;
    if (!host.start(&error)) {
        std::cerr << "failed to start sidecar host: " << error << '\n';
        return 1;
    }

    while (g_running.load()) {
        if (!host.pollOnce(std::chrono::milliseconds(250), &error)) {
            std::cerr << "poll failed: " << error << '\n';
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        if (auto *adapter = dynamic_cast<FritzIpcSidecar *>(host.adapter()))
            adapter->tick();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }

    host.stop();
    std::cerr << "stopping phi_adapter_fritz_ipc" << '\n';
    return 0;
}
