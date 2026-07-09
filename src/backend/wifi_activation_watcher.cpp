#include "wifi_activation_watcher.h"

#include <QDBusConnection>

namespace
{
constexpr const char *kNmService = "org.freedesktop.NetworkManager";
constexpr const char *kActiveConnIface = "org.freedesktop.NetworkManager.Connection.Active";
}

namespace nm
{

WifiActivationWatcher::WifiActivationWatcher(const QString &activeConnectionPath, QObject *parent)
    : QObject(parent)
    , mActiveConnectionPath(activeConnectionPath)
{
    mTimeout.setSingleShot(true);
    mTimeout.setInterval(45000);
    connect(&mTimeout, &QTimer::timeout, this, [this] {
        finish(Outcome::Failed, tr("Connection timed out."));
    });
}

void WifiActivationWatcher::start()
{
    if (mActiveConnectionPath.isEmpty() || mActiveConnectionPath == QStringLiteral("/")) {
        finish(Outcome::Failed, tr("NetworkManager did not report an active connection to watch."));
        return;
    }

    QDBusConnection::systemBus().connect(QString::fromLatin1(kNmService),
                                         mActiveConnectionPath,
                                         QString::fromLatin1(kActiveConnIface),
                                         QStringLiteral("StateChanged"),
                                         this,
                                         SLOT(onAcStateChanged(uint,uint)));
    mTimeout.start();
}

void WifiActivationWatcher::onAcStateChanged(uint state, uint reason)
{
    // NMActiveConnectionState: 2 = ACTIVATED, 4 = DEACTIVATED.
    // NMActiveConnectionStateReason: 9 = NO_SECRETS, 10 = LOGIN_FAILED.
    if (state == 2) {
        finish(Outcome::Success, {});
    } else if (state == 4) {
        if (reason == 9 || reason == 10) {
            finish(Outcome::WrongOrMissingPassword, tr("The Wi-Fi password was not accepted."));
        } else {
            finish(Outcome::Failed, tr("Connection failed (reason %1).").arg(reason));
        }
    }
}

void WifiActivationWatcher::finish(Outcome outcome, const QString &message)
{
    if (mFinished) {
        return;
    }
    mFinished = true;
    mTimeout.stop();
    QDBusConnection::systemBus().disconnect(QString::fromLatin1(kNmService),
                                            mActiveConnectionPath,
                                            QString::fromLatin1(kActiveConnIface),
                                            QStringLiteral("StateChanged"),
                                            this,
                                            SLOT(onAcStateChanged(uint,uint)));
    emit finished(outcome, message);
}

} // namespace nm
