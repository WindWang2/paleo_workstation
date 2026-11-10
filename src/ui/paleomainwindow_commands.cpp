// 层：视图
// paleomainwindow_commands — UI 重设计 U1 的命令宿主域：统一 CommandDispatcher 的
// 创建与宿主上下文，以及工程级命令（文件菜单 / 快速访问 / 起始页 / 布局右键菜单 /
// Ctrl+1..N）的处理器。Ribbon 面板命令在 ribbonpanels.cpp 建动作的同一处绑定。
// 原先文件菜单经 findChild<QPushButton>()->click() 转调起始页/数据页按钮，现在
// 两个入口调同一条命令（处理器即原按钮的实现）。
#include "paleomainwindow.h"

#include "commandactionbinder.h"
#include "uienv_internal.h"
#include "dialogs/projectmapsettingsdialog.h"
#include "datapreview/datapreviewtabs.h"
#include "pages/pageshared.h"
#include "paleotheme.h"
#include "../workflow/commanddispatcher.h"
#include "../workflow/projectsetup.h"
#include "../linkage/selectioncontext.h"
#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/qgisprojectservice.h"
#include "../qgis/onlinebasemap.h"

#include <QCoreApplication>
#include <QFileDialog>
#include <QPointer>
#include <QStatusBar>
#include <QTranslator>

using paleo::command::CommandContext;
using paleo::command::CommandDispatcher;
using paleo::command::CommandState;
using paleo::ui_detail::isOffscreen;

CommandDispatcher *PaleoMainWindow::commandDispatcher()
{
  if (m_commands)
    return m_commands;
  m_commands = new CommandDispatcher(this);
  m_commands->setObjectName(QStringLiteral("paleoCommandDispatcher"));
  // 宿主上下文：Agent 不可伪造——callerScope 之外的字段只从这里来。
  m_commands->setContextProvider([this] {
    CommandContext ctx;
    ctx.documentId = m_projectSvc ? m_projectSvc->projectPath() : QString();
    ctx.activeViewId = m_currentPage;
    ctx.horizon = m_selection ? m_selection->activeHorizon() : QString();
    ctx.callerScope = QStringLiteral("ui");
    return ctx;
  });
  m_commandBinder = new CommandActionBinder(m_commands, this);
  bindProjectCommands();
  return m_commands;
}

CommandActionBinder *PaleoMainWindow::commandBinder()
{
  commandDispatcher();
  return m_commandBinder;
}

void PaleoMainWindow::bindCommand(const QString &commandId, std::function<void(const QVariantMap &)> handler,
                                  std::function<QString()> disabledReason)
{
  CommandDispatcher::StateProvider state;
  if (disabledReason)
    state = [disabledReason](const CommandContext &) {
      CommandState st;
      st.reason = disabledReason();
      st.enabled = st.reason.isEmpty();
      return st;
    };
  commandDispatcher()->bind(
      commandId, [handler](const CommandContext &, const QVariantMap &params) { handler(params); }, state);
}

// 处理器型动作：处理器在分派器里，状态跟随动作本身（禁用 + tooltip 原因）——
// Ctrl+K / Agent 看到的可用性与 ribbon 按钮一致，不会把禁用的列成可用。
QAction *PaleoMainWindow::bindActionCommand(QAction *action, const QString &commandId,
                                            std::function<void(const QVariantMap &)> handler)
{
  bindCommand(commandId, std::move(handler), [a = QPointer<QAction>(action)]() -> QString {
    return !a || a->isEnabled() ? QString() : (a->toolTip().isEmpty() ? a->text() : a->toolTip());
  });
  return routeCommand(action, commandId);
}

QAction *PaleoMainWindow::routeCommand(QAction *action, const QString &commandId, const QVariantMap &params)
{
  return commandBinder()->route(action, commandId, params);
}

bool PaleoMainWindow::triggerCommand(const QString &commandId, const QVariantMap &params)
{
  CommandDispatcher *d = commandDispatcher();
  return d->trigger(commandId, d->currentContext(), params);
}

void PaleoMainWindow::promptOpenProject()
{
  if (isOffscreen() || !m_projectSvc)
    return;
  const QString p = QFileDialog::getOpenFileName(this, tr("打开工程"), QString(),
                                                 tr("Paleo 工程 (*.paleo);;QGIS 工程 (*.qgz *.qgs)"));
  if (p.isEmpty())
    return;
  // openPath checks unsaved changes once before starting the switch.
  openPath(p);
}

// PROJECT_FILE_DESIGN：从工区文件夹新建——选目录后
//   已有 project.paleo → 直接打开（幂等，不重建）；
//   已有 *.qgz → 打开收养（openProject 自动写 project.paleo）；
//   否则     → 在目录内建 <目录名>.qgz + project.paleo，再跑工区导入。
void PaleoMainWindow::promptNewProjectFromFolder()
{
  if (isOffscreen() || !m_projectSvc)
    return;
  const QString dir = QFileDialog::getExistingDirectory(this, tr("从工区文件夹新建工程"));
  if (dir.isEmpty())
    return;
  if (ProjectSetupWorkflow::containsProject(dir))
    openPath(dir); // 脏状态检查在 openPath 内（#282）
  else
    runNewProject(dir);
}

void PaleoMainWindow::applyOnlineBasemapPreset(const QString &presetId)
{
  for (auto *preview : findChildren<DataPreviewTabs *>())
    if (preview->applyBasemapToCurrentMap(presetId))
      return;
  if (!m_projectSvc || m_projectSvc->isOpening() || m_projectSvc->projectPath().isEmpty())
    return;
  QString error;
  auto *layer = paleo::onlinebasemap::addPresetToProject(m_projectSvc->project(), presetId, &error);
  if (!layer)
  {
    PaleoNotify::warning(this, tr("在线底图未加载"), error);
    return;
  }
  statusBar()->showMessage(tr("在线底图已加入图层树，正在按当前视图范围加载"), 5000);
  if (m_canvasCtl)
  {
    QPointer<PaleoMainWindow> guard(this);
    paleo::onlinebasemap::watchFailures(m_canvasCtl->canvas(), layer, 15000, [guard](const QString &message) {
      if (guard)
        guard->statusBar()->showMessage(message, 10000);
    });
  }
}

void PaleoMainWindow::bindProjectCommands()
{
  // 工程打开途中起始页三钮被置灰（paleomainwindow_status.cpp）；文件菜单入口同一状态。
  const auto notOpening = [this]() -> QString {
    return m_projectSvc && m_projectSvc->isOpening() ? tr("工程正在打开，请稍候") : QString();
  };
  const auto needProject = [this]() -> QString {
    if (!m_projectSvc || m_projectSvc->projectPath().isEmpty())
      return tr("请先打开工程");
    return m_projectSvc->isOpening() ? tr("工程正在打开，请稍候") : QString();
  };
  bindCommand(QStringLiteral("project.new"), [this](const QVariantMap &) {
    if (!isOffscreen() && m_projectSvc)
      runNewProject();
  }, notOpening);
  bindCommand(QStringLiteral("project.open"), [this](const QVariantMap &) { promptOpenProject(); }, notOpening);
  bindCommand(QStringLiteral("project.newFromFolder"),
              [this](const QVariantMap &) { promptNewProjectFromFolder(); }, notOpening);
  bindCommand(QStringLiteral("project.sources"), [this](const QVariantMap &) { runNewProject({}, true); },
              [this, needProject]() -> QString {
                const QString why = needProject();
                return why.isEmpty() && isProjectReadOnly() ? tr("工程处于只读模式") : why;
              });
  bindCommand(QStringLiteral("project.mapSettings"), [this](const QVariantMap &) {
    if (!m_projectSvc || m_projectSvc->projectPath().isEmpty())
      return;
    auto *dialog = new ProjectMapSettingsDialog(m_projectSvc->mapConfiguration(), this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    const auto session = m_projectSvc->sessionId();
    connect(dialog, &ProjectMapSettingsDialog::saveRequested, this, [this, dialog, session] {
      QString error;
      if (session != m_projectSvc->sessionId())
        error = tr("工程已切换，请重新打开设置");
      else if (m_projectSvc->updateMapConfiguration(dialog->configuration(), &error))
      {
        dialog->accept();
        statusBar()->showMessage(tr("工程坐标与底图设置已保存"), 5000);
        return;
      }
      dialog->showError(error);
    });
    dialog->open();
  }, needProject);
  bindCommand(QStringLiteral("project.basemap.online"), [this](const QVariantMap &p) {
    applyOnlineBasemapPreset(p.value(QStringLiteral("presetId")).toString());
  });
  bindCommand(QStringLiteral("project.home"), [this](const QVariantMap &) { showStartup(); });
  bindCommand(QStringLiteral("app.quit"), [this](const QVariantMap &) { close(); });

  // Ctrl+1..N：页签切换（键序登记在 shortcuts/shortcutcatalog main.page.<id>）。
  for (const QString &page : paleo::pagesinternal::kRibbonIds)
    bindCommand(QStringLiteral("view.stage.") + page, [this, page](const QVariantMap &) {
      ribbonBar()->raiseCategory(categoryForPage(page));
    });

  // 布局右键菜单 / 「布局」按钮菜单（showPanelMenu 每次现建动作，按 commandId 路由）。
  bindCommand(QStringLiteral("view.errorHistory"), [this](const QVariantMap &) { showErrorHistory(); },
              [this]() -> QString { return m_errorHistoryPanelDock ? QString() : tr("错误历史面板未就绪"); });
  bindCommand(QStringLiteral("view.theme.dark"), [this](const QVariantMap &p) {
    setDarkThemeEnabled(CommandActionBinder::targetChecked(p, PaleoTheme::currentTheme() == PaleoTheme::Theme::Dark));
  });
  bindCommand(QStringLiteral("view.density.compact"), [this](const QVariantMap &p) {
    setCompactDensityEnabled(
        CommandActionBinder::targetChecked(p, PaleoTheme::currentDensity() == PaleoTheme::Density::Compact));
  });
}

// 英文命令标题：私有加载内嵌 :/i18n/paleo_en.qm（PaleoCommands 上下文），不全局安装——
// 英文骨架大多 unfinished，全局装会让界面中英混排；这里只给 Ctrl+K 当匹配别名。
static QString englishCommandTitle(const char *source)
{
  static QTranslator *en = [] {
    auto *t = new QTranslator(QCoreApplication::instance());
    if (!t->load(QStringLiteral(":/i18n/paleo_en.qm")))
      qWarning("paleo: :/i18n/paleo_en.qm missing; Ctrl+K English command titles unavailable");
    return t;
  }();
  return en->translate("PaleoCommands", source);
}

// Ctrl+K：命令过滤器数据面（CommandRegistry 中带 Locator 入口的命令 + 当前状态/禁用原因）。
// 过滤器本体在 src/qgis/commandlocatorfilter（QGIS 封装层），由 attachShellSurfaces 注册。
QVector<CommandLocatorEntry> PaleoMainWindow::commandLocatorEntries()
{
  QVector<CommandLocatorEntry> out;
  CommandDispatcher *d = commandDispatcher();
  const CommandContext ctx = d->currentContext();
  for (const paleo::command::CommandSpec &spec :
       paleo::command::CommandRegistry::forSurface(paleo::command::SurfaceLocator))
  {
    CommandLocatorEntry e;
    e.commandId = spec.commandId;
    e.title = QCoreApplication::translate("PaleoCommands", spec.title);
    e.aliases = {QString::fromUtf8(spec.title), englishCommandTitle(spec.title)};
    e.tooltip = QCoreApplication::translate("PaleoCommands", spec.tooltip);
    e.group = spec.ribbonGroup;
    const CommandState st = d->state(spec.commandId, ctx);
    e.enabled = st.enabled;
    e.reason = st.reason;
    out << e;
  }
  return out;
}

void PaleoMainWindow::triggerCommandFromLocator(const QString &commandId)
{
  CommandDispatcher *d = commandDispatcher();
  QString reason;
  if (!d->trigger(commandId, d->currentContext(), {}, &reason))
    statusBar()->showMessage(tr("命令不可用：%1").arg(reason), 6000);
}
