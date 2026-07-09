/*COPYRIGHT_HEADER

This file is a part of nm-tray.

Copyright (c)
    2016~now Palo Kisa <palo.kisa@gmail.com>

nm-tray is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

COPYRIGHT_HEADER*/

#include "windowmenu.h"
#include "nmmodel.h"
#include "nmproxy.h"
#include "menuview.h"
#include "backend/connectivity_monitor.h"

#include <QWidgetAction>
#include <QPushButton>
#include <QLabel>
#include <QAbstractItemModel>
#include <QAbstractProxyModel>
#include <functional>
#include <QTimer>

class WindowMenuPrivate
{
public:
    NmModel * mNmModel;
    nm::ConnectivityMonitor *mConnectivity = nullptr;

    QScopedPointer<NmProxy> mWirelessModel;
    QWidgetAction * mWirelessAction;

    QScopedPointer<NmProxy> mConnectionModel;
    QWidgetAction * mConnectionAction;

    QAction * mMakeDirtyAction;
    QTimer mDelaySizeRefreshTimer;

    WindowMenuPrivate(WindowMenu * q);
    template <typename F>
    void onActivated(QModelIndex const & index
            , QAbstractItemModel const * topParent
            , F const & functor);
    void forceSizeRefresh();
    void onViewRowChange(QAction * viewAction, QAbstractItemModel const * model);
private:
    WindowMenu * q_ptr;
    Q_DECLARE_PUBLIC(WindowMenu);
};

WindowMenuPrivate::WindowMenuPrivate(WindowMenu * q)
    : q_ptr{q}
{
}

template <typename F>
void WindowMenuPrivate::onActivated(QModelIndex const & index
        , QAbstractItemModel const * topParent
        , F const & functor)
{
    QModelIndex i = index;
    for (QAbstractProxyModel const * proxy = qobject_cast<QAbstractProxyModel const *>(index.model())
            ; nullptr != proxy && topParent != proxy
            ; proxy = qobject_cast<QAbstractProxyModel const *>(proxy->sourceModel())
            )
    {
        i = proxy->mapToSource(i);
    }
    functor(i);
}

void WindowMenuPrivate::forceSizeRefresh()
{
    Q_Q(WindowMenu);
    if (!q->isVisible())
    {
        return;
    }

    const QSize old_size = q->size();
    //TODO: how to force the menu to recalculate it's size in a more elegant way?
    q->addAction(mMakeDirtyAction);
    q->removeAction(mMakeDirtyAction);
    // ensure to be visible (should the resize make it out of screen)
    Q_UNUSED(old_size);
}

void WindowMenuPrivate::onViewRowChange(QAction * viewAction, QAbstractItemModel const * model)
{
    viewAction->setVisible(model->rowCount(QModelIndex{}) > 0);
    mDelaySizeRefreshTimer.start();
}




WindowMenu::WindowMenu(NmModel * nmModel, nm::ConnectivityMonitor *connectivity, QWidget * parent /*= nullptr*/)
    : QMenu{parent}
    , d_ptr{new WindowMenuPrivate{this}}
{
    Q_D(WindowMenu);
    d->mNmModel = nmModel;
    d->mConnectivity = connectivity;
    d->mNmModel->setOrderHold(true);
    connect(this, &QMenu::aboutToHide, this, [d] {
        d->mNmModel->setOrderHold(false);
    });
    d->mNmModel->requestAllWifiScan();

    auto *statusButton = new QPushButton();
    statusButton->setFlat(true);
    statusButton->setCursor(Qt::PointingHandCursor);
    auto refreshStatus = [this, d, statusButton] {
        const NmModel::ManagerState state = d->mNmModel->managerState();
        bool hasPrimaryConnection = false;
        QString statusText = tr("No active connection");
        if (!state.primaryName.isEmpty()) {
            hasPrimaryConnection = true;
            statusText = tr("Connected: %1").arg(state.primaryName);
            if (state.wifiStrength >= 0) {
                statusText += tr(" (%1%)").arg(state.wifiStrength);
            }
        }
        statusButton->setText(statusText);
        statusButton->setEnabled(hasPrimaryConnection);
        statusButton->setToolTip(hasPrimaryConnection
                ? tr("Open connection information")
                : tr("No active connection"));
    };
    refreshStatus();
    connect(d->mNmModel, &NmModel::managerStateChanged, statusButton, refreshStatus);
    auto *statusAction = new QWidgetAction(this);
    statusAction->setDefaultWidget(statusButton);
    addAction(statusAction);
    connect(statusButton, &QPushButton::clicked, this, [this, d] {
        Q_UNUSED(d);
        emit requestConnectionInfo();
    });
    auto *internetLabel = new QLabel(this);
    internetLabel->setMargin(4);
    auto refreshInternet = [d, internetLabel] {
        internetLabel->setText(d->mConnectivity ? d->mConnectivity->statusText() : WindowMenu::tr("Internet access unknown"));
    };
    refreshInternet();
    if (d->mConnectivity != nullptr) {
        connect(d->mConnectivity, &nm::ConnectivityMonitor::statusChanged, internetLabel, [refreshInternet] { refreshInternet(); });
    }
    auto *internetAction = new QWidgetAction(this);
    internetAction->setDefaultWidget(internetLabel);
    addAction(internetAction);
    QAction *disconnectAction = addAction(tr("Disconnect"));
    disconnectAction->setToolTip(tr("Disconnect this connection and allow NetworkManager to choose another one"));
    connect(disconnectAction, &QAction::triggered, d->mNmModel, &NmModel::disconnectPrimaryConnection);
    QAction *stayOffAction = addAction(tr("Disconnect and stay offline"));
    stayOffAction->setToolTip(tr("Disconnect this device and prevent automatic reconnect until you choose a network"));
    connect(stayOffAction, &QAction::triggered, d->mNmModel, &NmModel::disconnectPrimaryConnectionAndStayOff);
    addSeparator();

    //wireless proxy & widgets
    d->mWirelessModel.reset(new NmProxy);
    d->mWirelessModel->setNmModel(d->mNmModel, NmModel::WifiNetworkType);
    MenuView * wifi_view = new MenuView{d->mWirelessModel.data()};
    connect(wifi_view, &MenuView::activatedNoMiddleRight, [this, d] (const QModelIndex & index) {
        d->onActivated(index, d->mWirelessModel.data(), std::bind(&NmProxy::activateConnection, d->mWirelessModel.data(), std::placeholders::_1));
        close();
    });

    d->mWirelessAction = new QWidgetAction{this};
    d->mWirelessAction->setDefaultWidget(wifi_view);
    connect(d->mWirelessModel.data(), &QAbstractItemModel::modelReset, [d] { d->onViewRowChange(d->mWirelessAction, d->mWirelessModel.data()); });
    connect(d->mWirelessModel.data(), &QAbstractItemModel::rowsInserted, [d] { d->onViewRowChange(d->mWirelessAction, d->mWirelessModel.data()); });
    connect(d->mWirelessModel.data(), &QAbstractItemModel::rowsRemoved, [d] { d->onViewRowChange(d->mWirelessAction, d->mWirelessModel.data()); });

    //connection proxy & widgets: used only for "Other saved networks"
    d->mConnectionModel.reset(new NmProxy);
    d->mConnectionModel->setNmModel(d->mNmModel, NmModel::ConnectionType);
    MenuView * connection_view = new MenuView{d->mConnectionModel.data()};
    connect(connection_view, &MenuView::activatedNoMiddleRight, [this, d] (const QModelIndex & index) {
        d->onActivated(index, d->mConnectionModel.data(), std::bind(&NmProxy::activateConnection, d->mConnectionModel.data(), std::placeholders::_1));
        close();
    });

    d->mConnectionAction = new QWidgetAction{this};
    d->mConnectionAction->setDefaultWidget(connection_view);
    connect(d->mConnectionModel.data(), &QAbstractItemModel::modelReset, [d] { d->onViewRowChange(d->mConnectionAction, d->mConnectionModel.data()); });
    connect(d->mConnectionModel.data(), &QAbstractItemModel::rowsInserted, [d] { d->onViewRowChange(d->mConnectionAction, d->mConnectionModel.data()); });
    connect(d->mConnectionModel.data(), &QAbstractItemModel::rowsRemoved, [d] { d->onViewRowChange(d->mConnectionAction, d->mConnectionModel.data()); });

    addAction(tr("Wi-Fi network(s)"))->setEnabled(false);
    addAction(d->mWirelessAction);
    QAction *otherSaved = addAction(tr("Other saved networks"));
    otherSaved->setEnabled(false);
    addAction(d->mConnectionAction);
    QAction *hiddenAction = addAction(tr("Connect to hidden network…"));
    connect(hiddenAction, &QAction::triggered, d->mNmModel, &NmModel::promptAndCreateHiddenWifiConnection);
    addSeparator();
    auto *toggleButton = new QPushButton(d->mNmModel->showLowSignalNetworks()
            ? tr("Hide low signal networks")
            : tr("Show low signal networks"));
    toggleButton->setFlat(true);
    toggleButton->setCursor(Qt::PointingHandCursor);
    toggleButton->setToolTip(tr("Toggle visibility of weak Wi-Fi networks"));
    auto *toggleAction = new QWidgetAction(this);
    toggleAction->setDefaultWidget(toggleButton);
    addAction(toggleAction);
    connect(toggleButton, &QPushButton::clicked, this, [d] {
        d->mNmModel->setShowLowSignalNetworks(!d->mNmModel->showLowSignalNetworks());
    });
    connect(d->mNmModel, &QAbstractItemModel::modelReset, toggleButton, [d, toggleButton] {
        toggleButton->setText(d->mNmModel->showLowSignalNetworks()
                ? WindowMenu::tr("Hide low signal networks")
                : WindowMenu::tr("Show low signal networks"));
    });

    d->mMakeDirtyAction = new QAction{this};
    d->mDelaySizeRefreshTimer.setInterval(200);
    d->mDelaySizeRefreshTimer.setSingleShot(true);
    connect(&d->mDelaySizeRefreshTimer, &QTimer::timeout, [d] { d->forceSizeRefresh(); });

    d->onViewRowChange(d->mWirelessAction, d->mWirelessModel.data());
    d->onViewRowChange(d->mConnectionAction, d->mConnectionModel.data());
}

WindowMenu::~WindowMenu()
{
}
