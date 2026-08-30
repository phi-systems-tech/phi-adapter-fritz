#pragma once

// Blocking HTTP/SOAP transport to the FRITZ!Box, mirroring hue's HttpClient.
//
// The requests are synchronous by design - the TR-064 flows read a value and
// act on it - so each one parks the calling thread in a nested event loop. That
// is only safe because the instance runs on its own execution backend and
// because the wait is cancellable: see setCancelProbe().

#include <functional>
#include <memory>

#include <QByteArray>
#include <QMap>
#include <QNetworkReply>
#include <QString>

class QNetworkAccessManager;

namespace phicore::fritz::ipc {

struct HttpResult {
    bool success = false;
    QByteArray payload;
    QNetworkReply::NetworkError networkError = QNetworkReply::NoError;
    int statusCode = 0;
    QString error;
};

class FritzHttp
{
public:
    FritzHttp();
    ~FritzHttp();

    FritzHttp(const FritzHttp &) = delete;
    FritzHttp &operator=(const FritzHttp &) = delete;

    /// Credentials answered on the router's authentication challenge.
    void setCredentials(const QString &user, const QString &password);

    /**
     * @brief Cancellation hook, polled while a request is in flight.
     *
     * Without it a shutdown has to wait out the request timeout. The instance
     * installs `stopRequested()` here, so a teardown aborts the reply within
     * one poll interval instead of up to `timeoutMs` (F-33).
     */
    void setCancelProbe(std::function<bool()> probe);

    HttpResult sendSoap(const QString &baseUrl,
                        const QString &controlPath,
                        const QString &serviceType,
                        const QString &action,
                        const QMap<QString, QString> &params = {},
                        int timeoutMs = 3000);

    HttpResult sendGet(const QString &url, int timeoutMs = 3000);

private:
    HttpResult waitForReply(QNetworkReply *reply, int timeoutMs);
    bool cancelRequested() const;

public:
    /// Whether a request is on the stack right now.
    ///
    /// Asked by the instance before it destroys this object. A nested event
    /// loop runs whatever is queued for the thread, so the host's stop can
    /// arrive while a request is still in flight - and freeing the manager
    /// there would pull the reply out from under the frame that is waiting on
    /// it. The teardown is deferred instead; nothing is left running, because
    /// the cancel probe has already told the loop to give up.
    [[nodiscard]] bool busy() const { return m_inFlight > 0; }

private:
    int m_inFlight = 0;

    static constexpr int kCancelPollIntervalMs = 50;

    std::unique_ptr<QNetworkAccessManager> m_manager;
    std::function<bool()> m_cancelProbe;
    QString m_user;
    QString m_password;
};

} // namespace phicore::fritz::ipc
