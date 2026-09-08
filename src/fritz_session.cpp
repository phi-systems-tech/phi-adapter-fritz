#include "fritz_session.h"

#include <utility>

#include "phi/adapter/net/http_client.h"
#include "phi/runtime/loop.h"

#include "fritz_soap.h"

namespace phicore::fritz::ipc {

namespace net = phicore::adapter::net;

namespace {

/// Long enough for a router that is busy, short enough that a poll which is
/// going nowhere does not overlap the next one.
constexpr auto kRequestTimeout = std::chrono::milliseconds(4000);

} // namespace

struct Tr064Session::Impl {
    explicit Impl(phi::runtime::Loop &loop) : client(loop) {}

    net::HttpClient client;
    std::string baseUrl;
    net::Credentials credentials;
};

Tr064Session::Tr064Session(phi::runtime::Loop &loop)
    : m_impl(std::make_unique<Impl>(loop))
{
}

Tr064Session::~Tr064Session() = default;

void Tr064Session::setEndpoint(std::string baseUrl)
{
    if (m_impl->baseUrl == baseUrl)
        return;
    m_impl->baseUrl = std::move(baseUrl);
    // A different endpoint is a different device; a challenge from the old one
    // would come back refused and read as bad credentials.
    m_impl->client.forgetAuthentication();
}

void Tr064Session::setCredentials(std::string user, std::string password)
{
    if (m_impl->credentials.user == user && m_impl->credentials.password == password)
        return;
    m_impl->credentials.user = std::move(user);
    m_impl->credentials.password = std::move(password);
    m_impl->client.forgetAuthentication();
}

bool Tr064Session::busy() const
{
    return m_impl->client.busy();
}

bool Tr064Session::addressable() const
{
    return !m_impl->baseUrl.empty();
}

void Tr064Session::cancel()
{
    m_impl->client.cancel();
}

bool Tr064Session::call(const Service &service,
                        std::string_view action,
                        std::map<std::string, std::string> params,
                        Done done)
{
    if (!addressable())
        return false;

    net::HttpClient::Call request;
    request.url = m_impl->baseUrl + std::string(service.controlPath);
    request.method = "POST";
    request.headers = {
        {"Content-Type", "text/xml; charset=\"utf-8\""},
        {"SOAPAction", "\"" + std::string(service.type) + "#" + std::string(action) + "\""},
    };
    request.body = buildSoapEnvelope(service.type, action, params);
    request.credentials = m_impl->credentials;
    request.timeout = kRequestTimeout;

    return m_impl->client.send(std::move(request), [done = std::move(done)](
                                                       net::HttpClient::Result result) {
        Reply reply;
        reply.ok = result.ok;
        reply.unauthorized = result.unauthorized;
        reply.payload = std::move(result.body);
        if (!reply.ok) {
            reply.invalidAction = isInvalidActionFault(reply.payload);
            const std::string description = faultDescription(reply.payload);
            reply.error = description.empty() ? result.error : description;
        }
        if (done)
            done(std::move(reply));
    });
}

bool Tr064Session::get(std::string_view path, Done done)
{
    if (!addressable())
        return false;

    net::HttpClient::Call request;
    request.url = m_impl->baseUrl
        + (path.empty() || path.front() == '/' ? std::string(path) : "/" + std::string(path));
    request.method = "GET";
    request.credentials = m_impl->credentials;
    // The host-list document is the largest thing this adapter fetches, and it
    // is fetched when somebody is waiting for a picker to fill in.
    request.timeout = std::chrono::milliseconds(8000);

    return m_impl->client.send(std::move(request), [done = std::move(done)](
                                                       net::HttpClient::Result result) {
        Reply reply;
        reply.ok = result.ok;
        reply.unauthorized = result.unauthorized;
        reply.payload = std::move(result.body);
        if (!reply.ok)
            reply.error = result.error;
        if (done)
            done(std::move(reply));
    });
}

} // namespace phicore::fritz::ipc
