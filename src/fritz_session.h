#pragma once

// TR-064 over HTTP: one call at a time, on the loop.
//
// Thin on purpose. What it adds over the SDK's HTTP client is the SOAP
// envelope, the SOAPAction header, and telling apart the three answers a
// caller has to act on differently: it worked, the router does not have that
// action, and the credentials were refused.

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "fritz_tr064.h"

namespace phi::runtime {
class Loop;
}

namespace phicore::fritz::ipc {

class Tr064Session
{
public:
    struct Reply {
        bool ok = false;
        /// The router answered, and said it has no such action. Not a failure
        /// of the network or of the credentials - a fact about this model,
        /// worth remembering so the call is not made again.
        bool invalidAction = false;
        /// Reached and refused. Sends the user to their password rather than
        /// to their network.
        bool unauthorized = false;
        std::string payload;
        std::string error;
    };

    using Done = std::function<void(Reply)>;

    explicit Tr064Session(phi::runtime::Loop &loop);
    ~Tr064Session();

    Tr064Session(const Tr064Session &) = delete;
    Tr064Session &operator=(const Tr064Session &) = delete;

    /// `http://host:port`, with no trailing slash.
    void setEndpoint(std::string baseUrl);
    void setCredentials(std::string user, std::string password);

    [[nodiscard]] bool busy() const;
    [[nodiscard]] bool addressable() const;

    /// False when one is in flight or there is no endpoint; `done` is then
    /// never called.
    bool call(const Service &service,
              std::string_view action,
              std::map<std::string, std::string> params,
              Done done);

    /// A plain GET against the same host, for the host-list document.
    bool get(std::string_view path, Done done);

    void cancel();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace phicore::fritz::ipc
