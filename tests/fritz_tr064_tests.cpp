// The TR-064 vocabulary: the XML sent to a FRITZ!Box and the XML it answers.
//
// `fritz_tr064.h` was already written to be testable - no sockets, no
// credentials, no adapter state - and nothing had ever tested it.

#include <phi/adapter/testing/check.h>

#include "fritz_tr064.h"

#include <QCoreApplication>

using namespace phicore::fritz::ipc;

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // --- the vocabulary ----------------------------------------------------

    // MACs are compared as strings everywhere, so they have to be written one
    // way. A router that answers in capitals is the same router.
    PHI_CHECK(normalizeMac(QStringLiteral(" 00:11:22:AA:BB:CC ")) == QStringLiteral("00:11:22:aa:bb:cc"));
    PHI_CHECK(normalizeMac(QString()).isEmpty());

    // TR-064 booleans are "1" and "0".
    PHI_CHECK(isTruthy(QStringLiteral("1")));
    PHI_CHECK(!isTruthy(QStringLiteral("0")));
    PHI_CHECK(!isTruthy(QString()));
    PHI_CHECK(toSoapBoolean(true) == QStringLiteral("1"));
    PHI_CHECK(toSoapBoolean(false) == QStringLiteral("0"));

    // A port comes from the config as either a number or a string, and anything
    // that is not a port is not one - 0 rather than a guess.
    PHI_CHECK(parsePortValue(QJsonValue(49000)) == 49000);
    PHI_CHECK(parsePortValue(QJsonValue(QStringLiteral("49000"))) == 49000);
    PHI_CHECK(parsePortValue(QJsonValue(0)) == 0);
    PHI_CHECK(parsePortValue(QJsonValue(70000)) == 0);
    PHI_CHECK(parsePortValue(QJsonValue(QStringLiteral("not a port"))) == 0);

    // --- what is sent ------------------------------------------------------

    const QByteArray envelope = buildSoapEnvelope(
        QString::fromLatin1(kHostsServiceType),
        QStringLiteral("GetSpecificHostEntry"),
        {{QStringLiteral("NewMACAddress"), QStringLiteral("00:11:22:aa:bb:cc")}});
    // The action, its service and its argument all have to be in there, and the
    // argument namespaced to the action - a box answers a malformed envelope
    // with a fault that says nothing useful.
    PHI_CHECK(envelope.contains("GetSpecificHostEntry"));
    PHI_CHECK(envelope.contains(kHostsServiceType));
    PHI_CHECK(envelope.contains("<NewMACAddress>00:11:22:aa:bb:cc</NewMACAddress>"));
    PHI_CHECK(envelope.contains("Envelope"));

    // --- what comes back ---------------------------------------------------

    // A GetGenericHostEntry answer - the indexed one, which is what carries a
    // MAC. `parseHostEntryFromSoap` requires that field and reports failure
    // without it, which is right: an entry that cannot be identified is not one.
    const QByteArray hostAnswer = QByteArrayLiteral(
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
        "<u:GetGenericHostEntryResponse xmlns:u=\"urn:dslforum-org:service:Hosts:1\">"
        "<NewMACAddress>00:11:22:AA:BB:CC</NewMACAddress>"
        "<NewIPAddress>192.168.1.42</NewIPAddress>"
        "<NewHostName>Kitchen tablet</NewHostName>"
        "<NewInterfaceType>802.11</NewInterfaceType>"
        "<NewActive>1</NewActive>"
        "</u:GetGenericHostEntryResponse></s:Body></s:Envelope>");

    QString ip;
    PHI_CHECK(parseSoapValue(hostAnswer, QStringLiteral("NewIPAddress"), &ip));
    PHI_CHECK(ip == QStringLiteral("192.168.1.42"));
    QString missing;
    PHI_CHECK(!parseSoapValue(hostAnswer, QStringLiteral("NewNothingLikeThat"), &missing));

    HostEntry entry;
    PHI_CHECK(parseHostEntryFromSoap(hostAnswer, &entry));
    PHI_CHECK(entry.ip == QStringLiteral("192.168.1.42"));
    PHI_CHECK(entry.name == QStringLiteral("Kitchen tablet"));
    PHI_CHECK(entry.active);

    // And a device that is switched off reads as switched off, which is the
    // whole point of tracking presence.
    HostEntry away;
    QByteArray awayAnswer = hostAnswer;
    awayAnswer.replace("<NewActive>1</NewActive>", "<NewActive>0</NewActive>");
    PHI_CHECK(parseHostEntryFromSoap(awayAnswer, &away));
    PHI_CHECK(!away.active);

    // The bulk list, which is a different document from a different URL. The
    // parser looks for <Host> elements. Whether that is what a FRITZ!Box
    // actually serves at X_AVM-DE_GetHostListPath is not verified here - the
    // document needs the router's credentials to fetch - and the adapter falls
    // back to asking per index when this yields nothing, so a mismatch would
    // cost one request per host and say nothing. See the note in the commit.
    const QByteArray hostList = QByteArrayLiteral(
        "<?xml version=\"1.0\"?><List>"
        "<Host><Index>0</Index><IPAddress>192.168.1.42</IPAddress>"
        "<MACAddress>00:11:22:AA:BB:CC</MACAddress><HostName>Kitchen tablet</HostName>"
        "<InterfaceType>802.11</InterfaceType><Active>1</Active></Host>"
        "<Host><Index>1</Index><IPAddress>192.168.1.43</IPAddress>"
        "<MACAddress>00:11:22:AA:BB:CD</MACAddress><HostName>Printer</HostName>"
        "<InterfaceType>Ethernet</InterfaceType><Active>0</Active></Host>"
        "</List>");
    const QList<HostEntry> hosts = parseHostList(hostList);
    PHI_CHECK_MSG(hosts.size() == 2, "expected two hosts, got %d", int(hosts.size()));
    if (hosts.size() == 2) {
        // Normalised on the way in, so the two sources agree about one device.
        PHI_CHECK(hosts.at(0).mac == QStringLiteral("00:11:22:aa:bb:cc"));
        PHI_CHECK(hosts.at(0).active);
        PHI_CHECK(!hosts.at(1).active);
        PHI_CHECK(hosts.at(1).name == QStringLiteral("Printer"));
    }

    // A document of <Item> elements - the other shape this could have been -
    // yields nothing today, and the adapter then asks per index instead.
    PHI_CHECK(parseHostList(QByteArrayLiteral(
                  "<List><Item><MACAddress>00:11:22:AA:BB:CC</MACAddress></Item></List>"))
                  .isEmpty());

    // Older firmware answers an action it does not have with a fault, and that
    // means "no such feature" rather than "something went wrong".
    const QByteArray fault = QByteArrayLiteral(
        "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
        "<s:Body><s:Fault><faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring>"
        "<detail><UPnPError xmlns=\"urn:dslforum-org:control-1-0\">"
        "<errorCode>401</errorCode><errorDescription>Invalid Action</errorDescription>"
        "</UPnPError></detail></s:Fault></s:Body></s:Envelope>");
    PHI_CHECK(isInvalidActionFault(fault));
    PHI_CHECK(!isInvalidActionFault(hostAnswer));

    return phi::testing::report("fritz_tr064_tests");
}
