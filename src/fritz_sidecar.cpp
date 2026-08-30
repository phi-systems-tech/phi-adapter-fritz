#include "fritz_sidecar.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QtGlobal>

#include "fritz_http.h"
#include "fritz_runtime_convert.h"
#include "fritz_schema.h"
#include "fritz_tr064.h"

namespace phicore::fritz::ipc {

namespace v1 = phicore::adapter::v1;
namespace sdk = phicore::adapter::sdk;

namespace {

class FritzIpcInstance final : public sdk::AdapterInstance
{
public:
    bool start() override
    {
        m_started = false;
        m_knownDevices.clear();
        m_routerEmitted = false;
        m_routerName.clear();
        m_routerFirmware.clear();
        m_lastPollError.clear();
        m_lastPollErrorLogMs = 0;
        m_nextPollDueMs = 0;
        setConnected(false);

        // Created here, on this instance's execution thread, so the timer fires
        // on the same thread that owns m_http and runs every other instance
        // callback. Driving tick() from the host thread instead would race with
        // those callbacks over the whole instance state.
        m_pollTimer = std::make_unique<QTimer>();
        QObject::connect(m_pollTimer.get(), &QTimer::timeout, [this]() { tick(); });
        m_pollTimer->start(kTickIntervalMs);
        return true;
    }

    void stop() override
    {
        m_started = false;

        // This can arrive while one of our own calls is still on the stack.
        // A request runs a nested event loop, and a nested loop dispatches
        // whatever is queued for this thread - including the queued stop. What
        // used to happen then: the reset below freed the network manager, the
        // loop returned, and the frame waiting on the reply read through freed
        // memory. Measured as a SIGSEGV on roughly every second core restart.
        //
        // So the objects are left alone while a request is in flight. Nothing
        // keeps running because of it: the cancel probe already reports the
        // stop, so the loop gives up within one poll interval, and both objects
        // are destroyed with the instance - after the execution backend has
        // joined, which is the moment that guarantees nobody is inside them.
        if (m_http && m_http->busy()) {
            if (m_pollTimer)
                m_pollTimer->stop();
            sdk::AdapterInstance::stop();
            return;
        }

        // Both are thread-affine and belong to this thread; the SDK calls stop()
        // on it before the execution backend goes away.
        m_pollTimer.reset();
        m_http.reset();
        sdk::AdapterInstance::stop();
    }

    void onConfigChanged(const sdk::ConfigChangedRequest &request) override
    {
        AdapterInstance::onConfigChanged(request);
        m_info = request.adapter;
        m_meta = parseJsonObject(request.adapter.metaJson);
        refreshConfig();
        m_started = true;
        ensureHttp();

        std::cerr << "fritz-ipc config.changed adapterId=" << request.adapterId
                  << " externalId=" << m_info.externalId
                  << " pluginType=" << m_info.pluginType
                  << " tracked=" << m_trackedMacs.size()
                  << " known=" << knownHostMapFromMeta(m_meta).size()
                  << '\n';

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

    void onChannelInvoke(const sdk::ChannelInvokeRequest &request) override
    {
        v1::CmdResponse resp;
        resp.id = request.cmdId;
        resp.tsMs = nowMs();

        auto submitResponse = [this, &resp]() {
            v1::Utf8String err;
            if (!sendResult(resp, &err))
                std::cerr << "fritz-ipc onChannelInvoke sendResult failed: " << err << '\n';
        };

        if (request.deviceExternalId != kRouterDeviceId) {
            resp.status = v1::CmdStatus::NotSupported;
            resp.error = "Channel only supported for router device";
            submitResponse();
            return;
        }

        const auto enabled = scalarToBool(request.value);
        if (!enabled.has_value()) {
            resp.status = v1::CmdStatus::InvalidArgument;
            resp.error = "Expected boolean value";
            submitResponse();
            return;
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
            submitResponse();
            return;
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
            submitResponse();
            return;
        }

        resp.status = v1::CmdStatus::NotSupported;
        resp.error = "Channel not supported";
        submitResponse();
    }

    void onAdapterActionInvoke(const sdk::AdapterActionInvokeRequest &request) override
    {
        v1::ActionResponse resp;
        resp.id = request.cmdId;
        resp.tsMs = nowMs();

        auto submitResponse = [this, &resp]() {
            v1::Utf8String err;
            if (!sendResult(resp, &err))
                std::cerr << "fritz-ipc onAdapterActionInvoke sendResult failed: " << err << '\n';
        };

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
            submitResponse();
            return;
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
                submitResponse();
                return;
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
            submitResponse();
            return;
        }

        if (actionId == QLatin1String("probe")) {
            const QJsonObject params = parseJsonObject(request.paramsJson);
            const QJsonObject factoryAdapter = resolveFactoryAdapterFromParams(params);
            const QString host = resolveProbeHost(factoryAdapter);
            const quint16 port = resolveProbePort(factoryAdapter);
            const QString user = resolveProbeUser(factoryAdapter);
            const QString password = resolveProbePassword(factoryAdapter);
            const bool useTls = resolveProbeUseTls(factoryAdapter);
            const QString endpoint = QStringLiteral("%1://%2:%3")
                .arg(useTls ? QStringLiteral("https") : QStringLiteral("http"), host)
                .arg(port > 0 ? port : static_cast<quint16>(kDefaultTr064Port));

            std::cerr << "fritz-ipc probe endpoint=" << endpoint.toStdString()
                      << " userSet=" << (!user.isEmpty() ? "true" : "false")
                      << '\n';

            if (host.isEmpty()) {
                resp.status = v1::CmdStatus::InvalidArgument;
                resp.error = "Probe requires host or ip";
                submitResponse();
                return;
            }

            QString error;
            if (probeConnection(host, port, user, password, useTls, &error)) {
                resp.status = v1::CmdStatus::Success;
                resp.resultType = v1::ActionResultType::String;
                resp.resultValue = QStringLiteral("%1:%2").arg(host).arg(port).toStdString();
            } else {
                resp.status = v1::CmdStatus::Failure;
                resp.error = (error.isEmpty()
                                  ? QStringLiteral("Probe failed (%1)").arg(endpoint)
                                  : QStringLiteral("%1 (%2)").arg(error, endpoint))
                                 .toStdString();
                resp.errorContext = "factory.action";
            }
            submitResponse();
            return;
        }

        resp.status = v1::CmdStatus::NotSupported;
        resp.error = "Adapter action not supported";
        submitResponse();
    }

    void onDeviceNameUpdate(const sdk::DeviceNameUpdateRequest &request) override
    {
        v1::CmdResponse resp;
        resp.id = request.cmdId;
        resp.status = v1::CmdStatus::NotImplemented;
        resp.error = "Device rename not supported";
        resp.tsMs = nowMs();

        v1::Utf8String error;
        if (!sendResult(resp, &error))
            std::cerr << "fritz-ipc onDeviceNameUpdate sendResult failed: " << error << '\n';
    }

    void onDeviceEffectInvoke(const sdk::DeviceEffectInvokeRequest &request) override
    {
        v1::CmdResponse resp;
        resp.id = request.cmdId;
        resp.status = v1::CmdStatus::NotImplemented;
        resp.error = "Device effect not supported";
        resp.tsMs = nowMs();

        v1::Utf8String error;
        if (!sendResult(resp, &error))
            std::cerr << "fritz-ipc onDeviceEffectInvoke sendResult failed: " << error << '\n';
    }

    void onSceneInvoke(const sdk::SceneInvokeRequest &request) override
    {
        v1::CmdResponse resp;
        resp.id = request.cmdId;
        resp.status = v1::CmdStatus::NotImplemented;
        resp.error = "Scene invocation not supported";
        resp.tsMs = nowMs();

        v1::Utf8String error;
        if (!sendResult(resp, &error))
            std::cerr << "fritz-ipc onSceneInvoke sendResult failed: " << error << '\n';
    }

private:
    // Poll due-times are checked at this granularity; the actual interval comes
    // from pollIntervalMs / retryIntervalMs via scheduleNextPoll().
    static constexpr int kTickIntervalMs = 100;
    // How often a nested request loop checks whether the host asked us to stop.
    static constexpr int kCancelPollIntervalMs = 50;

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


    // Creates the client on first use and re-pushes credentials every time, so
    // a changed user/password takes effect on the next request - the previous
    // QNetworkAccessManager read them from m_info at challenge time and got
    // that for free.
    void ensureHttp()
    {
        if (!m_http) {
            m_http = std::make_unique<FritzHttp>();
            m_http->setCancelProbe([this]() { return stopRequested(); });
        }
        m_http->setCredentials(QString::fromStdString(m_info.user),
                               QString::fromStdString(m_info.password));
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
        if (m_tr064Port > 0)
            return m_tr064Port;
        if (m_info.port > 0)
            return m_info.port;
        return static_cast<quint16>(kDefaultTr064Port);
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
        quint16 port = parsePortValue(factoryAdapter.value(QStringLiteral("tr064Port")));
        if (port == 0) {
            const QJsonObject meta = factoryAdapter.value(QStringLiteral("meta")).toObject();
            port = parsePortValue(meta.value(QStringLiteral("tr064Port")));
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
        QJsonObject resolved = params.value(QStringLiteral("factoryAdapter")).toObject();
        if (resolved.isEmpty())
            return params;

        // Form values are passed at top-level params and must override the
        // persisted/discovered candidate values in factoryAdapter.
        for (auto it = params.constBegin(); it != params.constEnd(); ++it) {
            if (it.key() == QLatin1String("factoryAdapter"))
                continue;
            resolved.insert(it.key(), it.value());
        }
        return resolved;
    }

    bool probeConnection(const QString &host,
                         quint16 port,
                         const QString &user,
                         const QString &password,
                         bool useTls,
                         QString *error)
    {
        // A throwaway client: the probe validates credentials that are not the
        // instance's own yet. It still needs the cancel probe, or a shutdown
        // waits out the request timeout (F-33).
        FritzHttp http;
        http.setCredentials(user, password);
        http.setCancelProbe([this]() { return stopRequested(); });

        const QString baseUrl = QStringLiteral("%1://%2:%3")
            .arg(useTls ? QStringLiteral("https") : QStringLiteral("http"), host)
            .arg(port > 0 ? port : static_cast<quint16>(kDefaultTr064Port));

        const HttpResult result = http.sendSoap(
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
        ensureHttp();

        const QString baseUrl = resolveBaseUrl();
        if (baseUrl.isEmpty()) {
            if (error)
                *error = QStringLiteral("Host is required");
            return false;
        }

        const HttpResult countResult = m_http->sendSoap(
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
            // One SOAP round trip per host: without this the loop would keep
            // going for total x timeoutMs after a stop request (F-33).
            if (stopRequested()) {
                if (error)
                    *error = QStringLiteral("Cancelled: adapter is stopping");
                return false;
            }
            const HttpResult entryResult = m_http->sendSoap(
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
        ensureHttp();

        const QString baseUrl = resolveBaseUrl();
        if (baseUrl.isEmpty()) {
            if (error)
                *error = QStringLiteral("Host is required");
            return false;
        }

        const HttpResult listPathResult = m_http->sendSoap(
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
        const HttpResult listResult = m_http->sendGet(listUrl);
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
        ensureHttp();

        const QString baseUrl = resolveBaseUrl();
        if (baseUrl.isEmpty()) {
            if (error)
                *error = QStringLiteral("Host is required");
            return false;
        }

        HttpResult deviceInfo = m_http->sendSoap(
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

        HttpResult updateInfo = m_http->sendSoap(
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

        HttpResult wlan24 = m_http->sendSoap(
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

        HttpResult wlan5 = m_http->sendSoap(
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

        HttpResult wan = m_http->sendSoap(
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
        ensureHttp();

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

        const HttpResult result = m_http->sendSoap(
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
        const quint16 tr064Port = parsePortValue(m_meta.value(QStringLiteral("tr064Port")));
        m_tr064Port = tr064Port > 0 ? tr064Port : static_cast<quint16>(kDefaultTr064Port);

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
            logPollError(hostError);
            return;
        }

        QList<HostEntry> hosts;
        QString hostFetchError;
        if (!fetchHostSnapshot(hosts, &hostFetchError)) {
            setConnected(false);
            logPollError(hostFetchError);
            return;
        }

        setConnected(true);
        handleHostSnapshot(hosts);

        RouterSnapshot snapshot;
        QString snapshotError;
        if (fetchRouterSnapshot(snapshot, &snapshotError))
            applyRouterSnapshot(snapshot);

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


private:
    std::unique_ptr<FritzHttp> m_http;
    std::unique_ptr<QTimer> m_pollTimer;
    v1::Adapter m_info;
    QJsonObject m_meta;
    QSet<QString> m_trackedMacs;
    QSet<QString> m_knownDevices;

    bool m_connected = false;
    bool m_started = false;
    bool m_routerEmitted = false;

    int m_pollIntervalMs = 5000;
    int m_retryIntervalMs = 10000;
    quint16 m_tr064Port = static_cast<quint16>(kDefaultTr064Port);
    std::int64_t m_nextPollDueMs = 0;
    std::int64_t m_lastPollErrorLogMs = 0;

    QString m_routerName;
    QString m_routerFirmware;
    QString m_lastPollError;
};

} // namespace

std::unique_ptr<sdk::AdapterInstance> makeInstance()
{
    return std::make_unique<FritzIpcInstance>();
}

} // namespace phicore::fritz::ipc
