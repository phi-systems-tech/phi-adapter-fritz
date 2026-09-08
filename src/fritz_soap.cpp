#include "fritz_soap.h"

#include <algorithm>
#include <cctype>

namespace phicore::fritz::ipc {

namespace {

bool isWs(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string_view trim(std::string_view s)
{
    while (!s.empty() && isWs(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && isWs(s.back()))
        s.remove_suffix(1);
    return s;
}

std::string toLowerAscii(std::string_view s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

/// The element's text, or false. `pos` moves past it so a caller can go on.
bool nextElementText(std::string_view payload, std::string_view key, std::size_t *pos,
                     std::string *text)
{
    const std::string open = "<" + std::string(key) + ">";
    const std::string close = "</" + std::string(key) + ">";
    const std::size_t start = payload.find(open, *pos);
    if (start == std::string_view::npos)
        return false;
    const std::size_t from = start + open.size();
    const std::size_t end = payload.find(close, from);
    if (end == std::string_view::npos)
        return false;
    *text = unescapeXml(trim(payload.substr(from, end - from)));
    *pos = end + close.size();
    return true;
}

} // namespace

std::string escapeXml(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}

std::string unescapeXml(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') {
            out.push_back(text[i]);
            continue;
        }
        const std::size_t semicolon = text.find(';', i);
        if (semicolon == std::string_view::npos || semicolon - i > 10) {
            out.push_back(text[i]);
            continue;
        }
        const std::string_view entity = text.substr(i + 1, semicolon - i - 1);
        if (entity == "amp")
            out.push_back('&');
        else if (entity == "lt")
            out.push_back('<');
        else if (entity == "gt")
            out.push_back('>');
        else if (entity == "quot")
            out.push_back('"');
        else if (entity == "apos")
            out.push_back('\'');
        else if (!entity.empty() && entity.front() == '#') {
            // A numeric reference. Only the ASCII range is turned back into a
            // character; anything above it is left as written rather than
            // guessed at, because this is a name on a screen, not a protocol
            // field.
            const bool hex = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X');
            int value = 0;
            bool ok = !entity.empty();
            for (std::size_t k = hex ? 2 : 1; k < entity.size(); ++k) {
                const char c = entity[k];
                int digit = -1;
                if (c >= '0' && c <= '9')
                    digit = c - '0';
                else if (hex && c >= 'a' && c <= 'f')
                    digit = c - 'a' + 10;
                else if (hex && c >= 'A' && c <= 'F')
                    digit = c - 'A' + 10;
                if (digit < 0) {
                    ok = false;
                    break;
                }
                value = value * (hex ? 16 : 10) + digit;
                if (value > 0x10FFFF) {
                    ok = false;
                    break;
                }
            }
            if (ok && value > 0 && value < 0x80)
                out.push_back(static_cast<char>(value));
            else
                out.append(text.substr(i, semicolon - i + 1));
        } else {
            out.append(text.substr(i, semicolon - i + 1));
            i = semicolon;
            continue;
        }
        i = semicolon;
    }
    return out;
}

std::string buildSoapEnvelope(std::string_view serviceType,
                              std::string_view action,
                              const std::map<std::string, std::string> &params)
{
    std::string body;
    body += R"(<?xml version="1.0" encoding="utf-8"?>)";
    body += R"(<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" )";
    body += R"(s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">)";
    body += "<s:Body>";
    body += "<u:";
    body += action;
    body += R"( xmlns:u=")";
    body += serviceType;
    body += R"(">)";
    for (const auto &[key, value] : params) {
        body += "<" + key + ">";
        body += escapeXml(value);
        body += "</" + key + ">";
    }
    body += "</u:";
    body += action;
    body += ">";
    body += "</s:Body></s:Envelope>";
    return body;
}

bool soapValue(std::string_view payload, std::string_view key, std::string *value)
{
    std::size_t pos = 0;
    std::string text;
    if (!nextElementText(payload, key, &pos, &text))
        return false;
    if (value)
        *value = std::move(text);
    return true;
}

std::vector<std::string> soapValues(std::string_view payload, std::string_view key)
{
    std::vector<std::string> values;
    std::size_t pos = 0;
    std::string text;
    while (nextElementText(payload, key, &pos, &text))
        values.push_back(std::move(text));
    return values;
}

bool isInvalidActionFault(std::string_view payload)
{
    const std::string lower = toLowerAscii(payload);
    return lower.find("invalid action") != std::string::npos
        || lower.find("<errorcode>401</errorcode>") != std::string::npos;
}

std::string faultDescription(std::string_view payload)
{
    std::string description;
    if (soapValue(payload, "errorDescription", &description))
        return description;
    if (soapValue(payload, "faultstring", &description))
        return description;
    return {};
}

} // namespace phicore::fritz::ipc
