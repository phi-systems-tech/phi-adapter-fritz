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

#include <string>

using namespace phicore::fritz::ipc;
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
        PHI_CHECK(lean.front().externalId == kChannelUptime);

    const v1::ChannelList full = buildRouterChannels(true, true, true, true);
    PHI_CHECK(full.size() == 6);
    bool sawTx = false, sawRx = false, sawUpdate = false;
    for (const v1::Channel &channel : full) {
        sawTx |= channel.externalId == kChannelTxRate;
        sawRx |= channel.externalId == kChannelRxRate;
        sawUpdate |= channel.externalId == kChannelSoftwareUpdate;
    }
    PHI_CHECK(sawTx && sawRx && sawUpdate);

    // The fingerprint follows what is offered, so a router that grows a
    // capability is announced again.
    PHI_CHECK(routerFingerprint("n", "f", false, false, false, false)
              != routerFingerprint("n", "f", false, false, true, false));
}

void testADeviceIsNotReannouncedForGoingOffline()
{
    HostEntry host;
    host.mac = "aa:bb:cc:dd:ee:ff";
    host.name = "phone";
    host.ip = "192.168.1.5";
    host.interfaceType = "802.11";
    host.active = true;

    HostEntry offline = host;
    offline.active = false;
    // `active` is a channel value, not part of what the device is. A descriptor
    // that changes with it is a device announced twice a minute.
    PHI_CHECK_MSG(hostFingerprint(host) == hostFingerprint(offline),
                  "going offline changed the device descriptor");

    HostEntry renamed = host;
    renamed.ip = "192.168.1.6";
    PHI_CHECK(hostFingerprint(host) != hostFingerprint(renamed));

    const v1::Device device = buildHostDevice(host);
    PHI_CHECK(device.externalId == "aa:bb:cc:dd:ee:ff");
    PHI_CHECK(device.name == "phone");
    PHI_CHECK(v1::hasFlag(device.flags, v1::DeviceFlag::Wireless));

    HostEntry wired = host;
    wired.interfaceType = "Ethernet";
    PHI_CHECK(!v1::hasFlag(buildHostDevice(wired).flags, v1::DeviceFlag::Wireless));

    // No signal, no RSSI channel.
    PHI_CHECK(buildHostChannels(host).size() == 1);
    HostEntry wifi = host;
    wifi.hasSignal = true;
    wifi.signalDbm = -58;
    PHI_CHECK(buildHostChannels(wifi).size() == 2);
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

void testThePickerNeverLosesASelection()
{
    const Json meta = parseObject(R"({
        "knownHosts": [
            {"mac": "AA:BB:CC:DD:EE:FF", "name": "phone", "ip": "192.168.1.5"},
            {"mac": "11:22:33:44:55:66", "ip": "192.168.1.6"},
            {"mac": "77:88:99:AA:BB:CC"}
        ],
        "trackedMacs": ["aa:bb:cc:dd:ee:ff", "de:ad:be:ef:00:01"]
    })");

    const v1::AdapterConfigOptionList options = buildTrackedOptions(meta);
    PHI_CHECK_MSG(options.size() == 4, "%d options, expected 4", int(options.size()));
    if (options.size() != 4)
        return;
    PHI_CHECK(options[0].value == "aa:bb:cc:dd:ee:ff");
    PHI_CHECK(options[0].label == "phone (192.168.1.5)");
    PHI_CHECK(options[1].label == "192.168.1.6");
    PHI_CHECK(options[2].label == "77:88:99:aa:bb:cc");
    // A tracked address the router no longer lists is still selectable, or the
    // selection looks like it cleared itself.
    PHI_CHECK_MSG(options[3].value == "de:ad:be:ef:00:01",
                  "a tracked address dropped out of the picker");

    PHI_CHECK(buildTrackedOptions(parseObject("{}")).empty());
}

} // namespace

int main()
{
    testOnlyNewsIsReported();
    testTheRateComesFromTwoSamples();
    testWhatTheRouterTurnsOutToImplement();
    testAChannelThisRouterCannotFillIsNotOffered();
    testADeviceIsNotReannouncedForGoingOffline();
    testWhereTheProbeGetsItsValues();
    testThePickerNeverLosesASelection();
    return phi::testing::report("fritz_behaviour_tests");
}
