// src/adapters/plugins/fritz/fritzadapter.h
#pragma once

#include <QHash>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <functional>

#include "adapterinterface.h"

namespace phicore::adapter {

class FritzAdapter : public AdapterInterface
{
    Q_OBJECT

public:
    explicit FritzAdapter(QObject *parent = nullptr);
    ~FritzAdapter() override;

protected:
    bool start(QString &errorString) override;
    void stop() override;
    void requestFullSync() override;
    void adapterConfigUpdated() override;
    void invokeAdapterAction(const QString &actionId, const QJsonObject &params, CmdId cmdId) override;
    void updateChannelState(const QString &deviceExternalId,
                            const QString &channelExternalId,
                            const QVariant &value,
                            CmdId cmdId) override;

private:
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

    void pollOnce();
    void fetchRouterSnapshot();
    void applyRouterSnapshot(const RouterSnapshot &snapshot);
    void ensureRouterDevice();
    void setWlanEnabled(int band, bool enabled, CmdId cmdId);
    ChannelList buildRouterChannels() const;
    void fetchHostSnapshot(std::function<void(bool, QList<HostEntry>, const QString &)> callback);
    void fetchHostEntries(std::function<void(bool, QList<HostEntry>, const QString &)> callback);
    void handleHostSnapshot(const QList<HostEntry> &hosts);
    void updateDeviceFromHost(const HostEntry &host);
    void removeMissingDevices(const QSet<QString> &deviceIds);
    void emitActionError(CmdId cmdId, const QString &error);
    void setConnected(bool connected);
    void updatePollInterval();
    void logPollError(const QString &error);
    bool ensureHostAvailable(QString &error);

    QString resolveBaseUrl() const;
    void refreshConfig();

    QNetworkReply *sendSoapRequest(const QString &url,
                                   const QString &serviceType,
                                   const QString &action,
                                   const QMap<QString, QString> &params);
    QNetworkReply *sendGetRequest(const QString &url);
    void trackReply(QNetworkReply *reply);
    void clearReplies();

    bool parseHostListPath(const QByteArray &payload, QString &path, QString &error) const;
    bool parseSoapValue(const QByteArray &payload, const QString &key, QString &value) const;
    bool parseHostEntryFromSoap(const QByteArray &payload, HostEntry &entry) const;
    bool isInvalidActionFault(const QByteArray &payload) const;
    QList<HostEntry> parseHostList(const QByteArray &payload) const;

    QPointer<QNetworkAccessManager> m_network;
    QSet<QNetworkReply *> m_pendingReplies;
    QTimer *m_pollTimer = nullptr;
    bool m_connected = false;
    bool m_pendingFullSync = false;
    int m_pollIntervalMs = 5000;
    int m_retryIntervalMs = 10000;
    QSet<QString> m_trackedMacs;
    QSet<QString> m_knownDevices;
    bool m_routerEmitted = false;
    QString m_routerName;
    QString m_routerFirmware;
};

} // namespace phicore::adapter
