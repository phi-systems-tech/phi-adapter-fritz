// The TR-064 vocabulary: the XML sent to a FRITZ!Box and the XML it answers.
//
// The payloads below are the shapes a FRITZ!Box 6850 5G on firmware 258.08.25
// actually returns, including the fault it gives for an action it does not
// have - which turned out to be three of the ones this adapter was issuing.

#include <phi/adapter/testing/check.h>

#include "fritz_soap.h"
#include "fritz_tr064.h"

#include <string>
#include <vector>

using namespace phicore::fritz::ipc;

namespace {

const char *kInvalidActionFault = R"(<?xml version="1.0"?>
<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">
<s:Body><s:Fault><faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring>
<detail><UPnPError xmlns="urn:dslforum-org:control-1-0">
<errorCode>401</errorCode><errorDescription>Invalid Action</errorDescription>
</UPnPError></detail></s:Fault></s:Body></s:Envelope>)";

void testTheVocabulary()
{
    // MACs are compared as strings everywhere, so they have to be written one
    // way. A router that answers in capitals is the same router.
    PHI_CHECK(normalizeMac(" 00:11:22:AA:BB:CC ") == "00:11:22:aa:bb:cc");
    PHI_CHECK(normalizeMac("").empty());

    PHI_CHECK(isTruthy("1"));
    PHI_CHECK(isTruthy("true"));
    PHI_CHECK(!isTruthy("0"));
    PHI_CHECK(!isTruthy(""));
    PHI_CHECK(toSoapBoolean(true) == "1");
    PHI_CHECK(toSoapBoolean(false) == "0");

    PHI_CHECK(normalizedPort(49000) == 49000);
    PHI_CHECK(normalizedPort(0) == 0);
    PHI_CHECK(normalizedPort(70000) == 0);
}

void testTheEnvelopeThatGoesOut()
{
    const std::string envelope =
        buildSoapEnvelope(kHosts.type, "GetSpecificHostEntry",
                          {{"NewMACAddress", "00:11:22:aa:bb:cc"}});
    PHI_CHECK(envelope.find("<u:GetSpecificHostEntry xmlns:u=\"urn:dslforum-org:service:Hosts:1\">")
              != std::string::npos);
    PHI_CHECK(envelope.find("<NewMACAddress>00:11:22:aa:bb:cc</NewMACAddress>")
              != std::string::npos);
    PHI_CHECK(envelope.find("</u:GetSpecificHostEntry></s:Body></s:Envelope>")
              != std::string::npos);

    // Values are escaped. They were not, which stayed harmless only for as long
    // as every parameter was a number or "0"/"1"; a device name is neither.
    const std::string risky =
        buildSoapEnvelope(kHosts.type, "X", {{"NewName", R"(Ann & Bob's <box>)"}});
    PHI_CHECK_MSG(risky.find("Ann &amp; Bob&apos;s &lt;box&gt;") != std::string::npos,
                  "unescaped: %s", risky.c_str());
    PHI_CHECK(risky.find("<box>") == std::string::npos);
}

void testReadingAnAnswer()
{
    const char *payload =
        R"(<?xml version="1.0"?><s:Envelope><s:Body>)"
        R"(<u:GetInfoResponse><NewUpTime>1763824</NewUpTime>)"
        R"(<NewSoftwareVersion> 258.08.25 </NewSoftwareVersion>)"
        R"(<NewModelName>FRITZ!Box 6850 5G</NewModelName>)"
        R"(<NewDeviceLog>a &amp; b &lt;c&gt;</NewDeviceLog>)"
        R"(</u:GetInfoResponse></s:Body></s:Envelope>)";

    std::string value;
    PHI_CHECK(soapValue(payload, "NewUpTime", &value) && value == "1763824");
    // Trimmed, because a router pads.
    PHI_CHECK(soapValue(payload, "NewSoftwareVersion", &value) && value == "258.08.25");
    PHI_CHECK(soapValue(payload, "NewModelName", &value) && value == "FRITZ!Box 6850 5G");
    // And unescaped, so a name with an ampersand in it reads as it was written.
    PHI_CHECK(soapValue(payload, "NewDeviceLog", &value) && value == "a & b <c>");
    PHI_CHECK(!soapValue(payload, "NewNothing", &value));

    PHI_CHECK(escapeXml("a&b") == "a&amp;b");
    PHI_CHECK(unescapeXml("a&amp;b") == "a&b");
    PHI_CHECK(unescapeXml("&#65;&#x42;") == "AB");
    // Something this does not understand is left as written rather than
    // guessed at: it is a name on a screen, not a protocol field.
    PHI_CHECK(unescapeXml("&nbsp;x") == "&nbsp;x");
    PHI_CHECK(unescapeXml("a & b") == "a & b");
}

void testTheFaultForAnActionTheRouterDoesNotHave()
{
    // TR-064 error 401 is "Invalid Action" - not HTTP 401, which is the
    // opposite problem. Telling them apart is what lets the adapter stop
    // asking for something this model does not have, instead of asking twelve
    // times a minute forever.
    PHI_CHECK(isInvalidActionFault(kInvalidActionFault));
    PHI_CHECK(faultDescription(kInvalidActionFault) == "Invalid Action");

    PHI_CHECK(!isInvalidActionFault("<html>401 Unauthorized</html>"));
    PHI_CHECK(!isInvalidActionFault(""));
}

void testOneHost()
{
    // GetSpecificHostEntry: the answer to "tell me about this address", which
    // does not repeat the address back.
    const char *specific =
        R"(<s:Envelope><s:Body><u:GetSpecificHostEntryResponse>)"
        R"(<NewIPAddress>192.168.1.32</NewIPAddress>)"
        R"(<NewAddressSource>DHCP</NewAddressSource>)"
        R"(<NewLeaseTimeRemaining>0</NewLeaseTimeRemaining>)"
        R"(<NewInterfaceType>Ethernet</NewInterfaceType><NewActive>1</NewActive>)"
        R"(<NewHostName>Pioneer-SC-LX701</NewHostName>)"
        R"(</u:GetSpecificHostEntryResponse></s:Body></s:Envelope>)";

    HostEntry entry;
    PHI_CHECK(parseHostEntry(specific, "00:09:B0:B5:0A:2B", &entry));
    PHI_CHECK_MSG(entry.mac == "00:09:b0:b5:0a:2b", "mac '%s'", entry.mac.c_str());
    PHI_CHECK(entry.name == "Pioneer-SC-LX701");
    PHI_CHECK(entry.ip == "192.168.1.32");
    PHI_CHECK(entry.active);
    PHI_CHECK(entry.interfaceType == "Ethernet");
    PHI_CHECK(!entry.hasSignal);

    // GetGenericHostEntry: the same shape with the address in it.
    const char *generic =
        R"(<s:Body><u:GetGenericHostEntryResponse>)"
        R"(<NewMACAddress>AA:BB:CC:DD:EE:FF</NewMACAddress>)"
        R"(<NewHostName>phone</NewHostName><NewActive>0</NewActive>)"
        R"(<NewSignalStrength>-62</NewSignalStrength>)"
        R"(</u:GetGenericHostEntryResponse></s:Body>)";
    PHI_CHECK(parseHostEntry(generic, {}, &entry));
    PHI_CHECK(entry.mac == "aa:bb:cc:dd:ee:ff");
    PHI_CHECK(!entry.active);
    PHI_CHECK(entry.hasSignal && entry.signalDbm == -62);

    // A fault is not a host.
    PHI_CHECK(!parseHostEntry(kInvalidActionFault, "00:11:22:33:44:55", &entry));
    // Nor is an answer with nothing in it.
    PHI_CHECK(!parseHostEntry("<s:Body></s:Body>", "00:11:22:33:44:55", &entry));
}

void testTheHostListDocument()
{
    // The list is <Item> elements, not <Host>. Looking for the wrong one found
    // nothing, and every sync fell back to asking the router once per host.
    const char *document =
        R"(<?xml version="1.0"?><List>)"
        R"(<Item><Index>0</Index><IPAddress>192.168.1.32</IPAddress>)"
        R"(<MACAddress>00:09:B0:B5:0A:2B</MACAddress><Active>1</Active>)"
        R"(<HostName>Pioneer-SC-LX701</HostName><InterfaceType>Ethernet</InterfaceType></Item>)"
        R"(<Item><Index>1</Index><IPAddress>192.168.1.55</IPAddress>)"
        R"(<MACAddress>AA:BB:CC:DD:EE:FF</MACAddress><Active>0</Active>)"
        R"(<HostName>Ann &amp; Bob</HostName><InterfaceType>802.11</InterfaceType>)"
        R"(<SignalStrength>-58</SignalStrength></Item>)"
        R"(<Item><Index>2</Index><HostName>no address</HostName></Item>)"
        R"(</List>)";

    const std::vector<HostEntry> hosts = parseHostList(document);
    PHI_CHECK_MSG(hosts.size() == 2, "%d hosts, expected 2 - the third has no address",
                  int(hosts.size()));
    if (hosts.size() != 2)
        return;
    PHI_CHECK(hosts[0].mac == "00:09:b0:b5:0a:2b");
    PHI_CHECK(hosts[0].active);
    PHI_CHECK(hosts[1].mac == "aa:bb:cc:dd:ee:ff");
    PHI_CHECK(!hosts[1].active);
    PHI_CHECK(hosts[1].name == "Ann & Bob");
    PHI_CHECK(hosts[1].hasSignal && hosts[1].signalDbm == -58);

    PHI_CHECK(parseHostList("").empty());
    PHI_CHECK(parseHostList("<List><Item><Index>0</Index>").empty());   // truncated
}

} // namespace

int main()
{
    testTheVocabulary();
    testTheEnvelopeThatGoesOut();
    testReadingAnAnswer();
    testTheFaultForAnActionTheRouterDoesNotHave();
    testOneHost();
    testTheHostListDocument();
    return phi::testing::report("fritz_tr064_tests");
}
