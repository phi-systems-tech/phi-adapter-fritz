#pragma once

// What phi-core is told a router and a tracked host are. Data, not behaviour.

#include <string>

#include "phi/adapter/v1/channel.h"
#include "phi/adapter/v1/device.h"

#include "fritz_tr064.h"

namespace phicore::fritz::ipc {

inline constexpr const char kRouterDeviceId[] = "router";
inline constexpr const char kChannelUptime[] = "uptime";
inline constexpr const char kChannelSoftwareUpdate[] = "device_software_update";
inline constexpr const char kChannelWlan24[] = "wlan_24_enabled";
inline constexpr const char kChannelWlan5[] = "wlan_5_enabled";
inline constexpr const char kChannelTxRate[] = "tx_rate";
inline constexpr const char kChannelRxRate[] = "rx_rate";
inline constexpr const char kChannelOnline[] = "online";
inline constexpr const char kChannelRssi[] = "rssi";

/**
 * @brief The router's channels.
 *
 * `capabilities` decides which of them are offered: a channel this router
 * cannot fill is not advertised. tx_rate and rx_rate stood in the interface of
 * the box in the field from the day it was set up and never once carried a
 * value, because the action they were read from does not exist on it.
 */
phicore::adapter::v1::ChannelList buildRouterChannels(bool hasWlan24,
                                                      bool hasWlan5,
                                                      bool hasRates,
                                                      bool hasUpdateState);

phicore::adapter::v1::Device buildRouterDevice(const std::string &name,
                                               const std::string &firmware);

phicore::adapter::v1::Device buildHostDevice(const HostEntry &host);
phicore::adapter::v1::ChannelList buildHostChannels(const HostEntry &host);

/// Everything the descriptors above are built from, in one string. Two equal
/// ones describe the same device, so the second need not be sent.
std::string routerFingerprint(const std::string &name,
                              const std::string &firmware,
                              bool hasWlan24,
                              bool hasWlan5,
                              bool hasRates,
                              bool hasUpdateState);
std::string hostFingerprint(const HostEntry &host);

} // namespace phicore::fritz::ipc
