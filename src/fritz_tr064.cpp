#include "fritz_tr064.h"

#include <algorithm>
#include <cctype>

#include "fritz_soap.h"
#include "phi/runtime/str.h"

namespace phicore::fritz::ipc {

namespace str = phi::str;

namespace {

/// One `<Item>` block, read with the same tag scanner as everything else.
HostEntry parseItem(std::string_view item)
{
    HostEntry entry;
    std::string value;
    if (soapValue(item, "MACAddress", &value))
        entry.mac = normalizeMac(value);
    if (soapValue(item, "HostName", &value))
        entry.name = str::trimmed(value);
    if (soapValue(item, "IPAddress", &value))
        entry.ip = str::trimmed(value);
    if (soapValue(item, "Active", &value))
        entry.active = isTruthy(value);
    if (soapValue(item, "InterfaceType", &value))
        entry.interfaceType = str::trimmed(value);
    if (soapValue(item, "SignalStrength", &value)) {
        bool ok = false;
        const int signal = str::toInt(str::trimmed(value), &ok);
        if (ok) {
            entry.hasSignal = true;
            entry.signalDbm = signal;
        }
    }
    return entry;
}

} // namespace

std::string normalizeMac(std::string_view mac)
{
    return str::toLower(str::trimmed(mac));
}

bool isTruthy(std::string_view value)
{
    const std::string normalized = str::toLower(str::trimmed(value));
    return normalized == "1" || normalized == "true";
}

std::string toSoapBoolean(bool enabled)
{
    return enabled ? "1" : "0";
}

std::uint16_t normalizedPort(int value)
{
    if (value <= 0 || value > 65535)
        return 0;
    return static_cast<std::uint16_t>(value);
}

bool parseHostEntry(std::string_view payload, std::string_view mac, HostEntry *entry)
{
    if (!entry)
        return false;

    HostEntry parsed;
    parsed.mac = normalizeMac(mac);

    std::string value;
    // A generic-entry answer carries the address; a specific-entry one does not,
    // because it was the question.
    if (soapValue(payload, "NewMACAddress", &value) && !str::trimmed(value).empty())
        parsed.mac = normalizeMac(value);
    if (parsed.mac.empty())
        return false;

    bool sawAnything = false;
    if (soapValue(payload, "NewHostName", &value)) {
        parsed.name = str::trimmed(value);
        sawAnything = true;
    }
    if (soapValue(payload, "NewIPAddress", &value)) {
        parsed.ip = str::trimmed(value);
        sawAnything = true;
    }
    if (soapValue(payload, "NewActive", &value)) {
        parsed.active = isTruthy(value);
        sawAnything = true;
    }
    if (soapValue(payload, "NewInterfaceType", &value))
        parsed.interfaceType = str::trimmed(value);
    if (soapValue(payload, "NewSignalStrength", &value)) {
        bool ok = false;
        const int signal = str::toInt(str::trimmed(value), &ok);
        if (ok) {
            parsed.hasSignal = true;
            parsed.signalDbm = signal;
        }
    }
    if (!sawAnything)
        return false;

    *entry = std::move(parsed);
    return true;
}

std::vector<HostEntry> parseHostList(std::string_view payload)
{
    std::vector<HostEntry> hosts;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t start = payload.find("<Item", pos);
        if (start == std::string_view::npos)
            break;
        const std::size_t end = payload.find("</Item>", start);
        if (end == std::string_view::npos)
            break;
        HostEntry entry = parseItem(payload.substr(start, end - start));
        if (!entry.mac.empty())
            hosts.push_back(std::move(entry));
        pos = end + 7;
    }
    return hosts;
}

} // namespace phicore::fritz::ipc
