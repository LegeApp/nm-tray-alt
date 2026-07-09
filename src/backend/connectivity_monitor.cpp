#include "connectivity_monitor.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace nm
{

ConnectivityMonitor::ConnectivityMonitor(QObject *parent)
    : QObject(parent)
{
}

QString ConnectivityMonitor::statusText() const
{
    switch (mStatus) {
    case InternetStatus::Full:
        return tr("Internet access");
    case InternetStatus::Portal:
        return tr("Sign-in required (captive portal)");
    case InternetStatus::NoInternet:
        return tr("Connected — no internet access");
    case InternetStatus::NoNetwork:
        return tr("Not connected");
    case InternetStatus::Checking:
        return tr("Checking internet access…");
    case InternetStatus::Unknown:
        return tr("Internet access unknown");
    }
    return {};
}

void ConnectivityMonitor::setProbeUrl(const QUrl &url)
{
    if (url.isValid() && !url.isEmpty()) {
        mProbeUrl = url;
    }
}

void ConnectivityMonitor::updateFromSnapshot(uint nmState,
                                             uint connectivity,
                                             bool nmCheckEnabled,
                                             const QUrl &configuredProbe)
{
    setProbeUrl(configuredProbe);

    if (nmState < 50) {
        if (nmState != 40) {
            // Anything below "connecting" is a definitive state; "connecting" (40)
            // is transient, so keep showing the last known status instead of
            // flashing a "checking" message.
            setStatus(InternetStatus::NoNetwork);
        }
        return;
    }

    if (nmCheckEnabled) {
        switch (connectivity) {
        case 4:
            setStatus(InternetStatus::Full);
            return;
        case 3:
            setStatus(InternetStatus::NoInternet);
            return;
        case 2:
            setStatus(InternetStatus::Portal);
            return;
        case 1:
            setStatus(InternetStatus::NoNetwork);
            return;
        default:
            break;
        }
    }

    startProbe();
}

void ConnectivityMonitor::recheckNow()
{
    // Keep showing the last known status while the recheck is in flight;
    // only update once a definitive result comes back.
    askNmToRecheck();
    startProbe();
}

void ConnectivityMonitor::askNmToRecheck()
{
    QDBusMessage msg = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.NetworkManager"),
                                                      QStringLiteral("/org/freedesktop/NetworkManager"),
                                                      QStringLiteral("org.freedesktop.NetworkManager"),
                                                      QStringLiteral("CheckConnectivity"));
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(msg, 8000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [](QDBusPendingCallWatcher *w) {
        w->deleteLater();
    });
}

void ConnectivityMonitor::startProbe()
{
    if (mProbeRunning) {
        return;
    }
    mProbeRunning = true;

    QNetworkRequest req(mProbeUrl);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    req.setTransferTimeout(5000);
    req.setRawHeader("User-Agent", "nm-tray-alt-conncheck/1.0");

    QNetworkReply *reply = mNam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        mProbeRunning = false;

        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray nmHeader = reply->rawHeader("X-NetworkManager-Status").trimmed();

        if (reply->error() == QNetworkReply::NoError
            && (http == 204 || nmHeader == "online")) {
            setStatus(InternetStatus::Full);
        } else if (http >= 200 && http < 400) {
            setStatus(InternetStatus::Portal);
        } else {
            setStatus(InternetStatus::NoInternet);
        }
    });
}

void ConnectivityMonitor::setStatus(InternetStatus status)
{
    if (mStatus == status) {
        return;
    }
    mStatus = status;
    emit statusChanged(status);
}

} // namespace nm
