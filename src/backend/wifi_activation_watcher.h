#ifndef WIFI_ACTIVATION_WATCHER_H
#define WIFI_ACTIVATION_WATCHER_H

#include <QObject>
#include <QString>
#include <QTimer>

namespace nm
{

class WifiActivationWatcher : public QObject
{
    Q_OBJECT

public:
    enum class Outcome
    {
        Success,
        WrongOrMissingPassword,
        Failed
    };

    explicit WifiActivationWatcher(const QString &activeConnectionPath, QObject *parent = nullptr);
    void start();

Q_SIGNALS:
    void finished(nm::WifiActivationWatcher::Outcome outcome, const QString &humanError);

private Q_SLOTS:
    void onAcStateChanged(uint state, uint reason);

private:
    void finish(Outcome outcome, const QString &message);

    QString mActiveConnectionPath;
    bool mFinished = false;
    QTimer mTimeout;
};

} // namespace nm

Q_DECLARE_METATYPE(nm::WifiActivationWatcher::Outcome)

#endif
