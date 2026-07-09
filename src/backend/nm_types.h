#ifndef NM_TYPES_H
#define NM_TYPES_H

#include <QDateTime>
#include <QList>
#include <QMap>
#include <QMetaType>
#include <QByteArray>
#include <QString>
#include <QStringList>

namespace nm
{

enum class DeviceType : uint32_t
{
    Unknown = 0,
    Ethernet = 1,
    Wifi = 2,
};

enum class ActiveState : uint32_t
{
    Unknown = 0,
    Activating = 1,
    Activated = 2,
    Deactivating = 3,
    Deactivated = 4,
};

struct AccessPointRecord
{
    QString path;
    QString devicePath;
    QString ssid;
    QByteArray ssidBytes;
    QString bssid;
    int strength = 0;
    uint32_t flags = 0;
    uint32_t wpaFlags = 0;
    uint32_t rsnFlags = 0;
    uint32_t frequency = 0;
    bool privacy = false;

    bool operator==(const AccessPointRecord &) const = default;
};

struct DeviceRecord
{
    QString path;
    QString interfaceName;
    QString ipInterfaceName;
    DeviceType type = DeviceType::Unknown;
    uint32_t state = 0;
    QString activeConnectionPath;
    QString activeAccessPointPath;
    QList<QString> accessPointPaths;
    QString hardwareAddress;
    int bitrateKbps = -1;
    uint32_t ip4Connectivity = 0;
    uint32_t ip6Connectivity = 0;
    qulonglong rxBytes = 0;
    qulonglong txBytes = 0;

    bool operator==(const DeviceRecord &) const = default;
};

struct SavedConnectionRecord
{
    QString path;
    QString id;
    QString wifiSsid;
    QByteArray wifiSsidBytes;
    QString uuid;
    QString type;
    QString interfaceName;
    qint64 timestamp = 0;
    int autoconnectPriority = 0;
    bool autoconnect = true;

    bool operator==(const SavedConnectionRecord &) const = default;
};

struct ActiveConnectionRecord
{
    QString path;
    QString connectionPath;
    QString specificObjectPath;
    QString id;
    QString uuid;
    QString type;
    QList<QString> devices;
    ActiveState state = ActiveState::Unknown;
    bool isVpn = false;
    bool isDefault4 = false;
    bool isDefault6 = false;
    QString ip4ConfigPath;
    QString ip6ConfigPath;
    QStringList ip4Addresses;
    QStringList ip6Addresses;
    QString ip4Gateway;
    QString ip6Gateway;
    QStringList ip4Dns;
    QStringList ip6Dns;
    int ip4RouteCount = 0;
    int ip6RouteCount = 0;

    bool operator==(const ActiveConnectionRecord &) const = default;
};

struct ManagerState
{
    bool networkingEnabled = false;
    bool wirelessEnabled = false;
    bool wirelessHardwareEnabled = false;
    QString primaryConnectionPath;
    uint32_t state = 0;
    uint32_t connectivity = 0;
    bool connectivityCheckAvailable = false;
    bool connectivityCheckEnabled = false;
    QString connectivityCheckUri;
    QString lastError;

    bool operator==(const ManagerState &) const = default;
};

struct Snapshot
{
    ManagerState manager;
    QMap<QString, DeviceRecord> devices;
    QMap<QString, AccessPointRecord> accessPoints;
    QMap<QString, SavedConnectionRecord> savedConnections;
    QMap<QString, ActiveConnectionRecord> activeConnections;
    QDateTime collectedAt;

    bool operator==(const Snapshot &other) const
    {
        // collectedAt is intentionally excluded; it changes every refresh even when
        // the NetworkManager state did not.
        return manager == other.manager
            && devices == other.devices
            && accessPoints == other.accessPoints
            && savedConnections == other.savedConnections
            && activeConnections == other.activeConnections;
    }
};

bool isVpnType(const QString &type);
bool isWirelessType(const QString &type);
bool isUserFacingConnectionType(const QString &type);
int connectionTypeSortKey(const QString &type);
QString connectionTypeLabel(const QString &type);

} // namespace nm

Q_DECLARE_METATYPE(nm::Snapshot)

#endif
