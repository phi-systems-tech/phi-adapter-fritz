// src/adapters/plugins/fritz/fritzadapter.cpp

#include "fritzadapter.h"

#include <QAuthenticator>
#include <QDateTime>
#include <QJsonArray>
#include <QMap>
#include <QNetworkRequest>
#include <QSharedPointer>
#include <QUrl>
#include <QXmlStreamReader>

Q_LOGGING_CATEGORY(adapterLog, "phi-core.adapters.fritz")

namespace phicore::adapter {

namespace {

constexpr const char *kHostsServiceType = "urn:dslforum-org:service:Hosts:1";
constexpr const char *kHostsControlPath = "/upnp/control/hosts";
constexpr const char *kDeviceInfoServiceType = "urn:dslforum-org:service:DeviceInfo:1";
constexpr const char *kDeviceInfoControlPath = "/upnp/control/deviceinfo";
constexpr const char *kWanCommonServiceType = "urn:dslforum-org:service:WANCommonInterfaceConfig:1";
constexpr const char *kWanCommonControlPath = "/upnp/control/wancommonifconfig";
constexpr const char *kWlan24ServiceType = "urn:dslforum-org:service:WLANConfiguration:1";
constexpr const char *kWlan24ControlPath = "/upnp/control/wlanconfig1";
constexpr const char *kWlan5ServiceType = "urn:dslforum-org:service:WLANConfiguration:2";
constexpr const char *kWlan5ControlPath = "/upnp/control/wlanconfig2";
constexpr auto kDeviceSoftwareUpdateChannelId = "device_software_update";
constexpr auto kRouterDeviceId = "router";

QString toLowerTrimmed(const QString &value)
{
    return value.trimmed().toLower();
}

bool isTruthy(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    return normalized == QLatin1String("1") || normalized == QLatin1String("true");
}

QString normalizeMac(const QString &mac)
{
    return mac.trimmed().toLower();
}

QString toSoapBoolean(bool enabled)
{
    return enabled ? QStringLiteral("1") : QStringLiteral("0");
}

QMap<QString, QJsonObject> buildKnownHostMap(const QJsonObject &meta)
{
    QMap<QString, QJsonObject> map;
    const QJsonValue knownValue = meta.value(QStringLiteral("knownHosts"));
    if (!knownValue.isArray()) {
        return map;
    }
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

} // namespace

FritzAdapter::FritzAdapter(QObject *parent)
    : AdapterInterface(parent)
{
}

FritzAdapter::~FritzAdapter()
{
    stop();
}

bool FritzAdapter::start(QString &errorString)
{
    errorString.clear();
    refreshConfig();

    const QString ip = adapter().ip.trimmed();
    if (ip.isEmpty()) {
        qCWarning(adapterLog) << "FritzAdapter: IP not configured; staying disconnected";
    }

    m_network = new QNetworkAccessManager(this);
    connect(m_network, &QNetworkAccessManager::authenticationRequired,
            this, [this](QNetworkReply *, QAuthenticator *auth) {
        const QString user = adapter().user.trimmed();
        const QString pw = adapter().pw;
        if (!user.isEmpty())
            auth->setUser(user);
        auth->setPassword(pw);
    });

    m_pollTimer = new QTimer(this);
    m_pollTimer->setSingleShot(false);
    updatePollInterval();
    connect(m_pollTimer, &QTimer::timeout, this, &FritzAdapter::pollOnce);

    m_pollTimer->start();
    pollOnce();

    return true;
}

void FritzAdapter::stop()
{
    if (m_pollTimer) {
        m_pollTimer->stop();
        m_pollTimer->deleteLater();
        m_pollTimer = nullptr;
    }
    clearReplies();
    if (m_network) {
        m_network->deleteLater();
        m_network = nullptr;
    }
    setConnected(false);
    m_routerEmitted = false;
    m_routerName.clear();
}

void FritzAdapter::requestFullSync()
{
    m_pendingFullSync = true;
    pollOnce();
}

void FritzAdapter::adapterConfigUpdated()
{
    refreshConfig();
    pollOnce();
}

void FritzAdapter::updateChannelState(const QString &deviceExternalId,
                                      const QString &channelExternalId,
                                      const QVariant &value,
                                      CmdId cmdId)
{
    if (cmdId == 0)
        return;

    const QString deviceId = deviceExternalId.trimmed();
    const QString channelId = channelExternalId.trimmed();
    if (deviceId != QLatin1String(kRouterDeviceId)) {
        AdapterInterface::updateChannelState(deviceExternalId, channelExternalId, value, cmdId);
        return;
    }

    if (channelId == QLatin1String("wlan_24_enabled")) {
        setWlanEnabled(24, value.toBool(), cmdId);
        return;
    }
    if (channelId == QLatin1String("wlan_5_enabled")) {
        setWlanEnabled(5, value.toBool(), cmdId);
        return;
    }

    AdapterInterface::updateChannelState(deviceExternalId, channelExternalId, value, cmdId);
}

void FritzAdapter::invokeAdapterAction(const QString &actionId, const QJsonObject &params, CmdId cmdId)
{
    if (cmdId == 0)
        return;

    const QString trimmed = actionId.trimmed();
    if (trimmed == QLatin1String("settings")) {
        AdapterInterface::invokeAdapterAction(trimmed, params, cmdId);
        return;
    }
    if (trimmed != QLatin1String("browseHosts")) {
        emitActionError(cmdId, QStringLiteral("Unsupported action"));
        return;
    }

    fetchHostSnapshot([this, cmdId](bool ok, QList<HostEntry> hosts, const QString &error) {
        if (!ok) {
            emitActionError(cmdId, error);
            return;
        }

        QStringList macs;
        macs.reserve(hosts.size());
        QMap<QString, QJsonObject> knownMap = buildKnownHostMap(adapter().meta);
        QMap<QString, QJsonObject> nextMap;
        for (const HostEntry &entry : std::as_const(hosts)) {
            if (!entry.active)
                continue;
            const QString mac = normalizeMac(entry.mac);
            if (mac.isEmpty())
                continue;
            macs.push_back(mac);
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
        macs.removeDuplicates();
        macs.sort(Qt::CaseInsensitive);
        if (!nextMap.isEmpty()) {
            QJsonArray knownHosts;
            for (auto it = nextMap.cbegin(); it != nextMap.cend(); ++it) {
                knownHosts.append(it.value());
            }
            QJsonObject patch;
            patch.insert(QStringLiteral("knownHosts"), knownHosts);
            emit adapterMetaUpdated(patch);
        }

        ActionResponse resp;
        resp.id = cmdId;
        resp.status = CmdStatus::Success;
        resp.resultType = ActionResultType::StringList;
        resp.resultValue = macs;
        resp.tsMs = QDateTime::currentMSecsSinceEpoch();
        emit actionResult(resp);
    });
}

void FritzAdapter::pollOnce()
{
    QString resolveError;
    if (!ensureHostAvailable(resolveError)) {
        setConnected(false);
        if (m_pendingFullSync) {
            m_pendingFullSync = false;
            emit fullSyncCompleted();
        }
        logPollError(resolveError);
        return;
    }

    fetchHostSnapshot([this](bool ok, QList<HostEntry> hosts, const QString &error) {
        if (!ok) {
            setConnected(false);
            if (m_pendingFullSync) {
                m_pendingFullSync = false;
                emit fullSyncCompleted();
            }
            logPollError(error);
            return;
        }

        setConnected(true);

        handleHostSnapshot(hosts);
        fetchRouterSnapshot();

        if (m_pendingFullSync) {
            m_pendingFullSync = false;
            emit fullSyncCompleted();
        }
    });
}

void FritzAdapter::fetchRouterSnapshot()
{
    if (!m_network)
        return;

    const QString baseUrl = resolveBaseUrl();
    if (baseUrl.isEmpty())
        return;

    ensureRouterDevice();

    auto state = QSharedPointer<RouterSnapshot>::create();

    auto finalize = [this, state]() {
        applyRouterSnapshot(*state);
    };

    auto fetchWan = [this, baseUrl, state, finalize]() {
        QNetworkReply *reply = sendSoapRequest(
            baseUrl + QString::fromLatin1(kWanCommonControlPath),
            QString::fromLatin1(kWanCommonServiceType),
            QStringLiteral("GetAddonInfos"),
            {});
        trackReply(reply);
        connect(reply, &QNetworkReply::finished, this, [this, reply, state, finalize]() {
            const QByteArray payload = reply->readAll();
            const QNetworkReply::NetworkError error = reply->error();
            reply->deleteLater();

            if (error == QNetworkReply::NoError) {
                QString value;
                if (parseSoapValue(payload, QStringLiteral("NewByteSendRate"), value)) {
                    bool ok = false;
                    const double bytesPerSec = value.toDouble(&ok);
                    if (ok) {
                        state->hasTxRate = true;
                        state->txRateKbit = (bytesPerSec * 8.0) / 1000.0;
                    }
                }
                if (parseSoapValue(payload, QStringLiteral("NewByteReceiveRate"), value)) {
                    bool ok = false;
                    const double bytesPerSec = value.toDouble(&ok);
                    if (ok) {
                        state->hasRxRate = true;
                        state->rxRateKbit = (bytesPerSec * 8.0) / 1000.0;
                    }
                }
            }

            finalize();
        });
    };

    auto fetchWlan5 = [this, baseUrl, state, fetchWan]() {
        QNetworkReply *reply = sendSoapRequest(
            baseUrl + QString::fromLatin1(kWlan5ControlPath),
            QString::fromLatin1(kWlan5ServiceType),
            QStringLiteral("GetInfo"),
            {});
        trackReply(reply);
        connect(reply, &QNetworkReply::finished, this, [this, reply, state, fetchWan]() {
            const QByteArray payload = reply->readAll();
            const QNetworkReply::NetworkError error = reply->error();
            reply->deleteLater();

            if (error == QNetworkReply::NoError) {
                QString value;
                if (parseSoapValue(payload, QStringLiteral("NewEnable"), value)) {
                    state->hasWlan5 = true;
                    state->wlan5Enabled = isTruthy(value);
                }
            }

            fetchWan();
        });
    };

    auto fetchWlan24 = [this, baseUrl, state, fetchWlan5]() {
        QNetworkReply *reply = sendSoapRequest(
            baseUrl + QString::fromLatin1(kWlan24ControlPath),
            QString::fromLatin1(kWlan24ServiceType),
            QStringLiteral("GetInfo"),
            {});
        trackReply(reply);
        connect(reply, &QNetworkReply::finished, this, [this, reply, state, fetchWlan5]() {
            const QByteArray payload = reply->readAll();
            const QNetworkReply::NetworkError error = reply->error();
            reply->deleteLater();

            if (error == QNetworkReply::NoError) {
                QString value;
                if (parseSoapValue(payload, QStringLiteral("NewEnable"), value)) {
                    state->hasWlan24 = true;
                    state->wlan24Enabled = isTruthy(value);
                }
            }

            fetchWlan5();
        });
    };

    auto fetchUpdateInfo = [this, baseUrl, state, fetchWlan24]() {
        QNetworkReply *reply = sendSoapRequest(
            baseUrl + QString::fromLatin1(kDeviceInfoControlPath),
            QString::fromLatin1(kDeviceInfoServiceType),
            QStringLiteral("X_AVM-DE_GetAutoUpdateInfo"),
            {});
        trackReply(reply);
        connect(reply, &QNetworkReply::finished, this, [this, reply, state, fetchWlan24]() {
            const QByteArray payload = reply->readAll();
            const QNetworkReply::NetworkError error = reply->error();
            reply->deleteLater();

            if (error == QNetworkReply::NoError) {
                QString value;
                if (parseSoapValue(payload, QStringLiteral("NewUpdateAvailable"), value)) {
                    state->hasUpdateAvailable = true;
                    state->updateAvailable = isTruthy(value);
                }
            }

            fetchWlan24();
        });
    };

    QNetworkReply *deviceInfoReply = sendSoapRequest(
        baseUrl + QString::fromLatin1(kDeviceInfoControlPath),
        QString::fromLatin1(kDeviceInfoServiceType),
        QStringLiteral("GetInfo"),
        {});
    trackReply(deviceInfoReply);
    connect(deviceInfoReply, &QNetworkReply::finished, this, [this, deviceInfoReply, state, fetchUpdateInfo]() {
        const QByteArray payload = deviceInfoReply->readAll();
        const QNetworkReply::NetworkError error = deviceInfoReply->error();
        deviceInfoReply->deleteLater();

        if (error == QNetworkReply::NoError) {
            QString value;
            if (parseSoapValue(payload, QStringLiteral("NewUpTime"), value)) {
                bool ok = false;
                const qint64 uptime = value.toLongLong(&ok);
                if (ok) {
                    state->hasUptime = true;
                    state->uptimeSec = uptime;
                }
            }
            if (parseSoapValue(payload, QStringLiteral("NewSoftwareVersion"), value)) {
                const QString version = value.trimmed();
                if (!version.isEmpty()) {
                    state->hasSoftwareVersion = true;
                    state->softwareVersion = version;
                }
            }
            if (parseSoapValue(payload, QStringLiteral("NewFriendlyName"), value)) {
                const QString name = value.trimmed();
                if (!name.isEmpty())
                    state->friendlyName = name;
            } else if (parseSoapValue(payload, QStringLiteral("NewDeviceName"), value)) {
                const QString name = value.trimmed();
                if (!name.isEmpty())
                    state->friendlyName = name;
            }
        }

        fetchUpdateInfo();
    });
}

void FritzAdapter::applyRouterSnapshot(const RouterSnapshot &snapshot)
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

    const qint64 tsMs = QDateTime::currentMSecsSinceEpoch();
    if (snapshot.hasUptime)
        emit channelStateUpdated(QString::fromLatin1(kRouterDeviceId), QStringLiteral("uptime"), snapshot.uptimeSec, tsMs);
    if (snapshot.hasUpdateAvailable || snapshot.hasSoftwareVersion) {
        QJsonObject payload;
        QString status = QStringLiteral("Unknown");
        if (snapshot.hasUpdateAvailable) {
            status = snapshot.updateAvailable
                ? QStringLiteral("UpdateAvailable")
                : QStringLiteral("UpToDate");
        }
        payload.insert(QStringLiteral("status"), status);
        payload.insert(QStringLiteral("timestampMs"), tsMs);
        emit channelStateUpdated(QString::fromLatin1(kRouterDeviceId),
                                 QString::fromLatin1(kDeviceSoftwareUpdateChannelId),
                                 payload,
                                 tsMs);
    }
    if (snapshot.hasWlan24) {
        emit channelStateUpdated(QString::fromLatin1(kRouterDeviceId),
                                 QStringLiteral("wlan_24_enabled"),
                                 snapshot.wlan24Enabled,
                                 tsMs);
    }
    if (snapshot.hasWlan5) {
        emit channelStateUpdated(QString::fromLatin1(kRouterDeviceId),
                                 QStringLiteral("wlan_5_enabled"),
                                 snapshot.wlan5Enabled,
                                 tsMs);
    }
    if (snapshot.hasTxRate) {
        emit channelStateUpdated(QString::fromLatin1(kRouterDeviceId),
                                 QStringLiteral("tx_rate"),
                                 snapshot.txRateKbit,
                                 tsMs);
    }
    if (snapshot.hasRxRate) {
        emit channelStateUpdated(QString::fromLatin1(kRouterDeviceId),
                                 QStringLiteral("rx_rate"),
                                 snapshot.rxRateKbit,
                                 tsMs);
    }
}

void FritzAdapter::ensureRouterDevice()
{
    if (m_routerEmitted)
        return;

    Device device;
    device.id = QString::fromLatin1(kRouterDeviceId);
    device.name = m_routerName.isEmpty() ? QStringLiteral("FRITZ!Box") : m_routerName;
    device.deviceClass = DeviceClass::Gateway;
    device.manufacturer = QStringLiteral("AVM");
    if (!m_routerFirmware.isEmpty())
        device.firmware = m_routerFirmware;

    emit deviceUpdated(device, buildRouterChannels());
    m_routerEmitted = true;
}

void FritzAdapter::setWlanEnabled(int band, bool enabled, CmdId cmdId)
{
    if (!m_network) {
        CmdResponse resp;
        resp.id = cmdId;
        resp.status = CmdStatus::Failure;
        resp.error = QStringLiteral("Network manager not initialized");
        resp.tsMs = QDateTime::currentMSecsSinceEpoch();
        emit cmdResult(resp);
        return;
    }

    const QString baseUrl = resolveBaseUrl();
    if (baseUrl.isEmpty()) {
        CmdResponse resp;
        resp.id = cmdId;
        resp.status = CmdStatus::Failure;
        resp.error = QStringLiteral("Host is required");
        resp.tsMs = QDateTime::currentMSecsSinceEpoch();
        emit cmdResult(resp);
        return;
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
        CmdResponse resp;
        resp.id = cmdId;
        resp.status = CmdStatus::Failure;
        resp.error = QStringLiteral("Unsupported WLAN band");
        resp.tsMs = QDateTime::currentMSecsSinceEpoch();
        emit cmdResult(resp);
        return;
    }

    QNetworkReply *reply = sendSoapRequest(
        baseUrl + controlPath,
        serviceType,
        QStringLiteral("SetEnable"),
        { { QStringLiteral("NewEnable"), toSoapBoolean(enabled) } });
    trackReply(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, cmdId]() {
        const QByteArray payload = reply->readAll();
        const QNetworkReply::NetworkError error = reply->error();
        const QString replyError = reply->errorString();
        reply->deleteLater();

        CmdResponse resp;
        resp.id = cmdId;
        resp.tsMs = QDateTime::currentMSecsSinceEpoch();
        if (error == QNetworkReply::NoError) {
            resp.status = CmdStatus::Success;
        } else if (isInvalidActionFault(payload)) {
            resp.status = CmdStatus::Failure;
            resp.error = QStringLiteral("WLAN configuration not available");
        } else {
            resp.status = CmdStatus::Failure;
            resp.error = replyError;
        }
        emit cmdResult(resp);
    });
}

ChannelList FritzAdapter::buildRouterChannels() const
{
    ChannelList channels;

    Channel uptime;
    uptime.id = QStringLiteral("uptime");
    uptime.name = QStringLiteral("Uptime");
    uptime.kind = ChannelKind::Unknown;
    uptime.dataType = ChannelDataType::Int;
    uptime.flags = ChannelFlagDefaultRead;
    uptime.unit = QStringLiteral("s");
    channels.push_back(uptime);

    Channel softwareUpdate;
    softwareUpdate.id = QString::fromLatin1(kDeviceSoftwareUpdateChannelId);
    softwareUpdate.name = QStringLiteral("Software Update");
    softwareUpdate.kind = ChannelKind::DeviceSoftwareUpdate;
    softwareUpdate.dataType = ChannelDataType::Enum;
    softwareUpdate.flags = ChannelFlagDefaultRead;
    channels.push_back(softwareUpdate);

    Channel wlan24;
    wlan24.id = QStringLiteral("wlan_24_enabled");
    wlan24.name = QStringLiteral("WLAN 2.4 GHz");
    wlan24.kind = ChannelKind::PowerOnOff;
    wlan24.dataType = ChannelDataType::Bool;
    wlan24.flags = ChannelFlagDefaultWrite;
    wlan24.meta.insert(QStringLiteral("forceLabel"), true);
    channels.push_back(wlan24);

    Channel wlan5;
    wlan5.id = QStringLiteral("wlan_5_enabled");
    wlan5.name = QStringLiteral("WLAN 5 GHz");
    wlan5.kind = ChannelKind::PowerOnOff;
    wlan5.dataType = ChannelDataType::Bool;
    wlan5.flags = ChannelFlagDefaultWrite;
    wlan5.meta.insert(QStringLiteral("forceLabel"), true);
    channels.push_back(wlan5);

    Channel txRate;
    txRate.id = QStringLiteral("tx_rate");
    txRate.name = QStringLiteral("TX rate");
    txRate.kind = ChannelKind::Unknown;
    txRate.dataType = ChannelDataType::Float;
    txRate.flags = ChannelFlagDefaultRead;
    txRate.unit = QStringLiteral("kbit/s");
    channels.push_back(txRate);

    Channel rxRate;
    rxRate.id = QStringLiteral("rx_rate");
    rxRate.name = QStringLiteral("RX rate");
    rxRate.kind = ChannelKind::Unknown;
    rxRate.dataType = ChannelDataType::Float;
    rxRate.flags = ChannelFlagDefaultRead;
    rxRate.unit = QStringLiteral("kbit/s");
    channels.push_back(rxRate);

    return channels;
}

void FritzAdapter::fetchHostSnapshot(std::function<void(bool, QList<HostEntry>, const QString &)> callback)
{
    if (!m_network) {
        callback(false, {}, QStringLiteral("Network manager not initialized"));
        return;
    }

    const QString baseUrl = resolveBaseUrl();
    if (baseUrl.isEmpty()) {
        callback(false, {}, QStringLiteral("Host is required"));
        return;
    }

    QNetworkReply *reply = sendSoapRequest(
        baseUrl + QString::fromLatin1(kHostsControlPath),
        QString::fromLatin1(kHostsServiceType),
        QStringLiteral("GetHostListPath"),
        {});

    trackReply(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, callback, baseUrl]() {
        const QByteArray payload = reply->readAll();
        const QNetworkReply::NetworkError error = reply->error();
        const int statusCode =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString errorText = reply->errorString();
        reply->deleteLater();

        if (error != QNetworkReply::NoError) {
            if (isInvalidActionFault(payload)) {
                fetchHostEntries(callback);
                return;
            }
            const QString body = QString::fromUtf8(payload).trimmed();
            const QString detail = body.isEmpty()
                ? errorText
                : QStringLiteral("HTTP %1: %2").arg(statusCode).arg(body);
            callback(false, {}, detail);
            return;
        }

        QString listPath;
        QString parseError;
        if (!parseHostListPath(payload, listPath, parseError)) {
            if (isInvalidActionFault(payload)) {
                fetchHostEntries(callback);
                return;
            }
            callback(false, {}, parseError);
            return;
        }
        if (listPath.isEmpty()) {
            callback(false, {}, QStringLiteral("Host list path missing"));
            return;
        }

        const QString listUrl = listPath.startsWith(QLatin1Char('/'))
            ? baseUrl + listPath
            : baseUrl + QLatin1Char('/') + listPath;

        QNetworkReply *listReply = sendGetRequest(listUrl);
        trackReply(listReply);
        connect(listReply, &QNetworkReply::finished, this, [listReply, callback, this]() {
            const QByteArray listPayload = listReply->readAll();
            const QNetworkReply::NetworkError listError = listReply->error();
            const int listStatusCode =
                listReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QString listErrorText = listReply->errorString();
            listReply->deleteLater();

            if (listError != QNetworkReply::NoError) {
                const QString body = QString::fromUtf8(listPayload).trimmed();
                const QString detail = body.isEmpty()
                    ? listErrorText
                    : QStringLiteral("HTTP %1: %2").arg(listStatusCode).arg(body);
                callback(false, {}, detail);
                return;
            }

            QList<HostEntry> hosts = parseHostList(listPayload);
            callback(true, hosts, QString());
        });
    });
}

void FritzAdapter::fetchHostEntries(std::function<void(bool, QList<HostEntry>, const QString &)> callback)
{
    if (!m_network) {
        callback(false, {}, QStringLiteral("Network manager not initialized"));
        return;
    }

    const QString baseUrl = resolveBaseUrl();
    if (baseUrl.isEmpty()) {
        callback(false, {}, QStringLiteral("Host is required"));
        return;
    }

    QNetworkReply *countReply = sendSoapRequest(
        baseUrl + QString::fromLatin1(kHostsControlPath),
        QString::fromLatin1(kHostsServiceType),
        QStringLiteral("GetHostNumberOfEntries"),
        {});

    trackReply(countReply);
    connect(countReply, &QNetworkReply::finished, this, [this, countReply, callback, baseUrl]() {
        const QByteArray payload = countReply->readAll();
        const QNetworkReply::NetworkError error = countReply->error();
        const QString errorText = countReply->errorString();
        countReply->deleteLater();

        if (error != QNetworkReply::NoError) {
            callback(false, {}, errorText);
            return;
        }

        QString countValue;
        if (!parseSoapValue(payload, QStringLiteral("NewHostNumberOfEntries"), countValue)) {
            callback(false, {}, QStringLiteral("Host count missing"));
            return;
        }
        bool ok = false;
        const int total = countValue.toInt(&ok);
        if (!ok || total <= 0) {
            callback(true, {}, QString());
            return;
        }

        struct FetchState {
            int index = 0;
            int total = 0;
            QList<HostEntry> hosts;
        };

        auto state = QSharedPointer<FetchState>::create();
        state->total = total;

        auto fetchNext = QSharedPointer<std::function<void()>>::create();
        *fetchNext = [this, baseUrl, state, callback, fetchNext]() {
            if (state->index >= state->total) {
                callback(true, state->hosts, QString());
                return;
            }

            const int currentIndex = state->index;
            QNetworkReply *entryReply = sendSoapRequest(
                baseUrl + QString::fromLatin1(kHostsControlPath),
                QString::fromLatin1(kHostsServiceType),
                QStringLiteral("GetGenericHostEntry"),
                { { QStringLiteral("NewIndex"), QString::number(currentIndex) } });

            trackReply(entryReply);
            connect(entryReply, &QNetworkReply::finished, this, [this, entryReply, state, callback, &fetchNext]() {
                const QByteArray payload = entryReply->readAll();
                const QNetworkReply::NetworkError error = entryReply->error();
                const QString errorText = entryReply->errorString();
                entryReply->deleteLater();

                if (error == QNetworkReply::NoError) {
                    HostEntry entry;
                    if (parseHostEntryFromSoap(payload, entry) && !entry.mac.isEmpty()) {
                        entry.mac = normalizeMac(entry.mac);
                        state->hosts.push_back(entry);
                    }
                } else {
                    callback(false, {}, errorText);
                    return;
                }

                state->index += 1;
                (*fetchNext)();
            });
        };

        (*fetchNext)();
    });
}

void FritzAdapter::handleHostSnapshot(const QList<HostEntry> &hosts)
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

void FritzAdapter::updateDeviceFromHost(const HostEntry &host)
{
    Device device;
    device.id = normalizeMac(host.mac);
    device.name = host.name.trimmed().isEmpty() ? device.id : host.name.trimmed();
    device.deviceClass = DeviceClass::Sensor;
    device.manufacturer = QStringLiteral("AVM");

    if (host.interfaceType.contains(QLatin1String("802.11"), Qt::CaseInsensitive) ||
        host.interfaceType.contains(QLatin1String("wlan"), Qt::CaseInsensitive)) {
        device.flags |= DeviceFlag::DeviceFlagWireless;
    }
    QJsonObject meta;
    if (!host.ip.isEmpty())
        meta.insert(QStringLiteral("ip"), host.ip);
    meta.insert(QStringLiteral("mac"), device.id);
    if (!host.interfaceType.isEmpty())
        meta.insert(QStringLiteral("interfaceType"), host.interfaceType);
    device.meta = meta;

    ChannelList channels;

    Channel online;
    online.id = QStringLiteral("online");
    online.name = QStringLiteral("Online");
    online.kind = ChannelKind::ConnectivityStatus;
    online.dataType = ChannelDataType::Enum;
    online.flags = ChannelFlagDefaultRead;
    channels.push_back(online);

    Channel rssi;
    if (host.hasSignal) {
        rssi.id = QStringLiteral("rssi");
        rssi.name = QStringLiteral("RSSI");
        rssi.kind = ChannelKind::SignalStrength;
        rssi.dataType = ChannelDataType::Float;
        rssi.flags = ChannelFlagDefaultRead;
        rssi.unit = QStringLiteral("dBm");
        rssi.minValue = -100.0;
        rssi.maxValue = 0.0;
        channels.push_back(rssi);
    }

    emit deviceUpdated(device, channels);

    const auto statusValue = host.active
        ? static_cast<int>(ConnectivityStatus::Connected)
        : static_cast<int>(ConnectivityStatus::Disconnected);
    emit channelStateUpdated(device.id, QStringLiteral("online"), statusValue, QDateTime::currentMSecsSinceEpoch());

    if (host.hasSignal) {
        emit channelStateUpdated(device.id, QStringLiteral("rssi"), host.signalDbm, QDateTime::currentMSecsSinceEpoch());
    }
}

void FritzAdapter::removeMissingDevices(const QSet<QString> &deviceIds)
{
    if (m_knownDevices.isEmpty())
        return;
    for (const QString &deviceId : std::as_const(m_knownDevices)) {
        if (!deviceIds.contains(deviceId))
            emit deviceRemoved(deviceId);
    }
}

void FritzAdapter::emitActionError(CmdId cmdId, const QString &error)
{
    ActionResponse resp;
    resp.id = cmdId;
    resp.status = CmdStatus::Failure;
    resp.error = error;
    resp.tsMs = QDateTime::currentMSecsSinceEpoch();
    emit actionResult(resp);
}

QString FritzAdapter::resolveBaseUrl() const
{
    const QString host = adapter().ip.trimmed();
    if (host.isEmpty())
        return QString();

    const quint16 port = adapter().port > 0 ? adapter().port : 49000;
    const QString scheme =
        (adapter().flags & AdapterFlag::AdapterFlagUseTls) ? QStringLiteral("https") : QStringLiteral("http");

    return QStringLiteral("%1://%2:%3").arg(scheme, host).arg(port);
}

void FritzAdapter::refreshConfig()
{
    const QJsonObject meta = adapter().meta;
    const int interval = meta.value(QStringLiteral("pollIntervalMs")).toInt(5000);
    if (interval >= 500) {
        m_pollIntervalMs = interval;
    }
    const int retry = meta.value(QStringLiteral("retryIntervalMs")).toInt(10000);
    if (retry >= 1000) {
        m_retryIntervalMs = retry;
    }

    m_trackedMacs.clear();
    const QJsonValue trackedValue = meta.value(QStringLiteral("trackedMacs"));
    if (trackedValue.isArray()) {
        const QJsonArray arr = trackedValue.toArray();
        for (const QJsonValue &entry : arr) {
            if (entry.isString()) {
                const QString mac = normalizeMac(entry.toString());
                if (!mac.isEmpty())
                    m_trackedMacs.insert(mac);
            }
        }
    } else if (trackedValue.isString()) {
        const QString mac = normalizeMac(trackedValue.toString());
        if (!mac.isEmpty())
            m_trackedMacs.insert(mac);
    }

    updatePollInterval();
}

void FritzAdapter::setConnected(bool connected)
{
    if (m_connected == connected)
        return;
    m_connected = connected;
    updatePollInterval();
    emit connectionStateChanged(m_connected);
}

void FritzAdapter::updatePollInterval()
{
    if (!m_pollTimer)
        return;
    const int interval = m_connected ? m_pollIntervalMs : m_retryIntervalMs;
    if (m_pollTimer->interval() != interval)
        m_pollTimer->setInterval(interval);
}

void FritzAdapter::logPollError(const QString &error)
{
    qCWarning(adapterLog).noquote()
        << QStringLiteral("FritzAdapter: host list failed: %1").arg(error);
}

bool FritzAdapter::ensureHostAvailable(QString &error)
{
    const QString ip = adapter().ip.trimmed();
    if (ip.isEmpty()) {
        error = QStringLiteral("IP is required");
        return false;
    }
    return true;
}

QNetworkReply *FritzAdapter::sendSoapRequest(const QString &url,
                                             const QString &serviceType,
                                             const QString &action,
                                             const QMap<QString, QString> &params)
{
    const QString soapAction = QStringLiteral("\"%1#%2\"").arg(serviceType, action);
    QString body;
    body += QStringLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?>");
    body += QStringLiteral("<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" ");
    body += QStringLiteral("s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">");
    body += QStringLiteral("<s:Body>");
    body += QStringLiteral("<u:%1 xmlns:u=\"%2\">").arg(action, serviceType);
    for (auto it = params.cbegin(); it != params.cend(); ++it) {
        body += QStringLiteral("<%1>%2</%1>").arg(it.key(), it.value());
    }
    body += QStringLiteral("</u:%1>").arg(action);
    body += QStringLiteral("</s:Body>");
    body += QStringLiteral("</s:Envelope>");

    QNetworkRequest request{ QUrl(url) };
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("text/xml; charset=\"utf-8\""));
    request.setRawHeader("SOAPAction", soapAction.toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    return m_network->post(request, body.toUtf8());
}

QNetworkReply *FritzAdapter::sendGetRequest(const QString &url)
{
    QNetworkRequest request{ QUrl(url) };
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    return m_network->get(request);
}

void FritzAdapter::trackReply(QNetworkReply *reply)
{
    if (!reply)
        return;
    m_pendingReplies.insert(reply);
    connect(reply, &QObject::destroyed, this, [this, reply]() {
        m_pendingReplies.remove(reply);
    });
}

void FritzAdapter::clearReplies()
{
    for (QNetworkReply *reply : std::as_const(m_pendingReplies)) {
        if (reply) {
            reply->abort();
            reply->deleteLater();
        }
    }
    m_pendingReplies.clear();
}

bool FritzAdapter::parseHostListPath(const QByteArray &payload, QString &path, QString &error) const
{
    QXmlStreamReader reader(payload);
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement() && reader.name() == QLatin1String("NewHostListPath")) {
            path = reader.readElementText().trimmed();
            return true;
        }
    }
    if (reader.hasError()) {
        error = reader.errorString();
        return false;
    }
    error = QStringLiteral("Host list path missing");
    return false;
}

bool FritzAdapter::parseSoapValue(const QByteArray &payload, const QString &key, QString &value) const
{
    QXmlStreamReader reader(payload);
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement() && reader.name() == key) {
            value = reader.readElementText().trimmed();
            return true;
        }
    }
    return false;
}

bool FritzAdapter::parseHostEntryFromSoap(const QByteArray &payload, HostEntry &entry) const
{
    QXmlStreamReader reader(payload);
    bool foundMac = false;
    while (!reader.atEnd()) {
        reader.readNext();
        if (!reader.isStartElement())
            continue;
        const QStringView name = reader.name();
        if (name == QLatin1String("NewMACAddress")) {
            entry.mac = reader.readElementText().trimmed();
            foundMac = true;
        } else if (name == QLatin1String("NewHostName")) {
            entry.name = reader.readElementText().trimmed();
        } else if (name == QLatin1String("NewIPAddress")) {
            entry.ip = reader.readElementText().trimmed();
        } else if (name == QLatin1String("NewActive")) {
            entry.active = isTruthy(reader.readElementText());
        } else if (name == QLatin1String("NewInterfaceType")) {
            entry.interfaceType = reader.readElementText().trimmed();
        } else if (name == QLatin1String("NewSignalStrength")) {
            const QString value = reader.readElementText().trimmed();
            bool ok = false;
            const int signal = value.toInt(&ok);
            if (ok) {
                entry.hasSignal = true;
                entry.signalDbm = signal;
            }
        }
    }
    return foundMac;
}

bool FritzAdapter::isInvalidActionFault(const QByteArray &payload) const
{
    const QByteArray lower = payload.toLower();
    return lower.contains("invalid action") || lower.contains("<errorcode>401</errorcode>");
}

QList<FritzAdapter::HostEntry> FritzAdapter::parseHostList(const QByteArray &payload) const
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

} // namespace phicore::adapter
