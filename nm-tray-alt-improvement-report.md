# nm-tray-alt — Technical Improvement Report

**Scope:** the `nm-tray-2` source tree (nm-tray-alt fork, v0.5.1, Qt 6 / C++23, direct QtDBus backend, ~5,300 LOC).
**Covers:** (1) connect/disconnect reliability, (2) internet-access status à la Windows, (3) Wi-Fi password persistence, (4) general bugfixes, (5) redundancy removal, (6) a connection-troubleshooting screen with working code.

All file/line references are to the uploaded tree. Claims about NetworkManager behavior below were verified against the NM D-Bus API docs and the current NM/nmcli source where it mattered (secret handling on `Update`, nmcli failure behavior, connectivity-check semantics).

---

## 0. Architecture recap (so the diagnoses below make sense)

The fork's data flow is:

```
NetworkManager (system D-Bus)
        │  signals: StateChanged, PropertiesChanged, AccessPointAdded/Removed, ...
        ▼
NmDbusClient (worker QThread)          src/backend/nm_dbus_client.cpp
        │  on ANY relevant signal → debounce 120 ms → refreshSnapshot():
        │  re-polls the ENTIRE world (manager, every device, every AP,
        │  every saved profile via GetSettings, every active connection + IP configs)
        ▼  emits snapshotChanged(Snapshot)  (queued, cross-thread copy)
NmModel (GUI thread)                   src/nmmodel.cpp
        │  rebuildFromSnapshot() → applySection() per section
        │  (active / connections / devices / wifi)
        ▼
NmProxy → QSortFilterProxyModel → MenuView (QListView inside QWidgetAction)
        │
WindowMenu (left-click popup), Tray context menu, ConnectionInfo, NmList
```

Actions (`src/backend/nm_actions.cpp`) bypass this pipeline entirely: they are **synchronous, blocking D-Bus calls (and one blocking `nmcli` subprocess) executed on the GUI thread**.

Two structural properties of this pipeline explain most of the symptoms you listed:

1. `applySection()` (nmmodel.cpp:945–1030) compares the **ordered sequence** of row keys. If the sequence differs at all — including a pure reorder — it clears the whole section and re-inserts everything (`beginRemoveRows(0, n-1)` … `beginInsertRows(0, n-1)`, nmmodel.cpp:989–999). The Wi-Fi section is sorted primarily by live signal strength (nm_cache.cpp, `wifiEntries`), and signal strength jitters every few seconds for every AP in range, so the section is effectively reset continuously.
2. Everything the user *does* blocks the GUI thread; everything the user *sees* is rebuilt wholesale.

Keep those two in mind; they come up in sections 1, 4 and 5.

---

## 1. Connect / disconnect reliability

> "currently for some reason its difficult to disconnect and connect to another network"

This is not one bug. Six independent defects compound into that experience. In rough order of impact:

### 1.1 Root cause A — the menu shuffles under the cursor

Opening the popup calls `requestAllWifiScan()` (windowmenu.cpp:~150), which asks NM to scan. Scanning is exactly when AP strengths and the AP list churn the most. Each strength change → `PropertiesChanged` on the AP path → full snapshot re-poll → new sort order (strength is the second sort key, nm_cache.cpp:~215) → key sequence differs → **the whole Wi-Fi section is removed and re-inserted while the menu is open**. Rows visibly jump; the row you are aiming at is replaced mid-click, so clicks land on the wrong SSID or on nothing. `forceSizeRefresh()` (windowmenu.cpp:~120) then re-calls `popup()` on the still-open menu, which can move the menu itself.

Additionally, every remove/insert cycle destroys the `QPersistentModelIndex`es the tray relies on (`mPrimaryConnection`, `mShownConnection` in tray.cpp), causing the tray icon to briefly flap through `NETWORK_OFFLINE`, and fires the notification storm described in §4.1.

**Fix (three parts, all cheap):**

**(a) Sort by a stable key.** Quantize strength into buckets so that ±10 % jitter never reorders:

```cpp
// nm_cache.cpp — replace the strength comparison in wifiEntries()'s sort
static int strengthBucket(int s) { return s / 20; }   // 0..5: reorder only on meaningful change

std::ranges::sort(out, [](const WifiViewRecord &a, const WifiViewRecord &b) {
    if (a.active != b.active)
        return a.active;
    const bool aSaved = !a.savedConnectionPath.isEmpty();
    const bool bSaved = !b.savedConnectionPath.isEmpty();
    if (aSaved != bSaved)                    // saved networks pinned above unknown ones
        return aSaved;
    if (strengthBucket(a.strength) != strengthBucket(b.strength))
        return strengthBucket(a.strength) > strengthBucket(b.strength);
    // stable tail: name, then path — never flips between refreshes
    const int byName = QString::compare(a.ssid, b.ssid, Qt::CaseInsensitive);
    if (byName != 0)
        return byName < 0;
    return a.apPath < b.apPath;
});
```

**(b) Replace the clear-and-reinsert in `applySection()` with a keyed diff** so that the *unchanged* rows keep their identity (and their `QPersistentModelIndex`es, and their position under the mouse). Lists here are small (<100 rows), so a simple three-pass diff is plenty:

```cpp
// nmmodel.cpp — drop-in replacement for the body of applySection()
// after computing currentKeys/nextKeys, parentIdx and leafId as today:

// Pass 1: remove rows whose key vanished (bottom-up keeps indices valid)
{
    const QSet<QString> nextSet(nextKeys.cbegin(), nextKeys.cend());
    for (int i = current.size() - 1; i >= 0; --i) {
        if (!nextSet.contains(keyOf(current.at(i)))) {
            beginRemoveRows(parentIdx, i, i);
            current.removeAt(i);
            endRemoveRows();
        }
    }
}

// Pass 2: walk target order; move or insert so current converges on next
for (int i = 0; i < next.size(); ++i) {
    const QString wantKey = keyOf(next.at(i));
    if (i < current.size() && keyOf(current.at(i)) == wantKey)
        continue;
    int j = -1;
    for (int k = i + 1; k < current.size(); ++k) {
        if (keyOf(current.at(k)) == wantKey) { j = k; break; }
    }
    if (j >= 0) {
        beginMoveRows(parentIdx, j, j, parentIdx, i);
        current.move(j, i);
        endMoveRows();
    } else {
        beginInsertRows(parentIdx, i, i);
        current.insert(i, next.at(i));
        endInsertRows();
    }
}

// Pass 3: payload-only changes → dataChanged (no structural churn)
for (int i = 0; i < current.size(); ++i) {
    if (!isSameItem(current.at(i), next.at(i))) {
        current[i] = next.at(i);
        emit dataChanged(createIndex(i, 0, leafId), createIndex(i, 0, leafId));
    }
}
```

Note `NmProxy` already forwards `rowsAboutToBeMoved`/`rowsMoved` (nmproxy.cpp:~260), so moves propagate to the views for free.

**(c) Freeze ordering while a popup is open.** Even with (a)+(b), a bucket boundary crossing would still move a row mid-aim. Give the model a hold latch that the menu toggles; while held, the model keeps the *current* order and only applies payload updates and removals of vanished rows:

```cpp
// nmmodel.h
public Q_SLOTS:
    void setOrderHold(bool held);          // true while a popup is visible
private:
    bool mOrderHeld = false;

// nmmodel.cpp — at the top of rebuildFromSnapshot(), after computing nextWifi:
if (mOrderHeld) {
    // keep existing order: rows we already show stay where they are,
    // genuinely new rows append at the end
    auto reorderLikeCurrent = [](const auto &current, auto &next, auto keyOf) {
        auto rest = std::move(next);
        next.clear();
        for (const auto &cur : current) {
            auto it = std::ranges::find_if(rest,
                [&](const auto &n) { return keyOf(n) == keyOf(cur); });
            if (it != rest.end()) { next.push_back(*it); rest.erase(it); }
        }
        next.append(rest);                       // newcomers go last while held
    };
    reorderLikeCurrent(mWifi, nextWifi, [](const nm::WifiViewRecord &w){ return w.apPath; });
}

void NmModel::setOrderHold(bool held)
{
    if (mOrderHeld == held) return;
    mOrderHeld = held;
    if (!held)
        rebuildFromSnapshot(mCache.snapshot());   // settle to the true order once closed
}
```

```cpp
// windowmenu.cpp — in the constructor:
d->mNmModel->setOrderHold(true);
connect(this, &QMenu::aboutToHide, this, [nmModel] { nmModel->setOrderHold(false); });
// (also drop the popup() call inside forceSizeRefresh(); adjusting size is fine,
//  re-popping the menu is what makes it jump)
```

### 1.2 Root cause B — every action blocks the GUI thread

`NmActions::*` use `QDBusConnection::systemBus().call()` (nm_actions.cpp:39, 50, 135 …), a blocking call with a default **25-second** timeout. `addWifiConnection()` is worse: `waitForStarted(5000)` + `waitForFinished(30000)` (nm_actions.cpp:187–190) — clicking a new secured network can freeze the entire tray, menu and icon **for up to 35 s** while `nmcli --wait 20` runs. During the freeze nothing repaints and clicks queue up; the user quite reasonably concludes "connecting is broken".

**Fix:** convert actions to `QDBusPendingCallWatcher` and deliver results via callback. One helper covers all of them:

```cpp
// nm_actions_async.h
#pragma once
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <functional>

namespace nm
{
using AsyncResult = std::function<void(bool ok, const QString &error, const QDBusMessage &reply)>;

inline void callAsync(QDBusMessage &&msg, QObject *ctx, AsyncResult done)
{
    auto pending = QDBusConnection::systemBus().asyncCall(std::move(msg),
                                                          /*timeout ms*/ 15000);
    auto *watcher = new QDBusPendingCallWatcher(pending, ctx);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, ctx,
        [done = std::move(done)](QDBusPendingCallWatcher *w) {
            w->deleteLater();
            const QDBusMessage reply = w->reply();
            const bool ok = !w->isError();
            done(ok, ok ? QString{} : w->error().message(), reply);
        });
}
} // namespace nm
```

Example use, replacing the synchronous activate path in `NmModel::activateConnection()`:

```cpp
QDBusMessage msg = QDBusMessage::createMethodCall(
    kNmService, kNmPath, kNmIface, QStringLiteral("ActivateConnection"));
msg << QDBusObjectPath(conn.connectionPath)
    << QDBusObjectPath(devicePath)
    << QDBusObjectPath(specificObject);

nm::callAsync(std::move(msg), this,
    [this, id = conn.id](bool ok, const QString &err, const QDBusMessage &) {
        if (!ok) {
            qCWarning(NM_TRAY).noquote() << "activate failed for" << id << ":" << err;
            emit actionFailed(tr("Could not connect to %1").arg(id), err);   // see 1.4
        }
    });
```

The `nmcli` subprocess should be removed entirely — see §3.2, which replaces it with `AddAndActivateConnection` (async, no argv password leak, no 30 s stall).

### 1.3 Root cause C — activating a saved Wi-Fi profile picks unusable devices

Both activation paths choose the first Wi-Fi device with `dev.state >= 20` (nmmodel.cpp:735 and :886). In the NM device state enum, **20 = UNAVAILABLE** (e.g. rfkill'd, or Wi-Fi soft-disabled); the first *usable* state is 30 = DISCONNECTED. Activating on an unavailable device fails immediately with `org.freedesktop.NetworkManager.Device.NotAllowed`-class errors — which today are only visible in the journal (root cause D). Also, `specificObject` is passed as `"/"` for the "Known connections" path, so NM can't be steered to the AP the user can actually see.

**Fix:**

```cpp
// shared helper — pick a device that can actually connect, prefer one that sees the SSID
struct WifiTarget { QString devicePath; QString apPath; };

static WifiTarget pickWifiTarget(const nm::Snapshot &snap,
                                 const QString &ifaceHint,
                                 const QByteArray &ssidBytes)
{
    WifiTarget best;
    for (const auto &dev : snap.devices) {
        if (dev.type != nm::DeviceType::Wifi)
            continue;
        if (!ifaceHint.isEmpty() && dev.interfaceName != ifaceHint)
            continue;
        if (dev.state < 30)                          // 30 = DISCONNECTED; 20 = UNAVAILABLE
            continue;
        if (best.devicePath.isEmpty())
            best.devicePath = dev.path;
        for (const QString &apPath : dev.accessPointPaths) {
            const auto ap = snap.accessPoints.constFind(apPath);
            if (ap != snap.accessPoints.cend() && ap->ssidBytes == ssidBytes) {
                return { dev.path, apPath };          // device that sees the network wins
            }
        }
    }
    return best;                                      // may be empty → report, don't fire blind
}
```

Pass `target.apPath` as the `specific_object` so NM associates with the intended AP.

### 1.4 Root cause D — failures are invisible

Every action failure ends in `qCWarning(NM_TRAY)` and nothing else. From the user's chair: click → nothing happens. Since the code already carries an `org.freedesktop.Notifications` proxy (tray.cpp), route errors through it:

```cpp
// nmmodel.h
Q_SIGNALS:
    void actionFailed(const QString &summary, const QString &detail);

// tray.cpp — in Tray's constructor
connect(&d->mNmModel, &NmModel::actionFailed, this,
    [this](const QString &summary, const QString &detail) {
        d->mNotification.Notify(QStringLiteral("nm-tray-alt"), 0,
            QStringLiteral("dialog-error"), summary,
            detail, {}, {}, 8000);
    });
```

Also translate the two most common raw NM errors into human wording before emitting: `...NoSecrets` / "no secrets" → *"A password is needed for this network"* (and trigger the re-prompt of §3.3); `...Device.NotAllowed` / unavailable → *"Wi-Fi adapter is unavailable (check the Wi-Fi/airplane switch)"*.

### 1.5 Root cause E — disconnect has a hidden side effect and a stale header

`disconnectActiveConnection()` prefers `Device.Disconnect` for device-backed links (nmmodel.cpp:~1120). Per the NM API, that call *"disconnects a device and **prevents the device from automatically activating further connections** without user intervention"*. That is the right call for "stay offline until I say otherwise" — but it means that after disconnecting, NM will not autoconnect to *anything* on that radio until the user manually activates a network. Combined with root causes A–D, the visible behavior is "I disconnected and now it won't connect to the other network either".

Two changes:

1. Make the semantics explicit in the UI: label the status-line click *"Disconnect (stays off until you pick a network)"*, or offer both actions — `DeactivateConnection` (drop this connection, allow autoconnect to the next one) and `Device.Disconnect` (hold offline). For your stated goal — *disconnect and connect to another network* — `DeactivateConnection` on the active connection is the better default; keep `Device.Disconnect` behind "Disconnect and stay off".
2. The status button text (`"Connected: X (76 %)"`) is captured **once** in the `WindowMenu` constructor and never updated, so after clicking disconnect the open menu still claims you're connected. Rebind it:

```cpp
// windowmenu.cpp — after creating statusButton
auto refreshStatus = [this, d, statusButton] {
    const auto s = d->mNmModel->managerState();
    if (s.primaryName.isEmpty()) {
        statusButton->setText(tr("No active connection"));
        statusButton->setEnabled(false);
    } else {
        QString text = tr("Connected: %1").arg(s.primaryName);
        if (s.wifiStrength >= 0) text += tr(" (%1%)").arg(s.wifiStrength);
        statusButton->setText(text);
        statusButton->setEnabled(true);
    }
};
refreshStatus();
connect(d->mNmModel, &NmModel::managerStateChanged, statusButton, refreshStatus);
```

### 1.6 Root cause F — wrong/expired passwords dead-end the connect flow

Covered fully in §3: once *any* profile exists for an SSID (even one holding a wrong password, which nmcli leaves behind on failure — verified against nmcli source, `add_and_activate_check_state()` prints the error and exits without deleting), `activateConnection()` short-circuits into `ActivateConnection` and never prompts again. There is no secret agent in the process, so NM's `need-auth` phase fails with *"no secrets: No agents were available for this request"*. The fix (async connect + failure-reason detection + re-prompt + psk update) is the core of section 3.

---

## 2. Internet-access status in Connection Information (Windows-style, foolproof)

### 2.1 What the code already has, and where it stops

The backend already reads `Manager.Connectivity` into every snapshot (nm_dbus_client.cpp:393) and subscribes to its changes (:298). `toOverallState()` (nmmodel.cpp:49) even maps it to an `OverallState::Limited`. But grep the tree: **the only consumer of that state is AutoTz**. Neither the tray icon, nor the tooltip, nor the popup, nor the Connection Information dialog ever tells the user "connected, but no internet" — which is precisely your request.

There is also a correctness gap in `toOverallState()`: it treats connectivity `1|2|3` as one lump ("Limited"), and treats `0` (UNKNOWN) as fully connected. The NM values are:

| value | meaning | Windows equivalent |
|---|---|---|
| 0 UNKNOWN | connectivity checking disabled or not yet run | — (must probe ourselves) |
| 1 NONE | not connected to any network | "Not connected" |
| 2 PORTAL | captive portal is hijacking traffic | "Action needed / Sign in" |
| 3 LIMITED | on a network, no route to the internet | "No internet access" |
| 4 FULL | internet reachable | "Internet access" |

PORTAL and LIMITED demand different UI (a portal wants a "Open sign-in page" button), and UNKNOWN must not be presented as "online".

### 2.2 Why NM's flag alone is not foolproof — and the three-source design

`Manager.Connectivity` is only trustworthy when connectivity checking is configured *and* enabled. Many distros ship it off; NM exposes `ConnectivityCheckAvailable` and `ConnectivityCheckEnabled` properties exactly so a UI can know whether the value means anything. There are also historical cases of the aggregate value being stale or wrong for edge states (e.g. NM issue #138, LIMITED reported while disconnected). "Foolproof" therefore means:

1. **NM's verdict** — `Connectivity` + `CheckConnectivity()` for an on-demand re-check, but only trusted when `ConnectivityCheckEnabled == true`.
2. **Per-device verdicts** — `Device.Ip4Connectivity` / `Ip6Connectivity` (NM ≥ 1.16), useful in the troubleshooter (§6) to distinguish "Wi-Fi has internet, VPN is the problem".
3. **An in-process probe** as authoritative fallback whenever NM reports UNKNOWN (or the user hits "Check now"): plain-HTTP GET of a 204-style endpoint with redirects *disabled*. A 204/expected body = FULL; an unexpected 2xx/3xx = PORTAL (that's how captive portals reveal themselves — this is also exactly how Windows NCSI works); timeout/refused = LIMITED-or-worse. HTTP (not HTTPS) on purpose: portals can't cleanly intercept TLS, you'd just get opaque handshake errors instead of a detectable redirect.

### 2.3 Code — `ConnectivityMonitor`

```cpp
// src/backend/connectivity_monitor.h
#pragma once
#include <QObject>
#include <QNetworkAccessManager>
#include <QUrl>

namespace nm
{
enum class InternetStatus { Unknown, Checking, NoNetwork, NoInternet, Portal, Full };

class ConnectivityMonitor : public QObject
{
    Q_OBJECT
public:
    explicit ConnectivityMonitor(QObject *parent = nullptr);

    InternetStatus status() const { return mStatus; }
    QString statusText() const;                    // user-facing wording
    QUrl portalProbeUrl() const { return mProbeUrl; }

public Q_SLOTS:
    // Feed from the snapshot: manager state (10..70), connectivity (0..4),
    // and whether NM's checker is enabled.
    void updateFromSnapshot(uint nmState, uint connectivity, bool nmCheckEnabled);
    void recheckNow();                             // user pressed "Check now"

Q_SIGNALS:
    void statusChanged(nm::InternetStatus status);

private:
    void setStatus(InternetStatus s);
    void startProbe();
    void askNmToRecheck();

    QNetworkAccessManager mNam;
    InternetStatus mStatus = InternetStatus::Unknown;
    QUrl mProbeUrl{QStringLiteral("http://connectivity-check.ubuntu.com/")};
    bool mProbeRunning = false;
};
} // namespace nm
```

```cpp
// src/backend/connectivity_monitor.cpp
#include "connectivity_monitor.h"
#include "../log.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace nm
{
ConnectivityMonitor::ConnectivityMonitor(QObject *parent) : QObject(parent) {}

QString ConnectivityMonitor::statusText() const
{
    switch (mStatus) {
    case InternetStatus::Full:       return tr("Internet access");
    case InternetStatus::Portal:     return tr("Sign-in required (captive portal)");
    case InternetStatus::NoInternet: return tr("Connected — no internet access");
    case InternetStatus::NoNetwork:  return tr("Not connected");
    case InternetStatus::Checking:   return tr("Checking internet access…");
    case InternetStatus::Unknown:    return tr("Internet access unknown");
    }
    return {};
}

void ConnectivityMonitor::updateFromSnapshot(uint nmState, uint connectivity, bool nmCheckEnabled)
{
    if (nmState < 50) {                                       // not even locally connected
        setStatus(nmState == 40 ? InternetStatus::Checking : InternetStatus::NoNetwork);
        return;
    }
    if (nmCheckEnabled) {
        switch (connectivity) {
        case 4: setStatus(InternetStatus::Full);       return;
        case 2: setStatus(InternetStatus::Portal);     return;
        case 3: setStatus(InternetStatus::NoInternet); return;
        case 1: setStatus(InternetStatus::NoNetwork);  return;
        default: break;                                        // 0 → fall through to probe
        }
    }
    // NM can't tell us (checker disabled or UNKNOWN) → find out ourselves.
    startProbe();
}

void ConnectivityMonitor::recheckNow()
{
    setStatus(InternetStatus::Checking);
    askNmToRecheck();      // harmless if checking is disabled
    startProbe();          // our own verdict regardless
}

void ConnectivityMonitor::askNmToRecheck()
{
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.NetworkManager"),
        QStringLiteral("/org/freedesktop/NetworkManager"),
        QStringLiteral("org.freedesktop.NetworkManager"),
        QStringLiteral("CheckConnectivity"));
    auto *w = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(msg), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [](QDBusPendingCallWatcher *w) {
        w->deleteLater();   // result arrives via the normal Connectivity PropertiesChanged
    });
}

void ConnectivityMonitor::startProbe()
{
    if (mProbeRunning)
        return;
    mProbeRunning = true;
    setStatus(InternetStatus::Checking);

    QNetworkRequest req(mProbeUrl);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::ManualRedirectPolicy);      // redirects == portal evidence
    req.setTransferTimeout(5000);
    req.setRawHeader("User-Agent", "nm-tray-alt-conncheck/1.0");

    QNetworkReply *reply = mNam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        mProbeRunning = false;

        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray nmHeader = reply->rawHeader("X-NetworkManager-Status");

        if (reply->error() == QNetworkReply::NoError
            && (http == 204 || nmHeader.trimmed() == "online")) {
            setStatus(InternetStatus::Full);
        } else if (http >= 300 && http < 400) {
            setStatus(InternetStatus::Portal);                   // hijacked → sign-in page
        } else if (http >= 200 && http < 300) {
            setStatus(InternetStatus::Portal);                   // wrong body from "our" URL
        } else {
            setStatus(InternetStatus::NoInternet);               // timeout / refused / DNS fail
        }
    });
}

void ConnectivityMonitor::setStatus(InternetStatus s)
{
    if (mStatus == s)
        return;
    mStatus = s;
    emit statusChanged(s);
}
} // namespace nm
```

Backend plumbing: add `connectivityCheckEnabled`/`connectivityCheckAvailable` to `ManagerState` (nm_types.h) and read them in `refreshSnapshot()` next to `Connectivity`:

```cpp
next.manager.connectivityCheckEnabled =
    nmProps.value(QStringLiteral("ConnectivityCheckEnabled")).toBool();
next.manager.connectivityCheckAvailable =
    nmProps.value(QStringLiteral("ConnectivityCheckAvailable")).toBool();
```

(Also read the daemon's `ConnectivityCheckUri` and, when non-empty, feed it into `mProbeUrl` so the tray probes the same endpoint the system is configured for.)

### 2.4 Surfacing it

**Connection Information dialog** — a status banner above the tabs (connectioninfo.ui gains a row-0 widget):

```cpp
// connectioninfo.cpp — constructor, after setupUi()
mConnectivity = new nm::ConnectivityMonitor(this);
auto *banner   = new QLabel(this);
auto *checkBtn = new QPushButton(tr("Check now"), this);
auto *portalBtn= new QPushButton(tr("Open sign-in page"), this);
portalBtn->hide();

auto *row = new QHBoxLayout;
row->addWidget(banner, 1);
row->addWidget(portalBtn);
row->addWidget(checkBtn);
static_cast<QGridLayout*>(layout())->addLayout(row, 0, 0);   // shift tabWidget to row 1

auto *bannerIcon = new QLabel(this);          // small themed status glyph
row->insertWidget(0, bannerIcon);

auto render = [this, banner, bannerIcon, portalBtn] {
    static const QHash<nm::InternetStatus, const char *> icon = {
        {nm::InternetStatus::Full,       "network-transmit-receive"},
        {nm::InternetStatus::Portal,     "dialog-warning"},
        {nm::InternetStatus::NoInternet, "network-error"},
        {nm::InternetStatus::NoNetwork,  "network-offline"},
        {nm::InternetStatus::Checking,   "view-refresh"},
        {nm::InternetStatus::Unknown,    "dialog-question"},
    };
    bannerIcon->setPixmap(QIcon::fromTheme(
        QLatin1String(icon.value(mConnectivity->status()))).pixmap(22, 22));
    banner->setTextFormat(Qt::PlainText);      // never rich text for dynamic strings
    banner->setText(mConnectivity->statusText());
    portalBtn->setVisible(mConnectivity->status() == nm::InternetStatus::Portal);
};
connect(mConnectivity, &nm::ConnectivityMonitor::statusChanged, this, render);
connect(checkBtn,  &QPushButton::clicked, mConnectivity, &nm::ConnectivityMonitor::recheckNow);
connect(portalBtn, &QPushButton::clicked, this, [this] {
    QDesktopServices::openUrl(mConnectivity->portalProbeUrl());  // portal will intercept it
});

// feed it whenever the model's manager state moves
auto feed = [this] {
    const auto s = mModel->managerState();
    mConnectivity->updateFromSnapshot(s.rawNmState, s.rawConnectivity, s.connectivityCheckEnabled);
};
connect(mModel, &NmModel::managerStateChanged, this, feed);
feed();
```

(That requires exposing the raw `state`/`connectivity` ints on `NmModel::ManagerState` alongside `overallState` — a 4-line change in `rebuildFromSnapshot()`.)

**Tray** — same monitor instance owned by `TrayPrivate`; append the verdict to the tooltip and, for the two bad states, overlay a warning emblem on the icon the same way `MultiIconDelegate` overlays the padlock:

```cpp
// tray.cpp — TrayPrivate::refreshIcon()
QIcon base = icons::getIcon(mIconCurrent, false);
if (mInetStatus == nm::InternetStatus::NoInternet || mInetStatus == nm::InternetStatus::Portal) {
    QPixmap px = base.pixmap(64, 64);
    QPainter p(&px);
    const QPixmap badge = QIcon::fromTheme(
        mInetStatus == nm::InternetStatus::Portal ? "dialog-warning" : "emblem-important")
            .pixmap(32, 32);
    p.drawPixmap(px.width() - badge.width(), px.height() - badge.height(), badge);
    p.end();
    mTrayIcon.setIcon(QIcon(px));
} else {
    mTrayIcon.setIcon(base);
}
```

**Popup** — one line under the status button: `Internet access` / `No internet access` / `Sign-in required`, bound to the same `statusChanged` signal. That completes the Windows parity: the state is visible before the user even opens a dialog.

---

## 3. Remembering Wi-Fi passwords

### 3.1 What actually happens today (verified)

The current connect-to-new-network path is `promptAndCreateWifiConnection()` → `NmActions::addWifiConnection()` → **spawn `nmcli device wifi connect <ssid> password <pw>`** (nm_actions.cpp:157–205). Consequences:

1. **The password is world-readable while nmcli runs.** It is passed on the command line, so any local process can read it from `/proc/<pid>/cmdline` (`ps aux` shows it). This is a real secret leak, independent of everything else.
2. **The password is `.trimmed()`** (nm_actions.cpp:162) and the dialog's Connect button also keys off `trimmed()` (wifi_password_dialog.cpp:~95). WPA passphrases may legitimately begin or end with spaces; those networks are simply unjoinable.
3. **The GUI freezes up to 35 s** (§1.2) while nmcli runs.
4. On success, nmcli creates a profile with the psk stored **system-owned** (`psk-flags=0`, written to `/etc/NetworkManager/system-connections/`), so in the happy path the password *is* remembered across reboots with no keyring.
5. On failure — wrong password, `--wait 20` expiring during a slow association, or nmcli simply not installed — the error goes to `qCWarning` only, **and the newly created profile is left behind with the wrong psk** (confirmed in nmcli's source: the failure path prints and exits; it never deletes the profile it created).
6. From then on, `NmModel::activateConnection()` sees `savedConnectionPath` non-empty and goes straight to `ActivateConnection` — **the password dialog can never appear again for that SSID.** Activation reaches `need-auth`, NM asks its registered secret agents, this process has none, and the attempt dies with *"no secrets: No agents were available for this request"* — silently (root cause 1.4). This is the mechanism behind "it doesn't remember / I can't get back in".
7. The same no-agent gap breaks profiles created by *other* front-ends: GNOME/KDE applets often store the psk **agent-owned** (`psk-flags=1`, in the user keyring). On a setup where nm-tray-alt is the only applet, those profiles can never yield secrets, so they fail identically.

One thing that is **not** broken (worth writing down so nobody "fixes" it): the autoconnect checkbox path (`setConnectionAutoconnect()`, GetSettings → modify → Update) does *not* wipe the stored password. I verified in the NM daemon source (`nm-settings-connection.c`, `update_auth_cb`): when a D-Bus `Update` carries **no secrets at all**, NM deliberately merges all existing secrets back into the new settings. The contract is fragile though — an Update that includes *any* secret replaces the rest — so keep the current "never touch secret keys in that path" behavior and add a comment saying why.

### 3.2 Fix part 1 — replace nmcli with `AddAndActivateConnection`

`AddAndActivateConnection(a{sa{sv}} connection, o device, o specific_object)` creates the profile and activates it in one async D-Bus call. Passing the AP as `specific_object` lets NM complete SSID/band details itself; we supply the security setting chosen from the AP's RSN/WPA flags so WPA2-PSK, WPA3-SAE, OWE and open networks all work:

```cpp
// nm_actions_async additions
namespace nm
{
// Map AP capability flags → key-mgmt. NM80211ApSecurityFlags bit meanings:
// 0x100 = KEY_MGMT_PSK, 0x200 = KEY_MGMT_802_1X, 0x400 = KEY_MGMT_SAE,
// 0x800 = KEY_MGMT_OWE, 0x1000 = KEY_MGMT_OWE_TM
inline QString keyMgmtForAp(uint32_t wpaFlags, uint32_t rsnFlags, bool privacy)
{
    const uint32_t all = wpaFlags | rsnFlags;
    if ((all & 0x400) && !(all & 0x100))
                      return QStringLiteral("sae");        // WPA3-only
    if (all & 0x100)  return QStringLiteral("wpa-psk");    // WPA2 / WPA2+WPA3 mixed
    if (all & 0x200)  return QStringLiteral("wpa-eap");    // enterprise → caller must bail
    if (all & (0x800 | 0x1000))
                      return QStringLiteral("owe");        // Enhanced Open (+transition)
    if (privacy)      return QStringLiteral("wpa-psk");    // best effort (legacy)
    return {};                                             // open network
}
// Note: for "wpa-eap" the tray cannot build a valid profile from just a password —
// detect it before prompting and point the user at nm-connection-editor instead.

inline void addAndActivateWifi(QObject *ctx,
                               const AccessPointRecord &ap,
                               const QString &devicePath,
                               const QString &password,     // NOT trimmed — verbatim
                               AsyncResult done)
{
    QVariantMap connection{
        { QStringLiteral("id"),          QString::fromUtf8(ap.ssidBytes) },
        { QStringLiteral("type"),        QStringLiteral("802-11-wireless") },
        { QStringLiteral("autoconnect"), true },            // remember AND auto-rejoin
    };
    QVariantMap wireless{
        { QStringLiteral("ssid"), ap.ssidBytes },
        { QStringLiteral("mode"), QStringLiteral("infrastructure") },
    };
    QMap<QString, QVariantMap> settings;
    settings.insert(QStringLiteral("connection"),      connection);
    settings.insert(QStringLiteral("802-11-wireless"), wireless);

    const QString keyMgmt = keyMgmtForAp(ap.wpaFlags, ap.rsnFlags, ap.privacy);
    if (!keyMgmt.isEmpty()) {
        QVariantMap sec{ { QStringLiteral("key-mgmt"), keyMgmt } };
        if (keyMgmt != QLatin1String("owe"))
            sec.insert(QStringLiteral("psk"), password);
        // psk-flags defaults to 0 (system-owned) → stored by NM on disk,
        // works with no keyring/agent, survives reboots. Exactly what we want.
        settings.insert(QStringLiteral("802-11-wireless-security"), sec);
    }

    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.NetworkManager"),
        QStringLiteral("/org/freedesktop/NetworkManager"),
        QStringLiteral("org.freedesktop.NetworkManager"),
        QStringLiteral("AddAndActivateConnection"));
    msg << QVariant::fromValue(settings)
        << QDBusObjectPath(devicePath)
        << QDBusObjectPath(ap.path);
    callAsync(std::move(msg), ctx, std::move(done));
}
} // namespace nm
```

Register the `QMap<QString, QVariantMap>` (`a{sa{sv}}`) metatype once at startup:

```cpp
// main.cpp
qDBusRegisterMetaType<QMap<QString, QVariantMap>>();   // a{sa{sv}} — connection settings
qDBusRegisterMetaType<QList<uint>>();                  // au — used by ipv4.dns in §6.2
```

Client-side validation belongs in the dialog, not in trimming: for `wpa-psk`, a passphrase must be 8–63 characters (or exactly 64 hex digits); show the rule inline instead of silently letting NM bounce it. And remove both `.trimmed()` calls.

### 3.3 Fix part 2 — recover from wrong/changed passwords instead of dead-ending

Watch the activation you started. `AddAndActivateConnection`/`ActivateConnection` return the active-connection path; subscribe to its `StateChanged(u state, u reason)` and react to the failure reasons:

```cpp
// wifi_connector.h — owns one connect attempt end to end
class WifiConnector : public QObject
{
    Q_OBJECT
public:
    // Result the UI acts on
    enum class Outcome { Success, WrongOrMissingPassword, Failed };
    Q_SIGNAL void finished(Outcome outcome, const QString &humanError);

    void watchActiveConnection(const QString &acPath)
    {
        QDBusConnection::systemBus().connect(
            QStringLiteral("org.freedesktop.NetworkManager"), acPath,
            QStringLiteral("org.freedesktop.NetworkManager.Connection.Active"),
            QStringLiteral("StateChanged"),
            this, SLOT(onAcStateChanged(uint,uint)));
    }

private Q_SLOTS:
    void onAcStateChanged(uint state, uint reason)
    {
        // NMActiveConnectionState: 2 = ACTIVATED, 4 = DEACTIVATED
        // NMActiveConnectionStateReason: 9 = NO_SECRETS, 10 = LOGIN_FAILED
        if (state == 2) {
            emit finished(Outcome::Success, {});
        } else if (state == 4) {
            if (reason == 9 || reason == 10)
                emit finished(Outcome::WrongOrMissingPassword,
                              tr("The password was not accepted."));
            else
                emit finished(Outcome::Failed,
                              tr("Connection failed (reason %1).").arg(reason));
        }
    }
};
```

Then the model's decision tree for a click on a Wi-Fi row becomes:

```cpp
void NmModel::connectToWifi(const nm::WifiViewRecord &wifi)
{
    const auto target = pickWifiTarget(mCache.snapshot(), /*ifaceHint*/ {}, ssidBytesOf(wifi));
    if (target.devicePath.isEmpty()) {
        emit actionFailed(tr("No usable Wi-Fi adapter"), tr("Enable Wi-Fi and try again."));
        return;
    }

    if (!wifi.savedConnectionPath.isEmpty()) {
        // Saved profile: try it. On WrongOrMissingPassword → re-prompt and UPDATE the psk,
        // instead of today's silent dead end.
        activateSaved(wifi.savedConnectionPath, target, wifi);
        return;
    }
    promptThenAddAndActivate(wifi, target);   // §3.2 path, async, with WifiConnector watching
}

void NmModel::onSavedActivationFailedNeedAuth(const nm::WifiViewRecord &wifi,
                                              const QString &connectionPath,
                                              const WifiTarget &target)
{
    auto *dialog = new WifiPasswordDialog(wifi.ssid, /*secure*/ true, nullptr);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setInfoText(tr("The saved password for “%1” no longer works. Enter it again.")
                            .arg(wifi.ssid));
    connect(dialog, &QDialog::accepted, this, [=, this] {
        updateSavedPsk(connectionPath, dialog->password(), [=, this](bool ok, const QString &err) {
            if (!ok) { emit actionFailed(tr("Could not save password"), err); return; }
            activateSaved(connectionPath, target, wifi);       // retry with the new secret
        });
    });
    dialog->show(); dialog->raise(); dialog->activateWindow();
}
```

`updateSavedPsk()` is a GetSettings → inject secret → Update round trip; because this Update *does* contain a secret, include only that secret and rely on NM merging is no longer applicable — so fetch-modify-write the security section explicitly:

```cpp
void NmModel::updateSavedPsk(const QString &connectionPath, const QString &psk,
                             std::function<void(bool, QString)> done)
{
    QDBusMessage get = QDBusMessage::createMethodCall(
        kNmService, connectionPath, kSettingsConnIface, QStringLiteral("GetSettings"));
    nm::callAsync(std::move(get), this,
        [=, this](bool ok, const QString &err, const QDBusMessage &reply) {
            if (!ok) { done(false, err); return; }
            auto settings = qdbus_cast<QMap<QString, QVariantMap>>(reply.arguments().at(0));

            QVariantMap sec = settings.value(QStringLiteral("802-11-wireless-security"));
            sec.insert(QStringLiteral("psk"), psk);
            sec.insert(QStringLiteral("psk-flags"), 0u);        // system-owned: remembered on disk
            settings.insert(QStringLiteral("802-11-wireless-security"), sec);

            QDBusMessage upd = QDBusMessage::createMethodCall(
                kNmService, connectionPath, kSettingsConnIface, QStringLiteral("Update"));
            upd << QVariant::fromValue(settings);
            nm::callAsync(std::move(upd), this,
                [done](bool ok2, const QString &err2, const QDBusMessage &) {
                    done(ok2, err2);
                });
        });
}
```

A pleasant side effect verified in the NM source: when an `Update` supplies new secrets, the daemon clears the profile's `NO_SECRETS` autoconnect-block and resets the retry counter — so a network that NM had given up on starts auto-joining again the moment the corrected password is saved.

### 3.4 Fix part 3 (recommended) — a minimal secret agent

Parts 1+2 make passwords entered *through the tray* fully persistent and recoverable. The remaining hole is profiles whose secrets are **agent-owned** (created by GNOME/KDE tools, `psk-flags=1`): NM will ask agents, and this process still isn't one. Registering a minimal agent closes that, and also lets NM route *any* future secrets request (VPN, 802.1x identity prompts you choose to support later) through your dialog:

```cpp
// secret_agent.h — minimal org.freedesktop.NetworkManager.SecretAgent
class TraySecretAgent : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.NetworkManager.SecretAgent")
public:
    explicit TraySecretAgent(QObject *parent = nullptr) : QObject(parent)
    {
        auto bus = QDBusConnection::systemBus();
        bus.registerObject(QStringLiteral("/org/freedesktop/NetworkManager/SecretAgent"),
                           this, QDBusConnection::ExportAllSlots);
        QDBusMessage reg = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.NetworkManager"),
            QStringLiteral("/org/freedesktop/NetworkManager/AgentManager"),
            QStringLiteral("org.freedesktop.NetworkManager.AgentManager"),
            QStringLiteral("Register"));
        reg << QStringLiteral("org.nmtrayalt.agent");
        bus.asyncCall(reg);
    }

public Q_SLOTS:
    QVariantMap GetSecrets(const QMap<QString, QVariantMap> &connection,
                           const QDBusObjectPath &connectionPath,
                           const QString &settingName,
                           const QStringList &hints, uint flags)
    {
        Q_UNUSED(hints)
        // flags bit 0x1 = ALLOW_INTERACTION; without it, never show UI.
        const bool mayPrompt = flags & 0x1;
        if (settingName != QLatin1String("802-11-wireless-security") || !mayPrompt) {
            sendErrorReply(QStringLiteral("org.freedesktop.NetworkManager.SecretAgent.NoSecrets"),
                           QStringLiteral("nothing stored here"));
            return {};
        }
        setDelayedReply(true);
        const QDBusMessage req = message();
        const QString ssid = QString::fromUtf8(
            connection.value(QStringLiteral("802-11-wireless"))
                      .value(QStringLiteral("ssid")).toByteArray());

        auto *dlg = new WifiPasswordDialog(ssid, /*secure*/ true, nullptr);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        connect(dlg, &QDialog::accepted, this, [req, dlg] {
            QMap<QString, QVariantMap> out;
            out[QStringLiteral("802-11-wireless-security")] =
                QVariantMap{ { QStringLiteral("psk"), dlg->password() } };
            QDBusConnection::systemBus().send(
                req.createReply(QVariant::fromValue(out)));
        });
        connect(dlg, &QDialog::rejected, this, [req] {
            QDBusConnection::systemBus().send(req.createErrorReply(
                QStringLiteral("org.freedesktop.NetworkManager.SecretAgent.UserCanceled"),
                QStringLiteral("canceled")));
        });
        dlg->show(); dlg->raise(); dlg->activateWindow();
        return {};
    }

    void CancelGetSecrets(const QDBusObjectPath &, const QString &) {}
    void SaveSecrets(const QMap<QString, QVariantMap> &, const QDBusObjectPath &) {}
    void DeleteSecrets(const QMap<QString, QVariantMap> &, const QDBusObjectPath &) {}
};
```

With the agent registered, the earlier "no secrets: No agents were available" failure class disappears entirely; NM drives the prompt at exactly the right moment in the activation state machine, and §3.3's re-prompt code becomes the fallback rather than the main path. (Register only when no other agent applet is expected to run — the README already tells users to disable duplicates.)
---

## 4. Other bugs found

The table is the index; items marked ▸ get a code note below it.

| # | Location | Bug | Severity |
|---|---|---|---|
| 4.1 ▸ | tray.cpp `notify()` + nmmodel `applySection()` | Notification storms: every clear-and-reinsert of the active section fires "Connection lost" for *every* active connection, then "Connection established" again | High (user-visible spam) |
| 4.2 ▸ | nmmodel.cpp `buildActiveInfo()` | SSIDs / connection IDs interpolated into rich-text HTML unescaped → an AP named `<h1>…` or with `<img>` tags corrupts the info dialog (HTML injection from over-the-air data) | High |
| 4.3 ▸ | backend + connectioninfo | "Data received/transmitted" never update: `Device.Statistics.RxBytes/TxBytes` only tick when `RefreshRateMs > 0`, which nothing ever sets | Medium |
| 4.4 ▸ | nm_dbus_client.cpp `decodeSsid()` (:74) vs nm_cache.cpp `kHiddenSsid` (:10) | Sentinel mismatch under translation: `decodeSsid` returns `QObject::tr("<hidden>")`, the cache compares against the untranslated literal — on any localized system the hidden-SSID filtering silently stops working | Medium |
| 4.5 ▸ | nm_dbus_client.cpp | No `QDBusServiceWatcher` on `org.freedesktop.NetworkManager`: if NM restarts (or isn't up yet at login), the tray can sit on stale/empty state until some unrelated signal happens to arrive | Medium |
| 4.6 | nm_dbus_client.cpp :445 | `QDBusInterface` constructed per saved profile per refresh — its constructor does a **blocking Introspect round-trip**, so every refresh pays ~2 extra bus calls per profile; use `QDBusMessage::createMethodCall` like everywhere else | Medium (perf) |
| 4.7 | nmmodel.cpp `connectionPathForSsid()` (via nm_cache) | Matches any profile whose `id` equals the SSID, including ethernet/VPN profiles that happen to share the name → wrong profile activated on a Wi-Fi device; filter `type == "802-11-wireless"` | Medium |
| 4.8 | nm_dbus_client.cpp `refreshSnapshot()` | Ethernet `hardwareAddress` never populated (only the Wi-Fi branch reads `HwAddress`); read `HwAddress` from the Device interface (present since NM 1.24) with a wired-iface fallback so the info dialog shows the MAC for ethernet | Low |
| 4.9 | main.cpp | No single-instance guard — the rebuild guide literally instructs `pkill` first. `QLockFile` in `QStandardPaths::RuntimeLocation`, or register a session D-Bus name and bail if taken | Low |
| 4.10 | main.cpp / tray.cpp | No wait for `QSystemTrayIcon::isSystemTrayAvailable()`: autostarting before the panel's StatusNotifier host is up can leave no icon; poll with a short timer before `show()` | Low |
| 4.11 | tray.cpp + connectioninfo.cpp | Double-ownership of ConnectionInfo: the dialog sets `WA_DeleteOnClose` on itself while `TrayPrivate` also owns it via `QScopedPointer` and resets on `finished` — works today by ordering luck, breaks the day someone reorders; and WindowMenu constructs *additional* independent instances, so several info dialogs can pile up. Pick one owner and one accessor (see §5.3) | Low (fragile) |
| 4.12 | wifi_password_dialog.cpp | No WPA length validation (8–63 chars / 64 hex); Connect enabled by any non-whitespace char; no failure feedback surface in the dialog itself | Low (fixed by §3.2/3.3) |
| 4.13 | nm_dbus_client.cpp | `mRefreshInProgress` can never be observed true from `scheduleRefresh()` — `refreshSnapshot()` runs synchronously on the same thread, so the queue flag is dead code; remove it or make refresh genuinely re-entrant | Cosmetic |
| 4.14 | nm_actions.cpp | `callAndCheckExpected()` is defined in namespace `nm` at TU scope instead of the anonymous namespace — exported symbol for no reason; also it isn't declared in the header, which is an ODR accident waiting to happen | Cosmetic |
| 4.15 | autotz.cpp | AutoTz phones five third-party geo-IP services (revealing the machine's IP) and rewrites the **system timezone** via timedate1 on every new connection, with no setting to turn it off, and it also fires on `Limited` state where a captive portal will answer the HTTP request. Make it opt-in (QSettings + context-menu toggle, default off), and skip lookups unless status == Full from §2 | Medium (privacy/robustness) |
| 4.16 | menu/UX | No way to join a hidden SSID: `decodeSsid` placeholder entries are filtered out and no "Connect to hidden network…" action exists. Add a small dialog that builds the §3.2 settings map with `802-11-wireless.hidden = true` and the typed SSID | Feature gap |

**4.1 — notification storm.** `rowsAboutToBeRemoved` unconditionally sends "Connection lost" for each removed row (tray.cpp `notify()`), and `rowsInserted` re-arms every inserted row for an "established" toast. Because `applySection()` clears + reinserts the whole active section whenever its key sequence changes (VPN up/down, switching networks, a second device joining), unrelated healthy connections get a lost+established pair each time. The structural fix is §1.1(b) (granular diff → only genuinely added/removed rows emit row signals). Belt-and-braces, decouple notifications from model rows entirely — track by active-connection **path**, which is stable across model resets:

```cpp
// tray.cpp — replace QPersistentModelIndex-based bookkeeping
QSet<QString> mAnnouncedActive;   // AC paths we've toasted "established" for

void TrayPrivate::onActiveDiff(const QList<nm::ActiveConnectionRecord> &now)
{
    QSet<QString> current;
    for (const auto &ac : now) {
        current.insert(ac.path);
        const bool activated = ac.state == nm::ActiveState::Activated;
        if (activated && !mAnnouncedActive.contains(ac.path)) {
            mAnnouncedActive.insert(ac.path);
            sendToast(tr("Connection established"),
                      tr("Now connected to %1 '%2'.")
                          .arg(nm::connectionTypeLabel(ac.type), ac.id));
        }
    }
    for (auto it = mAnnouncedActive.begin(); it != mAnnouncedActive.end();) {
        if (!current.contains(*it)) {
            // find the id from the previous snapshot if you want it in the text
            sendToast(tr("Connection lost"), tr("No longer connected."));
            it = mAnnouncedActive.erase(it);
        } else ++it;
    }
}
```

**4.2 — HTML injection.** Everywhere `buildActiveInfo()` streams external strings, escape them:

```cpp
str << "<td>" << active.id.toHtmlEscaped() << "</td>";
str << "<td>" << devIt->interfaceName.toHtmlEscaped() << "</td>";
// …and every SSID, UUID, gateway, DNS string that came off the bus
```

Same for `mTrayIcon.setToolTip(...)` in tray.cpp, which wraps the connection name in `<pre>…</pre>` rich text.

**4.3 — frozen byte counters.** Enable stats only while the info dialog is open, so the daemon isn't polled forever:

```cpp
// connectioninfo.cpp — ctor / dtor pair
static void setStatsRate(const QString &devicePath, uint ms)
{
    QDBusMessage m = QDBusMessage::createMethodCall(
        "org.freedesktop.NetworkManager", devicePath,
        "org.freedesktop.DBus.Properties", "Set");
    m << QStringLiteral("org.freedesktop.NetworkManager.Device.Statistics")
      << QStringLiteral("RefreshRateMs") << QVariant::fromValue(QDBusVariant(ms));
    QDBusConnection::systemBus().asyncCall(m);
}
// on open: setStatsRate(devPath, 2000) for each device backing a shown tab
// on close: setStatsRate(devPath, 0)
```

The backend already listens for `RxBytes/TxBytes` PropertiesChanged, so the labels start moving with no further work. (Note: with 2 s ticks arriving, `deviceEquivalent()` including rx/tx bytes means a `dataChanged` every tick — fine once §1.1(b) makes updates in-place instead of resets.)

**4.4 — hidden-SSID sentinel.** Stop using a translatable, displayable string as an in-band sentinel. Keep `ssidBytes` as the source of truth and make emptiness the sentinel:

```cpp
// nm_dbus_client.cpp
QString decodeSsid(const QVariant &value)
{
    const auto bytes = value.toByteArray();
    return bytes.isEmpty() ? QString{} : QString::fromUtf8(bytes);
}
// nm_cache.cpp
bool hasConcreteSsid(const nm::AccessPointRecord &ap) { return !ap.ssidBytes.isEmpty(); }
// UI layer decides how to *render* an empty ssid: tr("<hidden>")
```

**4.5 — NM lifecycle.** One watcher makes restarts and late starts self-healing:

```cpp
// nm_dbus_client.cpp — in start()
auto *watch = new QDBusServiceWatcher(QStringLiteral("org.freedesktop.NetworkManager"),
                                      QDBusConnection::systemBus(),
                                      QDBusServiceWatcher::WatchForOwnerChange, this);
connect(watch, &QDBusServiceWatcher::serviceOwnerChanged, this,
        [this](const QString &, const QString &, const QString &newOwner) {
            if (!newOwner.isEmpty()) {           // NM (re)appeared: paths all changed
                mDynamicPropertyPaths.clear();   // stale match rules are harmless but tidy up
                scheduleRefresh();
            } else {
                mSnapshot = {};                  // NM gone: reflect reality
                emit snapshotChanged(mSnapshot);
                emit managerStateChanged();
            }
        });
```

---

## 5. Redundancy — menu display and backend

### 5.1 The popup shows the same network up to four times

For a typical laptop, a saved, in-range Wi-Fi network currently appears as: (1) the status line *"Connected: X"*, (2) a row in *Recent connection(s)*, (3) a row in *Wi-Fi network(s)*, and (4) a row in *Known connection(s)* — because `knownConnections()` returns **all** saved wifi/ethernet/bluetooth profiles regardless of visibility, and the wifi list independently shows every in-range SSID. Worse, rows (3) and (4) do subtly different things when clicked: the wifi row activates with the AP as `specific_object`, the known-connections row activates with `"/"` and the §1.3 device-guessing bug. Same label, different code path, different failure modes.

Proposed structure — one list, one code path, no duplicates:

```
[ Connected: HomeLAN — Internet access        ]   ← status + §2 verdict; click = details
[ Disconnect                                   ]
────────────────────────────────────────────────
Wi-Fi networks                                     ← in-range list only (active pinned top,
  ● HomeWifi        ▂▄▆█  🔒 (saved)                 saved above unknown, §1.1 stable order)
    CafeGuest       ▂▄▆_  🔒
    Neighbor5G      ▂___  🔒
────────────────────────────────────────────────
Other saved networks              ▸                ← submenu: saved-but-not-in-range +
Connect to hidden network…                           wired/bluetooth profiles (replaces both
Show low-signal networks                             "Recent" and "Known connections")
```

Concretely: delete the *Recent connection(s)* block (it is `knownConnections` re-sorted by timestamp and truncated — pure duplication), and turn *Known connection(s)* into an *Other saved networks* submenu whose model filters out any profile whose SSID is currently visible (`wifiEntries` already computes `savedConnectionPath`, so the join is one pass). Route *both* remaining click paths through the single `connectToWifi()` of §3.3.

### 5.2 One "connection information" entry point, one instance

Right now the same dialog is reachable as *Connection information* (right-click context menu, singleton in `TrayPrivate::mInfoDialog`) and *Connection details / usage…* (left-click popup, `new ConnectionInfo` + `WA_DeleteOnClose` each click, unbounded instances). Keep one label and one owner:

```cpp
// tray.h / tray.cpp — the only way anyone opens it
void Tray::showConnectionInfo()
{
    if (d->mInfoDialog.isNull()) {
        d->mInfoDialog.reset(new ConnectionInfo(&d->mNmModel));
        d->mInfoDialog->setAttribute(Qt::WA_DeleteOnClose, false);   // Tray owns it, period
        connect(d->mInfoDialog.data(), &QDialog::finished,
                this, [this] { d->mInfoDialog.reset(); });
    }
    d->openCloseDialog(d->mInfoDialog.data());
}
```

and have `WindowMenu` emit a `requestConnectionInfo()` signal that `Tray` connects to this slot, instead of constructing dialogs itself.

### 5.3 ~200 duplicated lines of field-by-field equality

`nm_dbus_client.cpp` (anonymous namespace) and `nmmodel.cpp` (anonymous namespace) each hand-roll `deviceEquivalent`, `accessPointEquivalent`, `activeConnectionEquivalent`, `savedConnectionEquivalent`, `wifiEquivalent`, `connectionEquivalent`, plus `mapEquivalent`/`snapshotEquivalent`. Every field added to a record must now be added in two or three places or change-detection silently rots (this is exactly the class of bug that produced 4.8). The project is C++23; defaulted comparisons erase all of it:

```cpp
// nm_types.h — one line per struct
struct AccessPointRecord    { /* fields… */ bool operator==(const AccessPointRecord &) const = default; };
struct DeviceRecord         { /* fields… */ bool operator==(const DeviceRecord &) const = default; };
struct SavedConnectionRecord{ /* fields… */ bool operator==(const SavedConnectionRecord &) const = default; };
struct ActiveConnectionRecord{ /* fields… */ bool operator==(const ActiveConnectionRecord &) const = default; };
struct ManagerState         { /* fields… */ bool operator==(const ManagerState &) const = default; };
struct Snapshot             { /* fields… */ bool operator==(const Snapshot &) const = default; };
// nm_cache.h — same for WifiViewRecord / ConnectionViewRecord
```

`QString/QByteArray/QList/QMap/QStringList` all provide `operator==`, so `snapshotEquivalent(a,b)` becomes `a == b` and both anonymous-namespace blocks (~90 lines each) are deleted. (`Snapshot::collectedAt` should be excluded from equality — move it out of the struct or compare members explicitly minus that field; otherwise every refresh looks "changed".)

### 5.4 Backend duplication and wasted D-Bus traffic

Beyond §4.6 (per-profile Introspect), three more consolidations:

1. `interfaceNameForDevicePath()` in nm_actions.cpp re-fetches over D-Bus what the snapshot already holds — pass the interface name down from the model (it's in `DeviceRecord`) and delete the helper. It disappears anyway when nmcli goes (§3.2).
2. `connTypeToInt()` / `isWirelessType()` in nmmodel.cpp duplicate classification that `nm_types` half-owns (`isVpnType`, `connectionTypeLabel`). Move them all into `nm_types.{h,cpp}` so menu, cache and model agree on one taxonomy.
3. The refresh strategy itself is the deepest redundancy: any single `Strength: 57→58` on one AP re-fetches **everything** — every device, every AP, every profile's `GetSettings`, every active connection and both IP configs (dozens of round trips). With the diffing model of §1.1(b) in place, add one fast path in `onPropertiesChanged()` for the highest-frequency case:

```cpp
// nm_dbus_client.cpp — before scheduleRefresh(), inside onPropertiesChanged()
if (interfaceName == QLatin1String(kAccessPointIface)) {
    const QString apPath = msg.path();   // see note below on obtaining the sender path
    auto it = mSnapshot.accessPoints.find(apPath);
    if (it != mSnapshot.accessPoints.end()) {
        bool touched = false;
        if (auto v = changedProperties.constFind(QStringLiteral("Strength"));
            v != changedProperties.cend()) { it->strength = v->toInt(); touched = true; }
        if (touched) {
            emit snapshotChanged(mSnapshot);          // cheap: no re-poll at all
            return;
        }
    }
}
```

(To obtain the sender path, extend the slot signature with a trailing `const QDBusMessage &msg` parameter — QtDBus fills it in automatically for slots connected via `QDBusConnection::connect` — and update the `SLOT(...)` signatures accordingly; keep the full refresh for structural signals. This one change removes the vast majority of bus traffic and of §1.1's churn pressure at the source.)

---

## 6. Connection troubleshooting screen

### 6.1 Design

A **Troubleshoot…** button in the Connection Information dialog opens a modeless window that runs a staged pipeline top-to-bottom, painting each stage ✓ / ✗ / – as results arrive, then prints a verdict in plain language plus the actions that make sense. The stages are ordered so that the *first* failure localizes the fault (radio → link → IP → gateway → WAN routing → DNS → HTTP/portal), which is exactly the "no DNS? no packets at all?" separation you asked for. The final stage answers "might another saved Wi-Fi work?": it joins the live AP list against saved profiles, checks that the password is actually retrievable (system-owned `psk-flags == 0`, i.e. usable without a keyring), and offers a one-click switch.

```
┌─ Troubleshoot connection ──────────────────────────────────────┐
│ ✓ NetworkManager running, Wi-Fi enabled                        │
│ ✓ Associated with “CafeGuest” (signal 62 %, 866 Mb/s)          │
│ ✓ IP address 10.4.1.23/24, gateway 10.4.1.1, DNS 10.4.1.1      │
│ ✓ Gateway responds to ping (2.1 ms)                            │
│ ✓ Internet responds to ping (1.1.1.1, 18 ms)                   │
│ ✗ DNS lookup failed (system resolver)                          │
│ ✗ DNS lookup failed (connection's server 10.4.1.1)             │
│ – HTTP check skipped (needs DNS)                               │
│ ✓ 2 saved networks in range could work                         │
│                                                                │
│ Verdict: Packets reach the internet but names don’t resolve —  │
│ the network’s DNS server is not answering.                     │
│ [ Use public DNS on this connection ]  [ Re-run ]              │
│ Alternatives:  HomeWifi (84 %, password saved)   [Connect]     │
│                Hotspot-5G (41 %, password saved) [Connect]     │
└────────────────────────────────────────────────────────────────┘
```

### 6.2 The diagnostics engine

```cpp
// src/diagnostics.h
#pragma once
#include "backend/nm_types.h"
#include <QDnsLookup>
#include <QNetworkAccessManager>
#include <QObject>
#include <QProcess>

class NmModel;

struct DiagResult
{
    enum class Verdict { Pending, Running, Pass, Fail, Skipped };
    QString title;      // "Gateway reachability"
    QString detail;     // "10.4.1.1 responded in 2.1 ms"
    Verdict verdict = Verdict::Pending;
};

struct WifiCandidate
{
    QString ssid;
    QString connectionPath;   // saved profile
    QString devicePath;
    QString apPath;
    int strength = 0;
    bool passwordOnDisk = false;   // psk-flags == 0 → works with no agent/keyring
};

class NetDiagnostics : public QObject
{
    Q_OBJECT
public:
    enum Stage {
        StNmState, StLink, StIpConfig, StGatewayPing, StWanPing,
        StDnsSystem, StDnsDirect, StHttpPortal, StNmVerdict, StAlternatives,
        StageCount
    };
    Q_ENUM(Stage)

    explicit NetDiagnostics(NmModel *model, QObject *parent = nullptr);

    const QList<DiagResult> &results() const { return mResults; }
    const QList<WifiCandidate> &candidates() const { return mCandidates; }
    QString verdictText() const { return mVerdict; }
    QString suggestedActionId() const { return mAction; }   // "", "portal", "set-dns", "switch"

public Q_SLOTS:
    void run();

Q_SIGNALS:
    void stageUpdated(int stage);
    void finished();

private:
    void set(Stage s, DiagResult::Verdict v, const QString &detail);
    void ping(const QString &target, Stage stage, std::function<void(bool, QString)> done);
    void nextAfter(Stage s);

    // stages
    void stNmState();     void stLink();       void stIpConfig();
    void stGatewayPing(); void stWanPing();    void stDnsSystem();
    void stDnsDirect();   void stHttpPortal(); void stNmVerdict();
    void stAlternatives();
    void concludeVerdict();

    NmModel *mModel;
    QNetworkAccessManager mNam;
    QList<DiagResult> mResults;
    QList<WifiCandidate> mCandidates;
    QString mVerdict, mAction;

    // context captured at run() from the snapshot
    QString mGateway, mFirstDns, mIface;
    bool mWifi = false;
    bool mHaveIp = false;
};
```

```cpp
// src/diagnostics.cpp
#include "diagnostics.h"
#include "nmmodel.h"
#include "backend/nm_actions.h"   // for the async helper of §1.2
#include "log.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QHostAddress>
#include <QNetworkReply>
#include <QRegularExpression>

namespace
{
const QString kWanProbeIp   = QStringLiteral("1.1.1.1");
const QString kDnsProbeName = QStringLiteral("connectivity-check.ubuntu.com");
const QUrl    kHttpProbeUrl = QUrl(QStringLiteral("http://connectivity-check.ubuntu.com/"));
constexpr int kMinCandidateSignal = 35;
}

NetDiagnostics::NetDiagnostics(NmModel *model, QObject *parent)
    : QObject(parent), mModel(model)
{
    mResults.resize(StageCount);
    mResults[StNmState]     = { tr("NetworkManager & adapter state"), {}, {} };
    mResults[StLink]        = { tr("Link layer"), {}, {} };
    mResults[StIpConfig]    = { tr("IP configuration"), {}, {} };
    mResults[StGatewayPing] = { tr("Gateway reachability"), {}, {} };
    mResults[StWanPing]     = { tr("Internet reachability (by IP)"), {}, {} };
    mResults[StDnsSystem]   = { tr("DNS (system resolver)"), {}, {} };
    mResults[StDnsDirect]   = { tr("DNS (connection's server)"), {}, {} };
    mResults[StHttpPortal]  = { tr("Web check / captive portal"), {}, {} };
    mResults[StNmVerdict]   = { tr("NetworkManager's own verdict"), {}, {} };
    mResults[StAlternatives]= { tr("Other saved networks in range"), {}, {} };
}

void NetDiagnostics::set(Stage s, DiagResult::Verdict v, const QString &detail)
{
    mResults[s].verdict = v;
    mResults[s].detail  = detail;
    emit stageUpdated(s);
}

void NetDiagnostics::run()
{
    for (auto &r : mResults) { r.verdict = DiagResult::Verdict::Pending; r.detail.clear(); }
    mCandidates.clear(); mVerdict.clear(); mAction.clear();
    mGateway.clear(); mFirstDns.clear(); mIface.clear();
    mWifi = mHaveIp = false;
    stNmState();
}

// ── Stage 0: manager & device state ─────────────────────────────
void NetDiagnostics::stNmState()
{
    set(StNmState, DiagResult::Verdict::Running, {});
    const auto ms = mModel->managerState();
    if (!ms.networkingEnabled) {
        set(StNmState, DiagResult::Verdict::Fail, tr("Networking is disabled"));
        concludeVerdict(); return;
    }
    if (!ms.wirelessHardwareEnabled) {
        set(StNmState, DiagResult::Verdict::Fail,
            tr("Wi-Fi hardware switch is off (rfkill / airplane mode)"));
        concludeVerdict(); return;
    }
    set(StNmState, DiagResult::Verdict::Pass,
        ms.wirelessEnabled ? tr("Running; Wi-Fi enabled") : tr("Running; Wi-Fi disabled in software"));
    stLink();
}

// ── Stage 1: link ────────────────────────────────────────────────
void NetDiagnostics::stLink()
{
    set(StLink, DiagResult::Verdict::Running, {});
    const auto &snap = mModel->cacheSnapshot();            // add a const accessor on NmModel
    const QString primary = mModel->primaryPhysicalConnectionPath();
    const auto ac = snap.activeConnections.constFind(primary);
    if (ac == snap.activeConnections.cend() || ac->devices.isEmpty()) {
        set(StLink, DiagResult::Verdict::Fail, tr("No active physical connection"));
        stAlternatives();                                  // still worth listing alternatives
        return;
    }
    const auto dev = snap.devices.constFind(ac->devices.front());
    if (dev == snap.devices.cend()) {
        set(StLink, DiagResult::Verdict::Fail, tr("Device not found"));
        concludeVerdict(); return;
    }
    mIface = dev->interfaceName;
    mWifi  = dev->type == nm::DeviceType::Wifi;
    if (mWifi) {
        const auto ap = snap.accessPoints.constFind(dev->activeAccessPointPath);
        if (ap == snap.accessPoints.cend()) {
            set(StLink, DiagResult::Verdict::Fail, tr("Not associated with any access point"));
        } else {
            set(StLink, DiagResult::Verdict::Pass,
                tr("Associated with “%1” (signal %2%, %3 Mb/s)")
                    .arg(ap->ssid).arg(ap->strength)
                    .arg(dev->bitrateKbps > 0 ? dev->bitrateKbps / 1000 : 0));
        }
    } else {
        set(StLink, DiagResult::Verdict::Pass, tr("Wired link on %1").arg(mIface));
    }
    // capture IP context for later stages
    mGateway  = ac->ip4Gateway;
    mFirstDns = ac->ip4Dns.value(0);
    mHaveIp   = !ac->ip4Addresses.isEmpty() || !ac->ip6Addresses.isEmpty();
    stIpConfig();
}

// ── Stage 2: IP configuration ────────────────────────────────────
void NetDiagnostics::stIpConfig()
{
    set(StIpConfig, DiagResult::Verdict::Running, {});
    const auto &snap = mModel->cacheSnapshot();
    const auto ac = snap.activeConnections.constFind(mModel->primaryPhysicalConnectionPath());
    if (ac == snap.activeConnections.cend() || !mHaveIp) {
        set(StIpConfig, DiagResult::Verdict::Fail,
            tr("No IP address — DHCP did not complete"));
        stAlternatives(); return;
    }
    QStringList bits;
    if (!ac->ip4Addresses.isEmpty()) bits << ac->ip4Addresses.first();
    if (!mGateway.isEmpty()) bits << tr("gw %1").arg(mGateway);
    bits << (ac->ip4Dns.isEmpty() ? tr("no DNS servers!") : tr("DNS %1").arg(ac->ip4Dns.join(", ")));
    set(StIpConfig,
        (mGateway.isEmpty() || ac->ip4Dns.isEmpty())
            ? DiagResult::Verdict::Fail : DiagResult::Verdict::Pass,
        bits.join(QStringLiteral("  ·  ")));
    stGatewayPing();
}

// ── ping helper (async QProcess; ping is capability-enabled everywhere) ──
void NetDiagnostics::ping(const QString &target, Stage stage,
                          std::function<void(bool, QString)> done)
{
    auto *p = new QProcess(this);
    connect(p, &QProcess::finished, this,
        [p, done = std::move(done)](int code, QProcess::ExitStatus st) {
            const QString out = QString::fromLocal8Bit(p->readAllStandardOutput());
            p->deleteLater();
            if (st != QProcess::NormalExit || code != 0) { done(false, {}); return; }
            static const QRegularExpression rtt(QStringLiteral("time[=<]([0-9.]+) ms"));
            const auto m = rtt.match(out);
            done(true, m.hasMatch() ? m.captured(1) + QStringLiteral(" ms") : QString{});
        });
    connect(p, &QProcess::errorOccurred, this, [this, p, stage](QProcess::ProcessError) {
        p->deleteLater();
        set(stage, DiagResult::Verdict::Skipped, tr("ping utility not available"));
        nextAfter(stage);
    });
    p->start(QStringLiteral("ping"),
             { QStringLiteral("-n"), QStringLiteral("-c"), QStringLiteral("1"),
               QStringLiteral("-W"), QStringLiteral("2"), target });
}

void NetDiagnostics::nextAfter(Stage s)
{
    switch (s) {
    case StGatewayPing: stWanPing(); break;
    case StWanPing:     stDnsSystem(); break;
    default: break;
    }
}

// ── Stage 3: gateway ─────────────────────────────────────────────
void NetDiagnostics::stGatewayPing()
{
    if (mGateway.isEmpty()) {
        set(StGatewayPing, DiagResult::Verdict::Skipped, tr("no gateway configured"));
        stWanPing(); return;
    }
    set(StGatewayPing, DiagResult::Verdict::Running, mGateway);
    ping(mGateway, StGatewayPing, [this](bool ok, const QString &rtt) {
        set(StGatewayPing, ok ? DiagResult::Verdict::Pass : DiagResult::Verdict::Fail,
            ok ? tr("%1 responded (%2)").arg(mGateway, rtt)
               : tr("%1 did not respond").arg(mGateway));
        stWanPing();
    });
}

// ── Stage 4: WAN by IP (separates routing from DNS) ─────────────
void NetDiagnostics::stWanPing()
{
    set(StWanPing, DiagResult::Verdict::Running, kWanProbeIp);
    ping(kWanProbeIp, StWanPing, [this](bool ok, const QString &rtt) {
        set(StWanPing, ok ? DiagResult::Verdict::Pass : DiagResult::Verdict::Fail,
            ok ? tr("%1 responded (%2)").arg(kWanProbeIp, rtt)
               : tr("no route to the internet (%1 unreachable)").arg(kWanProbeIp));
        stDnsSystem();
    });
}

// ── Stage 5/6: DNS via system resolver, then via the connection's server ──
void NetDiagnostics::stDnsSystem()
{
    set(StDnsSystem, DiagResult::Verdict::Running, kDnsProbeName);
    auto *lk = new QDnsLookup(QDnsLookup::A, kDnsProbeName, this);
    connect(lk, &QDnsLookup::finished, this, [this, lk] {
        const bool ok = lk->error() == QDnsLookup::NoError && !lk->hostAddressRecords().isEmpty();
        set(StDnsSystem, ok ? DiagResult::Verdict::Pass : DiagResult::Verdict::Fail,
            ok ? tr("%1 → %2").arg(kDnsProbeName,
                     lk->hostAddressRecords().first().value().toString())
               : tr("lookup failed: %1").arg(lk->errorString()));
        lk->deleteLater();
        stDnsDirect();
    });
    lk->lookup();
}

void NetDiagnostics::stDnsDirect()
{
    if (mFirstDns.isEmpty()) {
        set(StDnsDirect, DiagResult::Verdict::Skipped, tr("connection lists no DNS server"));
        stHttpPortal(); return;
    }
    set(StDnsDirect, DiagResult::Verdict::Running, mFirstDns);
    auto *lk = new QDnsLookup(QDnsLookup::A, kDnsProbeName, this);
    lk->setNameserver(QHostAddress(mFirstDns));            // ask the DHCP-provided server directly
    connect(lk, &QDnsLookup::finished, this, [this, lk] {
        const bool ok = lk->error() == QDnsLookup::NoError && !lk->hostAddressRecords().isEmpty();
        set(StDnsDirect, ok ? DiagResult::Verdict::Pass : DiagResult::Verdict::Fail,
            ok ? tr("%1 answered").arg(mFirstDns)
               : tr("%1 did not answer").arg(mFirstDns));
        lk->deleteLater();
        stHttpPortal();
    });
    lk->lookup();
}

// ── Stage 7: HTTP + captive-portal detection ─────────────────────
void NetDiagnostics::stHttpPortal()
{
    if (mResults[StDnsSystem].verdict == DiagResult::Verdict::Fail
        && mResults[StDnsDirect].verdict != DiagResult::Verdict::Pass) {
        set(StHttpPortal, DiagResult::Verdict::Skipped, tr("needs working DNS"));
        stNmVerdict(); return;
    }
    set(StHttpPortal, DiagResult::Verdict::Running, kHttpProbeUrl.host());
    QNetworkRequest req(kHttpProbeUrl);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::ManualRedirectPolicy);
    req.setTransferTimeout(5000);
    QNetworkReply *reply = mNam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool genuine = reply->error() == QNetworkReply::NoError
            && (http == 204 || reply->rawHeader("X-NetworkManager-Status").trimmed() == "online");
        if (genuine)
            set(StHttpPortal, DiagResult::Verdict::Pass, tr("expected reply received"));
        else if (http >= 200 && http < 400)
            set(StHttpPortal, DiagResult::Verdict::Fail,
                tr("hijacked reply — captive portal detected"));
        else
            set(StHttpPortal, DiagResult::Verdict::Fail,
                tr("no HTTP reply (%1)").arg(reply->errorString()));
        stNmVerdict();
    });
}

// ── Stage 8: cross-check with NM's checker ───────────────────────
void NetDiagnostics::stNmVerdict()
{
    set(StNmVerdict, DiagResult::Verdict::Running, {});
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.NetworkManager"),
        QStringLiteral("/org/freedesktop/NetworkManager"),
        QStringLiteral("org.freedesktop.NetworkManager"),
        QStringLiteral("CheckConnectivity"));
    auto *w = new QDBusPendingCallWatcher(
        QDBusConnection::systemBus().asyncCall(msg, 8000), this);
    connect(w, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        QDBusPendingReply<uint> r = *w; w->deleteLater();
        static const char *names[] = { "unknown", "none", "portal", "limited", "full" };
        const uint v = r.isError() ? 0u : qMin<uint>(r.value(), 4u);
        set(StNmVerdict,
            v == 4 ? DiagResult::Verdict::Pass
                   : (v == 0 ? DiagResult::Verdict::Skipped : DiagResult::Verdict::Fail),
            r.isError() ? tr("check unavailable") : QString::fromLatin1(names[v]));
        stAlternatives();
    });
}

// ── Stage 9: saved networks in range that could work ─────────────
void NetDiagnostics::stAlternatives()
{
    set(StAlternatives, DiagResult::Verdict::Running, {});
    const auto &snap = mModel->cacheSnapshot();
    const QString currentSsid = mModel->managerState().primaryName;

    struct Pending { WifiCandidate c; };
    auto pendingChecks = std::make_shared<int>(0);
    auto finish = [this, pendingChecks] {
        if (*pendingChecks != 0) return;
        std::sort(mCandidates.begin(), mCandidates.end(),
                  [](const WifiCandidate &a, const WifiCandidate &b) {
                      if (a.passwordOnDisk != b.passwordOnDisk) return a.passwordOnDisk;
                      return a.strength > b.strength;
                  });
        set(StAlternatives,
            mCandidates.isEmpty() ? DiagResult::Verdict::Skipped : DiagResult::Verdict::Pass,
            mCandidates.isEmpty()
                ? tr("none in range")
                : tr("%n saved network(s) in range could work", nullptr, mCandidates.size()));
        concludeVerdict();
    };

    for (const auto &ap : snap.accessPoints) {
        if (ap.ssid.isEmpty() || ap.ssid == currentSsid || ap.strength < kMinCandidateSignal)
            continue;
        // find best matching saved wifi profile (same matching rules as NmCache)
        const nm::SavedConnectionRecord *best = nullptr;
        for (const auto &conn : snap.savedConnections) {
            if (conn.type != QLatin1String("802-11-wireless")) continue;
            if (!conn.wifiSsidBytes.isEmpty() ? conn.wifiSsidBytes != ap.ssidBytes
                                              : conn.id != ap.ssid) continue;
            if (!best || conn.timestamp > best->timestamp) best = &conn;
        }
        if (!best) continue;

        WifiCandidate c{ ap.ssid, best->path, ap.devicePath, ap.path, ap.strength, false };
        mCandidates.push_back(c);
        const int idx = mCandidates.size() - 1;

        // is the password retrievable without an agent?  psk-flags == 0 (default) → yes
        ++*pendingChecks;
        QDBusMessage get = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.NetworkManager"), best->path,
            QStringLiteral("org.freedesktop.NetworkManager.Settings.Connection"),
            QStringLiteral("GetSettings"));
        nm::callAsync(std::move(get), this,
            [this, idx, pendingChecks, finish](bool ok, const QString &, const QDBusMessage &reply) {
                if (ok) {
                    const auto settings = qdbus_cast<QMap<QString, QVariantMap>>(reply.arguments().at(0));
                    const QVariantMap sec = settings.value(QStringLiteral("802-11-wireless-security"));
                    const uint flags = sec.value(QStringLiteral("psk-flags"), 0u).toUInt();
                    const bool open  = sec.isEmpty();
                    mCandidates[idx].passwordOnDisk = open || flags == 0;   // 0 = system-owned
                }
                --*pendingChecks;
                finish();
            });
    }
    finish();   // handles the zero-candidates case
}

// ── verdict synthesis ────────────────────────────────────────────
void NetDiagnostics::concludeVerdict()
{
    using V = DiagResult::Verdict;
    auto v = [this](Stage s) { return mResults[s].verdict; };

    if (v(StNmState) == V::Fail) {
        mVerdict = tr("Networking or the Wi-Fi radio is switched off. Enable it and re-run.");
    } else if (v(StLink) == V::Fail) {
        mVerdict = mWifi
            ? tr("Not associated with any access point — wrong password, out of range, "
                 "or the AP rejected the connection.")
            : tr("No link — check the cable/port.");
        mAction = QStringLiteral("switch");
    } else if (v(StIpConfig) == V::Fail && !mHaveIp) {
        mVerdict = tr("Associated, but the router never handed out an IP address (DHCP failure). "
                      "This often means a wrong Wi-Fi password on WPA networks, or a router-side issue.");
        mAction = QStringLiteral("switch");
    } else if (v(StGatewayPing) == V::Fail && v(StWanPing) == V::Fail) {
        mVerdict = tr("No packets get through at all — the router is not responding on the local "
                      "network. Reboot the router or try another network.");
        mAction = QStringLiteral("switch");
    } else if (v(StGatewayPing) == V::Pass && v(StWanPing) == V::Fail) {
        mVerdict = tr("The router works, but it has no route to the internet — the problem is "
                      "upstream (modem/ISP), not this machine.");
        mAction = QStringLiteral("switch");
    } else if (v(StHttpPortal) == V::Fail
               && mResults[StHttpPortal].detail.contains(tr("captive portal"))) {
        mVerdict = tr("A captive portal is intercepting traffic — you need to sign in first.");
        mAction = QStringLiteral("portal");
    } else if (v(StWanPing) == V::Pass
               && v(StDnsSystem) == V::Fail && v(StDnsDirect) == V::Fail) {
        mVerdict = tr("Packets reach the internet but names don’t resolve — the network’s DNS "
                      "server is not answering. Overriding DNS on this connection should fix it.");
        mAction = QStringLiteral("set-dns");
    } else if (v(StDnsSystem) == V::Fail && v(StDnsDirect) == V::Pass) {
        mVerdict = tr("The network’s DNS server works, but this machine’s local resolver is "
                      "misconfigured (systemd-resolved / /etc/resolv.conf).");
    } else if (v(StNmVerdict) == V::Fail) {
        mVerdict = tr("Everything checks out here, but NetworkManager still reports limited "
                      "connectivity — its cached verdict may be stale; it has been asked to re-check.");
    } else {
        mVerdict = tr("No faults found — the connection looks healthy.");
    }
    emit finished();
}
```

The one supporting action worth shipping alongside — **"Use public DNS on this connection"** — is a GetSettings → set `ipv4.dns` + `ipv4.ignore-auto-dns` → Update → reactivate:

```cpp
void applyPublicDns(QObject *ctx, const QString &connectionPath, const QString &acPath)
{
    QDBusMessage get = QDBusMessage::createMethodCall(
        "org.freedesktop.NetworkManager", connectionPath,
        "org.freedesktop.NetworkManager.Settings.Connection", "GetSettings");
    nm::callAsync(std::move(get), ctx,
        [ctx, connectionPath, acPath](bool ok, const QString &, const QDBusMessage &reply) {
            if (!ok) return;
            auto settings = qdbus_cast<QMap<QString, QVariantMap>>(reply.arguments().at(0));
            QVariantMap ip4 = settings.value(QStringLiteral("ipv4"));
            // NM's D-Bus 'dns' is an array of IPv4 addresses in network byte order
            ip4.insert(QStringLiteral("dns"),
                       QVariant::fromValue(QList<uint>{ qToBigEndian(0x01010101u),      // 1.1.1.1
                                                        qToBigEndian(0x09090909u) }));  // 9.9.9.9
            ip4.insert(QStringLiteral("ignore-auto-dns"), true);
            settings.insert(QStringLiteral("ipv4"), ip4);
            QDBusMessage upd = QDBusMessage::createMethodCall(
                "org.freedesktop.NetworkManager", connectionPath,
                "org.freedesktop.NetworkManager.Settings.Connection", "Update");
            upd << QVariant::fromValue(settings);
            nm::callAsync(std::move(upd), ctx, [](bool, const QString &, const QDBusMessage &) {
                // reactivate so the new DNS takes effect (ActivateConnection on same profile)
            });
        });
}
```

### 6.3 The dialog

```cpp
// src/troubleshootdialog.h/.cpp — launched from ConnectionInfo's "Troubleshoot…" button
class TroubleshootDialog : public QDialog
{
    Q_OBJECT
public:
    TroubleshootDialog(NmModel *model, QWidget *parent = nullptr)
        : QDialog(parent), mDiag(new NetDiagnostics(model, this)), mModel(model)
    {
        setWindowTitle(tr("Troubleshoot connection"));
        resize(560, 480);

        auto *lay = new QVBoxLayout(this);
        mList = new QTreeWidget(this);
        mList->setHeaderHidden(true);
        mList->setColumnCount(2);
        mList->setRootIsDecorated(false);
        for (int i = 0; i < NetDiagnostics::StageCount; ++i) {
            auto *it = new QTreeWidgetItem(mList);
            it->setText(1, mDiag->results().at(i).title);
        }
        lay->addWidget(mList, 1);

        mVerdict = new QLabel(this);
        mVerdict->setWordWrap(true);
        mVerdict->setTextFormat(Qt::PlainText);       // never render bus data as HTML
        lay->addWidget(mVerdict);

        mAltBox = new QVBoxLayout;                    // candidate rows appear here
        lay->addLayout(mAltBox);

        auto *buttons = new QHBoxLayout;
        mActionBtn = new QPushButton(this); mActionBtn->hide();
        auto *rerun = new QPushButton(tr("Re-run"), this);
        buttons->addWidget(mActionBtn);
        buttons->addStretch(1);
        buttons->addWidget(rerun);
        lay->addLayout(buttons);

        connect(rerun, &QPushButton::clicked, mDiag, &NetDiagnostics::run);
        connect(mDiag, &NetDiagnostics::stageUpdated, this, &TroubleshootDialog::paintStage);
        connect(mDiag, &NetDiagnostics::finished, this, &TroubleshootDialog::paintVerdict);
        mDiag->run();
    }

private Q_SLOTS:
    void paintStage(int i)
    {
        static const QHash<DiagResult::Verdict, const char *> icon = {
            { DiagResult::Verdict::Pass,    "emblem-default"  },   // ✓
            { DiagResult::Verdict::Fail,    "dialog-error"    },   // ✗
            { DiagResult::Verdict::Running, "view-refresh"    },
            { DiagResult::Verdict::Skipped, "dialog-question" },
            { DiagResult::Verdict::Pending, ""                },
        };
        const auto &r = mDiag->results().at(i);
        auto *it = mList->topLevelItem(i);
        it->setIcon(0, QIcon::fromTheme(QLatin1String(icon.value(r.verdict))));
        it->setText(1, r.detail.isEmpty() ? r.title
                                          : r.title + QStringLiteral(" — ") + r.detail);
    }

    void paintVerdict()
    {
        mVerdict->setText(mDiag->verdictText());

        // one-click follow-up action
        const QString act = mDiag->suggestedActionId();
        mActionBtn->setVisible(!act.isEmpty() && act != QLatin1String("switch"));
        if (act == QLatin1String("portal")) {
            mActionBtn->setText(tr("Open sign-in page"));
            mActionBtn->disconnect();
            connect(mActionBtn, &QPushButton::clicked, this, [] {
                QDesktopServices::openUrl(QUrl(QStringLiteral("http://connectivity-check.ubuntu.com/")));
            });
        } else if (act == QLatin1String("set-dns")) {
            mActionBtn->setText(tr("Use public DNS on this connection"));
            mActionBtn->disconnect();
            connect(mActionBtn, &QPushButton::clicked, this, [this] {
                const auto &snap = mModel->cacheSnapshot();
                const auto ac = snap.activeConnections.constFind(
                    mModel->primaryPhysicalConnectionPath());
                if (ac != snap.activeConnections.cend())
                    applyPublicDns(this, ac->connectionPath, ac->path);
            });
        }

        // candidate networks with a Connect button each
        while (auto *item = mAltBox->takeAt(0)) { delete item->widget(); delete item; }
        for (const auto &c : mDiag->candidates()) {
            auto *row = new QWidget(this);
            auto *h = new QHBoxLayout(row); h->setContentsMargins(0, 0, 0, 0);
            const QString label = QStringLiteral("%1 (%2%)").arg(c.ssid).arg(c.strength)
                + (c.passwordOnDisk ? tr(", password saved") : tr(", needs password"));
            h->addWidget(new QLabel(label, row), 1);
            auto *btn = new QPushButton(tr("Connect"), row);
            h->addWidget(btn);
            connect(btn, &QPushButton::clicked, this, [this, c] {
                mModel->activateSavedOnAp(c.connectionPath, c.devicePath, c.apPath);  // §1.3 path
            });
            mAltBox->addWidget(row);
        }
    }

private:
    NetDiagnostics *mDiag;
    NmModel *mModel;
    QTreeWidget *mList;
    QLabel *mVerdict;
    QPushButton *mActionBtn;
    QVBoxLayout *mAltBox;
};
```

Wiring: in `connectioninfo.cpp` add next to the §2.4 banner row —

```cpp
auto *troubleshootBtn = new QPushButton(tr("Troubleshoot…"), this);
row->insertWidget(1, troubleshootBtn);
connect(troubleshootBtn, &QPushButton::clicked, this, [this] {
    auto *dlg = new TroubleshootDialog(mModel, nullptr);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show(); dlg->raise(); dlg->activateWindow();
});
```

Model additions used above (all thin): `const nm::Snapshot &NmModel::cacheSnapshot() const { return mCache.snapshot(); }`, and `activateSavedOnAp(connectionPath, devicePath, apPath)` which is just `ActivateConnection` via the §1.2 async helper with the AP as `specific_object`.

Design notes worth keeping in mind: all probes are async — the dialog never blocks the tray; the fault-isolation logic is ordered so the *first* red row is the diagnosis, and the verdict text spells out the classic triads you asked about (no packets at all → gateway+WAN both red; no DNS → WAN green, DNS red; portal → HTTP red with hijack evidence); and the alternatives stage only advertises "password saved" for profiles it verified are usable without an agent (`psk-flags == 0`), so "try another network" never dead-ends into the §3 secrets trap.

---

## 7. Prioritized roadmap

| Priority | Item | Sections | Effort | Why this order |
|---|---|---|---|---|
| P0 | Keyed-diff `applySection` + stable/bucketed Wi-Fi ordering + order-hold while menu open | 1.1 | ~1 day | Directly fixes "can't click the network I want"; prerequisite for sane notifications and info-dialog tabs |
| P0 | Async actions (drop all blocking `.call()`s), error notifications | 1.2, 1.4 | ~1 day | Removes the 25–35 s freezes; makes every later failure visible |
| P0 | Replace nmcli with `AddAndActivateConnection`; stop trimming; validate WPA length | 3.2 | ~0.5 day | Fixes password persistence for the happy path, kills the argv secret leak and the 30 s stall |
| P1 | Failure-reason watch + re-prompt + `updateSavedPsk` | 3.3 | ~1 day | Un-dead-ends wrong/changed passwords (the core of complaints 1 and 3) |
| P1 | ConnectivityMonitor + banner/tooltip/menu surfacing + tray badge | 2 | ~1 day | The Windows-style internet indicator, foolproof against disabled NM checking |
| P1 | Device pick fix (`state >= 30`, AP as specific object); deactivate-vs-device-disconnect split; live status line | 1.3, 1.5 | ~0.5 day | Small, high-yield correctness |
| P1 | Notification dedup by AC path; HTML escaping | 4.1, 4.2 | ~0.5 day | Visible polish + removes an injection surface |
| P2 | Troubleshoot dialog + diagnostics engine + public-DNS action | 6 | ~2–3 days | New feature; depends on the async helper (P0) |
| P2 | Menu consolidation (drop Recent, Known → "Other saved networks", single info entry point) | 5.1, 5.2 | ~1 day | Redundancy removal, fewer code paths to keep correct |
| P2 | Secret agent | 3.4 | ~1 day | Closes the agent-owned-secrets gap; makes NM drive prompts at the right moment |
| P3 | `operator== = default` dedup; kill per-profile Introspect; strength fast-path; NM service watcher; stats RefreshRateMs; hidden-SSID sentinel; single-instance; tray-availability wait; AutoTz opt-in; hidden-network join | 4.x, 5.3, 5.4 | ~2 days total | Hygiene and robustness batch |

A final observation that ties the whole report together: almost every user-visible symptom traces back to the two structural choices named in §0 — wholesale section resets, and synchronous actions. The P0 row fixes both; everything after it is building features on ground that no longer moves.
