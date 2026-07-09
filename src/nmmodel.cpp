#include "nmmodel.h"

#include "backend/nm_actions.h"
#include "backend/wifi_activation_watcher.h"
#include "icons.h"
#include "log.h"
#include "wifi_password_dialog.h"

#include <QCheckBox>
#include <QDBusObjectPath>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLocale>
#include <QMetaObject>
#include <QMetaType>
#include <QFormLayout>
#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSet>
#include <algorithm>

namespace
{
int connTypeToInt(const QString &type)
{
    return nm::connectionTypeSortKey(type);
}

bool isWirelessType(const QString &type)
{
    return nm::isWirelessType(type);
}

QString formatBytes(qulonglong bytes)
{
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    return QLocale::system().toString(value, 'f', value >= 100.0 ? 0 : (value >= 10.0 ? 1 : 2))
        + QLatin1Char(' ')
        + QLatin1String(units[unit]);
}

struct WifiTarget
{
    QString devicePath;
    QString apPath;
};

WifiTarget pickWifiTarget(const nm::Snapshot &snapshot,
                          const QString &ifaceHint,
                          const QByteArray &ssidBytes)
{
    WifiTarget fallback;
    for (const auto &dev : snapshot.devices) {
        if (dev.type != nm::DeviceType::Wifi) {
            continue;
        }
        if (!ifaceHint.isEmpty() && dev.interfaceName != ifaceHint) {
            continue;
        }
        if (dev.state < 30) {
            continue;
        }
        if (fallback.devicePath.isEmpty()) {
            fallback.devicePath = dev.path;
        }
        for (const QString &apPath : dev.accessPointPaths) {
            const auto apIt = snapshot.accessPoints.find(apPath);
            if (apIt != snapshot.accessPoints.end() && apIt->ssidBytes == ssidBytes) {
                return { dev.path, apPath };
            }
        }
    }
    return fallback;
}

QString firstObjectPathFromReply(const QDBusMessage &reply)
{
    for (const QVariant &arg : reply.arguments()) {
        if (arg.canConvert<QDBusObjectPath>()) {
            const QString path = arg.value<QDBusObjectPath>().path();
            if (!path.isEmpty() && path != QStringLiteral("/")) {
                return path;
            }
        }
    }
    return {};
}

QString lastObjectPathFromReply(const QDBusMessage &reply)
{
    for (auto it = reply.arguments().crbegin(); it != reply.arguments().crend(); ++it) {
        if (it->canConvert<QDBusObjectPath>()) {
            const QString path = it->value<QDBusObjectPath>().path();
            if (!path.isEmpty() && path != QStringLiteral("/")) {
                return path;
            }
        }
    }
    return {};
}

NmModel::OverallState toOverallState(uint nmState, uint connectivity)
{
    // NM states: 20 disconnected, 40 connecting, 50 connected(local), 60 connected(site), 70 connected(global)
    if (nmState == 40) {
        return NmModel::OverallState::Connecting;
    }
    if (nmState == 50 || nmState == 60 || nmState == 70) {
        if (connectivity == 2 || connectivity == 3) {
            return NmModel::OverallState::Limited;
        }
        return connectivity == 1 ? NmModel::OverallState::Disconnected : NmModel::OverallState::Connected;
    }
    return NmModel::OverallState::Disconnected;
}

bool managerStateEquivalent(const NmModel::ManagerState &a, const NmModel::ManagerState &b)
{
    return a.overallState == b.overallState
        && a.primaryKind == b.primaryKind
        && a.primaryName == b.primaryName
        && a.wifiStrength == b.wifiStrength
        && a.vpnActive == b.vpnActive
        && a.lastError == b.lastError
        && a.networkingEnabled == b.networkingEnabled
        && a.wirelessEnabled == b.wirelessEnabled
        && a.wirelessHardwareEnabled == b.wirelessHardwareEnabled
        && a.primaryConnectionPath == b.primaryConnectionPath
        && a.rawNmState == b.rawNmState
        && a.rawConnectivity == b.rawConnectivity
        && a.connectivityCheckAvailable == b.connectivityCheckAvailable
        && a.connectivityCheckEnabled == b.connectivityCheckEnabled
        && a.connectivityCheckUri == b.connectivityCheckUri;
}

bool activeEquivalent(const nm::ActiveConnectionRecord &a, const nm::ActiveConnectionRecord &b)
{
    return a == b;
}

bool connectionEquivalent(const nm::ConnectionViewRecord &a, const nm::ConnectionViewRecord &b)
{
    return a == b;
}

bool deviceEquivalent(const nm::DeviceRecord &a, const nm::DeviceRecord &b)
{
    return a == b;
}

bool wifiEquivalent(const nm::WifiViewRecord &a, const nm::WifiViewRecord &b)
{
    return a == b;
}

} // namespace

NmModel::NmModel(QObject *parent)
    : QAbstractItemModel(parent)
{
    qRegisterMetaType<nm::Snapshot>();

    mDbus = new nm::NmDbusClient;
    mDbus->moveToThread(&mDbusThread);
    connect(&mDbusThread, &QThread::finished, mDbus, &QObject::deleteLater);
    connect(mDbus, &nm::NmDbusClient::snapshotChanged, this, &NmModel::onSnapshotChanged, Qt::QueuedConnection);

    mDbusThread.start();
    QMetaObject::invokeMethod(mDbus, "start", Qt::QueuedConnection);

    rebuildFromSnapshot(nm::Snapshot{});
}

NmModel::~NmModel()
{
    if (mDbusThread.isRunning()) {
        mDbusThread.quit();
        mDbusThread.wait();
    }
}

int NmModel::rowCount(const QModelIndex &parent) const
{
    if (!parent.isValid()) {
        return 1;
    }

    const ItemId id = static_cast<ItemId>(parent.internalId());
    if (id == ITEM_ROOT) {
        return 4;
    }
    if (id == ITEM_ACTIVE) {
        return mActive.size();
    }
    if (id == ITEM_CONNECTION) {
        return mConnections.size();
    }
    if (id == ITEM_DEVICE) {
        return mDevices.size();
    }
    if (id == ITEM_WIFINET) {
        return mWifi.size();
    }
    return 0;
}

int NmModel::columnCount(const QModelIndex &) const
{
    return 1;
}

QModelIndex NmModel::index(int row, int column, const QModelIndex &parent) const
{
    if (!hasIndex(row, column, parent)) {
        return {};
    }

    if (!parent.isValid()) {
        return createIndex(row, column, ITEM_ROOT);
    }

    const ItemId id = static_cast<ItemId>(parent.internalId());
    if (id == ITEM_ROOT) {
        switch (row) {
        case 0:
            return createIndex(row, column, ITEM_ACTIVE);
        case 1:
            return createIndex(row, column, ITEM_CONNECTION);
        case 2:
            return createIndex(row, column, ITEM_DEVICE);
        case 3:
            return createIndex(row, column, ITEM_WIFINET);
        default:
            return {};
        }
    }

    if (id == ITEM_ACTIVE) {
        return createIndex(row, column, ITEM_ACTIVE_LEAF);
    }
    if (id == ITEM_CONNECTION) {
        return createIndex(row, column, ITEM_CONNECTION_LEAF);
    }
    if (id == ITEM_DEVICE) {
        return createIndex(row, column, ITEM_DEVICE_LEAF);
    }
    if (id == ITEM_WIFINET) {
        return createIndex(row, column, ITEM_WIFINET_LEAF);
    }
    return {};
}

QModelIndex NmModel::parent(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return {};
    }

    const ItemId id = static_cast<ItemId>(index.internalId());
    switch (id) {
    case ITEM_ROOT:
        return {};
    case ITEM_ACTIVE:
    case ITEM_CONNECTION:
    case ITEM_DEVICE:
    case ITEM_WIFINET:
        return createIndex(0, 0, ITEM_ROOT);
    case ITEM_ACTIVE_LEAF:
        return createIndex(0, 0, ITEM_ACTIVE);
    case ITEM_CONNECTION_LEAF:
        return createIndex(1, 0, ITEM_CONNECTION);
    case ITEM_DEVICE_LEAF:
        return createIndex(2, 0, ITEM_DEVICE);
    case ITEM_WIFINET_LEAF:
        return createIndex(3, 0, ITEM_WIFINET);
    }
    return {};
}

QVariant NmModel::data(const QModelIndex &index, int role) const
{
    if (!isValidDataIndex(index)) {
        return {};
    }

    const ItemId id = static_cast<ItemId>(index.internalId());

    if (role == ItemTypeRole) {
        switch (id) {
        case ITEM_ACTIVE_LEAF:
            return ActiveConnectionType;
        case ITEM_CONNECTION_LEAF:
            return ConnectionType;
        case ITEM_DEVICE_LEAF:
            return DeviceType;
        case ITEM_WIFINET_LEAF:
            return WifiNetworkType;
        default:
            return HelperType;
        }
    }

    if (role == Qt::DisplayRole || role == NameRole) {
        switch (id) {
        case ITEM_ROOT:
            return tr("root");
        case ITEM_ACTIVE:
            return tr("active connection(s)");
        case ITEM_CONNECTION:
            return tr("connection(s)");
        case ITEM_DEVICE:
            return tr("device(s)");
        case ITEM_WIFINET:
            return tr("Wi-Fi network(s)");
        case ITEM_ACTIVE_LEAF:
            return mActive.at(index.row()).id;
        case ITEM_CONNECTION_LEAF:
            return mConnections.at(index.row()).id;
        case ITEM_DEVICE_LEAF:
            return mDevices.at(index.row()).interfaceName;
        case ITEM_WIFINET_LEAF:
            return mWifi.at(index.row()).ssid;
        }
    }

    if (role == Qt::ToolTipRole) {
        switch (id) {
        case ITEM_WIFINET_LEAF:
            return tr("Signal strength: %1%").arg(mWifi.at(index.row()).strength);
        case ITEM_ACTIVE_LEAF:
            if (isWirelessType(mActive.at(index.row()).type)) {
                const int signal = data(index, SignalRole).toInt();
                if (signal >= 0) {
                    return tr("Signal strength: %1%").arg(signal);
                }
            }
            break;
        case ITEM_CONNECTION_LEAF:
            if (isWirelessType(mConnections.at(index.row()).type)) {
                for (const auto &wifi : mWifi) {
                    if (wifi.ssid == mConnections.at(index.row()).id) {
                        return tr("Signal strength: %1%").arg(wifi.strength);
                    }
                }
            }
            break;
        default:
            break;
        }
    }

    if (role == ConnectionTypeRole) {
        switch (id) {
        case ITEM_ACTIVE_LEAF:
            return connTypeToInt(mActive.at(index.row()).type);
        case ITEM_CONNECTION_LEAF:
            return connTypeToInt(mConnections.at(index.row()).type);
        case ITEM_WIFINET_LEAF:
            return connTypeToInt(QStringLiteral("802-11-wireless"));
        case ITEM_DEVICE_LEAF:
            if (mDevices.at(index.row()).type == nm::DeviceType::Wifi) {
                return connTypeToInt(QStringLiteral("802-11-wireless"));
            }
            return connTypeToInt(QStringLiteral("802-3-ethernet"));
        default:
            return {};
        }
    }

    if (role == ConnectionTypeStringRole) {
        switch (id) {
        case ITEM_ACTIVE_LEAF:
            return nm::connectionTypeLabel(mActive.at(index.row()).type);
        case ITEM_CONNECTION_LEAF:
            return nm::connectionTypeLabel(mConnections.at(index.row()).type);
        case ITEM_WIFINET_LEAF:
            return QStringLiteral("wireless");
        case ITEM_DEVICE_LEAF:
            return mDevices.at(index.row()).type == nm::DeviceType::Wifi ? QStringLiteral("wireless") : QStringLiteral("ethernet");
        default:
            return {};
        }
    }

    if (role == ConnectionUuidRole) {
        if (id == ITEM_ACTIVE_LEAF) {
            return mActive.at(index.row()).uuid;
        }
        if (id == ITEM_CONNECTION_LEAF) {
            return mConnections.at(index.row()).uuid;
        }
    }

    if (role == ConnectionPathRole) {
        if (id == ITEM_ACTIVE_LEAF) {
            return mActive.at(index.row()).path;
        }
        if (id == ITEM_CONNECTION_LEAF) {
            return mConnections.at(index.row()).connectionPath;
        }
    }

    if (role == ActiveConnectionStateRole && id == ITEM_ACTIVE_LEAF) {
        return static_cast<int>(mActive.at(index.row()).state);
    }

    if (role == SavedConnectionPathRole) {
        switch (id) {
        case ITEM_ACTIVE_LEAF:
            return mActive.at(index.row()).connectionPath;
        case ITEM_CONNECTION_LEAF:
            return mConnections.at(index.row()).connectionPath;
        case ITEM_WIFINET_LEAF:
            return mWifi.at(index.row()).savedConnectionPath;
        default:
            return {};
        }
    }

    if (role == AutoConnectRole) {
        auto autoconnectForPath = [this](const QString &connectionPath) -> QVariant {
            const auto it = mCache.snapshot().savedConnections.find(connectionPath);
            if (it == mCache.snapshot().savedConnections.end()) {
                return {};
            }
            return it->autoconnect;
        };
        switch (id) {
        case ITEM_ACTIVE_LEAF:
            return autoconnectForPath(mActive.at(index.row()).connectionPath);
        case ITEM_CONNECTION_LEAF:
            return autoconnectForPath(mConnections.at(index.row()).connectionPath);
        case ITEM_WIFINET_LEAF:
            if (mWifi.at(index.row()).savedConnectionPath.isEmpty()) {
                return {};
            }
            return mWifi.at(index.row()).autoconnect;
        default:
            return {};
        }
    }

    if (role == AutoConnectSupportedRole) {
        switch (id) {
        case ITEM_ACTIVE_LEAF:
            return isWirelessType(mActive.at(index.row()).type) && !mActive.at(index.row()).connectionPath.isEmpty();
        case ITEM_CONNECTION_LEAF:
            return isWirelessType(mConnections.at(index.row()).type);
        case ITEM_WIFINET_LEAF:
            return !mWifi.at(index.row()).savedConnectionPath.isEmpty();
        default:
            return false;
        }
    }

    if (role == ActiveConnectionMasterRole && id == ITEM_ACTIVE_LEAF) {
        return QStringLiteral("/");
    }

    if (role == ActiveConnectionDevicesRole && id == ITEM_ACTIVE_LEAF) {
        return mActive.at(index.row()).devices;
    }

    if (role == SignalRole) {
        if (id == ITEM_WIFINET_LEAF) {
            return mWifi.at(index.row()).strength;
        }
        if (id == ITEM_ACTIVE_LEAF) {
            const auto &active = mActive.at(index.row());
            const auto apIt = mCache.snapshot().accessPoints.find(active.specificObjectPath);
            if (apIt != mCache.snapshot().accessPoints.end()) {
                return apIt->strength;
            }
            return -1;
        }
        return -1;
    }

    if (role == IconSecurityTypeRole) {
        if (id == ITEM_WIFINET_LEAF) {
            return mWifi.at(index.row()).secure ? icons::SECURITY_HIGH : icons::SECURITY_LOW;
        }
        return -1;
    }

    if (role == IconSecurityRole) {
        const int securityType = data(index, IconSecurityTypeRole).toInt();
        if (securityType >= 0) {
            return icons::getIcon(static_cast<icons::Icon>(securityType), true);
        }
        return {};
    }

    if (role == ActiveConnectionInfoRole && id == ITEM_ACTIVE_LEAF) {
        return buildActiveInfo(mActive.at(index.row()));
    }

    if (role == IconTypeRole) {
        if (id == ITEM_WIFINET_LEAF) {
            return icons::wifiSignalIcon(mWifi.at(index.row()).strength);
        }

        if (id == ITEM_ACTIVE_LEAF) {
            const auto &active = mActive.at(index.row());
            if (nm::isVpnType(active.type) || active.isVpn) {
                return icons::NETWORK_VPN;
            }
            if (isWirelessType(active.type)) {
                if (active.state == nm::ActiveState::Activating) {
                    return icons::NETWORK_WIFI_ACQUIRING;
                }
                if (active.state == nm::ActiveState::Activated) {
                    const int signal = data(index, SignalRole).toInt();
                    return icons::wifiSignalIcon(signal);
                }
                return icons::NETWORK_WIFI_DISCONNECTED;
            }
            return active.state == nm::ActiveState::Activated ? icons::NETWORK_WIRED : icons::NETWORK_WIRED_DISCONNECTED;
        }

        if (id == ITEM_CONNECTION_LEAF) {
            const auto &conn = mConnections.at(index.row());
            if (nm::isVpnType(conn.type)) {
                return icons::NETWORK_VPN;
            }
            if (isWirelessType(conn.type)) {
                if (conn.active) {
                    const QString acPath = mCache.activeConnectionForSettingsPath(conn.connectionPath);
                    for (int i = 0; i < mActive.size(); ++i) {
                        if (mActive.at(i).path == acPath) {
                            const int signal = data(createIndex(i, 0, ITEM_ACTIVE_LEAF), SignalRole).toInt();
                            return icons::wifiSignalIcon(signal);
                        }
                    }
                }
                return icons::NETWORK_WIFI_DISCONNECTED;
            }
            return conn.active ? icons::NETWORK_WIRED : icons::NETWORK_WIRED_DISCONNECTED;
        }

        if (id == ITEM_DEVICE_LEAF) {
            const auto &dev = mDevices.at(index.row());
            if (dev.type == nm::DeviceType::Wifi) {
                if (!dev.activeAccessPointPath.isEmpty()) {
                    const auto apIt = mCache.snapshot().accessPoints.find(dev.activeAccessPointPath);
                    if (apIt != mCache.snapshot().accessPoints.end()) {
                        return icons::wifiSignalIcon(apIt->strength);
                    }
                }
                return icons::NETWORK_WIFI_DISCONNECTED;
            }
            return dev.activeConnectionPath.isEmpty() ? icons::NETWORK_WIRED_DISCONNECTED : icons::NETWORK_WIRED;
        }

        return -1;
    }

    if (role == Qt::DecorationRole || role == IconRole) {
        const int iconType = data(index, IconTypeRole).toInt();
        if (iconType >= 0) {
            return icons::getIcon(static_cast<icons::Icon>(iconType), true);
        }
    }

    return {};
}

QModelIndex NmModel::indexTypeRoot(ItemType type) const
{
    switch (type) {
    case HelperType:
        return createIndex(0, 0, ITEM_ROOT);
    case ActiveConnectionType:
        return createIndex(0, 0, ITEM_ACTIVE);
    case ConnectionType:
        return createIndex(1, 0, ITEM_CONNECTION);
    case DeviceType:
        return createIndex(2, 0, ITEM_DEVICE);
    case WifiNetworkType:
        return createIndex(3, 0, ITEM_WIFINET);
    }
    return {};
}

bool NmModel::networkingEnabled() const
{
    return mManagerState.networkingEnabled;
}

bool NmModel::wirelessEnabled() const
{
    return mManagerState.wirelessEnabled;
}

bool NmModel::wirelessHardwareEnabled() const
{
    return mManagerState.wirelessHardwareEnabled;
}

QString NmModel::primaryConnectionPath() const
{
    return mManagerState.primaryConnectionPath;
}

QString NmModel::primaryPhysicalConnectionPath() const
{
    auto isPhysical = [](const nm::ActiveConnectionRecord &active) {
        return !active.isVpn && !nm::isVpnType(active.type) && !active.devices.isEmpty();
    };

    const auto primaryIt = std::find_if(mActive.cbegin(), mActive.cend(), [this](const nm::ActiveConnectionRecord &active) {
        return active.path == mManagerState.primaryConnectionPath;
    });
    if (primaryIt != mActive.cend() && isPhysical(*primaryIt)) {
        return primaryIt->path;
    }

    const auto defaultIt = std::find_if(mActive.cbegin(), mActive.cend(), [isPhysical](const nm::ActiveConnectionRecord &active) {
        return isPhysical(active) && (active.isDefault4 || active.isDefault6);
    });
    if (defaultIt != mActive.cend()) {
        return defaultIt->path;
    }

    const auto firstPhysicalIt = std::find_if(mActive.cbegin(), mActive.cend(), isPhysical);
    return firstPhysicalIt == mActive.cend() ? QString{} : firstPhysicalIt->path;
}

QString NmModel::primaryPhysicalInterfaceName() const
{
    auto interfaceForActive = [this](const nm::ActiveConnectionRecord &active) -> QString {
        if (active.isVpn || nm::isVpnType(active.type)) {
            return {};
        }
        for (const QString &devicePath : active.devices) {
            const auto devIt = mCache.snapshot().devices.find(devicePath);
            if (devIt != mCache.snapshot().devices.end() && !devIt->interfaceName.isEmpty()) {
                return devIt->interfaceName;
            }
        }
        return {};
    };

    const auto primaryIt = std::find_if(mActive.cbegin(), mActive.cend(), [this](const nm::ActiveConnectionRecord &active) {
        return active.path == mManagerState.primaryConnectionPath;
    });
    if (primaryIt != mActive.cend()) {
        const QString iface = interfaceForActive(*primaryIt);
        if (!iface.isEmpty()) {
            return iface;
        }
    }

    const auto defaultIt = std::find_if(mActive.cbegin(), mActive.cend(), [](const nm::ActiveConnectionRecord &active) {
        return !active.isVpn && !nm::isVpnType(active.type) && (active.isDefault4 || active.isDefault6);
    });
    if (defaultIt != mActive.cend()) {
        const QString iface = interfaceForActive(*defaultIt);
        if (!iface.isEmpty()) {
            return iface;
        }
    }

    for (const auto &active : mActive) {
        const QString iface = interfaceForActive(active);
        if (!iface.isEmpty()) {
            return iface;
        }
    }
    return {};
}

bool NmModel::showLowSignalNetworks() const
{
    return mShowLowSignalNetworks;
}

const nm::Snapshot &NmModel::cacheSnapshot() const
{
    return mCache.snapshot();
}

QList<nm::ActiveConnectionRecord> NmModel::activeConnections() const
{
    return mActive;
}

NmModel::ManagerState NmModel::managerState() const
{
    return mManagerState;
}

QList<NmModel::RecentConnection> NmModel::recentConnections(int maxCount) const
{
    QList<RecentConnection> recent;
    QString currentConnectionPath;
    const auto activeIt = std::find_if(mActive.cbegin(), mActive.cend(), [this](const nm::ActiveConnectionRecord &active) {
        return active.path == mManagerState.primaryConnectionPath;
    });
    if (activeIt != mActive.cend()) {
        currentConnectionPath = activeIt->connectionPath;
    }

    for (const auto &conn : mConnections) {
        if (conn.lastUsedTimestamp <= 0) {
            continue;
        }
        if (!currentConnectionPath.isEmpty() && conn.connectionPath == currentConnectionPath) {
            continue;
        }
        RecentConnection r;
        r.id = conn.id;
        r.connectionPath = conn.connectionPath;
        r.lastUsedTimestamp = conn.lastUsedTimestamp;
        recent.push_back(r);
    }

    std::sort(recent.begin(), recent.end(), [](const RecentConnection &a, const RecentConnection &b) {
        if (a.lastUsedTimestamp != b.lastUsedTimestamp) {
            return a.lastUsedTimestamp > b.lastUsedTimestamp;
        }
        return QString::compare(a.id, b.id, Qt::CaseInsensitive) < 0;
    });
    if (maxCount > 0 && recent.size() > maxCount) {
        recent = recent.mid(0, maxCount);
    }
    return recent;
}

void NmModel::activateConnection(const QModelIndex &index)
{
    if (!isValidDataIndex(index)) {
        return;
    }
    const ItemId id = static_cast<ItemId>(index.internalId());

    if (id == ITEM_CONNECTION_LEAF) {
        const auto &conn = mConnections.at(index.row());
        QString devicePath = QStringLiteral("/");
        QString specificObject = QStringLiteral("/");

        if (isWirelessType(conn.type)) {
            const auto sIt = mCache.snapshot().savedConnections.find(conn.connectionPath);
            const QString ifaceHint = sIt == mCache.snapshot().savedConnections.end() ? QString{} : sIt->interfaceName;
            const QByteArray ssidBytes = sIt == mCache.snapshot().savedConnections.end() ? QByteArray{} : sIt->wifiSsidBytes;
            const WifiTarget target = pickWifiTarget(mCache.snapshot(), ifaceHint, ssidBytes);
            if (target.devicePath.isEmpty()) {
                emit actionFailed(tr("No usable Wi-Fi adapter"), tr("Enable Wi-Fi and try again."));
                return;
            }
            devicePath = target.devicePath;
            specificObject = target.apPath.isEmpty() ? QStringLiteral("/") : target.apPath;
        }

        nm::NmActions::activateConnection(conn.connectionPath, devicePath, specificObject, this,
            [this, id = conn.id](bool ok, const QString &err, const QDBusMessage &) {
                if (!ok) {
                    qCWarning(NM_TRAY).noquote() << QStringLiteral("activateConnection failed for '%1': %2").arg(id, err);
                    emit actionFailed(tr("Could not connect to %1").arg(id), err);
                }
            });
        return;
    }

    if (id == ITEM_WIFINET_LEAF) {
        connectToWifi(mWifi.at(index.row()));
    }
}

void NmModel::deactivateConnection(const QModelIndex &index)
{
    if (!isValidDataIndex(index) || static_cast<ItemId>(index.internalId()) != ITEM_ACTIVE_LEAF) {
        return;
    }
    disconnectActiveConnection(mActive.at(index.row()), false);
}

void NmModel::requestScan(const QModelIndex &index) const
{
    if (!isValidDataIndex(index) || static_cast<ItemId>(index.internalId()) != ITEM_DEVICE_LEAF) {
        return;
    }
    const auto &dev = mDevices.at(index.row());
    if (dev.type != nm::DeviceType::Wifi) {
        return;
    }
    nm::NmActions::requestScan(dev.path, const_cast<NmModel *>(this),
        [iface = dev.interfaceName](bool ok, const QString &err, const QDBusMessage &) {
            if (!ok) {
                qCWarning(NM_TRAY).noquote() << QStringLiteral("requestScan failed for '%1': %2").arg(iface, err);
            }
        });
}

void NmModel::requestAllWifiScan() const
{
    for (int i = 0; i < mDevices.size(); ++i) {
        const auto &dev = mDevices.at(i);
        if (dev.type != nm::DeviceType::Wifi) {
            continue;
        }
        nm::NmActions::requestScan(dev.path, const_cast<NmModel *>(this),
            [iface = dev.interfaceName](bool ok, const QString &err, const QDBusMessage &) {
                if (!ok) {
                    qCWarning(NM_TRAY).noquote() << QStringLiteral("requestScan failed for '%1': %2").arg(iface, err);
                }
            });
    }
}

void NmModel::setNetworkingEnabled(bool enabled)
{
    nm::NmActions::setNetworkingEnabled(enabled, this, [this](bool ok, const QString &err, const QDBusMessage &) {
        if (!ok) {
            qCWarning(NM_TRAY).noquote() << QStringLiteral("setNetworkingEnabled failed: %1").arg(err);
            emit actionFailed(tr("Could not change networking state"), err);
        }
    });
}

void NmModel::setWirelessEnabled(bool enabled)
{
    nm::NmActions::setWirelessEnabled(enabled, this, [this](bool ok, const QString &err, const QDBusMessage &) {
        if (!ok) {
            qCWarning(NM_TRAY).noquote() << QStringLiteral("setWirelessEnabled failed: %1").arg(err);
            emit actionFailed(tr("Could not change Wi-Fi state"), err);
        }
    });
}

void NmModel::setConnectionAutoconnect(const QString &connectionPath, bool enabled)
{
    if (connectionPath.isEmpty() || connectionPath == QStringLiteral("/")) {
        return;
    }

    nm::NmActions::setConnectionAutoconnect(connectionPath, enabled, this,
        [this, connectionPath](bool ok, const QString &err, const QDBusMessage &) {
            if (!ok) {
                qCWarning(NM_TRAY).noquote() << QStringLiteral("setConnectionAutoconnect failed for '%1': %2").arg(connectionPath, err);
                emit actionFailed(tr("Could not change automatic connection"), err);
                return;
            }
            QMetaObject::invokeMethod(mDbus, "refreshNow", Qt::QueuedConnection);
        });
}

void NmModel::setShowLowSignalNetworks(bool enabled)
{
    if (mShowLowSignalNetworks == enabled) {
        return;
    }
    mShowLowSignalNetworks = enabled;
    rebuildFromSnapshot(mCache.snapshot());
}

void NmModel::setOrderHold(bool held)
{
    if (mOrderHeld == held) {
        return;
    }
    mOrderHeld = held;
    if (!mOrderHeld) {
        rebuildFromSnapshot(mCache.snapshot());
    }
}

void NmModel::disconnectPrimaryConnection()
{
    if (mManagerState.primaryConnectionPath.isEmpty() || mManagerState.primaryConnectionPath == QStringLiteral("/")) {
        return;
    }

    const auto activeIt = std::find_if(mActive.cbegin(), mActive.cend(), [this](const nm::ActiveConnectionRecord &active) {
        return active.path == mManagerState.primaryConnectionPath;
    });
    if (activeIt == mActive.cend()) {
        nm::NmActions::deactivateConnection(mManagerState.primaryConnectionPath, this,
            [this](bool ok, const QString &err, const QDBusMessage &) {
                if (!ok) {
                    qCWarning(NM_TRAY).noquote() << QStringLiteral("disconnectPrimaryConnection failed: %1").arg(err);
                    emit actionFailed(tr("Could not disconnect"), err);
                }
            });
        return;
    }

    disconnectActiveConnection(*activeIt, false);
}

void NmModel::disconnectPrimaryConnectionAndStayOff()
{
    if (mManagerState.primaryConnectionPath.isEmpty() || mManagerState.primaryConnectionPath == QStringLiteral("/")) {
        return;
    }
    const auto activeIt = std::find_if(mActive.cbegin(), mActive.cend(), [this](const nm::ActiveConnectionRecord &active) {
        return active.path == mManagerState.primaryConnectionPath;
    });
    if (activeIt != mActive.cend()) {
        disconnectActiveConnection(*activeIt, true);
    }
}

void NmModel::activateConnectionPath(const QString &connectionPath)
{
    if (connectionPath.isEmpty()) {
        return;
    }

    const auto connIt = std::find_if(mConnections.cbegin(), mConnections.cend(), [&connectionPath](const nm::ConnectionViewRecord &conn) {
        return conn.connectionPath == connectionPath;
    });
    if (connIt == mConnections.cend()) {
        return;
    }

    QString devicePath = QStringLiteral("/");
    QString specificObject = QStringLiteral("/");
    if (isWirelessType(connIt->type)) {
        QString ifaceHint;
        const auto sIt = mCache.snapshot().savedConnections.find(connIt->connectionPath);
        if (sIt != mCache.snapshot().savedConnections.end()) {
            ifaceHint = sIt->interfaceName;
        }
        const QByteArray ssidBytes = sIt == mCache.snapshot().savedConnections.end() ? QByteArray{} : sIt->wifiSsidBytes;
        const WifiTarget target = pickWifiTarget(mCache.snapshot(), ifaceHint, ssidBytes);
        if (target.devicePath.isEmpty()) {
            emit actionFailed(tr("No usable Wi-Fi adapter"), tr("Enable Wi-Fi and try again."));
            return;
        }
        devicePath = target.devicePath;
        specificObject = target.apPath.isEmpty() ? QStringLiteral("/") : target.apPath;
    }

    nm::NmActions::activateConnection(connIt->connectionPath, devicePath, specificObject, this,
        [this, id = connIt->id](bool ok, const QString &err, const QDBusMessage &) {
            if (!ok) {
                qCWarning(NM_TRAY).noquote() << QStringLiteral("activateConnectionPath failed for '%1': %2").arg(id, err);
                emit actionFailed(tr("Could not connect to %1").arg(id), err);
            }
        });
}

void NmModel::activateSavedOnAp(const QString &connectionPath, const QString &devicePath, const QString &apPath)
{
    if (connectionPath.isEmpty()) {
        return;
    }
    nm::NmActions::activateConnection(connectionPath, devicePath, apPath, this,
        [this](bool ok, const QString &err, const QDBusMessage &) {
            if (!ok) {
                emit actionFailed(tr("Could not connect"), err);
            }
        });
}

void NmModel::onSnapshotChanged(const nm::Snapshot &snapshot)
{
    rebuildFromSnapshot(snapshot);
}

bool NmModel::isValidDataIndex(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return false;
    }

    switch (static_cast<ItemId>(index.internalId())) {
    case ITEM_ROOT:
    case ITEM_ACTIVE:
    case ITEM_CONNECTION:
    case ITEM_DEVICE:
    case ITEM_WIFINET:
        return true;
    case ITEM_ACTIVE_LEAF:
        return index.row() >= 0 && index.row() < mActive.size() && index.column() == 0;
    case ITEM_CONNECTION_LEAF:
        return index.row() >= 0 && index.row() < mConnections.size() && index.column() == 0;
    case ITEM_DEVICE_LEAF:
        return index.row() >= 0 && index.row() < mDevices.size() && index.column() == 0;
    case ITEM_WIFINET_LEAF:
        return index.row() >= 0 && index.row() < mWifi.size() && index.column() == 0;
    }
    return false;
}

void NmModel::rebuildFromSnapshot(const nm::Snapshot &snapshot)
{
    static constexpr int kMinShownSignalPercent = 25;
    const ManagerState oldManagerState = mManagerState;

    mCache.setSnapshot(snapshot);
    QList<nm::ActiveConnectionRecord> nextActive = mCache.activeConnections();
    QList<nm::ConnectionViewRecord> nextConnections = mCache.knownConnections(false);
    QList<nm::DeviceRecord> nextDevices = mCache.devices();
    QList<nm::WifiViewRecord> nextWifi = mCache.wifiEntries(false);
    if (!mShowLowSignalNetworks) {
        QList<nm::WifiViewRecord> filtered;
        filtered.reserve(nextWifi.size());
        for (const auto &wifi : nextWifi) {
            if (wifi.active || wifi.strength >= kMinShownSignalPercent) {
                filtered.push_back(wifi);
            }
        }
        nextWifi = std::move(filtered);
    }

    if (mOrderHeld) {
        QList<nm::WifiViewRecord> rest = std::move(nextWifi);
        nextWifi.clear();
        for (const auto &currentWifi : mWifi) {
            const auto it = std::find_if(rest.begin(), rest.end(), [&currentWifi](const nm::WifiViewRecord &wifi) {
                return wifi.apPath == currentWifi.apPath;
            });
            if (it != rest.end()) {
                nextWifi.push_back(*it);
                rest.erase(it);
            }
        }
        nextWifi.append(rest);
    }

    auto applySection = [this](ItemId sectionId, auto &current, const auto &next, auto keyOf, auto isSameItem) {
        int sectionRow = -1;
        ItemId leafId = ITEM_ROOT;
        switch (sectionId) {
        case ITEM_ACTIVE:
            sectionRow = 0;
            leafId = ITEM_ACTIVE_LEAF;
            break;
        case ITEM_CONNECTION:
            sectionRow = 1;
            leafId = ITEM_CONNECTION_LEAF;
            break;
        case ITEM_DEVICE:
            sectionRow = 2;
            leafId = ITEM_DEVICE_LEAF;
            break;
        case ITEM_WIFINET:
            sectionRow = 3;
            leafId = ITEM_WIFINET_LEAF;
            break;
        default:
            break;
        }
        if (sectionRow < 0 || leafId == ITEM_ROOT) {
            current = next;
            return;
        }

        const QModelIndex parentIdx = createIndex(sectionRow, 0, sectionId);

        QSet<QString> nextSet;
        nextSet.reserve(next.size());
        for (const auto &item : next) {
            nextSet.insert(keyOf(item));
        }

        for (int i = current.size() - 1; i >= 0; --i) {
            if (!nextSet.contains(keyOf(current.at(i)))) {
                beginRemoveRows(parentIdx, i, i);
                current.removeAt(i);
                endRemoveRows();
            }
        }

        for (int i = 0; i < next.size(); ++i) {
            const QString wantKey = keyOf(next.at(i));
            if (i < current.size() && keyOf(current.at(i)) == wantKey) {
                continue;
            }
            int found = -1;
            for (int k = i + 1; k < current.size(); ++k) {
                if (keyOf(current.at(k)) == wantKey) {
                    found = k;
                    break;
                }
            }
            if (found >= 0) {
                beginMoveRows(parentIdx, found, found, parentIdx, i);
                current.move(found, i);
                endMoveRows();
            } else {
                beginInsertRows(parentIdx, i, i);
                current.insert(i, next.at(i));
                endInsertRows();
            }
        }

        for (int i = 0; i < current.size(); ++i) {
            if (!isSameItem(current.at(i), next.at(i))) {
                current[i] = next.at(i);
                emit dataChanged(createIndex(i, 0, leafId), createIndex(i, 0, leafId));
            }
        }
    };

    applySection(ITEM_ACTIVE, mActive, nextActive, [](const nm::ActiveConnectionRecord &item) { return item.path; }, activeEquivalent);
    applySection(ITEM_CONNECTION, mConnections, nextConnections, [](const nm::ConnectionViewRecord &item) { return item.connectionPath; }, connectionEquivalent);
    applySection(ITEM_DEVICE, mDevices, nextDevices, [](const nm::DeviceRecord &item) { return item.path; }, deviceEquivalent);
    applySection(ITEM_WIFINET, mWifi, nextWifi, [](const nm::WifiViewRecord &item) { return item.apPath; }, wifiEquivalent);

    ManagerState nextManagerState;
    nextManagerState.networkingEnabled = mCache.snapshot().manager.networkingEnabled;
    nextManagerState.wirelessEnabled = mCache.snapshot().manager.wirelessEnabled;
    nextManagerState.wirelessHardwareEnabled = mCache.snapshot().manager.wirelessHardwareEnabled;
    nextManagerState.primaryConnectionPath = mCache.snapshot().manager.primaryConnectionPath;
    nextManagerState.rawNmState = mCache.snapshot().manager.state;
    nextManagerState.rawConnectivity = mCache.snapshot().manager.connectivity;
    nextManagerState.connectivityCheckAvailable = mCache.snapshot().manager.connectivityCheckAvailable;
    nextManagerState.connectivityCheckEnabled = mCache.snapshot().manager.connectivityCheckEnabled;
    nextManagerState.connectivityCheckUri = mCache.snapshot().manager.connectivityCheckUri;
    nextManagerState.overallState = toOverallState(mCache.snapshot().manager.state, mCache.snapshot().manager.connectivity);
    nextManagerState.primaryKind = PrimaryKind::Unknown;
    nextManagerState.primaryName.clear();
    nextManagerState.wifiStrength = -1;
    nextManagerState.vpnActive.clear();
    nextManagerState.lastError = mCache.snapshot().manager.lastError;

    for (const auto &active : mActive) {
        if (active.isVpn || nm::isVpnType(active.type)) {
            nextManagerState.vpnActive.push_back(active.id);
        }
    }

    const auto primaryIt = std::find_if(mActive.cbegin(), mActive.cend(), [this, &nextManagerState](const nm::ActiveConnectionRecord &active) {
        return active.path == nextManagerState.primaryConnectionPath;
    });
    if (primaryIt != mActive.cend()) {
        nextManagerState.primaryName = primaryIt->id;
        if (isWirelessType(primaryIt->type)) {
            nextManagerState.primaryKind = PrimaryKind::Wifi;
            const auto apIt = mCache.snapshot().accessPoints.find(primaryIt->specificObjectPath);
            if (apIt != mCache.snapshot().accessPoints.end()) {
                nextManagerState.primaryName = apIt->ssid;
                nextManagerState.wifiStrength = apIt->strength;
            } else if (!primaryIt->devices.isEmpty()) {
                const auto devIt = mCache.snapshot().devices.find(primaryIt->devices.first());
                if (devIt != mCache.snapshot().devices.end()) {
                    const auto activeAp = mCache.snapshot().accessPoints.find(devIt->activeAccessPointPath);
                    if (activeAp != mCache.snapshot().accessPoints.end()) {
                        nextManagerState.primaryName = activeAp->ssid;
                        nextManagerState.wifiStrength = activeAp->strength;
                    }
                }
            }
        } else {
            nextManagerState.primaryKind = PrimaryKind::Wired;
        }
    }

    mManagerState = nextManagerState;
    if (!managerStateEquivalent(oldManagerState, nextManagerState)) {
        emit managerStateChanged();
    }
}

void NmModel::disconnectActiveConnection(const nm::ActiveConnectionRecord &active, bool stayOff)
{
    const bool isDeviceBacked = !active.isVpn
        && (active.type == QStringLiteral("802-11-wireless")
            || active.type == QStringLiteral("802-3-ethernet")
            || active.type == QStringLiteral("bluetooth"));

    if (stayOff && isDeviceBacked && !active.devices.isEmpty()) {
        const QString devicePath = active.devices.front();
        nm::NmActions::disconnectDevice(devicePath, this,
            [this, id = active.id, devicePath](bool ok, const QString &err, const QDBusMessage &) {
                if (!ok) {
                    qCWarning(NM_TRAY).noquote()
                        << QStringLiteral("disconnectDevice failed for '%1' on '%2': %3").arg(id, devicePath, err);
                    emit actionFailed(tr("Could not disconnect and stay offline"), err);
                }
            });
        return;
    }

    nm::NmActions::deactivateConnection(active.path, this,
        [this, id = active.id](bool ok, const QString &err, const QDBusMessage &) {
            if (!ok) {
                qCWarning(NM_TRAY).noquote()
                    << QStringLiteral("deactivateConnection failed for '%1': %2").arg(id, err);
                emit actionFailed(tr("Could not disconnect %1").arg(id), err);
            }
        });
}

QString NmModel::buildActiveInfo(const nm::ActiveConnectionRecord &active) const
{
    QString info;
    QDebug str(&info);
    str.noquote();
    str.nospace();

    str << QStringLiteral("<table>")
        << QStringLiteral("<tr><td colspan='2'><big><strong>") << tr("General", "Active connection information") << QStringLiteral("</strong></big></td></tr>")
        << QStringLiteral("<tr><td><strong>") << tr("Connection", "Active connection information") << QStringLiteral("</strong>: </td><td>") << active.id.toHtmlEscaped() << QStringLiteral("</td></tr>")
        << QStringLiteral("<tr><td><strong>") << tr("Type", "Active connection information") << QStringLiteral("</strong>: </td><td>") << nm::connectionTypeLabel(active.type).toHtmlEscaped() << QStringLiteral("</td></tr>")
        << QStringLiteral("<tr><td><strong>") << tr("UUID", "Active connection information") << QStringLiteral("</strong>: </td><td>") << active.uuid.toHtmlEscaped() << QStringLiteral("</td></tr>");

    if (!active.devices.isEmpty()) {
        const auto devIt = mCache.snapshot().devices.find(active.devices.front());
        if (devIt != mCache.snapshot().devices.end()) {
            str << QStringLiteral("<tr><td><strong>") << tr("Interface", "Active connection information") << QStringLiteral("</strong>: </td><td>")
                << devIt->interfaceName.toHtmlEscaped() << QStringLiteral("</td></tr>");
            if (!devIt->hardwareAddress.isEmpty()) {
                str << QStringLiteral("<tr><td><strong>") << tr("Hardware Address", "Active connection information") << QStringLiteral("</strong>: </td><td>")
                    << devIt->hardwareAddress.toHtmlEscaped() << QStringLiteral("</td></tr>");
            }
            if (devIt->bitrateKbps > 0) {
                str << QStringLiteral("<tr><td><strong>") << tr("Speed", "Active connection information") << QStringLiteral("</strong>: </td><td>")
                    << QLocale::system().toString(static_cast<double>(devIt->bitrateKbps) / 1000.0, 'g', 5) << QStringLiteral(" Mb/s</td></tr>");
            }
            if (devIt->rxBytes > 0 || devIt->txBytes > 0) {
                str << QStringLiteral("<tr><td><strong>") << tr("Data received", "Active connection information") << QStringLiteral("</strong>: </td><td>")
                    << formatBytes(devIt->rxBytes) << QStringLiteral("</td></tr>")
                    << QStringLiteral("<tr><td><strong>") << tr("Data transmitted", "Active connection information") << QStringLiteral("</strong>: </td><td>")
                    << formatBytes(devIt->txBytes) << QStringLiteral("</td></tr>");
            }
        }
    }

    auto appendIpConfig = [&str](const QString &title,
                                 const QStringList &addresses,
                                 const QString &gateway,
                                 const QStringList &dns,
                                 int routeCount) {
        if (addresses.isEmpty() && gateway.isEmpty() && dns.isEmpty() && routeCount <= 0) {
            return;
        }

        str << QStringLiteral("<tr/><tr><td colspan='2'><big><strong>") << title << QStringLiteral("</strong></big></td></tr>");

        for (int i = 0; i < addresses.size(); ++i) {
            const QString suffix = i > 0 ? QStringLiteral("(%1)").arg(i + 1) : QString{};
            str << QStringLiteral("<tr><td><strong>") << tr("IP Address", "Active connection information") << suffix
                << QStringLiteral("</strong>: </td><td>") << addresses.at(i).toHtmlEscaped() << QStringLiteral("</td></tr>");
        }

        if (!gateway.isEmpty()) {
            str << QStringLiteral("<tr><td><strong>") << tr("Default route", "Active connection information")
                << QStringLiteral("</strong>: </td><td>") << gateway.toHtmlEscaped() << QStringLiteral("</td></tr>");
        }

        for (int i = 0; i < dns.size(); ++i) {
            str << QStringLiteral("<tr><td><strong>") << tr("DNS(%1)", "Active connection information").arg(i + 1)
                << QStringLiteral("</strong>: </td><td>") << dns.at(i).toHtmlEscaped() << QStringLiteral("</td></tr>");
        }

        if (routeCount > 0) {
            str << QStringLiteral("<tr><td><strong>") << tr("Routes", "Active connection information")
                << QStringLiteral("</strong>: </td><td>") << routeCount << QStringLiteral("</td></tr>");
        }
    };

    appendIpConfig(tr("IPv4", "Active connection information"),
                   active.ip4Addresses,
                   active.ip4Gateway,
                   active.ip4Dns,
                   active.ip4RouteCount);
    appendIpConfig(tr("IPv6", "Active connection information"),
                   active.ip6Addresses,
                   active.ip6Gateway,
                   active.ip6Dns,
                   active.ip6RouteCount);

    str << QStringLiteral("</table>");
    return info;
}

void NmModel::connectToWifi(const nm::WifiViewRecord &wifi)
{
    const auto apIt = mCache.snapshot().accessPoints.find(wifi.apPath);
    if (apIt == mCache.snapshot().accessPoints.end()) {
        emit actionFailed(tr("Could not connect to %1").arg(wifi.ssid), tr("This access point is no longer in range."));
        return;
    }

    const WifiTarget target = pickWifiTarget(mCache.snapshot(), {}, apIt->ssidBytes);
    if (target.devicePath.isEmpty()) {
        emit actionFailed(tr("No usable Wi-Fi adapter"), tr("Enable Wi-Fi and try again."));
        return;
    }

    QString savedConnectionPath = wifi.savedConnectionPath;
    if (savedConnectionPath.isEmpty()) {
        savedConnectionPath = mCache.connectionPathForSsid(wifi.ssid);
    }

    if (!savedConnectionPath.isEmpty()) {
        nm::NmActions::activateConnection(savedConnectionPath, target.devicePath, target.apPath, this,
            [this, wifi, savedConnectionPath, target](bool ok, const QString &err, const QDBusMessage &reply) {
                if (!ok) {
                    qCWarning(NM_TRAY).noquote() << QStringLiteral("activateConnection failed for SSID '%1': %2").arg(wifi.ssid, err);
                    if (err.contains(QStringLiteral("password"), Qt::CaseInsensitive)
                        || err.contains(QStringLiteral("secret"), Qt::CaseInsensitive)) {
                        promptForUpdatedWifiPassword(wifi, savedConnectionPath, target.devicePath, target.apPath);
                    } else {
                        emit actionFailed(tr("Could not connect to %1").arg(wifi.ssid), err);
                    }
                    return;
                }
                startActivationWatch(firstObjectPathFromReply(reply), wifi, savedConnectionPath, target.devicePath, target.apPath, true);
            });
        return;
    }

    const QString keyMgmt = nm::keyMgmtForAp(apIt->wpaFlags, apIt->rsnFlags, apIt->privacy);
    if (keyMgmt == QLatin1String("wpa-eap")) {
        emit actionFailed(tr("Enterprise Wi-Fi needs setup"),
                          tr("Open the connection editor to create a profile for %1.").arg(wifi.ssid));
        return;
    }
    promptAndCreateWifiConnection(wifi.ssid, target.devicePath, wifi.secure);
}

void NmModel::startActivationWatch(const QString &activeConnectionPath,
                                   const nm::WifiViewRecord &wifi,
                                   const QString &settingsPath,
                                   const QString &devicePath,
                                   const QString &apPath,
                                   bool savedActivation)
{
    if (activeConnectionPath.isEmpty()) {
        return;
    }

    auto *watcher = new nm::WifiActivationWatcher(activeConnectionPath, this);
    connect(watcher, &nm::WifiActivationWatcher::finished, this,
            [this, watcher, wifi, settingsPath, devicePath, apPath, savedActivation](nm::WifiActivationWatcher::Outcome outcome,
                                                                                    const QString &message) {
                watcher->deleteLater();
                if (outcome == nm::WifiActivationWatcher::Outcome::Success) {
                    return;
                }
                if (outcome == nm::WifiActivationWatcher::Outcome::WrongOrMissingPassword && savedActivation) {
                    promptForUpdatedWifiPassword(wifi, settingsPath, devicePath, apPath);
                    return;
                }
                if (outcome == nm::WifiActivationWatcher::Outcome::WrongOrMissingPassword && !settingsPath.isEmpty()) {
                    promptForUpdatedWifiPassword(wifi, settingsPath, devicePath, apPath);
                    return;
                }
                emit actionFailed(tr("Could not connect to %1").arg(wifi.ssid), message);
            });
    watcher->start();
}

void NmModel::promptForUpdatedWifiPassword(const nm::WifiViewRecord &wifi,
                                           const QString &settingsPath,
                                           const QString &devicePath,
                                           const QString &apPath)
{
    if (settingsPath.isEmpty()) {
        emit actionFailed(tr("A password is needed"), tr("No saved Wi-Fi profile was available to update."));
        return;
    }

    auto *dialog = new WifiPasswordDialog(wifi.ssid, true, nullptr);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setInfoText(tr("The saved password for “%1” no longer works. Enter it again.").arg(wifi.ssid));
    connect(dialog, &QDialog::accepted, this, [this, dialog, wifi, settingsPath, devicePath, apPath] {
        nm::NmActions::updateSavedPsk(settingsPath, dialog->password(), this,
            [this, wifi, settingsPath, devicePath, apPath](bool ok, const QString &err, const QDBusMessage &) {
                if (!ok) {
                    emit actionFailed(tr("Could not save password"), err);
                    return;
                }
                nm::NmActions::activateConnection(settingsPath, devicePath, apPath, this,
                    [this, wifi, settingsPath, devicePath, apPath](bool ok2, const QString &err2, const QDBusMessage &reply2) {
                        if (!ok2) {
                            emit actionFailed(tr("Could not connect to %1").arg(wifi.ssid), err2);
                            return;
                        }
                        startActivationWatch(firstObjectPathFromReply(reply2), wifi, settingsPath, devicePath, apPath, true);
                    });
            });
    });
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void NmModel::promptAndCreateWifiConnection(const QString &ssid, const QString &devicePath, bool secure)
{
    auto *dialog = new WifiPasswordDialog(ssid, secure, nullptr);
    dialog->setAttribute(Qt::WA_DeleteOnClose);

    connect(dialog, &QDialog::accepted, this, [this, ssid, devicePath, secure, dialog]() {
        const auto apIt = std::find_if(mCache.snapshot().accessPoints.cbegin(),
                                       mCache.snapshot().accessPoints.cend(),
                                       [&ssid, &devicePath](const nm::AccessPointRecord &ap) {
                                           return ap.ssid == ssid && ap.devicePath == devicePath;
                                       });
        if (apIt == mCache.snapshot().accessPoints.cend()) {
            emit actionFailed(tr("Could not connect to %1").arg(ssid), tr("This access point is no longer in range."));
            return;
        }
        const QString password = dialog->password();
        nm::NmActions::addAndActivateWifi(*apIt, devicePath, password, this,
            [this, ssid, secure, devicePath, apPath = apIt->path](bool ok, const QString &err, const QDBusMessage &reply) {
                if (!ok) {
                    qCWarning(NM_TRAY).noquote() << QStringLiteral("Failed to create Wi-Fi connection for '%1': %2").arg(ssid, err);
                    emit actionFailed(tr("Could not connect to %1").arg(ssid), err);
                    return;
                }
                if (mDbus != nullptr) {
                    QMetaObject::invokeMethod(mDbus, "refreshNow", Qt::QueuedConnection);
                }
                nm::WifiViewRecord wifi;
                wifi.ssid = ssid;
                wifi.secure = secure;
                wifi.devicePath = devicePath;
                wifi.apPath = apPath;
                const QString settingsPath = firstObjectPathFromReply(reply);
                const QString activePath = lastObjectPathFromReply(reply);
                startActivationWatch(activePath, wifi, settingsPath, devicePath, apPath, false);
            });
    });

    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void NmModel::promptAndCreateHiddenWifiConnection()
{
    QString devicePath;
    for (const auto &dev : mDevices) {
        if (dev.type == nm::DeviceType::Wifi && dev.state >= 30) {
            devicePath = dev.path;
            break;
        }
    }
    if (devicePath.isEmpty()) {
        emit actionFailed(tr("No usable Wi-Fi adapter"), tr("Enable Wi-Fi and try again."));
        return;
    }

    auto *dialog = new QDialog(nullptr);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Connect to hidden Wi-Fi network"));
    auto *layout = new QFormLayout(dialog);
    auto *ssidEdit = new QLineEdit(dialog);
    auto *secureCheck = new QCheckBox(tr("WPA/WPA2/WPA3 personal security"), dialog);
    secureCheck->setChecked(true);
    auto *passwordEdit = new QLineEdit(dialog);
    passwordEdit->setEchoMode(QLineEdit::Password);
    auto *hint = new QLabel(tr("Passwords must be 8–63 characters, or exactly 64 hex digits."), dialog);
    hint->setWordWrap(true);
    layout->addRow(tr("Network name:"), ssidEdit);
    layout->addRow(QString{}, secureCheck);
    layout->addRow(tr("Password:"), passwordEdit);
    layout->addRow(QString{}, hint);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    auto *okButton = buttons->button(QDialogButtonBox::Ok);
    okButton->setText(tr("Connect"));
    layout->addRow(buttons);

    auto validate = [ssidEdit, secureCheck, passwordEdit, okButton] {
        const bool ssidOk = !ssidEdit->text().isEmpty();
        const bool passOk = !secureCheck->isChecked() || nm::isWpaPskValid(passwordEdit->text());
        okButton->setEnabled(ssidOk && passOk);
    };
    connect(ssidEdit, &QLineEdit::textChanged, dialog, validate);
    connect(passwordEdit, &QLineEdit::textChanged, dialog, validate);
    connect(secureCheck, &QCheckBox::toggled, dialog, [passwordEdit, hint, validate](bool checked) {
        passwordEdit->setEnabled(checked);
        hint->setVisible(checked);
        validate();
    });
    validate();

    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(dialog, &QDialog::accepted, this, [this, dialog, ssidEdit, secureCheck, passwordEdit, devicePath] {
        const QString ssid = ssidEdit->text();
        nm::AccessPointRecord ap;
        ap.ssid = ssid;
        ap.ssidBytes = ssid.toUtf8();
        ap.devicePath = devicePath;
        ap.path = QStringLiteral("/");
        ap.privacy = secureCheck->isChecked();
        if (secureCheck->isChecked()) {
            ap.rsnFlags = 0x100U; // KEY_MGMT_PSK
        }
        nm::NmActions::addAndActivateWifi(ap, devicePath, passwordEdit->text(), this,
            [this, ssid, devicePath](bool ok, const QString &err, const QDBusMessage &reply) {
                if (!ok) {
                    emit actionFailed(tr("Could not connect to hidden network %1").arg(ssid), err);
                    return;
                }
                nm::WifiViewRecord wifi;
                wifi.ssid = ssid;
                wifi.ssidBytes = ssid.toUtf8();
                wifi.devicePath = devicePath;
                wifi.secure = true;
                const QString settingsPath = firstObjectPathFromReply(reply);
                const QString activePath = lastObjectPathFromReply(reply);
                startActivationWatch(activePath, wifi, settingsPath, devicePath, QStringLiteral("/"), false);
            },
            true);
    });

    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}
