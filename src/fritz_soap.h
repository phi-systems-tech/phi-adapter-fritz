#pragma once

// SOAP for TR-064: the envelope that goes out, and reading the one that comes
// back. Pure over strings, no sockets, no credentials.
//
// The parser is a tag scanner rather than a full XML reader. TR-064 answers are
// a flat list of <NewSomething> elements inside one body, and what a full
// reader bought - namespaces, nesting, entity handling - is either unused here
// or written out below. What it costs is that a document this does not
// understand reads as "field absent", which is the same answer it gives for a
// field the router did not send.

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace phicore::fritz::ipc {

/**
 * @brief A SOAP request body.
 *
 * Parameter values are escaped. They were not, which was harmless only for as
 * long as every parameter was a number or "0"/"1"; the first one carrying a
 * device name would have put its `&` into the document as markup.
 */
std::string buildSoapEnvelope(std::string_view serviceType,
                              std::string_view action,
                              const std::map<std::string, std::string> &params = {});

/// XML text with the five predefined entities replaced.
std::string escapeXml(std::string_view text);

/// The reverse, for values read back out.
std::string unescapeXml(std::string_view text);

/// The first element named `key`, trimmed and unescaped. False when absent.
bool soapValue(std::string_view payload, std::string_view key, std::string *value);

/// Every element named `key`, in order.
std::vector<std::string> soapValues(std::string_view payload, std::string_view key);

/**
 * @brief The fault a router returns for an action it does not implement.
 *
 * TR-064 error 401 is "Invalid Action" - not HTTP 401, which is the opposite
 * problem. A caller reads this as "this model does not have that feature" and,
 * importantly, stops asking: three of the actions this adapter used to issue
 * every five seconds have never existed on the router in the field.
 */
bool isInvalidActionFault(std::string_view payload);

/// The `<errorDescription>` of a fault, for a log that says something.
std::string faultDescription(std::string_view payload);

} // namespace phicore::fritz::ipc
