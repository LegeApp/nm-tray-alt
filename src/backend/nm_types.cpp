#include "nm_types.h"

namespace nm
{

bool isVpnType(const QString &type)
{
    return type == QStringLiteral("vpn") || type == QStringLiteral("wireguard");
}

bool isWirelessType(const QString &type)
{
    return type == QStringLiteral("802-11-wireless");
}

bool isUserFacingConnectionType(const QString &type)
{
    return isWirelessType(type)
        || type == QStringLiteral("802-3-ethernet")
        || type == QStringLiteral("bluetooth");
}

int connectionTypeSortKey(const QString &type)
{
    if (isWirelessType(type)) {
        return 1;
    }
    if (type == QStringLiteral("802-3-ethernet")) {
        return 2;
    }
    if (isVpnType(type)) {
        return 3;
    }
    return 4;
}

QString connectionTypeLabel(const QString &type)
{
    if (isWirelessType(type)) {
        return QStringLiteral("wireless");
    }
    if (type == QStringLiteral("802-3-ethernet")) {
        return QStringLiteral("ethernet");
    }
    if (type == QStringLiteral("wireguard")) {
        return QStringLiteral("wireguard");
    }
    if (type == QStringLiteral("vpn")) {
        return QStringLiteral("vpn");
    }
    return type;
}

} // namespace nm
