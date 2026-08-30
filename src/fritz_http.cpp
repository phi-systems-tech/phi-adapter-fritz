#include "fritz_http.h"

#include <utility>

#include <QAuthenticator>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QPointer>
#include <QScopeGuard>
#include <QTimer>
#include <QUrl>

#include "fritz_tr064.h"

namespace phicore::fritz::ipc {

FritzHttp::FritzHttp()
    : m_manager(std::make_unique<QNetworkAccessManager>())
{
    QObject::connect(m_manager.get(),
                     &QNetworkAccessManager::authenticationRequired,
                     [this](QNetworkReply *, QAuthenticator *auth) {
                         if (!auth)
                             return;
                         if (!m_user.isEmpty())
                             auth->setUser(m_user);
                         auth->setPassword(m_password);
                     });
}

FritzHttp::~FritzHttp() = default;

void FritzHttp::setCredentials(const QString &user, const QString &password)
{
    m_user = user.trimmed();
    m_password = password;
}

void FritzHttp::setCancelProbe(std::function<bool()> probe)
{
    m_cancelProbe = std::move(probe);
}

bool FritzHttp::cancelRequested() const
{
    return m_cancelProbe && m_cancelProbe();
}

HttpResult FritzHttp::waitForReply(QNetworkReply *rawReply, int timeoutMs)
{
    // Watched rather than held. The loop below is a nested event loop on this
    // thread, and a nested loop dispatches whatever is queued for the thread -
    // including the host's stop, which used to destroy the network manager and
    // with it this very reply. The loop then returned and the next line read
    // through freed memory. Measured: SIGSEGV at 0xdd1 inside readAll(),
    // on roughly every second core restart.
    QPointer<QNetworkReply> reply(rawReply);

    // Counted rather than a flag: sendSoap and sendGet both come through here,
    // and a request made from inside a handler of another one would clear a
    // flag that is still true.
    ++m_inFlight;
    const auto leave = qScopeGuard([this]() { --m_inFlight; });

    HttpResult result;
    if (!reply) {
        result.error = QStringLiteral("No reply object");
        return result;
    }
    if (cancelRequested()) {
        reply->abort();
        result.error = QStringLiteral("Cancelled: adapter is stopping");
        reply->deleteLater();
        return result;
    }

    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.start(timeoutMs);

    bool cancelled = false;
    QTimer cancelPoll;
    cancelPoll.setInterval(kCancelPollIntervalMs);

    QEventLoop loop;
    QObject::connect(&cancelPoll, &QTimer::timeout, &loop, [this, &loop, &cancelled]() {
        if (!cancelRequested())
            return;
        cancelled = true;
        loop.quit();
    });
    cancelPoll.start();
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    // It may not have survived the loop, and that is not a failure of the
    // router: it means this adapter was told to stop while the request was in
    // flight. Said as a cancellation, which is what it is.
    if (!reply) {
        result.error = QStringLiteral("Cancelled: the adapter stopped while the request was in"
                                      " flight");
        return result;
    }

    if (cancelled) {
        reply->abort();
        result.error = QStringLiteral("Cancelled: adapter is stopping");
        reply->deleteLater();
        return result;
    }

    if (!timeout.isActive()) {
        reply->abort();
        result.error = QStringLiteral("Connection timed out");
        reply->deleteLater();
        return result;
    }

    result.payload = reply->readAll();
    result.networkError = reply->error();
    result.statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.error = reply->errorString();
    result.success = (result.networkError == QNetworkReply::NoError);
    reply->deleteLater();
    return result;
}

HttpResult FritzHttp::sendSoap(const QString &baseUrl,
                               const QString &controlPath,
                               const QString &serviceType,
                               const QString &action,
                               const QMap<QString, QString> &params,
                               int timeoutMs)
{
    HttpResult result;
    if (!m_manager) {
        result.error = QStringLiteral("Network manager not initialized");
        return result;
    }

    QNetworkRequest request(QUrl(baseUrl + controlPath));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("text/xml; charset=\"utf-8\""));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    const QString soapAction = QStringLiteral("\"%1#%2\"").arg(serviceType, action);
    request.setRawHeader("SOAPAction", soapAction.toUtf8());

    return waitForReply(m_manager->post(request, buildSoapEnvelope(serviceType, action, params)),
                        timeoutMs);
}

HttpResult FritzHttp::sendGet(const QString &url, int timeoutMs)
{
    HttpResult result;
    if (!m_manager) {
        result.error = QStringLiteral("Network manager not initialized");
        return result;
    }

    QNetworkRequest request{QUrl(url)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    return waitForReply(m_manager->get(request), timeoutMs);
}

} // namespace phicore::fritz::ipc
