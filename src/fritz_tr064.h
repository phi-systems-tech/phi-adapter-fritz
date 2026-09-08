#pragma once

// The TR-064 vocabulary: which services and actions exist, what the values
// mean, and the shapes the answers are read into.
//
// The action names are not guesses. Every one below was tried against a
// FRITZ!Box 6850 5G on firmware 258.08.25, and three of the ones this adapter
// used to issue do not exist on it:
//
//   GetHostListPath              -> Invalid Action   (it is X_AVM-DE_GetHostListPath)
//   X_AVM-DE_GetAutoUpdateInfo   -> Invalid Action   (the state is in UserInterface:1)
//   GetAddonInfos                -> Invalid Action   (on either control path)
//
// The first meant every poll fell back to one SOAP call per host - 110 of them,
// twelve times a minute. The other two meant tx_rate, rx_rate and the update
// state had never carried a value, which phi-core's database confirms.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace phicore::fritz::ipc {

inline constexpr std::uint16_t kDefaultTr064Port = 49000;

/// One service, and where its control endpoint lives.
struct Service {
    std::string_view type;
    std::string_view controlPath;
};

inline constexpr Service kHosts{"urn:dslforum-org:service:Hosts:1", "/upnp/control/hosts"};
inline constexpr Service kDeviceInfo{"urn:dslforum-org:service:DeviceInfo:1",
                                     "/upnp/control/deviceinfo"};
inline constexpr Service kWlan24{"urn:dslforum-org:service:WLANConfiguration:1",
                                 "/upnp/control/wlanconfig1"};
inline constexpr Service kWlan5{"urn:dslforum-org:service:WLANConfiguration:2",
                                "/upnp/control/wlanconfig2"};
/// The WAN counters live behind the numbered path; the unnumbered one answers
/// Invalid Action for everything.
inline constexpr Service kWanCommon{"urn:dslforum-org:service:WANCommonInterfaceConfig:1",
                                    "/upnp/control/wancommonifconfig1"};
/**
 * @brief Where FRITZ!OS keeps the firmware update state.
 *
 * Not in DeviceInfo, whose GetInfo carries a version and no word about whether
 * a newer one exists, and not in X_AVM-DE_GetAutoUpdateInfo, which this model
 * does not have. `GetInfo` here answers with NewUpgradeAvailable,
 * NewX_AVM-DE_UpdateState and NewX_AVM-DE_Version.
 */
inline constexpr Service kUserInterface{"urn:dslforum-org:service:UserInterface:1",
                                        "/upnp/control/userif"};

/// One entry of the router's host table.
struct HostEntry {
    std::string mac;
    std::string name;
    std::string ip;
    std::string interfaceType;
    bool active = false;
    bool hasSignal = false;
    int signalDbm = 0;
};

/// Router-wide readings. Each value carries a `has*` flag because TR-064
/// services are optional and a missing one must not read as zero.
struct RouterSnapshot {
    bool hasUptime = false;
    std::int64_t uptimeSec = 0;
    bool hasSoftwareVersion = false;
    std::string softwareVersion;
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
    std::string friendlyName;
};

/// Lowercased and trimmed; MAC addresses are compared as strings throughout.
std::string normalizeMac(std::string_view mac);

/// TR-064 booleans are "1"/"0"; anything else is false.
bool isTruthy(std::string_view value);
std::string toSoapBoolean(bool enabled);

/// 0 for anything outside 1..65535, which callers read as "not configured".
std::uint16_t normalizedPort(int value);

/// One host from a `GetSpecificHostEntry` or `GetGenericHostEntry` answer.
/// The MAC is not in a specific-entry answer - it was the question - so the
/// caller passes the one it asked about.
bool parseHostEntry(std::string_view payload, std::string_view mac, HostEntry *entry);

/**
 * @brief The bulk host list document.
 *
 * A `<List>` of `<Item>` elements; measured against a FRITZ!Box 6850 5G, which
 * serves 110 of them. It used to look for `<Host>`, found nothing, and every
 * sync fell back to asking the router once per host.
 */
std::vector<HostEntry> parseHostList(std::string_view payload);

} // namespace phicore::fritz::ipc
