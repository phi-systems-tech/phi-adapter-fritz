// src/adapters/plugins/fritz/fritzadapterfactory.h
#pragma once

#include "adapterfactory.h"

namespace phicore::adapter {

class FritzAdapterFactory : public AdapterFactory
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "phicore.AdapterFactory")
    Q_INTERFACES(phicore::adapter::AdapterFactory)

public:
    explicit FritzAdapterFactory(QObject *parent = nullptr) : AdapterFactory(parent) {}

    QString pluginType() const override { return QStringLiteral("fritz!"); }
    QString displayName() const override { return QStringLiteral("FRITZ!Box"); }
    QString description() const override { return QStringLiteral("AVM FRITZ!Box via TR-064 (clients only)"); }
    int deviceTimeout() const override { return 15000; }
    QString loggingCategory() const override { return QStringLiteral("phi-core.adapters.fritz"); }
    QByteArray icon() const override;

    AdapterCapabilities capabilities() const override;
    discovery::DiscoveryQueryList discoveryQueries() const override;
    AdapterConfigSchema configSchema(const Adapter &info) const override;
    ActionResponse invokeFactoryAction(const QString &actionId, Adapter &infoInOut,
        const QJsonObject &params) const override;

    AdapterInterface *create(QObject *parent) override;

private:
    static bool parseHostListPath(const QByteArray &payload, QString &path, QString &error);
};

} // namespace phicore::adapter
