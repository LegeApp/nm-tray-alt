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
    mRetryTimer.setInterval(60000);
    connect(&mRetryTimer, &QTimer::timeout, this, &ConnectivityMonitor::startProbe);
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
    if (url.isValid() && !url.isEmpty()
        && (url.scheme() == QStringLiteral("http") || url.scheme() == QStringLiteral("https"))) {
        mProbeUrl = url;
    } else {
        mProbeUrl = QUrl(QStringLiteral("http://connectivitycheck.gstatic.com/generate_204"));
    }
}

void ConnectivityMonitor::updateFromSnapshot(uint nmState,
                                             uint connectivity,
                                             bool nmCheckEnabled,
                                             const QUrl &configuredProbe)
{
    setProbeUrl(configuredProbe);
    mNetworkConnected = nmState >= 50;

    if (!mNetworkConnected) {
        mFallbackProbeNeeded = false;
        mRetryTimer.stop();
        cancelProbe();
        if (nmState != 40) {
            // Anything below "connecting" is a definitive state; "connecting" (40)
            // is transient, so keep showing the last known status instead of
            // flashing a "checking" message.
            setStatus(InternetStatus::NoNetwork);
        }
        return;
    }

    // NM_CONNECTIVITY_FULL and NM_STATE_CONNECTED_GLOBAL are useful even when
    // ConnectivityCheckEnabled is false. In that configuration NetworkManager
    // commonly reports "full" from its global routing state. Ignoring it made
    // the result depend entirely on one hard-coded web service.
    if (connectivity == 4 || nmState == 70) {
        mFallbackProbeNeeded = false;
        mRetryTimer.stop();
        cancelProbe();
        setStatus(InternetStatus::Full);
        return;
    }

    // Limited/portal results can be caused by a blocked or temporarily broken
    // distro probe. Verify them independently before putting a warning badge on
    // an otherwise active connection. Keep retrying so a failure is not sticky.
    Q_UNUSED(nmCheckEnabled)
    mFallbackProbeNeeded = true;
    if (!mRetryTimer.isActive()) {
        mRetryTimer.start();
    }
    if (mStatus == InternetStatus::Unknown || mStatus == InternetStatus::NoNetwork) {
        setStatus(InternetStatus::Checking);
    }
    startProbe();
}

void ConnectivityMonitor::recheckNow()
{
    // Keep showing the last known status while the recheck is in flight;
    // only update once a definitive result comes back.
    askNmToRecheck();
    if (mNetworkConnected && mFallbackProbeNeeded) {
        startProbe();
    }
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
    if (mProbeRunning || !mNetworkConnected || !mFallbackProbeNeeded) {
        return;
    }

    mProbeRunning = true;
    mSawPortalResponse = false;
    mProbeIndex = 0;
    mProbeUrls.clear();

    auto addProbe = [this](const QUrl &url) {
        if (url.isValid() && !url.isEmpty() && !mProbeUrls.contains(url)) {
            mProbeUrls.append(url);
        }
    };

    // Use more than one independently hosted endpoint. A connection is online
    // if any probe returns its expected response; only declare failure after all
    // probes have failed.
    addProbe(mProbeUrl);
    addProbe(QUrl(QStringLiteral("http://connectivitycheck.gstatic.com/generate_204")));
    addProbe(QUrl(QStringLiteral("http://network-test.debian.org/nm")));

    const quint64 generation = ++mProbeGeneration;
    startNextProbe(generation);
}

void ConnectivityMonitor::startNextProbe(quint64 generation)
{
    if (generation != mProbeGeneration || !mProbeRunning) {
        return;
    }

    if (mProbeIndex >= mProbeUrls.size()) {
        mProbeRunning = false;
        setStatus(mSawPortalResponse ? InternetStatus::Portal : InternetStatus::NoInternet);
        return;
    }

    const QUrl probeUrl = mProbeUrls.at(mProbeIndex++);
    QNetworkRequest req(probeUrl);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    req.setTransferTimeout(5000);
    req.setRawHeader("User-Agent", "nm-tray-alt-conncheck/1.0");

    QNetworkReply *reply = mNam.get(req);
    mActiveReply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
        const QByteArray body = reply->readAll().trimmed();
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray nmHeader = reply->rawHeader("X-NetworkManager-Status").trimmed();
        reply->deleteLater();
        if (mActiveReply == reply) {
            mActiveReply.clear();
        }

        if (generation != mProbeGeneration || !mProbeRunning) {
            return;
        }

        if (reply->error() == QNetworkReply::NoError
            && (http == 204
                || nmHeader.compare("online", Qt::CaseInsensitive) == 0
                || body == "NetworkManager is online")) {
            mProbeRunning = false;
            setStatus(InternetStatus::Full);
            return;
        }

        if (http >= 200 && http < 400) {
            mSawPortalResponse = true;
        }
        startNextProbe(generation);
    });
}

void ConnectivityMonitor::cancelProbe()
{
    ++mProbeGeneration;
    mProbeRunning = false;
    mProbeUrls.clear();
    if (mActiveReply) {
        mActiveReply->abort();
        mActiveReply.clear();
    }
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
