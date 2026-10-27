#include "fritz_devicemodel.h"

#include "phi/runtime/str.h"


namespace phicore::fritz::ipc {

namespace str = phi::str;
namespace sdk = phicore::adapter::sdk;
namespace v1 = phicore::adapter::v1;

v1::ChannelList buildRouterChannels(bool hasWlan24, bool hasWlan5, bool hasRates,
                                    bool hasUpdateState)
{
    v1::ChannelList channels;

    v1::Channel uptime;
    uptime.setExternalId(kChannelUptime);
    uptime.setName("Uptime");
    uptime.setKind(v1::ChannelKind::Unknown);
    uptime.setDataType(v1::ChannelDataType::Int);
    // Shown, not kept: a number that counts up once a poll was a third of a
    // million history rows a month and says nothing a restart time does not.
    uptime.setFlags(v1::kChannelFlagDefaultRead | v1::ChannelFlag::NoHistory);
    uptime.setUnit("s");
    channels.push_back(std::move(uptime));

    if (hasUpdateState) {
        v1::Channel update;
        update.setExternalId(kChannelSoftwareUpdate);
        update.setName("Software Update");
        update.setKind(v1::ChannelKind::DeviceSoftwareUpdate);
        // A status and two versions. Enum lost the versions, which are the
        // part a person actually reads.
        update.setDataType(v1::ChannelDataType::Json);
        update.setFlags(v1::kChannelFlagDefaultRead);
        channels.push_back(std::move(update));
    }

    if (hasWlan24) {
        v1::Channel wlan;
        wlan.setExternalId(kChannelWlan24);
        wlan.setName("WLAN 2.4 GHz");
        wlan.setKind(v1::ChannelKind::PowerOnOff);
        wlan.setDataType(v1::ChannelDataType::Bool);
        wlan.setFlags(v1::kChannelFlagDefaultWrite);
        wlan.setMetaJson(R"({"forceLabel":true})");
        channels.push_back(std::move(wlan));
    }

    if (hasWlan5) {
        v1::Channel wlan;
        wlan.setExternalId(kChannelWlan5);
        wlan.setName("WLAN 5 GHz");
        wlan.setKind(v1::ChannelKind::PowerOnOff);
        wlan.setDataType(v1::ChannelDataType::Bool);
        wlan.setFlags(v1::kChannelFlagDefaultWrite);
        wlan.setMetaJson(R"({"forceLabel":true})");
        channels.push_back(std::move(wlan));
    }

    if (hasRates) {
        v1::Channel tx;
        tx.setExternalId(kChannelTxRate);
        tx.setName("TX rate");
        tx.setKind(v1::ChannelKind::Unknown);
        tx.setDataType(v1::ChannelDataType::Float);
        // A rate per poll, as many rows as the uptime; a household that wants
        // the curve switches its history on.
        tx.setFlags(v1::kChannelFlagDefaultRead | v1::ChannelFlag::NoHistory);
        tx.setUnit("kbit/s");
        channels.push_back(std::move(tx));

        v1::Channel rx;
        rx.setExternalId(kChannelRxRate);
        rx.setName("RX rate");
        rx.setKind(v1::ChannelKind::Unknown);
        rx.setDataType(v1::ChannelDataType::Float);
        rx.setFlags(v1::kChannelFlagDefaultRead | v1::ChannelFlag::NoHistory);
        rx.setUnit("kbit/s");
        channels.push_back(std::move(rx));
    }

    return channels;
}

v1::Device buildRouterDevice(const std::string &name, const std::string &firmware)
{
    v1::Device device;
    device.setExternalId(kRouterDeviceId);
    device.setName(name.empty() ? "FRITZ!Box" : name);
    device.setDeviceClass(v1::DeviceClass::Gateway);
    device.setManufacturer("AVM");
    if (!firmware.empty())
        device.setFirmware(firmware);
    return device;
}

sdk::NetworkClientSighting sightingFor(const HostEntry &host)
{
    sdk::NetworkClientSighting sighting;
    sighting.mac = host.mac;
    sighting.name = host.name;
    if (host.ip.find(':') != std::string::npos)
        sighting.ipv6 = host.ip;
    else
        sighting.ipv4 = host.ip;
    if (str::containsIgnoreCase(host.interfaceType, "ethernet"))
        sighting.band = v1::WifiBand::Wired;
    if (host.hasSignal)
        sighting.rssiDbm = host.signalDbm;
    sighting.online = host.active;
    return sighting;
}

std::string routerFingerprint(const std::string &name, const std::string &firmware,
                              bool hasWlan24, bool hasWlan5, bool hasRates, bool hasUpdateState)
{
    return name + "\x1f" + firmware + "\x1f" + (hasWlan24 ? "1" : "0")
        + (hasWlan5 ? "1" : "0") + (hasRates ? "1" : "0") + (hasUpdateState ? "1" : "0");
}

} // namespace phicore::fritz::ipc
