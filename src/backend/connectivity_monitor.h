#ifndef CONNECTIVITY_MONITOR_H
#define CONNECTIVITY_MONITOR_H

#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>

class QNetworkReply;

namespace nm
{

enum class InternetStatus
{
    Unknown,
    Checking,
    NoNetwork,
    NoInternet,
    Portal,
    Full
};

class ConnectivityMonitor : public QObject
{
    Q_OBJECT

public:
    explicit ConnectivityMonitor(QObject *parent = nullptr);

    InternetStatus status() const { return mStatus; }
    QString statusText() const;
    QUrl portalProbeUrl() const { return mProbeUrl; }
    void setProbeUrl(const QUrl &url);

public Q_SLOTS:
    void updateFromSnapshot(uint nmState,
                            uint connectivity,
                            bool nmCheckEnabled,
                            const QUrl &configuredProbe = {});
    void recheckNow();

Q_SIGNALS:
    void statusChanged(nm::InternetStatus status);

private:
    void setStatus(InternetStatus status);
    void startProbe();
    void startNextProbe(quint64 generation);
    void cancelProbe();
    void askNmToRecheck();

    QNetworkAccessManager mNam;
    InternetStatus mStatus = InternetStatus::Unknown;
    QUrl mProbeUrl{QStringLiteral("http://connectivitycheck.gstatic.com/generate_204")};
    QList<QUrl> mProbeUrls;
    QPointer<QNetworkReply> mActiveReply;
    QTimer mRetryTimer;
    quint64 mProbeGeneration = 0;
    qsizetype mProbeIndex = 0;
    bool mSawPortalResponse = false;
    bool mNetworkConnected = false;
    bool mFallbackProbeNeeded = false;
    bool mProbeRunning = false;
};

} // namespace nm

Q_DECLARE_METATYPE(nm::InternetStatus)

#endif
