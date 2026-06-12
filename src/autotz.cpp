#include "autotz.h"
#include "nmmodel.h"
#include "log.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QFileInfo>
#include <QDBusInterface>
#include <QDBusConnection>
#include <QDBusReply>
#include <QUrl>
#include <QTimer>

namespace
{

struct Service {
    const char *url;
    std::initializer_list<const char *> keys;
};

// ipinfo.io is tried first: ISPs that backhaul island regions (e.g. Bali) through
// IP space registered to a mainland city cause MaxMind-derived databases
// (chrisdown/ipapi/ipwho.is) to all agree on the wrong mainland timezone.
// ipinfo.io's database has so far been the one to get those cases right.
static constexpr Service kServices[] = {
    {"https://ipinfo.io/json", {"timezone"}},
    {"https://geoip.chrisdown.name/", {"location", "time_zone"}},
    {"https://ipapi.co/json/", {"timezone"}},
    {"https://worldtimeapi.org/api/ip", {"timezone"}},
    {"https://ipwho.is/", {"timezone", "id"}},
};

QString extractTimezone(const QByteArray &data, const QStringList &keys)
{
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return {};
    }

    QJsonObject obj = doc.object();
    for (const QString &key : keys) {
        if (obj.contains(key)) {
            if (obj[key].isString()) {
                return obj[key].toString();
            }
            if (obj[key].isObject()) {
                obj = obj[key].toObject();
                continue;
            }
            break;
        }
        if (obj[key].isObject()) {
            obj = obj[key].toObject();
            continue;
        }
        break;
    }
    return {};
}

bool isValidTimezoneName(const QString &tz)
{
    return !tz.isEmpty()
        && !tz.startsWith(QLatin1Char('/'))
        && !tz.contains(QStringLiteral(".."))
        && QFileInfo::exists(QStringLiteral("/usr/share/zoneinfo/") + tz);
}

} // namespace

QStringList AutoTz::jsonKeysForUrl(const QString &url)
{
    for (const auto &svc : kServices) {
        if (url == QLatin1String(svc.url)) {
            QStringList keys;
            for (const char *k : svc.keys) {
                keys.append(QLatin1String(k));
            }
            return keys;
        }
    }
    return {};
}

AutoTz::AutoTz(NmModel *model, QObject *parent)
    : QObject(parent)
    , mModel(model)
{
    connect(mModel, &NmModel::managerStateChanged, this, &AutoTz::onManagerStateChanged);
    connect(&mCurl, &QProcess::finished, this, &AutoTz::onCurlFinished);
    connect(&mCurl, &QProcess::errorOccurred, this, &AutoTz::onCurlError);
    QTimer::singleShot(0, this, &AutoTz::maybeStartLookup);
}

void AutoTz::onManagerStateChanged()
{
    maybeStartLookup();
}

void AutoTz::maybeStartLookup()
{
    const auto state = mModel->managerState();
    const bool isConnected = state.overallState == NmModel::OverallState::Connected
        || state.overallState == NmModel::OverallState::Limited;
    if (!isConnected || state.primaryConnectionPath.isEmpty() || state.primaryConnectionPath == QStringLiteral("/")) {
        return;
    }

    const QString iface = mModel->primaryPhysicalInterfaceName();
    const QString physicalPath = mModel->primaryPhysicalConnectionPath();
    const QString connectionKey = !physicalPath.isEmpty() ? physicalPath : state.primaryConnectionPath;
    if (connectionKey.isEmpty()
            || mLookupInProgress
            || mAttemptedConnections.contains(connectionKey)) {
        return;
    }

    startGeoLookup(connectionKey, iface, !state.vpnActive.isEmpty());
}

void AutoTz::startGeoLookup(const QString &connectionKey, const QString &interfaceName, bool vpnActive)
{
    mLookupInProgress = true;
    mLookupInterface = interfaceName;
    mLookupHasVpn = vpnActive;
    mNextService = 0;
    mAttemptedConnections.insert(connectionKey);

    startNextService();
}

void AutoTz::startNextService()
{
    if (mNextService >= static_cast<int>(std::size(kServices))) {
        if (!mLookupInterface.isEmpty() && !mLookupHasVpn) {
            qCInfo(NM_TRAY) << "AutoTz: interface-bound lookups failed; retrying through default route";
            mLookupInterface.clear();
            mNextService = 0;
            startNextService();
            return;
        }
        qCWarning(NM_TRAY) << "AutoTz: all geo-IP timezone lookups failed";
        finishLookup();
        return;
    }

    const Service &svc = kServices[mNextService++];
    const QString url = QLatin1String(svc.url);

    if (!mLookupInterface.isEmpty()) {
        mUsingCurl = true;
        mCurl.setProgram(QStringLiteral("curl"));
        mCurl.setArguments({
            QStringLiteral("--silent"),
            QStringLiteral("--show-error"),
            QStringLiteral("--fail"),
            QStringLiteral("--location"),
            QStringLiteral("--max-time"), QStringLiteral("8"),
            QStringLiteral("--user-agent"), QStringLiteral("nm-tray/1.0"),
            QStringLiteral("--interface"), mLookupInterface,
            url,
        });
        qCInfo(NM_TRAY) << "AutoTz: querying timezone through interface" << mLookupInterface << url;
        mCurl.start();
        return;
    }

    if (mLookupHasVpn) {
        qCWarning(NM_TRAY) << "AutoTz: VPN is active and no physical interface was found; skipping unbound geo-IP lookup";
        finishLookup();
        return;
    }

    mUsingCurl = false;
    QNetworkRequest req{QUrl(url)};
    req.setRawHeader("User-Agent", "nm-tray/1.0");
    req.setTransferTimeout(8000);

    QNetworkReply *reply = mNet.get(req);
    reply->setProperty("_tz_url", url);
    connect(reply, &QNetworkReply::finished, this, &AutoTz::onReplyFinished);
}

void AutoTz::onReplyFinished()
{
    auto *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply) {
        return;
    }

    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        qCDebug(NM_TRAY) << "AutoTz: geo-IP lookup failed:" << reply->errorString();
        startNextService();
        return;
    }

    handleLookupData(reply->readAll(), reply->property("_tz_url").toString());
}

void AutoTz::onCurlFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (!mUsingCurl) {
        return;
    }

    const QString url = mCurl.arguments().isEmpty() ? QString{} : mCurl.arguments().last();
    const QByteArray data = mCurl.readAllStandardOutput();
    const QByteArray error = mCurl.readAllStandardError().trimmed();

    if (exitStatus != QProcess::NormalExit || exitCode != 0) {
        qCDebug(NM_TRAY) << "AutoTz: geo-IP lookup failed:" << error;
        startNextService();
        return;
    }

    handleLookupData(data, url);
}

void AutoTz::onCurlError(QProcess::ProcessError error)
{
    if (!mUsingCurl) {
        return;
    }

    qCWarning(NM_TRAY) << "AutoTz: curl lookup failed to start:" << error;
    if (mLookupHasVpn) {
        finishLookup();
        return;
    }

    mLookupInterface.clear();
    startNextService();
}

void AutoTz::handleLookupData(const QByteArray &data, const QString &url)
{
    const QStringList keys = jsonKeysForUrl(url);
    if (keys.isEmpty()) {
        startNextService();
        return;
    }

    const QString tz = extractTimezone(data, keys);
    if (!isValidTimezoneName(tz)) {
        qCDebug(NM_TRAY) << "AutoTz: geo-IP lookup returned invalid timezone" << tz;
        startNextService();
        return;
    }

    qCInfo(NM_TRAY) << "AutoTz: detected timezone" << tz;
    setTimezone(tz);
    finishLookup();
}

void AutoTz::finishLookup()
{
    mLookupInProgress = false;
    mUsingCurl = false;
    mLookupInterface.clear();
    mLookupHasVpn = false;
}

void AutoTz::setTimezone(const QString &timezone)
{
    if (!isValidTimezoneName(timezone)) {
        return;
    }

    QDBusInterface timedated(QStringLiteral("org.freedesktop.timedate1"),
                             QStringLiteral("/org/freedesktop/timedate1"),
                             QStringLiteral("org.freedesktop.timedate1"),
                             QDBusConnection::systemBus());

    if (timedated.property("Timezone").toString() == timezone) {
        qCInfo(NM_TRAY) << "AutoTz: timezone already set to" << timezone;
        return;
    }

    QDBusReply<void> reply_dbus = timedated.call(QStringLiteral("SetTimezone"), timezone, true);
    if (!reply_dbus.isValid()) {
        qCWarning(NM_TRAY) << "AutoTz: failed to set timezone via timedate1:" << reply_dbus.error().message();
    } else {
        qCInfo(NM_TRAY) << "AutoTz: timezone set to" << timezone;
    }
}
