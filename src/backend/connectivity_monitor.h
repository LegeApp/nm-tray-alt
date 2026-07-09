#ifndef CONNECTIVITY_MONITOR_H
#define CONNECTIVITY_MONITOR_H

#include <QNetworkAccessManager>
#include <QObject>
#include <QUrl>

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
    void askNmToRecheck();

    QNetworkAccessManager mNam;
    InternetStatus mStatus = InternetStatus::Unknown;
    QUrl mProbeUrl{QStringLiteral("http://connectivity-check.ubuntu.com/")};
    bool mProbeRunning = false;
};

} // namespace nm

Q_DECLARE_METATYPE(nm::InternetStatus)

#endif
