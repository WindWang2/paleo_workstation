// 层：视图
// paleomainwindow_errorhub — 错误呈现域（方向 64，方向 83 自成 TU）：错误历史
// dock（buildPageDocks 调用）、状态栏错误胶囊与 ErrorHub 计数同步
// （buildStatusBar 调用）、attachErrorHub/showErrorHistory 装配入口（自
// paleomainwindow_attach_shell.cpp 移入——ErrorHub 装配面独立可测）。
// ErrorHub 由 AppContext 持有并 installGlobal；这里只把呈现层与历史面板挂到
// 本窗口（视图层不持有服务生命周期）。
#include "paleomainwindow.h"

#include "notifications/errorhistorypanel.h"
#include "notifications/notificationcenter.h"
#include "../services/errorhub.h"

#include <QStatusBar>
#include <QToolButton>

// 方向76 删除了旧的 ErrorHistoryDock。错误历史只剩 attachErrorHub 挂上的
// ErrorHistoryPanel。这个入口留着，是为了 buildPageDocks 的调用序不变。
void PaleoMainWindow::buildErrorHistoryDock()
{
}

// 状态栏错误胶囊（点击唤出错误历史面板）+ ErrorHub 计数同步。原 buildShell
// 的两段（状态钮 960-975 / hub 同步 1013-1029）合并入本函数：两段间原有的
// 工程打开进度接线（buildStatusBar 尾段）涉及对象互不相同，连接序变化无
// 行为影响（记 ledger/PR body）。
void PaleoMainWindow::wireErrorHubStatus()
{
  // 错误历史状态栏胶囊按钮
  m_statusErrorBtn = new QToolButton(this);
  m_statusErrorBtn->setObjectName(QStringLiteral("statusErrorHistoryButton"));
  m_statusErrorBtn->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionHistory.svg")));
  m_statusErrorBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  m_statusErrorBtn->setAutoRaise(true);
  m_statusErrorBtn->setCursor(Qt::PointingHandCursor);
  m_statusErrorBtn->setVisible(false);
  connect(m_statusErrorBtn, &QToolButton::clicked, this, [this] {
    if (m_errorHistoryPanelDock) {
      m_errorHistoryPanelDock->show();
      m_errorHistoryPanelDock->raise();
      m_errorHistoryPanelDock->activateWindow();
    }
  });
  statusBar()->addPermanentWidget(m_statusErrorBtn);

  // 方向76：徽标与错误历史同源，订阅全局 ErrorHub。未装配时 global()
  // 为空，徽标保持隐藏。计数是环形历史条数。
  if (ErrorHub *hub = ErrorHub::global()) {
    const auto syncErrorStatus = [this, hub] {
      if (!m_statusErrorBtn)
        return;
      const int count = hub->size();
      if (count <= 0) {
        m_statusErrorBtn->setVisible(false);
      } else {
        m_statusErrorBtn->setVisible(true);
        m_statusErrorBtn->setText(QString::number(count));
        m_statusErrorBtn->setToolTip(tr("错误与警告历史（共 %1 条记录），点击查看").arg(count));
      }
    };
    connect(hub, &ErrorHub::errorRaised, this,
            [syncErrorStatus](const ErrorHub::Entry &, bool) { syncErrorStatus(); });
    connect(hub, &ErrorHub::historyCleared, this, syncErrorStatus);
    syncErrorStatus();
  }
}

QAction *PaleoMainWindow::errorHistoryAction() const
{
  return m_errorHistoryPanelDock ? m_errorHistoryPanelDock->toggleViewAction() : nullptr;
}

// 方向64：错误呈现接线。幂等（已接则忽略）。
void PaleoMainWindow::attachErrorHub(ErrorHub *hub)
{
  if (!hub || m_notifications)
    return;
  m_notifications = new NotificationCenter(this, hub);
  m_notifications->setStatusBar(statusBar());
  auto *panel = new ErrorHistoryPanel(hub, this);
  m_errorHistoryPanelDock = new PaleoDockWidget(tr("错误历史"), this);
  m_errorHistoryPanelDock->setObjectName(QStringLiteral("errorHistoryDock"));
  m_errorHistoryPanelDock->setWidget(panel);
  addDockWidget(Qt::BottomDockWidgetArea, m_errorHistoryPanelDock);
  m_errorHistoryPanelDock->hide(); // 按需唤出（布局与面板菜单 / showErrorHistory）
}

void PaleoMainWindow::showErrorHistory()
{
  if (!m_errorHistoryPanelDock)
    return;
  m_errorHistoryPanelDock->show();
  m_errorHistoryPanelDock->raise();
}
