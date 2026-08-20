#include "fritz_tr064.h"

#include <QLatin1Char>
#include <QLatin1String>
#include <QStringList>
#include <QXmlStreamReader>

namespace phicore::fritz::ipc {

QString toSoapBoolean(bool enabled)
{
    return enabled ? QStringLiteral("1") : QStringLiteral("0");
}

bool isTruthy(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    return normalized == QLatin1String("1") || normalized == QLatin1String("true");
}

quint16 parsePortValue(const QJsonValue &value)
{
    int parsed = 0;
    bool ok = false;
    if (value.isDouble()) {
        parsed = value.toInt();
        ok = true;
    } else if (value.isString()) {
        parsed = value.toString().trimmed().toInt(&ok);
    }
    if (!ok || parsed <= 0 || parsed > 65535)
        return 0;
    return static_cast<quint16>(parsed);
}

QString normalizeMac(const QString &mac)
{
    return mac.trimmed().toLower();
}

QByteArray buildSoapEnvelope(const QString &serviceType,
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

bool parseSoapValue(const QByteArray &payload, const QString &key, QString *value)
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

bool parseHostListPath(const QByteArray &payload, QString *path, QString *error)
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

bool parseHostEntryFromSoap(const QByteArray &payload, HostEntry *entry)
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

QList<HostEntry> parseHostList(const QByteArray &payload)
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

bool isInvalidActionFault(const QByteArray &payload)
{
    const QByteArray lower = payload.toLower();
    return lower.contains("invalid action") || lower.contains("<errorcode>401</errorcode>");
}

} // namespace phicore::fritz::ipc
