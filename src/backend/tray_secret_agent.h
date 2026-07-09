#ifndef TRAY_SECRET_AGENT_H
#define TRAY_SECRET_AGENT_H

#include "nm_actions.h"

#include <QDBusContext>
#include <QDBusObjectPath>
#include <QObject>

namespace nm
{

class TraySecretAgent : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.NetworkManager.SecretAgent")

public:
    explicit TraySecretAgent(QObject *parent = nullptr);
    void registerAgent();

public Q_SLOTS:
    nm::ConnectionSettings GetSecrets(const nm::ConnectionSettings &settings,
                                      const QDBusObjectPath &connection,
                                      const QString &settingName,
                                      const QStringList &hints,
                                      uint flags);
    void CancelGetSecrets(const QDBusObjectPath &connection, const QString &settingName);
    void SaveSecrets(const nm::ConnectionSettings &settings, const QDBusObjectPath &connection);
    void DeleteSecrets(const nm::ConnectionSettings &settings, const QDBusObjectPath &connection);

private:
    bool mRegistered = false;
};

} // namespace nm

#endif
