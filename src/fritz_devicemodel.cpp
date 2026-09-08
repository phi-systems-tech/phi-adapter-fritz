#include "fritz_devicemodel.h"

#include "phi/runtime/str.h"

#include "fritz_json.h"

namespace phicore::fritz::ipc {

namespace str = phi::str;
namespace v1 = phicore::adapter::v1;

namespace {

bool looksWireless(const std::string &interfaceType)
{
    return str::containsIgnoreCase(interfaceType, "802.11")
        || str::containsIgnoreCase(interfaceType, "wlan");
}

} // namespace

v1::ChannelList buildRouterChannels(bool hasWlan24, bool hasWlan5, bool hasRates,
                                    bool hasUpdateState)
{
    v1::ChannelList channels;

    v1::Channel uptime;
    uptime.externalId = kChannelUptime;
    uptime.name = "Uptime";
    uptime.kind = v1::ChannelKind::Unknown;
    uptime.dataType = v1::ChannelDataType::Int;
    uptime.flags = v1::kChannelFlagDefaultRead;
    uptime.unit = "s";
    channels.push_back(std::move(uptime));

    if (hasUpdateState) {
        v1::Channel update;
        update.externalId = kChannelSoftwareUpdate;
        update.name = "Software Update";
        update.kind = v1::ChannelKind::DeviceSoftwareUpdate;
        // A status and two versions. Enum lost the versions, which are the
        // part a person actually reads.
        update.dataType = v1::ChannelDataType::Json;
        update.flags = v1::kChannelFlagDefaultRead;
        channels.push_back(std::move(update));
    }

    if (hasWlan24) {
        v1::Channel wlan;
        wlan.externalId = kChannelWlan24;
        wlan.name = "WLAN 2.4 GHz";
        wlan.kind = v1::ChannelKind::PowerOnOff;
        wlan.dataType = v1::ChannelDataType::Bool;
        wlan.flags = v1::kChannelFlagDefaultWrite;
        wlan.metaJson = R"({"forceLabel":true})";
        channels.push_back(std::move(wlan));
    }

    if (hasWlan5) {
        v1::Channel wlan;
        wlan.externalId = kChannelWlan5;
        wlan.name = "WLAN 5 GHz";
        wlan.kind = v1::ChannelKind::PowerOnOff;
        wlan.dataType = v1::ChannelDataType::Bool;
        wlan.flags = v1::kChannelFlagDefaultWrite;
        wlan.metaJson = R"({"forceLabel":true})";
        channels.push_back(std::move(wlan));
    }

    if (hasRates) {
        v1::Channel tx;
        tx.externalId = kChannelTxRate;
        tx.name = "TX rate";
        tx.kind = v1::ChannelKind::Unknown;
        tx.dataType = v1::ChannelDataType::Float;
        tx.flags = v1::kChannelFlagDefaultRead;
        tx.unit = "kbit/s";
        channels.push_back(std::move(tx));

        v1::Channel rx;
        rx.externalId = kChannelRxRate;
        rx.name = "RX rate";
        rx.kind = v1::ChannelKind::Unknown;
        rx.dataType = v1::ChannelDataType::Float;
        rx.flags = v1::kChannelFlagDefaultRead;
        rx.unit = "kbit/s";
        channels.push_back(std::move(rx));
    }

    return channels;
}

v1::Device buildRouterDevice(const std::string &name, const std::string &firmware)
{
    v1::Device device;
    device.externalId = kRouterDeviceId;
    device.name = name.empty() ? "FRITZ!Box" : name;
    device.deviceClass = v1::DeviceClass::Gateway;
    device.manufacturer = "AVM";
    if (!firmware.empty())
        device.firmware = firmware;
    return device;
}

v1::Device buildHostDevice(const HostEntry &host)
{
    v1::Device device;
    device.externalId = host.mac;
    device.name = host.name.empty() ? host.mac : host.name;
    device.deviceClass = v1::DeviceClass::Sensor;
    device.manufacturer = "AVM";
    if (looksWireless(host.interfaceType))
        device.flags |= v1::DeviceFlag::Wireless;

    Json meta = Json::object();
    if (!host.ip.empty())
        meta["ip"] = host.ip;
    meta["mac"] = host.mac;
    if (!host.interfaceType.empty())
        meta["interfaceType"] = host.interfaceType;
    device.metaJson = dump(meta);
    return device;
}

v1::ChannelList buildHostChannels(const HostEntry &host)
{
    v1::ChannelList channels;

    v1::Channel online;
    online.externalId = kChannelOnline;
    online.name = "Online";
    online.kind = v1::ChannelKind::ConnectivityStatus;
    online.dataType = v1::ChannelDataType::Enum;
    online.flags = v1::kChannelFlagDefaultRead;
    channels.push_back(std::move(online));

    if (host.hasSignal) {
        v1::Channel rssi;
        rssi.externalId = kChannelRssi;
        rssi.name = "RSSI";
        rssi.kind = v1::ChannelKind::SignalStrength;
        rssi.dataType = v1::ChannelDataType::Float;
        rssi.flags = v1::kChannelFlagDefaultRead;
        rssi.unit = "dBm";
        rssi.minValue = -100.0;
        rssi.maxValue = 0.0;
        channels.push_back(std::move(rssi));
    }

    return channels;
}

std::string routerFingerprint(const std::string &name, const std::string &firmware,
                              bool hasWlan24, bool hasWlan5, bool hasRates, bool hasUpdateState)
{
    return name + "\x1f" + firmware + "\x1f" + (hasWlan24 ? "1" : "0")
        + (hasWlan5 ? "1" : "0") + (hasRates ? "1" : "0") + (hasUpdateState ? "1" : "0");
}

std::string hostFingerprint(const HostEntry &host)
{
    // Not `active`: that is a channel value, and a device whose descriptor
    // changes every time it goes offline is a device announced twice a minute.
    return host.mac + "\x1f" + host.name + "\x1f" + host.ip + "\x1f" + host.interfaceType
        + "\x1f" + (host.hasSignal ? "1" : "0");
}

} // namespace phicore::fritz::ipc
