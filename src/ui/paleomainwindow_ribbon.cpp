// 层：视图
// paleomainwindow_ribbon — ribbon 骨架域（方向 83 拆 TU）：buildRibbon（页签/
// 文件菜单/右侧全局按钮组，自本体移入，页快捷键段在 paleomainwindow_shortcuts
// .cpp 的 registerPageShortcuts，调用点保持原行序）+ 面板菜单与右键入口 +
// categoryForPage 查找。
#include "paleomainwindow.h"
#include "paleomainwindow_internal.h"

#include "paleoicons.h"
#include "paleotheme.h"
#include "paleodockmanager.h" // showPanelMenu 的布局菜单出口
#include "dialogs/projectmapsettingsdialog.h"
#include "help/helpsurface.h" // 方向63 帮助面（菜单 + F1 总表挂主窗）
#include "pages/pageshared.h" // kPageIds（页签序）
#include "pages/stratigraphicwebpage.h"
#include "datapreview/datapreviewtabs.h" // mapConfigurationChanged 重开测区标签
#include "../qgis/qgiscanvascontroller.h" // mapConfigurationChanged → zoomToFullExtent
#include "../qgis/qgisprojectservice.h"

#include <qgis.h> // Qgis::version()（方向63 关于框）

#include <QAction>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QMenu>
#include <QPoint>
#include <QPushButton>
#include <QStatusBar>
#include <QTimer>
#include <QToolButton>

using paleo::mainwindow_internal::pageLabels;

void PaleoMainWindow::buildRibbon()
{
  SARibbonBar *bar = ribbonBar();
  if (!bar)
    return;
  bar->setRibbonStyle(SARibbonBar::RibbonStyleCompactThreeRow);
  bar->setPanelSpacing(8);
  bar->setPanelToolButtonIconSize(QSize(16, 16), QSize(24, 24));
  bar->setEnableWordWrap(false);
  bar->setTabDoubleClickToMinimumMode(true); // 双击页签收起/展开 ribbon（Office 惯例）
  if (SARibbonTabBar *tabs = bar->ribbonTabBar())
  {
    tabs->setObjectName(QStringLiteral("workflowTabs"));
    tabs->setAccessibleName(tr("工作流步骤"));
    // T32 tab 溢出策略：超宽走滚动按钮（显式钉住防样式/平台漂移）。
    tabs->setUsesScrollButtons(true);
  }

  // ---- 六个页签 = 六个工作流页（页签序 = paleo::pagesinternal::kPageIds 序）----
  for (int i = 0; i < paleo::pagesinternal::kPageIds.size(); ++i)
  {
    SARibbonCategory *cat = bar->addCategoryPage(pageLabels().at(i));
    cat->setObjectName(QStringLiteral("ribbonCategory.") + paleo::pagesinternal::kPageIds.at(i));
    cat->setProperty("paleo.pageId", paleo::pagesinternal::kPageIds.at(i));
  }
  m_stratigraphicWebPage->buildRibbon(categoryForPage(QStringLiteral("correlation")));
  // W5 键盘可达 + 页循环快捷键（方向63 注册表）——paleomainwindow_shortcuts.cpp。
  registerPageShortcuts();
  connect(bar, &SARibbonBar::currentRibbonTabChanged, this, [this, bar](int idx) {
    SARibbonCategory *cat = bar->categoryByIndex(idx);
    const QString id = cat ? cat->property("paleo.pageId").toString() : QString();
    if (!id.isEmpty() && id != m_currentPage)
      showPage(id);
  });

  // ---- 「文件」应用按钮：工程级动作。起始页按钮是同一批动作的另一入口
  // （点它们的按钮 = 同一条代码路径，offscreen 下同样惰性）。----
  if (auto *appBtn = qobject_cast<QToolButton *>(bar->applicationButton()))
  {
    appBtn->setText(tr("文件"));
    appBtn->setAccessibleName(tr("文件菜单"));
    auto *menu = new SARibbonMenu(appBtn);
    menu->setObjectName(QStringLiteral("fileMenu"));
    const auto viaStartup = [this, menu](const QString &text, const char *iconName,
                                         const char *buttonName) {
      QAction *a = menu->addAction(PaleoIcons::qgisTheme(QLatin1String(iconName)), text);
      connect(a, &QAction::triggered, this, [this, buttonName] {
        if (auto *b = findChild<QPushButton *>(QLatin1String(buttonName)))
          b->click();
      });
    };
    viaStartup(tr("新建工程(&N)…"), "mActionFileNew.svg", "newProjectButton");
    viaStartup(tr("打开工程(&O)…"), "mActionFileOpen.svg", "openProjectButton");
    viaStartup(tr("从工区文件夹新建(&I)…"), "mIconFolderOpen.svg", "importFromFolderButton");
    menu->addSeparator()->setObjectName(QStringLiteral("fileMenuSaveAnchor"));
    auto *mapSettings = menu->addAction(PaleoIcons::qgisTheme(QStringLiteral("mActionMapSettings.svg")), tr("工程坐标与底图…"));
    mapSettings->setObjectName(QStringLiteral("projectMapSettingsAction"));
    connect(menu, &QMenu::aboutToShow, this, [this, mapSettings] {
      mapSettings->setEnabled(m_projectSvc && !m_projectSvc->projectPath().isEmpty() && !m_projectSvc->isOpening());
    });
    connect(mapSettings, &QAction::triggered, this, [this] {
      if (!m_projectSvc || m_projectSvc->projectPath().isEmpty()) return;
      auto *dialog = new ProjectMapSettingsDialog(m_projectSvc->mapConfiguration(), this);
      dialog->setAttribute(Qt::WA_DeleteOnClose);
      const auto session = m_projectSvc->sessionId();
      connect(dialog, &ProjectMapSettingsDialog::saveRequested, this, [this, dialog, session] {
        QString error;
        if (session != m_projectSvc->sessionId()) error = tr("工程已切换，请重新打开设置");
        else if (m_projectSvc->updateMapConfiguration(dialog->configuration(), &error)) {
          dialog->accept();
          statusBar()->showMessage(tr("工程坐标与底图设置已保存"), 5000);
          return;
        }
        dialog->showError(error);
      });
      dialog->open();
    });
    if (m_projectSvc)
      connect(m_projectSvc, &QgisProjectService::mapConfigurationChanged, this, [this] {
        if (m_previewTabs) {
          bool surveyOpen = false;
          for (int i = 0; i < m_previewTabs->tabCount(); ++i)
            surveyOpen |= m_previewTabs->assetIdAt(i) == QLatin1String("survey_area");
          if (surveyOpen) { m_previewTabs->closeAssetTab(QStringLiteral("survey_area")); m_previewTabs->openSurveyArea(); }
        }
        if (m_canvasCtl) QTimer::singleShot(0, this, [this] { m_canvasCtl->zoomToFullExtent(); });
      });
    QAction *home =
        menu->addAction(PaleoIcons::qgisTheme(QStringLiteral("mIconFolderHome.svg")), tr("起始页(&H)"));
    connect(home, &QAction::triggered, this, [this] { showStartup(); });
    menu->addSeparator();
    QAction *quit =
        menu->addAction(PaleoIcons::qgisTheme(QStringLiteral("mActionFileExit.svg")), tr("退出(&X)"));
    connect(quit, &QAction::triggered, this, &QWidget::close);
    appBtn->setMenu(menu);
    appBtn->setPopupMode(QToolButton::InstantPopup);
  }

  // ---- 右侧全局按钮组：[搜索][处理算法][面板][Web 服务] ----
  // 搜索（QgsLocatorWidget）与处理算法依赖服务，attachWorkflows 里补进来。
  SARibbonButtonGroupWidget *right = bar->rightButtonGroup();
  right->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  auto *locatorSlot = new QWidget(right);
  locatorSlot->setObjectName(QStringLiteral("locatorSlot"));
  auto *slotLay = new QHBoxLayout(locatorSlot);
  // SARibbon 原生标题行固定高度；纵向留白会裁切搜索文字，保持零内外留白。
  slotLay->setContentsMargins(0, 0, PaleoTheme::tokens().spacingSm, 0);
  right->addWidget(locatorSlot);

  // 布局管理入口：右键 dock 标题栏使用同一菜单，包含显隐与布局命令。
  auto *panelsBtn = new QToolButton(right);
  panelsBtn->setObjectName(QStringLiteral("panelsMenuButton"));
  panelsBtn->setText(tr("布局"));
  panelsBtn->setAccessibleName(tr("布局与面板管理"));
  panelsBtn->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionShowAllLayers.svg")));
  panelsBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  connect(panelsBtn, &QToolButton::clicked, this, [this, panelsBtn] {
    showPanelMenu(panelsBtn->mapToGlobal(QPoint(0, panelsBtn->height())));
  });
  right->addWidget(panelsBtn);

  // 方向63 帮助面骨架：「帮助」菜单（快捷键总表 / 这是什么？ / 关于）挂在右侧
  // 按钮组；F1、Shift+F1 动作挂在主窗上，菜单收起时同样生效。
  auto *help = new paleo::help::HelpSurface(this);
  help->setAboutDetails({tr("QGIS：%1").arg(Qgis::version())});
  auto *helpBtn = new QToolButton(right);
  helpBtn->setObjectName(QStringLiteral("helpMenuButton"));
  helpBtn->setText(tr("帮助"));
  helpBtn->setAccessibleName(tr("帮助菜单"));
  helpBtn->setToolTip(tr("快捷键总表、「这是什么？」与关于"));
  helpBtn->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionHelpContents.svg")));
  helpBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  helpBtn->setPopupMode(QToolButton::InstantPopup);
  helpBtn->setMenu(help->menu());
  right->addWidget(helpBtn);

  // Web 服务 dock 的 toggleViewAction：dock 标题栏 ✕ 关掉时按钮态跟随。
  if (auto *webDock = findChild<QDockWidget *>(QStringLiteral("webServiceDock")))
  {
    QAction *webAct = webDock->toggleViewAction();
    webAct->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionAddWmsLayer.svg")));
    right->addAction(webAct);
    if (auto *webToggle = qobject_cast<QToolButton *>(right->widgetForAction(webAct)))
    {
      webToggle->setObjectName(QStringLiteral("webServiceButton"));
      webToggle->setAccessibleName(tr("Web 服务面板"));
    }
  }
}

SARibbonCategory *PaleoMainWindow::categoryForPage(const QString &pageId) const
{
  SARibbonBar *bar = ribbonBar();
  return bar ? bar->categoryByObjectName(QStringLiteral("ribbonCategory.") + pageId) : nullptr;
}

void PaleoMainWindow::showPanelMenu(const QPoint &globalPos)
{
  // 每次现建，反映动态注册的面板与已保存的布局。
  if (QMenu *menu = m_dockManager->createMenu(this))
  {
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->addSeparator();
    // 方向64：错误历史入口（主窗口无字面「视图」菜单，此菜单即视图入口）。
    if (m_errorHistoryPanelDock)
    {
      QAction *history = menu->addAction(tr("错误历史"));
      history->setObjectName(QStringLiteral("errorHistoryAction"));
      connect(history, &QAction::triggered, this, &PaleoMainWindow::showErrorHistory);
    }
    QAction *dark = menu->addAction(tr("深色模式"));
    dark->setObjectName(QStringLiteral("themeToggleAction"));
    dark->setCheckable(true);
    dark->setChecked(PaleoTheme::currentTheme() == PaleoTheme::Theme::Dark);
    connect(dark, &QAction::toggled, this, &PaleoMainWindow::setDarkThemeEnabled);
    QAction *compact = menu->addAction(tr("紧凑密度"));
    compact->setObjectName(QStringLiteral("densityToggleAction"));
    compact->setCheckable(true);
    compact->setChecked(PaleoTheme::currentDensity() == PaleoTheme::Density::Compact);
    connect(compact, &QAction::toggled, this,
            &PaleoMainWindow::setCompactDensityEnabled);
    menu->popup(globalPos);
  }
}

void PaleoMainWindow::contextMenuEvent(QContextMenuEvent *event)
{
  // 命中件祖先链上有 QDockWidget 且不落在其内容子树里（标题栏/边距）
  // → 弹面板管理菜单；dock 内容件（图层树、表格…）与画布各有自己的
  // 右键语义，一路放行。
  QWidget *hit = childAt(event->pos());
  if (!hit) // 停靠区空白边距
  {
    showPanelMenu(event->globalPos());
    return;
  }
  for (QWidget *w = hit; w; w = w->parentWidget())
  {
    if (auto *dock = qobject_cast<QDockWidget *>(w))
    {
      if (dock->widget() && dock->widget()->isAncestorOf(hit))
        break;
      showPanelMenu(event->globalPos());
      return;
    }
  }
  SARibbonMainWindow::contextMenuEvent(event);
}
