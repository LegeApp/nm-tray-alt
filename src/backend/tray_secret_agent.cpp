#include "tray_secret_agent.h"

#include "../wifi_password_dialog.h"
#include "../log.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDialog>

namespace
{
constexpr const char *kNmService = "org.freedesktop.NetworkManager";
constexpr const char *kAgentPath = "/org/nmtrayalt/SecretAgent";
constexpr const char *kAgentManagerPath = "/org/freedesktop/NetworkManager/AgentManager";
constexpr const char *kAgentManagerIface = "org.freedesktop.NetworkManager.AgentManager";

QString ssidFromSettings(const nm::ConnectionSettings &settings)
{
    const QVariantMap wifi = settings.value(QStringLiteral("802-11-wireless"));
    const QByteArray ssid = wifi.value(QStringLiteral("ssid")).toByteArray();
    if (!ssid.isEmpty()) {
        return QString::fromUtf8(ssid);
    }
    const QVariantMap connection = settings.value(QStringLiteral("connection"));
    return connection.value(QStringLiteral("id")).toString();
}
}

namespace nm
{

TraySecretAgent::TraySecretAgent(QObject *parent)
    : QObject(parent)
{
}

void TraySecretAgent::registerAgent()
{
    if (mRegistered) {
        return;
    }
    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.registerObject(QString::fromLatin1(kAgentPath), this, QDBusConnection::ExportAllSlots)) {
        qCWarning(NM_TRAY) << "SecretAgent: failed to register D-Bus object";
        return;
    }

    QDBusMessage reg = QDBusMessage::createMethodCall(QString::fromLatin1(kNmService),
                                                      QString::fromLatin1(kAgentManagerPath),
                                                      QString::fromLatin1(kAgentManagerIface),
                                                      QStringLiteral("Register"));
    reg << QStringLiteral("org.nmtrayalt.agent");
    bus.asyncCall(reg, 5000);
    mRegistered = true;
}

ConnectionSettings TraySecretAgent::GetSecrets(const ConnectionSettings &settings,
                                               const QDBusObjectPath &connection,
                                               const QString &settingName,
                                               const QStringList &hints,
                                               uint flags)
{
    Q_UNUSED(connection);
    Q_UNUSED(hints);
    Q_UNUSED(flags);

    if (settingName != QLatin1String("802-11-wireless-security")) {
        sendErrorReply(QStringLiteral("org.freedesktop.NetworkManager.SecretAgent.Error.NoSecrets"),
                       QStringLiteral("Unsupported secret type"));
        return {};
    }

    const QString ssid = ssidFromSettings(settings);
    WifiPasswordDialog dialog(ssid.isEmpty() ? tr("Wi-Fi network") : ssid, true, nullptr);
    dialog.setInfoText(tr("NetworkManager needs the password for this saved Wi-Fi profile."));
    if (dialog.exec() != QDialog::Accepted) {
        sendErrorReply(QStringLiteral("org.freedesktop.NetworkManager.SecretAgent.Error.UserCanceled"),
                       QStringLiteral("User canceled"));
        return {};
    }

    if (!isWpaPskValid(dialog.password())) {
        sendErrorReply(QStringLiteral("org.freedesktop.NetworkManager.SecretAgent.Error.NoSecrets"),
                       tr("Invalid WPA password"));
        return {};
    }

    ConnectionSettings out;
    QVariantMap sec = settings.value(QStringLiteral("802-11-wireless-security"));
    sec.insert(QStringLiteral("psk"), dialog.password());
    sec.insert(QStringLiteral("psk-flags"), 0U);
    if (!sec.contains(QStringLiteral("key-mgmt"))) {
        sec.insert(QStringLiteral("key-mgmt"), QStringLiteral("wpa-psk"));
    }
    out.insert(QStringLiteral("802-11-wireless-security"), sec);
    return out;
}

void TraySecretAgent::CancelGetSecrets(const QDBusObjectPath &connection, const QString &settingName)
{
    Q_UNUSED(connection);
    Q_UNUSED(settingName);
}

void TraySecretAgent::SaveSecrets(const ConnectionSettings &settings, const QDBusObjectPath &connection)
{
    Q_UNUSED(settings);
    Q_UNUSED(connection);
}

void TraySecretAgent::DeleteSecrets(const ConnectionSettings &settings, const QDBusObjectPath &connection)
{
    Q_UNUSED(settings);
    Q_UNUSED(connection);
}

} // namespace nm
