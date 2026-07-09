#include "troubleshootdialog.h"

#include "backend/connectivity_monitor.h"
#include "backend/nm_actions.h"
#include "nmmodel.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace
{
QString connectivityName(uint v)
{
    switch (v) {
    case 1: return QObject::tr("none");
    case 2: return QObject::tr("portal");
    case 3: return QObject::tr("limited");
    case 4: return QObject::tr("full");
    default: return QObject::tr("unknown");
    }
}

bool sameSsid(const nm::SavedConnectionRecord &conn, const nm::AccessPointRecord &ap)
{
    if (!conn.wifiSsidBytes.isEmpty()) {
        return conn.wifiSsidBytes == ap.ssidBytes;
    }
    return (!conn.wifiSsid.isEmpty() && conn.wifiSsid == ap.ssid) || conn.id == ap.ssid;
}
}

NetDiagnostics::NetDiagnostics(NmModel *model, QObject *parent)
    : QObject(parent)
    , mModel(model)
{
    mResults = {
        { tr("NetworkManager state") },
        { tr("Link") },
        { tr("IP configuration") },
        { tr("Default route") },
        { tr("DNS servers") },
        { tr("HTTP / captive portal") },
        { tr("NetworkManager connectivity verdict") },
        { tr("Alternative saved networks") },
    };
}

void NetDiagnostics::run()
{
    for (auto &r : mResults) {
        r.detail.clear();
        r.verdict = DiagResult::Verdict::Pending;
    }
    mCandidates.clear();
    mVerdict.clear();
    mAction.clear();

    const auto state = mModel->managerState();
    const auto &snap = mModel->cacheSnapshot();
    const QString primary = mModel->primaryPhysicalConnectionPath();
    const auto acIt = snap.activeConnections.find(primary);

    if (!state.networkingEnabled || (state.primaryKind == NmModel::PrimaryKind::Wifi && !state.wirelessEnabled)) {
        set(StNmState, DiagResult::Verdict::Fail, tr("networking or Wi-Fi is disabled"));
        set(StLink, DiagResult::Verdict::Skipped);
        set(StIpConfig, DiagResult::Verdict::Skipped);
        set(StRoute, DiagResult::Verdict::Skipped);
        set(StDns, DiagResult::Verdict::Skipped);
        set(StHttpPortal, DiagResult::Verdict::Skipped);
        set(StNmVerdict, DiagResult::Verdict::Skipped);
        finish();
        return;
    }
    set(StNmState, DiagResult::Verdict::Pass, tr("state %1").arg(state.rawNmState));

    if (acIt == snap.activeConnections.end()) {
        set(StLink, DiagResult::Verdict::Fail, tr("no active physical connection"));
        set(StIpConfig, DiagResult::Verdict::Skipped);
        set(StRoute, DiagResult::Verdict::Skipped);
        set(StDns, DiagResult::Verdict::Skipped);
        set(StHttpPortal, DiagResult::Verdict::Skipped);
        set(StNmVerdict, DiagResult::Verdict::Skipped);
    } else {
        set(StLink, DiagResult::Verdict::Pass, acIt->id);
        const bool haveIp = !acIt->ip4Addresses.isEmpty() || !acIt->ip6Addresses.isEmpty();
        set(StIpConfig, haveIp ? DiagResult::Verdict::Pass : DiagResult::Verdict::Fail,
            haveIp ? (acIt->ip4Addresses + acIt->ip6Addresses).join(QStringLiteral(", ")) : tr("no IP address"));
        const bool haveRoute = !acIt->ip4Gateway.isEmpty() || !acIt->ip6Gateway.isEmpty() || acIt->isDefault4 || acIt->isDefault6;
        set(StRoute, haveRoute ? DiagResult::Verdict::Pass : DiagResult::Verdict::Fail,
            haveRoute ? tr("default route present") : tr("no default route"));
        const QStringList dns = acIt->ip4Dns + acIt->ip6Dns;
        set(StDns, dns.isEmpty() ? DiagResult::Verdict::Fail : DiagResult::Verdict::Pass,
            dns.isEmpty() ? tr("no DNS servers") : dns.join(QStringLiteral(", ")));
    }

    set(StHttpPortal, DiagResult::Verdict::Running, tr("probing"));
    auto *nam = new QNetworkAccessManager(this);
    QUrl probe(state.connectivityCheckUri);
    if (!probe.isValid() || probe.isEmpty()) {
        probe = QUrl(QStringLiteral("http://connectivity-check.ubuntu.com/"));
    }
    QNetworkRequest req(probe);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    req.setTransferTimeout(5000);
    QNetworkReply *reply = nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, nam, state, snap] {
        reply->deleteLater();
        nam->deleteLater();
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool genuine = reply->error() == QNetworkReply::NoError
            && (http == 204 || reply->rawHeader("X-NetworkManager-Status").trimmed() == "online");
        if (genuine) {
            set(StHttpPortal, DiagResult::Verdict::Pass, tr("expected reply received"));
        } else if (http >= 200 && http < 400) {
            set(StHttpPortal, DiagResult::Verdict::Fail, tr("captive portal or hijacked reply"));
            mAction = QStringLiteral("portal");
        } else {
            set(StHttpPortal, DiagResult::Verdict::Fail, reply->errorString());
        }

        set(StNmVerdict,
            state.rawConnectivity == 4 ? DiagResult::Verdict::Pass
                                       : (state.rawConnectivity == 0 ? DiagResult::Verdict::Skipped : DiagResult::Verdict::Fail),
            connectivityName(state.rawConnectivity));

        for (const auto &ap : snap.accessPoints) {
            if (ap.ssid.isEmpty() || ap.ssid == state.primaryName || ap.strength < 25) {
                continue;
            }
            const nm::SavedConnectionRecord *best = nullptr;
            for (const auto &conn : snap.savedConnections) {
                if (!nm::isWirelessType(conn.type) || !sameSsid(conn, ap)) {
                    continue;
                }
                if (best == nullptr || conn.timestamp > best->timestamp) {
                    best = &conn;
                }
            }
            if (best == nullptr) {
                continue;
            }
            WifiCandidate c;
            c.ssid = ap.ssid;
            c.connectionPath = best->path;
            c.devicePath = ap.devicePath;
            c.apPath = ap.path;
            c.strength = ap.strength;
            c.passwordOnDisk = true;
            mCandidates.push_back(c);
        }
        set(StAlternatives,
            mCandidates.isEmpty() ? DiagResult::Verdict::Skipped : DiagResult::Verdict::Pass,
            mCandidates.isEmpty() ? tr("none in range") : tr("%n saved network(s) in range", nullptr, mCandidates.size()));

        finish();
    });
}

void NetDiagnostics::set(Stage stage, DiagResult::Verdict verdict, const QString &detail)
{
    mResults[stage].verdict = verdict;
    mResults[stage].detail = detail;
    emit stageUpdated(stage);
}

void NetDiagnostics::finish()
{
    using V = DiagResult::Verdict;
    if (mResults[StNmState].verdict == V::Fail) {
        mVerdict = tr("Networking or Wi-Fi is switched off. Enable it and run the check again.");
    } else if (mResults[StLink].verdict == V::Fail) {
        mVerdict = tr("There is no active physical connection. Try another Wi-Fi network or check the cable.");
        mAction = QStringLiteral("switch");
    } else if (mResults[StIpConfig].verdict == V::Fail) {
        mVerdict = tr("The network did not provide an IP address. This can be a wrong Wi-Fi password, DHCP failure, or router issue.");
        mAction = QStringLiteral("switch");
    } else if (mResults[StRoute].verdict == V::Fail) {
        mVerdict = tr("The connection has no default route, so packets cannot leave the local network.");
    } else if (mResults[StDns].verdict == V::Fail) {
        mVerdict = tr("The connection has no DNS servers. Using public DNS on this connection may help.");
        mAction = QStringLiteral("set-dns");
    } else if (mResults[StHttpPortal].detail.contains(QStringLiteral("portal"), Qt::CaseInsensitive)) {
        mVerdict = tr("A captive portal appears to be intercepting traffic. Sign in first.");
        mAction = QStringLiteral("portal");
    } else if (mResults[StNmVerdict].verdict == V::Fail) {
        mVerdict = tr("NetworkManager reports limited connectivity even though the local connection exists.");
    } else {
        mVerdict = tr("No obvious faults found. The connection looks healthy.");
    }
    emit finished();
}

TroubleshootDialog::TroubleshootDialog(NmModel *model, QWidget *parent)
    : QDialog(parent)
    , mDiag(new NetDiagnostics(model, this))
    , mModel(model)
{
    setWindowTitle(tr("Troubleshoot connection"));
    resize(600, 480);

    auto *lay = new QVBoxLayout(this);
    mList = new QTreeWidget(this);
    mList->setHeaderHidden(true);
    mList->setColumnCount(1);
    mList->setRootIsDecorated(false);
    for (const auto &result : mDiag->results()) {
        auto *it = new QTreeWidgetItem(mList);
        it->setText(0, result.title);
    }
    lay->addWidget(mList, 1);

    mVerdict = new QLabel(this);
    mVerdict->setWordWrap(true);
    mVerdict->setTextFormat(Qt::PlainText);
    lay->addWidget(mVerdict);

    mAltBox = new QVBoxLayout;
    lay->addLayout(mAltBox);

    auto *buttons = new QHBoxLayout;
    mActionBtn = new QPushButton(this);
    mActionBtn->hide();
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

void TroubleshootDialog::paintStage(int stage)
{
    const auto &r = mDiag->results().at(stage);
    const char *icon = "";
    switch (r.verdict) {
    case DiagResult::Verdict::Pass: icon = "emblem-default"; break;
    case DiagResult::Verdict::Fail: icon = "dialog-error"; break;
    case DiagResult::Verdict::Running: icon = "view-refresh"; break;
    case DiagResult::Verdict::Skipped: icon = "dialog-question"; break;
    case DiagResult::Verdict::Pending: break;
    }
    auto *it = mList->topLevelItem(stage);
    it->setIcon(0, QIcon::fromTheme(QLatin1String(icon)));
    it->setText(0, r.detail.isEmpty() ? r.title : r.title + QStringLiteral(" — ") + r.detail);
}

void TroubleshootDialog::paintVerdict()
{
    mVerdict->setText(mDiag->verdictText());

    const QString act = mDiag->suggestedActionId();
    mActionBtn->setVisible(act == QLatin1String("portal") || act == QLatin1String("set-dns"));
    mActionBtn->disconnect();
    if (act == QLatin1String("portal")) {
        mActionBtn->setText(tr("Open sign-in page"));
        connect(mActionBtn, &QPushButton::clicked, this, [] {
            QDesktopServices::openUrl(QUrl(QStringLiteral("http://connectivity-check.ubuntu.com/")));
        });
    } else if (act == QLatin1String("set-dns")) {
        mActionBtn->setText(tr("Use public DNS on this connection"));
        connect(mActionBtn, &QPushButton::clicked, this, [this] {
            const auto &snap = mModel->cacheSnapshot();
            const QString acPath = mModel->primaryPhysicalConnectionPath();
            const auto acIt = snap.activeConnections.find(acPath);
            if (acIt != snap.activeConnections.end()) {
                nm::NmActions::applyPublicDns(acIt->connectionPath, acIt->path, this, [](bool, const QString &, const QDBusMessage &) {});
            }
        });
    }

    while (auto *item = mAltBox->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    for (const auto &c : mDiag->candidates()) {
        auto *row = new QWidget(this);
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->addWidget(new QLabel(QStringLiteral("%1 (%2%)").arg(c.ssid).arg(c.strength), row), 1);
        auto *btn = new QPushButton(tr("Connect"), row);
        h->addWidget(btn);
        connect(btn, &QPushButton::clicked, this, [this, c] {
            mModel->activateSavedOnAp(c.connectionPath, c.devicePath, c.apPath);
        });
        mAltBox->addWidget(row);
    }
}
