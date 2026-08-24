#pragma once

// TR-064 vocabulary and SOAP payload handling for the FRITZ!Box.
//
// Everything here is pure: it builds or reads XML and normalizes the values the
// router speaks in. No sockets, no credentials, no adapter state - that keeps
// the protocol testable and separates it from the adapter lifecycle.

#include <QByteArray>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QString>
#include <QtGlobal>

namespace phicore::fritz::ipc {

inline constexpr quint16 kDefaultTr064Port = 49000;

inline constexpr const char kHostsServiceType[] = "urn:dslforum-org:service:Hosts:1";
inline constexpr const char kHostsControlPath[] = "/upnp/control/hosts";
inline constexpr const char kDeviceInfoServiceType[] = "urn:dslforum-org:service:DeviceInfo:1";
inline constexpr const char kDeviceInfoControlPath[] = "/upnp/control/deviceinfo";
inline constexpr const char kWanCommonServiceType[] =
    "urn:dslforum-org:service:WANCommonInterfaceConfig:1";
inline constexpr const char kWanCommonControlPath[] = "/upnp/control/wancommonifconfig";
inline constexpr const char kWlan24ServiceType[] = "urn:dslforum-org:service:WLANConfiguration:1";
inline constexpr const char kWlan24ControlPath[] = "/upnp/control/wlanconfig1";
inline constexpr const char kWlan5ServiceType[] = "urn:dslforum-org:service:WLANConfiguration:2";
inline constexpr const char kWlan5ControlPath[] = "/upnp/control/wlanconfig2";

/// One entry of the router's host table.
struct HostEntry {
    QString mac;
    QString name;
    QString ip;
    QString interfaceType;
    bool active = false;
    bool hasSignal = false;
    int signalDbm = 0;
};

/// Router-wide readings; each value carries a `has*` flag because TR-064
/// services are optional and a missing one must not read as zero.
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

/// Lowercased and trimmed; MAC addresses are compared as strings throughout.
QString normalizeMac(const QString &mac);

/// TR-064 booleans are "1"/"0"; anything else is treated as false.
bool isTruthy(const QString &value);
QString toSoapBoolean(bool enabled);

/// Port from a JSON number or string; 0 for anything outside 1..65535.
quint16 parsePortValue(const QJsonValue &value);

QByteArray buildSoapEnvelope(const QString &serviceType,
                             const QString &action,
                             const QMap<QString, QString> &params);

/// First element named `key`, trimmed. False when the payload has no such node.
bool parseSoapValue(const QByteArray &payload, const QString &key, QString *value);

bool parseHostListPath(const QByteArray &payload, QString *path, QString *error);
bool parseHostEntryFromSoap(const QByteArray &payload, HostEntry *entry);

/// The bulk host list fetched from the router's host-list URL, which is a
/// document of <Item> elements under a <List> - measured against a FRITZ!Box
/// 6850 5G on firmware 258.08.25, which answers 109 of them. It used to look
/// for <Host>, found nothing, and every sync fell back to asking the router
/// once per host.
QList<HostEntry> parseHostList(const QByteArray &payload);

/// True for the SOAP fault older firmware returns for an unsupported action,
/// which callers treat as "feature absent" rather than as an error.
bool isInvalidActionFault(const QByteArray &payload);

} // namespace phicore::fritz::ipc
