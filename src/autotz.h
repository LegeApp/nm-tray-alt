#ifndef AUTOTZ_H
#define AUTOTZ_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QProcess>
#include <QSet>
#include <QStringList>

class NmModel;

class AutoTz : public QObject
{
    Q_OBJECT
public:
    explicit AutoTz(NmModel *model, QObject *parent = nullptr);

private Q_SLOTS:
    void onManagerStateChanged();
    void onReplyFinished();
    void onCurlFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onCurlError(QProcess::ProcessError error);

private:
    static QStringList jsonKeysForUrl(const QString &url);
    void maybeStartLookup();
    void startGeoLookup(const QString &connectionKey, const QString &interfaceName, bool vpnActive);
    void startNextService();
    void handleLookupData(const QByteArray &data, const QString &url);
    void finishLookup();
    void setTimezone(const QString &timezone);

    NmModel *mModel;
    QNetworkAccessManager mNet;
    QProcess mCurl;
    QSet<QString> mAttemptedConnections;
    QString mLookupInterface;
    bool mLookupHasVpn = false;
    int mNextService = 0;
    bool mLookupInProgress = false;
    bool mUsingCurl = false;
};

#endif
