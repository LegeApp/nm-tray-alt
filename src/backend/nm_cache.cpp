#include "nm_cache.h"

#include <QDateTime>
#include <algorithm>
#include <ranges>

namespace
{
constexpr qint64 kStaleSeconds = 30LL * 24LL * 60LL * 60LL;

bool hasConcreteSsid(const QString &ssid)
{
    // An empty SSID is the in-band sentinel for a hidden network (see decodeSsid()).
    return !ssid.isEmpty();
}

// Quantize signal strength into coarse buckets so that the small second-to-second
// jitter every AP shows during scanning never reorders the Wi-Fi list under the
// user's cursor. A row only changes position on a genuinely meaningful change.
int strengthBucket(int strength)
{
    return strength / 20; // 0..5
}

QString wifiGroupingKey(const nm::AccessPointRecord &ap)
{
    if (!ap.ssidBytes.isEmpty()) {
        return ap.devicePath + QLatin1Char('\x1f') + QString::fromLatin1(ap.ssidBytes.toBase64());
    }
    if (!ap.bssid.isEmpty()) {
        return ap.devicePath + QLatin1Char('\x1f') + ap.bssid.toLower();
    }
    return ap.devicePath + QLatin1Char('\x1f') + ap.path;
}

} // namespace

namespace nm
{

void NmCache::setSnapshot(const Snapshot &snapshot)
{
    mSnapshot = snapshot;
}

const Snapshot &NmCache::snapshot() const
{
    return mSnapshot;
}

QList<ActiveConnectionRecord> NmCache::activeConnections() const
{
    QList<ActiveConnectionRecord> out;
    out.reserve(mSnapshot.activeConnections.size());
    for (const auto &active : mSnapshot.activeConnections) {
        ActiveConnectionRecord item = active;
        if (!isUserFacingConnectionType(item.type) && !item.connectionPath.isEmpty()) {
            const auto savedIt = mSnapshot.savedConnections.find(item.connectionPath);
            if (savedIt != mSnapshot.savedConnections.end() && isUserFacingConnectionType(savedIt->type)) {
                if (item.type.isEmpty()) {
                    item.type = savedIt->type;
                }
                if (item.id.isEmpty()) {
                    item.id = savedIt->id;
                }
            }
        }
        if (!isUserFacingConnectionType(item.type)) {
            continue;
        }
        out.push_back(item);
    }
    return out;
}

QList<WifiViewRecord> NmCache::wifiEntries(bool hideStale) const
{
    QMap<QString, WifiViewRecord> merged;
    for (const auto &ap : mSnapshot.accessPoints) {
        WifiViewRecord item;
        item.apPath = ap.path;
        item.ssid = ap.ssid;
        if (!hasConcreteSsid(item.ssid)) {
            continue;
        }
        item.ssidBytes = ap.ssidBytes;
        item.devicePath = ap.devicePath;
        item.strength = ap.strength;
        item.secure = ap.privacy || ap.wpaFlags != 0 || ap.rsnFlags != 0;

        const SavedConnectionRecord *bestConn = nullptr;
        for (const auto &conn : mSnapshot.savedConnections) {
            if (conn.type != QStringLiteral("802-11-wireless")) {
                continue;
            }
            const bool connHasSsidBytes = !conn.wifiSsidBytes.isEmpty();
            if (connHasSsidBytes) {
                if (conn.wifiSsidBytes != ap.ssidBytes) {
                    continue;
                }
            } else if (hasConcreteSsid(conn.wifiSsid)) {
                if (conn.wifiSsid != item.ssid) {
                    continue;
                }
            } else if (conn.id != item.ssid) {
                continue;
            }
            if (bestConn == nullptr
                || conn.autoconnectPriority > bestConn->autoconnectPriority
                || (conn.autoconnectPriority == bestConn->autoconnectPriority && conn.timestamp > bestConn->timestamp)) {
                bestConn = &conn;
            }
        }
        if (bestConn != nullptr) {
            item.savedConnectionPath = bestConn->path;
            item.autoconnect = bestConn->autoconnect;
            item.autoconnectPriority = bestConn->autoconnectPriority;
            item.lastUsedTimestamp = bestConn->timestamp;
            item.stale = isConnectionStale(*bestConn);
        }

        const auto deviceIt = mSnapshot.devices.find(ap.devicePath);
        if (deviceIt != mSnapshot.devices.end()) {
            item.active = deviceIt->activeAccessPointPath == ap.path;
        }

        const QString key = wifiGroupingKey(ap);
        auto it = merged.find(key);
        if (it == merged.end()) {
            merged.insert(key, item);
            continue;
        }

        // Keep one deterministic entry per (device, SSID): prefer active AP, then strongest signal.
        const bool replace = (item.active && !it->active) || (item.active == it->active && item.strength > it->strength);
        if (replace) {
            item.savedConnectionPath = it->savedConnectionPath.isEmpty() ? item.savedConnectionPath : it->savedConnectionPath;
            item.autoconnect = it->savedConnectionPath.isEmpty() ? item.autoconnect : it->autoconnect;
            item.autoconnectPriority = std::max(item.autoconnectPriority, it->autoconnectPriority);
            item.lastUsedTimestamp = std::max(item.lastUsedTimestamp, it->lastUsedTimestamp);
            item.stale = item.savedConnectionPath.isEmpty() ? item.stale : it->stale;
            item.secure = item.secure || it->secure;
            *it = item;
        } else if (it->savedConnectionPath.isEmpty() && !item.savedConnectionPath.isEmpty()) {
            it->savedConnectionPath = item.savedConnectionPath;
            it->autoconnect = item.autoconnect;
            it->autoconnectPriority = item.autoconnectPriority;
            it->lastUsedTimestamp = item.lastUsedTimestamp;
            it->stale = item.stale;
            it->secure = it->secure || item.secure;
        } else if (!item.savedConnectionPath.isEmpty() && item.savedConnectionPath == it->savedConnectionPath) {
            it->autoconnect = it->autoconnect || item.autoconnect;
            it->autoconnectPriority = std::max(it->autoconnectPriority, item.autoconnectPriority);
            it->lastUsedTimestamp = std::max(it->lastUsedTimestamp, item.lastUsedTimestamp);
            it->secure = it->secure || item.secure;
        } else {
            it->secure = it->secure || item.secure;
        }
    }

    QList<WifiViewRecord> out;
    out.reserve(merged.size());
    std::ranges::copy_if(merged, std::back_inserter(out), [hideStale](const WifiViewRecord &item) {
        return !(hideStale && item.stale && !item.active);
    });

    std::ranges::sort(out, [](const WifiViewRecord &a, const WifiViewRecord &b) {
        if (a.active != b.active) {
            return a.active;
        }
        // Saved networks are pinned above unknown ones so the list the user cares
        // about stays put; only then do we sort by (bucketed) signal strength.
        if ((a.savedConnectionPath.isEmpty()) != (b.savedConnectionPath.isEmpty())) {
            return !a.savedConnectionPath.isEmpty();
        }
        if (strengthBucket(a.strength) != strengthBucket(b.strength)) {
            return strengthBucket(a.strength) > strengthBucket(b.strength);
        }
        if (a.autoconnectPriority != b.autoconnectPriority) {
            return a.autoconnectPriority > b.autoconnectPriority;
        }
        // Stable tail: name, then AP path — never flips between refreshes.
        const int byName = QString::compare(a.ssid, b.ssid, Qt::CaseInsensitive);
        if (byName != 0) {
            return byName < 0;
        }
        return a.apPath < b.apPath;
    });
    return out;
}

QList<ConnectionViewRecord> NmCache::knownConnections(bool hideStale) const
{
    QList<ConnectionViewRecord> out;
    for (const auto &conn : mSnapshot.savedConnections) {
        if (!isUserFacingConnectionType(conn.type)) {
            continue;
        }
        if (nm::isWirelessType(conn.type)) {
            bool visibleWifi = false;
            for (const auto &ap : mSnapshot.accessPoints) {
                if ((!conn.wifiSsidBytes.isEmpty() && conn.wifiSsidBytes == ap.ssidBytes)
                    || (hasConcreteSsid(conn.wifiSsid) && conn.wifiSsid == ap.ssid)
                    || (!conn.id.isEmpty() && conn.id == ap.ssid)) {
                    visibleWifi = true;
                    break;
                }
            }
            if (visibleWifi) {
                continue;
            }
        }
        ConnectionViewRecord item;
        item.connectionPath = conn.path;
        item.id = conn.id;
        item.uuid = conn.uuid;
        item.type = conn.type;
        item.stale = isConnectionStale(conn);
        item.lastUsedTimestamp = conn.timestamp;
        item.autoconnect = conn.autoconnect;

        for (const auto &active : mSnapshot.activeConnections) {
            if (active.connectionPath == conn.path || (!conn.uuid.isEmpty() && active.uuid == conn.uuid)) {
                item.active = true;
                break;
            }
        }

        if (hideStale && item.stale && !item.active) {
            continue;
        }
        out.push_back(item);
    }

    std::ranges::sort(out, [](const ConnectionViewRecord &a, const ConnectionViewRecord &b) {
        if (a.active != b.active) {
            return a.active;
        }
        if (a.autoconnect != b.autoconnect) {
            return a.autoconnect;
        }
        if (a.stale != b.stale) {
            return !a.stale;
        }
        return QString::compare(a.id, b.id, Qt::CaseInsensitive) < 0;
    });
    return out;
}

QList<DeviceRecord> NmCache::devices() const
{
    QList<DeviceRecord> out;
    out.reserve(mSnapshot.devices.size());
    for (const auto &it : mSnapshot.devices) {
        out.push_back(it);
    }
    std::ranges::sort(out, [](const DeviceRecord &a, const DeviceRecord &b) {
        return QString::compare(a.interfaceName, b.interfaceName, Qt::CaseInsensitive) < 0;
    });
    return out;
}

QString NmCache::activeConnectionForSettingsPath(const QString &connectionPath) const
{
    for (const auto &active : mSnapshot.activeConnections) {
        if (active.connectionPath == connectionPath) {
            return active.path;
        }
    }
    return {};
}

QString NmCache::connectionPathForUuid(const QString &uuid) const
{
    for (const auto &conn : mSnapshot.savedConnections) {
        if (conn.uuid == uuid) {
            return conn.path;
        }
    }
    return {};
}

QString NmCache::connectionPathForSsid(const QString &ssid) const
{
    const SavedConnectionRecord *best = nullptr;
    for (const auto &conn : mSnapshot.savedConnections) {
        // Only ever match Wi-Fi profiles here — an ethernet/VPN profile that happens
        // to share the name must never be activated on a Wi-Fi device (bug 4.7).
        if (!nm::isWirelessType(conn.type)) {
            continue;
        }
        if ((hasConcreteSsid(conn.wifiSsid) && conn.wifiSsid == ssid) || conn.id == ssid) {
            if (best == nullptr
                || conn.autoconnectPriority > best->autoconnectPriority
                || (conn.autoconnectPriority == best->autoconnectPriority && conn.timestamp > best->timestamp)) {
                best = &conn;
            }
        }
    }
    return best == nullptr ? QString{} : best->path;
}

QString NmCache::findWifiDeviceForAp(const QString &apPath) const
{
    const auto it = mSnapshot.accessPoints.find(apPath);
    if (it == mSnapshot.accessPoints.end()) {
        return {};
    }
    return it->devicePath;
}

bool NmCache::isConnectionStale(const SavedConnectionRecord &conn) const
{
    if (conn.timestamp <= 0) {
        return true;
    }
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    return conn.timestamp <= (now - kStaleSeconds);
}

} // namespace nm
