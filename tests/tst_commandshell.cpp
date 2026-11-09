// UI 重设计 U1：真实主窗（AppContext + PaleoMainWindow + attachWorkflows）上的命令闸——
//   - Ribbon 各面板 / 快速访问栏 / 文件菜单里每个 QAction 都挂着已登记的 paleoCommandId
//   - 每个命令 id 都在 CommandRegistry；每个 shortcutId 都在快捷键注册表
//   - Ctrl+K 定位器挂上 paleo_commands 过滤器，并能列出命令（禁用的带原因）
//   - 经分派器触发 View 命令有效；未接线 / 被拒的命令给出原因
//   - 源码闸：ribbon 两个构建文件里不再有 findChild<QPushButton…>()->click() 间接调用
#include <QtTest>

#include <QAction>
#include <QApplication>
#include <QFile>
#include <QMenu>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTranslator>
#include <QToolButton>
#include <QWidgetAction>

#include <SARibbon.h>

#include "../src/app/appcontext.h"
#include "../src/services/commandspec.h"
#include "../src/ui/commandactionbinder.h"
#include "../src/qgis/commandlocatorfilter.h"
#include "../src/ui/paleomainwindow.h"
#include "../src/ui/shortcuts/shortcutcatalog.h"
#include "../src/workflow/commanddispatcher.h"

#include <qgslocator.h>
#include <qgslocatorwidget.h>

using namespace paleo::command;

namespace
{
void collect(QAction *a, const QString &where, QVector<QPair<QAction *, QString>> &out, int depth = 0)
{
  if (!a || a->isSeparator() || qobject_cast<QWidgetAction *>(a) || depth > 4)
    return;
  // 纯子菜单标题（QMenu::menuAction，如「网络底图预设」）本身不是命令，只收其子项。
  const bool submenuTitle = a->menu() && a == a->menu()->menuAction();
  if (!submenuTitle)
    out.append({a, where});
  if (QMenu *m = a->menu())
    for (QAction *sub : m->actions())
      collect(sub, where + QStringLiteral(" > ") + a->text(), out, depth + 1);
}
} // namespace

class TestCommandShell : public QObject
{
  Q_OBJECT
public:
  explicit TestCommandShell(AppContext *ctx) : m_ctx(ctx) {}

private:
  AppContext *m_ctx;
  PaleoMainWindow *m_win = nullptr;

  QVector<QPair<QAction *, QString>> surfaceActions() const
  {
    QVector<QPair<QAction *, QString>> out;
    SARibbonBar *bar = m_win->ribbonBar();
    for (SARibbonCategory *cat : bar->categoryPages())
      for (SARibbonPanel *panel : cat->panelList())
        for (QAction *a : panel->actions())
          collect(a, cat->categoryName() + QStringLiteral("/") + panel->panelName(), out);
    if (bar->quickAccessBar())
      for (QAction *a : bar->quickAccessBar()->actions())
        collect(a, QStringLiteral("quickAccess"), out);
    if (auto *app = qobject_cast<QToolButton *>(bar->applicationButton()); app && app->menu())
      for (QAction *a : app->menu()->actions())
        collect(a, QStringLiteral("fileMenu"), out);
    return out;
  }

  CommandLocatorFilter *commandFilter() const
  {
    auto *lw = m_win->findChild<QgsLocatorWidget *>();
    if (!lw)
      return nullptr;
    for (QgsLocatorFilter *f : lw->locator()->filters())
      if (f->name() == QLatin1String("paleo_commands"))
        return qobject_cast<CommandLocatorFilter *>(f);
    return nullptr;
  }

  QStringList locatorIds(const QString &query)
  {
    CommandLocatorFilter *filter = commandFilter();
    QStringList ids;
    if (!filter)
      return ids;
    QMetaObject::Connection c = connect(filter, &QgsLocatorFilter::resultFetched, this,
                                        [&ids](const QgsLocatorResult &r) { ids << r.userData().toString(); });
    filter->fetchResults(query, QgsLocatorContext(), nullptr);
    disconnect(c);
    return ids;
  }

private slots:
  void initTestCase()
  {
    QVERIFY2(m_ctx->ready(), "AppContext failed to bring up QgisRuntime");
    QSettings(QStringLiteral("paleo"), QStringLiteral("paleo")).clear();
    m_win = new PaleoMainWindow(m_ctx->canvasCtl(), m_ctx->projectSvc(), m_ctx->layerSvc(),
                                m_ctx->toolSvc(), m_ctx->selection());
    m_win->attachWorkflows(m_ctx->predictionWf(), m_ctx->constraintWf(), m_ctx->compositionWf(),
                           m_ctx->validationWf(), m_ctx->importSvc(), m_ctx->seismicLink(),
                           m_ctx->processingSvc(), m_ctx->store(), m_ctx->editingSvc(),
                           m_ctx->layoutSvc(), m_ctx->taskSvc());
    m_win->show();
    QVERIFY(m_win->commandDispatcher());
  }

  void cleanupTestCase()
  {
    delete m_win;
    m_win = nullptr;
  }

  void registryHealthy() { QVERIFY2(CommandRegistry::validate().isEmpty(), qPrintable(CommandRegistry::validate().join('\n'))); }

  // 收编完整：三类入口上的每个动作都带已登记命令 id，且该命令在分派器上接了线。
  void everySurfaceActionIsACommand()
  {
    const auto actions = surfaceActions();
    QVERIFY2(actions.size() >= 60, qPrintable(QString::number(actions.size())));
    QStringList problems;
    for (const auto &[a, where] : actions)
    {
      const QString id = CommandActionBinder::commandIdOf(a);
      if (id.isEmpty())
        problems << QStringLiteral("未登记：%1 [%2] @ %3").arg(a->text(), a->objectName(), where);
      else if (!CommandRegistry::contains(id))
        problems << QStringLiteral("id 不在登记表：%1 (%2)").arg(id, a->objectName());
      else if (!m_win->commandDispatcher()->isBound(id))
        problems << QStringLiteral("未接线：%1 (%2)").arg(id, a->objectName());
    }
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
  }

  // 登记表里每条 UI 入口命令（非 Agent 独占）在真实主窗上都接了线。
  void everyUiCommandIsBound()
  {
    QStringList unbound;
    for (const CommandSpec &s : CommandRegistry::all())
    {
      if (s.kind == CommandKind::Compute && !(s.surfaces & ~int(SurfaceAgent)))
        continue; // 纯 Agent 计算命令：由网关桥直接执行，不需 UI 接线
      // 随编图工作台装配接线（attachMappingEditBasisEntry）；工作台未装配时入口本身也不存在。
      if (s.commandId == QLatin1String("compose.editBasis") || s.commandId == QLatin1String("compose.references"))
        continue;
      if (!m_win->commandDispatcher()->isBound(s.commandId))
        unbound << s.commandId;
    }
    QVERIFY2(unbound.isEmpty(), qPrintable(unbound.join(QStringLiteral(", "))));
  }

  void shortcutIdsResolve()
  {
    for (const CommandSpec &s : CommandRegistry::all())
      if (!s.shortcutId.isEmpty())
        QVERIFY2(!paleo::shortcuts::registry().entry(s.shortcutId).id.isEmpty(), qPrintable(s.shortcutId));
  }

  void locatorListsCommands()
  {
    auto *lw = m_win->findChild<QgsLocatorWidget *>();
    QVERIFY(lw);
    CommandLocatorFilter *filter = nullptr;
    for (QgsLocatorFilter *f : lw->locator()->filters())
      if (f->name() == QLatin1String("paleo_commands"))
        filter = qobject_cast<CommandLocatorFilter *>(f);
    QVERIFY(filter);
    QList<QgsLocatorResult> results;
    const QMetaObject::Connection c = connect(filter, &QgsLocatorFilter::resultFetched, this,
                                              [&results](const QgsLocatorResult &r) { results << r; });
    filter->fetchResults(QString(), QgsLocatorContext(), nullptr);
    disconnect(c); // results 是局部量：不断开会让后续用例的抓取写到悬空引用
    QCOMPARE(results.size(), CommandRegistry::forSurface(SurfaceLocator).size());
    bool sawReason = false;
    for (const QgsLocatorResult &r : results)
    {
      QVERIFY(!r.displayString.isEmpty());
      if (r.description.startsWith(QStringLiteral("不可用：")))
      {
        sawReason = true;
        QVERIFY2(r.description.size() > 5, qPrintable(r.userData().toString()));
      }
    }
    QVERIFY2(sawReason, "未开工程时应至少有一条命令带禁用原因");
  }

  // 审查 P2#3：英文标题可搜（任意界面语言）——PaleoCommands 上下文进了 en 目录且内嵌 qm。
  void englishCatalogCoversEveryCommand()
  {
    QTranslator en;
    QVERIFY(en.load(QStringLiteral(":/i18n/paleo_en.qm")));
    QStringList missing;
    for (const CommandSpec &s : CommandRegistry::all())
    {
      const QString t = en.translate("PaleoCommands", s.title);
      if (t.isEmpty() || t == QString::fromUtf8(s.title))
        missing << s.commandId;
      if (*s.tooltip && en.translate("PaleoCommands", s.tooltip).isEmpty())
        missing << s.commandId + QStringLiteral("(tooltip)");
    }
    QVERIFY2(missing.isEmpty(), qPrintable(missing.join(QStringLiteral(", "))));
  }

  void locatorMatchesEnglishAndSourceTitles()
  {
    QVERIFY(commandFilter());
    QCOMPARE(locatorIds(QStringLiteral("Open project")).value(0), QStringLiteral("project.open"));
    QCOMPARE(locatorIds(QStringLiteral("open PROJECT")).value(0), QStringLiteral("project.open"));
    QCOMPARE(locatorIds(QStringLiteral("打开工程")).value(0), QStringLiteral("project.open"));
    QVERIFY(locatorIds(QStringLiteral("Add point")).contains(QStringLiteral("edit.addPoint")));
    // 纯函数：别名参与匹配，且与显示标题同档打分。
    CommandLocatorEntry e;
    e.commandId = QStringLiteral("x.y");
    e.title = QStringLiteral("打开工程");
    e.aliases = {QStringLiteral("Open project")};
    e.enabled = true;
    const auto hit = CommandLocatorFilter::match({e}, QStringLiteral("open pro"));
    QCOMPARE(hit.size(), 1);
    QCOMPARE(hit.first().second, 0.9);
  }

  void triggerViewCommand()
  {
    CommandDispatcher *d = m_win->commandDispatcher();
    QSignalSpy spy(d, &CommandDispatcher::commandTriggered);
    QString reason;
    SARibbonBar *bar = m_win->ribbonBar();
    QVERIFY2(d->trigger(QStringLiteral("view.stage.compose"), d->currentContext(), {}, &reason), qPrintable(reason));
    QCOMPARE(spy.size(), 1);
    QVERIFY(bar->currentIndex() >= 0);
    const int composeIndex = bar->currentIndex();
    QVERIFY2(d->trigger(QStringLiteral("view.stage.data"), d->currentContext(), {}, &reason), qPrintable(reason));
    QVERIFY(bar->currentIndex() != composeIndex); // 真切了页签
  }

  void disabledCommandCarriesReason()
  {
    CommandDispatcher *d = m_win->commandDispatcher();
    // 未开工程：工程数据源不可用，且原因非空。
    const CommandState st = d->state(QStringLiteral("project.sources"), d->currentContext());
    QVERIFY(!st.enabled);
    QVERIFY(!st.reason.isEmpty());
    QString reason;
    QSignalSpy rejected(d, &CommandDispatcher::commandRejected);
    QVERIFY(!d->trigger(QStringLiteral("project.sources"), d->currentContext(), {}, &reason));
    QCOMPARE(reason, st.reason);
    QCOMPARE(rejected.size(), 1);
    QVERIFY(!d->trigger(QStringLiteral("no.such.command"), d->currentContext(), {}, &reason));
    QVERIFY(!reason.isEmpty());
  }

  void noButtonClickIndirectionInRibbonSources()
  {
    const QRegularExpression re(QStringLiteral(R"(findChild<\s*QPushButton\s*\*\s*>\([^;]*\)\s*\)?\s*->\s*click\(\))"));
    for (const char *rel : {"src/ui/paleomainwindow_ribbon.cpp", "src/ui/ribbonpanels.cpp"})
    {
      QFile f(QStringLiteral(PALEO_SOURCE_DIR "/") + QLatin1String(rel));
      QVERIFY2(f.open(QIODevice::ReadOnly), rel);
      const QString src = QString::fromUtf8(f.readAll());
      QVERIFY2(!re.match(src).hasMatch(), rel);
      QVERIFY2(!src.contains(QStringLiteral("->click()")), rel);
    }
  }
};

int main(int argc, char *argv[])
{
  if (qgetenv("QT_QPA_PLATFORM").isEmpty())
    qputenv("QT_QPA_PLATFORM", "offscreen");
  static QTemporaryDir settingsDir;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
  AppContext ctx(QStringLiteral("/usr"));
  if (!ctx.ready())
    qFatal("AppContext failed to initialize the QGIS runtime");
  TestCommandShell tc(&ctx);
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_commandshell.moc"
