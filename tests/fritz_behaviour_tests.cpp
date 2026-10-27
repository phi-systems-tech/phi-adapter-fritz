// The decisions the router runtime makes, without a router: what is worth
// saying again, what a byte counter means, what this model turns out to
// implement, and where the "Test connection" values come from.

#include <phi/adapter/testing/check.h>

#include "fritz_capabilities.h"
#include "fritz_devicemodel.h"
#include "fritz_json.h"
#include "fritz_probe.h"
#include "fritz_schema.h"
#include "fritz_state.h"
#include "fritz_tr064.h"

#include "phi/adapter/sdk/network_clients.h"

#include <map>
#include <string>
#include <vector>

using namespace phicore::fritz::ipc;
namespace sdk = phicore::adapter::sdk;
namespace v1 = phicore::adapter::v1;

namespace {

void testOnlyNewsIsReported()
{
    ReportedValues reported;
    PHI_CHECK(reported.isNews("router", "uptime", std::int64_t(100)));
    PHI_CHECK_MSG(!reported.isNews("router", "uptime", std::int64_t(100)),
                  "an unchanged value was reported again");
    PHI_CHECK(reported.isNews("router", "uptime", std::int64_t(105)));

    // Channels and devices do not shadow each other.
    PHI_CHECK(reported.isNews("aa:bb", "online", std::int64_t(1)));
    PHI_CHECK(reported.isNews("cc:dd", "online", std::int64_t(1)));
    PHI_CHECK(!reported.isNews("aa:bb", "online", std::int64_t(1)));

    // A descriptor is a value like any other: three tracked devices announced
    // every five seconds is three device reconciliations in phi-core, twelve
    // times a minute, to conclude nothing moved.
    PHI_CHECK(reported.descriptorIsNews("aa:bb", "phone|192.168.1.5"));
    PHI_CHECK(!reported.descriptorIsNews("aa:bb", "phone|192.168.1.5"));
    PHI_CHECK(reported.descriptorIsNews("aa:bb", "phone|192.168.1.6"));

    // A device announced with a channel it did not have before: phi-core
    // rejects a value for a channel it does not know yet, and says so only in
    // its own log - the send succeeded. Anything already recorded would never
    // be sent again, so the values are forgotten while the descriptor is kept.
    PHI_CHECK(!reported.isNews("aa:bb", "online", std::int64_t(1)));
    reported.forgetValues("aa:bb");
    PHI_CHECK_MSG(reported.isNews("aa:bb", "online", std::int64_t(1)),
                  "a value dropped by core would never be sent again");
    PHI_CHECK_MSG(!reported.descriptorIsNews("aa:bb", "phone|192.168.1.6"),
                  "forgetting values re-announced the device, which would loop");

    // Forgetting one device leaves the others alone.
    reported.forgetDevice("aa:bb");
    PHI_CHECK(reported.isNews("aa:bb", "online", std::int64_t(1)));
    PHI_CHECK(!reported.isNews("cc:dd", "online", std::int64_t(1)));

    reported.forget();
    PHI_CHECK(reported.isNews("cc:dd", "online", std::int64_t(1)));
}

void testTheRateComesFromTwoSamples()
{
    RateMeter meter;
    // Nothing to divide by yet.
    PHI_CHECK(!meter.sample(1000, 1000).has_value());

    // 1000 bytes in one second is 8 kbit/s.
    const auto rate = meter.sample(2000, 2000);
    PHI_CHECK(rate.has_value());
    if (rate)
        PHI_CHECK_MSG(*rate > 7.99 && *rate < 8.01, "%f kbit/s", *rate);

    // The counter is 32 bits on a FRITZ!Box and wraps. A sample that went
    // backwards is dropped rather than reported as an enormous rate.
    PHI_CHECK_MSG(!meter.sample(500, 3000).has_value(), "a wrapped counter produced a rate");
    // And the one after the wrap starts again from there.
    const auto after = meter.sample(1500, 4000);
    PHI_CHECK(after.has_value());

    // Two samples at the same instant have no rate between them.
    RateMeter same;
    (void)same.sample(0, 5000);
    PHI_CHECK(!same.sample(1000, 5000).has_value());

    meter.forget();
    PHI_CHECK(!meter.sample(9999, 9000).has_value());
}

void testWhatTheRouterTurnsOutToImplement()
{
    RouterCapabilities capabilities;
    // Never asked: worth one try.
    PHI_CHECK(capabilities.worthTrying(Feature::AutoUpdateInfo));
    PHI_CHECK(capabilities.availability(Feature::AutoUpdateInfo)
              == RouterCapabilities::Availability::Unknown);

    // The router said Invalid Action. On the box in the field that was true of
    // GetAddonInfos, X_AVM-DE_GetAutoUpdateInfo and GetHostListPath, and all
    // three were asked for again five seconds later, forever.
    capabilities.markAbsent(Feature::AutoUpdateInfo);
    PHI_CHECK_MSG(!capabilities.worthTrying(Feature::AutoUpdateInfo),
                  "an action the router does not have is still being asked for");

    // The update state has two possible homes, and the channel is offered when
    // either answered. Looking in only one of them is how it came to say
    // nothing but "Unknown" on a router that does publish it - just not there.
    PHI_CHECK(capabilities.worthTrying(Feature::UserInterfaceInfo));
    capabilities.markPresent(Feature::UserInterfaceInfo);
    PHI_CHECK(capabilities.availability(Feature::UserInterfaceInfo)
              == RouterCapabilities::Availability::Present);

    capabilities.markPresent(Feature::ByteCounters);
    PHI_CHECK(capabilities.worthTrying(Feature::ByteCounters));
    PHI_CHECK(capabilities.availability(Feature::ByteCounters)
              == RouterCapabilities::Availability::Present);

    // A different box knows different things.
    capabilities.forget();
    PHI_CHECK(capabilities.worthTrying(Feature::AutoUpdateInfo));
}

void testAChannelThisRouterCannotFillIsNotOffered()
{
    // tx_rate and rx_rate stood in the interface of the box in the field from
    // the day it was set up and never carried a value, because the action they
    // were read from does not exist on it. phi-core's database still says
    // "never a value" for both.
    const v1::ChannelList lean = buildRouterChannels(false, false, false, false);
    PHI_CHECK_MSG(lean.size() == 1, "%d channels, expected only uptime", int(lean.size()));
    if (!lean.empty())
        PHI_CHECK(lean.front().externalId() == kChannelUptime);

    const v1::ChannelList full = buildRouterChannels(true, true, true, true);
    PHI_CHECK(full.size() == 6);
    bool sawTx = false, sawRx = false, sawUpdate = false;
    for (const v1::Channel &channel : full) {
        sawTx |= channel.externalId() == kChannelTxRate;
        sawRx |= channel.externalId() == kChannelRxRate;
        if (channel.externalId() == kChannelSoftwareUpdate) {
            sawUpdate = true;
            // A status and two versions: composite, so it is a Json channel and
            // the kind names `status` as the field a history row and an
            // automation condition see.
            PHI_CHECK(channel.dataType() == v1::ChannelDataType::Json);
            PHI_CHECK(v1::channelProjectionField(channel.kind()) == "status");
        }
    }
    PHI_CHECK(sawTx && sawRx && sawUpdate);

    // The fingerprint follows what is offered, so a router that grows a
    // capability is announced again.
    PHI_CHECK(routerFingerprint("n", "f", false, false, false, false)
              != routerFingerprint("n", "f", false, false, true, false));
}

void testAHostBecomesASighting()
{
    // What a GetSpecificHostEntry or the host-list document said, as the
    // tracker takes it.
    HostEntry phone;
    phone.mac = "aa:bb:cc:dd:ee:ff";
    phone.name = "phone";
    phone.ip = "192.168.1.5";
    phone.interfaceType = "802.11";
    phone.active = true;
    phone.hasSignal = true;
    phone.signalDbm = -58;

    const sdk::NetworkClientSighting wifi = sightingFor(phone);
    PHI_CHECK(wifi.mac == "aa:bb:cc:dd:ee:ff");
    PHI_CHECK(wifi.name == "phone");
    PHI_CHECK(wifi.ipv4 == "192.168.1.5");
    PHI_CHECK(wifi.ipv6.empty());
    PHI_CHECK(wifi.online);
    PHI_CHECK(wifi.rssiDbm && *wifi.rssiDbm == -58);
    // The box says WLAN, not which band: that stays unknown rather than guessed.
    PHI_CHECK(wifi.band == v1::WifiBand::Unknown);
    PHI_CHECK(wifi.accessPoint.empty());
    PHI_CHECK(!wifi.blocked.has_value());

    // Asleep or away: still listed, not online. No signal, no RSSI.
    HostEntry asleep = phone;
    asleep.active = false;
    asleep.hasSignal = false;
    const sdk::NetworkClientSighting gone = sightingFor(asleep);
    PHI_CHECK(!gone.online);
    PHI_CHECK(!gone.rssiDbm.has_value());

    // A cable is a band the box does name.
    HostEntry receiver;
    receiver.mac = "00:09:b0:b5:0a:2b";
    receiver.ip = "fd00::32";
    receiver.interfaceType = "Ethernet";
    receiver.active = true;
    const sdk::NetworkClientSighting wired = sightingFor(receiver);
    PHI_CHECK(wired.band == v1::WifiBand::Wired);
    PHI_CHECK(wired.ipv6 == "fd00::32");
    PHI_CHECK(wired.ipv4.empty());
}

void testATrackedHostKeepsItsDeviceId()
{
    // The host-list document, through the tracker: the device a tracked client
    // becomes has the id the presence devices of this adapter always had, the
    // lower-case MAC with colons - existing devices in core stay theirs.
    const char *document =
        R"(<List><Item><MACAddress>AA:BB:CC:DD:EE:FF</MACAddress><IPAddress>192.168.1.55</IPAddress>)"
        R"(<Active>1</Active><HostName>phone</HostName><InterfaceType>802.11</InterfaceType>)"
        R"(<SignalStrength>-61</SignalStrength></Item>)"
        R"(<Item><MACAddress>00:09:B0:B5:0A:2B</MACAddress><Active>1</Active>)"
        R"(<HostName>Pioneer</HostName><InterfaceType>Ethernet</InterfaceType></Item></List>)";
    std::vector<sdk::NetworkClientSighting> seen;
    for (const HostEntry &host : parseHostList(document))
        seen.push_back(sightingFor(host));
    PHI_CHECK(seen.size() == 2);

    std::vector<std::string> deviceIds;
    std::vector<std::string> candidateIds;
    std::map<std::string, v1::ScalarValue> values;
    bool offersAccess = false;
    sdk::ClientTracker tracker;
    tracker.setSink({
        [&](const v1::Device &device, const v1::ChannelList &channels) {
            deviceIds.push_back(device.externalId());
            for (const v1::Channel &channel : channels)
                offersAccess |= channel.externalId() == sdk::kClientChannelNetworkAccess;
        },
        [&](const std::string &device, const std::string &channel, const v1::ScalarValue &value) {
            values[device + "/" + channel] = value;
        },
        [&](const v1::DeviceCandidate &candidate) { candidateIds.push_back(candidate.id()); },
        {},
        {},
    });
    tracker.update(seen, 1000);
    PHI_CHECK_MSG(deviceIds.empty(), "an untracked client became a device");

    tracker.listCandidates(60000, 1000);
    PHI_CHECK_MSG(candidateIds.size() == 2, "%d candidates", int(candidateIds.size()));

    PHI_CHECK(tracker.track("AA-BB-CC-DD-EE-FF", 2000));
    PHI_CHECK_MSG(deviceIds.size() == 1 && deviceIds.front() == "aa:bb:cc:dd:ee:ff",
                  "device id '%s'", deviceIds.empty() ? "" : deviceIds.front().c_str());
    PHI_CHECK(!offersAccess);
    tracker.update(seen, 3000);
    const auto presence = values.find(std::string("aa:bb:cc:dd:ee:ff/") + sdk::kClientChannelPresence);
    PHI_CHECK(presence != values.end() && std::get<bool>(presence->second));
}

void testClientsArePickedUnderAddDevice()
{
    // No picker in the form any more: the instance form carries the tracker's
    // settings, and the clients are a pairing listing.
    const v1::AdapterCapabilities caps = capabilities();
    PHI_CHECK(v1::hasFlag(caps.flags(), v1::AdapterCapability::NetworkClients));
    PHI_CHECK(v1::hasFlag(caps.pairingStyles(), v1::PairingStyle::Listing));

    const v1::AdapterConfigSchema schema = configSchema();
    bool sawAway = false, sawGuests = false, sawPrivate = false;
    for (const v1::AdapterConfigField &field : schema.instance().fields()) {
        sawAway |= field.key() == sdk::kClientMetaAwayDelayS;
        sawGuests |= field.key() == sdk::kClientMetaShowGuests;
        sawPrivate |= field.key() == sdk::kClientMetaIgnorePrivateMacs;
    }
    PHI_CHECK(sawAway && sawGuests && sawPrivate);
    PHI_CHECK_MSG(schema.instance().fields().size() == 3, "%d instance fields",
                  int(schema.instance().fields().size()));
}

void testWhereTheProbeGetsItsValues()
{
    // The form is what the person in front of the dialog just typed, so it wins
    // over the discovered candidate underneath it.
    const ProbeTarget target = probeTargetFromParams(parseObject(R"({
        "factoryAdapter": {"ip": "192.168.178.1", "tr064Port": 49000, "user": "old"},
        "host": "fritz.box",
        "user": "admin",
        "password": "secret"
    })"));
    PHI_CHECK_MSG(target.host == "fritz.box", "host '%s'", target.host.c_str());
    PHI_CHECK(target.user == "admin");
    PHI_CHECK(target.password == "secret");
    PHI_CHECK(target.port == 49000);
    PHI_CHECK(!target.useTls);
    PHI_CHECK(target.endpoint() == "http://fritz.box:49000");

    // An address beats a name, because it costs no lookup.
    const ProbeTarget both = probeTargetFromParams(
        parseObject(R"({"host": "fritz.box", "ip": "192.168.1.1"})"));
    PHI_CHECK(both.host == "192.168.1.1");
    // And with no port, the TR-064 default.
    PHI_CHECK(both.endpoint() == "http://192.168.1.1:49000");

    // Nothing to probe.
    PHI_CHECK(probeTargetFromParams(parseObject("{}")).endpoint().empty());

    // The flag that says https.
    PHI_CHECK(probeTargetFromParams(parseObject(R"({"ip":"h","flags":1})")).useTls);
    PHI_CHECK(!probeTargetFromParams(parseObject(R"({"ip":"h","flags":56})")).useTls);
}

} // namespace

int main()
{
    testOnlyNewsIsReported();
    testTheRateComesFromTwoSamples();
    testWhatTheRouterTurnsOutToImplement();
    testAChannelThisRouterCannotFillIsNotOffered();
    testAHostBecomesASighting();
    testATrackedHostKeepsItsDeviceId();
    testClientsArePickedUnderAddDevice();
    testWhereTheProbeGetsItsValues();
    return phi::testing::report("fritz_behaviour_tests");
}
