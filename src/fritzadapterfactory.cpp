// src/adapters/plugins/fritz/fritzadapterfactory.cpp

#include "fritzadapterfactory.h"

#include <QAuthenticator>
#include <QEventLoop>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QXmlStreamReader>

#include "fritzadapter.h"

namespace phicore::adapter {

namespace {

constexpr const char *kHostsServiceType = "urn:dslforum-org:service:Hosts:1";
constexpr const char *kHostsControlPath = "/upnp/control/hosts";
constexpr const char *kDeviceInfoServiceType = "urn:dslforum-org:service:DeviceInfo:1";
constexpr const char *kDeviceInfoControlPath = "/upnp/control/deviceinfo";

static const QByteArray kFritzIconSvg = QByteArrayLiteral(
    "<svg width=\"24\" height=\"24\" viewBox=\"0 0 24 24\" xmlns=\"http://www.w3.org/2000/svg\" role=\"img\" aria-label=\"FRITZ!Box logo\">\n"
    "  <rect x=\"4\" y=\"4\" width=\"16\" height=\"16\" rx=\"2\" fill=\"#FFD84D\" transform=\"rotate(45 12 12)\"/>\n"
    "  <text x=\"12\" y=\"15\" text-anchor=\"middle\" font-family=\"'Geist', 'Inter', 'Arial', sans-serif\" font-weight=\"700\" font-size=\"8.5\" fill=\"#D94A4A\">FRITZ!</text>\n"
    "</svg>\n"
);

QString normalizeMac(const QString &mac)
{
    return mac.trimmed().toLower();
}

AdapterConfigOptionList buildTrackedOptions(const QJsonObject &meta)
{
    AdapterConfigOptionList options;
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
                options.push_back({ mac, mac });
            } else if (entry.isObject()) {
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
                options.push_back({ mac, label });
            }
        }
    }

    const QJsonValue trackedValue = meta.value(QStringLiteral("trackedMacs"));
    if (trackedValue.isArray()) {
        const QJsonArray arr = trackedValue.toArray();
        for (const QJsonValue &entry : arr) {
            if (!entry.isString())
                continue;
            const QString mac = normalizeMac(entry.toString());
            if (mac.isEmpty() || seen.contains(mac))
                continue;
            seen.insert(mac);
            options.push_back({ mac, mac });
        }
    } else if (trackedValue.isString()) {
        const QString mac = normalizeMac(trackedValue.toString());
        if (!mac.isEmpty() && !seen.contains(mac)) {
            seen.insert(mac);
            options.push_back({ mac, mac });
        }
    }

    return options;
}

QByteArray buildSoapEnvelope(const QString &serviceType, const QString &action)
{
    QString body;
    body += QStringLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?>");
    body += QStringLiteral("<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" ");
    body += QStringLiteral("s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">");
    body += QStringLiteral("<s:Body>");
    body += QStringLiteral("<u:%1 xmlns:u=\"%2\"></u:%1>").arg(action, serviceType);
    body += QStringLiteral("</s:Body>");
    body += QStringLiteral("</s:Envelope>");
    return body.toUtf8();
}

} // namespace

QByteArray FritzAdapterFactory::icon() const
{
    return kFritzIconSvg;
}

AdapterCapabilities FritzAdapterFactory::capabilities() const
{
    AdapterCapabilities caps;
    caps.required = AdapterRequirement::UsesRetryInterval;
    caps.flags |= AdapterFlag::AdapterFlagSupportsDiscovery;
    caps.flags |= AdapterFlag::AdapterFlagSupportsProbe;
    caps.flags |= AdapterFlag::AdapterFlagRequiresPolling;
    caps.defaults.insert(QStringLiteral("pollIntervalMs"), 5000);
    caps.defaults.insert(QStringLiteral("retryIntervalMs"), 10000);

    AdapterActionDescriptor browse;
    browse.id = QStringLiteral("browseHosts");
    browse.label = QStringLiteral("Browse WLAN");
    browse.description = QStringLiteral("Fetch current WLAN/LAN clients");
    caps.instanceActions.push_back(browse);
    AdapterActionDescriptor settings;
    settings.id = QStringLiteral("settings");
    settings.label = QStringLiteral("Settings");
    settings.description = QStringLiteral("Edit tracked devices and router options.");
    settings.hasForm = true;
    caps.instanceActions.push_back(settings);

    return caps;
}

discovery::DiscoveryQueryList FritzAdapterFactory::discoveryQueries() const
{
    discovery::DiscoveryQuery tr64;
    tr64.kind = discovery::DiscoveryKind::Mdns;
    tr64.mdnsServiceType = QStringLiteral("_tr064._tcp");
    tr64.defaultPort = 49000;

    discovery::DiscoveryQuery fbox;
    fbox.kind = discovery::DiscoveryKind::Mdns;
    fbox.mdnsServiceType = QStringLiteral("_fbox._tcp");
    fbox.defaultPort = 49000;

    return { tr64, fbox };
}

AdapterConfigSchema FritzAdapterFactory::configSchema(const Adapter &info) const
{
    AdapterConfigSchema schema;
    schema.title = QStringLiteral("FRITZ!Box");
    schema.description = QStringLiteral("Connect via TR-064 to track network clients.");

    AdapterConfigField hostField;
    hostField.key = QStringLiteral("host");
    hostField.label = QStringLiteral("Host");
    hostField.placeholder = QStringLiteral("fritz.box");
    hostField.type = AdapterConfigFieldType::Hostname;
    hostField.flags = AdapterConfigFieldFlag::Required;
    schema.fields.push_back(hostField);

    AdapterConfigField portField;
    portField.key = QStringLiteral("port");
    portField.label = QStringLiteral("Port");
    portField.type = AdapterConfigFieldType::Port;
    portField.defaultValue = 49000;
    schema.fields.push_back(portField);

    AdapterConfigField userField;
    userField.key = QStringLiteral("user");
    userField.label = QStringLiteral("Username");
    userField.type = AdapterConfigFieldType::String;
    userField.flags = AdapterConfigFieldFlag::Required;
    schema.fields.push_back(userField);

    AdapterConfigField pwField;
    pwField.key = QStringLiteral("password");
    pwField.label = QStringLiteral("Password");
    pwField.type = AdapterConfigFieldType::Password;
    pwField.flags = AdapterConfigFieldFlag::Required | AdapterConfigFieldFlag::Secret;
    schema.fields.push_back(pwField);

    AdapterConfigField pollField;
    pollField.key = QStringLiteral("pollIntervalMs");
    pollField.label = QStringLiteral("Poll interval");
    pollField.type = AdapterConfigFieldType::Integer;
    pollField.defaultValue = 5000;
    schema.fields.push_back(pollField);

    AdapterConfigField retryField;
    retryField.key = QStringLiteral("retryIntervalMs");
    retryField.label = QStringLiteral("Retry interval");
    retryField.description = QStringLiteral("Reconnect interval while the router is offline.");
    retryField.type = AdapterConfigFieldType::Integer;
    retryField.defaultValue = 10000;
    schema.fields.push_back(retryField);

    AdapterConfigField trackedField;
    trackedField.key = QStringLiteral("trackedMacs");
    trackedField.label = QStringLiteral("Tracked devices");
    trackedField.description = QStringLiteral("Select WLAN/LAN clients to track.");
    trackedField.type = AdapterConfigFieldType::Select;
    trackedField.flags = AdapterConfigFieldFlag::Multi | AdapterConfigFieldFlag::InstanceOnly;
    trackedField.options = buildTrackedOptions(info.meta);
    trackedField.actionId = QStringLiteral("browseHosts");
    trackedField.actionLabel = QStringLiteral("Browse WLAN");
    trackedField.meta.insert(QStringLiteral("parentAction"), QStringLiteral("settings"));
    schema.fields.push_back(trackedField);

    return schema;
}

ActionResponse FritzAdapterFactory::invokeFactoryAction(const QString &actionId,
                                                        Adapter &infoInOut,
                                                        const QJsonObject &params) const
{
    ActionResponse resp;
    QString errorString;
    Q_UNUSED(params);
    if (actionId != QLatin1String("probe")) {
        resp.status = CmdStatus::NotImplemented;
        resp.error = QStringLiteral("Unsupported action");
        return resp;
    }

    QString host = infoInOut.host.trimmed();
    if (host.isEmpty())
        host = infoInOut.ip.trimmed();
    if (host.isEmpty()) {
        resp.status = CmdStatus::InvalidArgument;
        resp.error = QStringLiteral("Host is required");
        return resp;
    }

    const quint16 port = infoInOut.port > 0 ? infoInOut.port : 49000;
    const QString scheme =
        (infoInOut.flags & AdapterFlag::AdapterFlagUseTls) ? QStringLiteral("https") : QStringLiteral("http");
    const QString baseUrl = QStringLiteral("%1://%2:%3").arg(scheme, host).arg(port);

    QNetworkAccessManager manager;
    QObject::connect(&manager, &QNetworkAccessManager::authenticationRequired,
                     [&infoInOut](QNetworkReply *, QAuthenticator *auth) {
        if (!infoInOut.user.trimmed().isEmpty())
            auth->setUser(infoInOut.user.trimmed());
        auth->setPassword(infoInOut.pw);
    });

    auto sendSoapAction = [&](const QString &controlPath,
                              const QString &serviceType,
                              const QString &action,
                              QByteArray &payloadOut,
                              QString &errorOut) -> bool {
        QNetworkRequest request(QUrl(baseUrl + controlPath));
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("text/xml; charset=\"utf-8\""));
        const QString soapAction = QStringLiteral("\"%1#%2\"")
            .arg(serviceType, action);
        request.setRawHeader("SOAPAction", soapAction.toUtf8());

        QNetworkReply *reply = manager.post(request, buildSoapEnvelope(serviceType, action));

        QTimer timeout;
        timeout.setSingleShot(true);
        timeout.start(3000);
        QEventLoop loop;
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();

        if (!timeout.isActive()) {
            reply->abort();
            errorOut = QStringLiteral("Connection timed out");
            reply->deleteLater();
            return false;
        }

        payloadOut = reply->readAll();
        const QNetworkReply::NetworkError error = reply->error();
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString replyError = reply->errorString();
        reply->deleteLater();

        if (error != QNetworkReply::NoError) {
            const QString body = QString::fromUtf8(payloadOut).trimmed();
            const QString combined = body.isEmpty() ? replyError : body;
            if (combined.contains(QStringLiteral("401 Unauthorized"), Qt::CaseInsensitive)
                || combined.contains(QStringLiteral("ERR_NONE"), Qt::CaseInsensitive)) {
                errorOut = QStringLiteral("Invalid credentials");
                return false;
            }
            errorOut = body.isEmpty()
                ? replyError
                : QStringLiteral("HTTP %1: %2").arg(statusCode).arg(body);
            return false;
        }

        return true;
    };

    QByteArray payload;
    QString probeError;
    if (!sendSoapAction(QString::fromLatin1(kDeviceInfoControlPath),
                        QString::fromLatin1(kDeviceInfoServiceType),
                        QStringLiteral("GetInfo"),
                        payload,
                        probeError)) {
        errorString = probeError;
        resp.status = CmdStatus::Failure;
        resp.error = errorString;
        return resp;
    }

    resp.status = CmdStatus::Success;
    return resp;
}

AdapterInterface *FritzAdapterFactory::create(QObject *parent)
{
    return new FritzAdapter(parent);
}

bool FritzAdapterFactory::parseHostListPath(const QByteArray &payload, QString &path, QString &error)
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

} // namespace phicore::adapter
