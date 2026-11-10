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
#include <QPushButton>
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
#include "../src/ui/paleoribbon.h"
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
    QgsLocatorContext ctx;
    ctx.usingPrefix = true; // 前缀 "c"：空余串列出全部命令（无前缀空查询保持沉默，免得 Tab 困在定位器）
    filter->fetchResults(QString(), ctx, nullptr);
    const QList<QgsLocatorResult> listed = results;
    results.clear();
    filter->fetchResults(QString(), QgsLocatorContext(), nullptr); // usingPrefix=false：空闲沉默
    disconnect(c); // results 是局部量：不断开会让后续用例的抓取写到悬空引用
    QCOMPARE(listed.size(), CommandRegistry::forSurface(SurfaceLocator).size());
    QCOMPARE(results.size(), 0);
    bool sawReason = false;
    for (const QgsLocatorResult &r : listed)
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
#ifndef PALEO_TEST_HAS_TRANSLATIONS
    QSKIP("lrelease is unavailable in this build");
#endif
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
#ifdef PALEO_TEST_HAS_TRANSLATIONS
    QCOMPARE(locatorIds(QStringLiteral("Open project")).value(0), QStringLiteral("project.open"));
    QCOMPARE(locatorIds(QStringLiteral("open PROJECT")).value(0), QStringLiteral("project.open"));
    QVERIFY(locatorIds(QStringLiteral("Add point")).contains(QStringLiteral("edit.addPoint")));
#endif
    QCOMPARE(locatorIds(QStringLiteral("打开工程")).value(0), QStringLiteral("project.open"));
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

  // 审查 P2#1：勾选类命令缺 checked（Ctrl+K / Agent）= 取反当前态；显式 checked 照旧幂等。
  void targetCheckedSemantics()
  {
    QVERIFY(CommandActionBinder::targetChecked({}, false));
    QVERIFY(!CommandActionBinder::targetChecked({}, true));
    QVERIFY(CommandActionBinder::targetChecked({{QStringLiteral("checked"), true}}, true));
    QVERIFY(!CommandActionBinder::targetChecked({{QStringLiteral("checked"), false}}, false));
  }

  void unresolvedFilterTogglesFromLocator()
  {
    CommandDispatcher *d = m_win->commandDispatcher();
    auto *act = m_win->findChild<QAction *>(QStringLiteral("ribbonUnresolvedFilter"));
    QVERIFY(act);
    QVERIFY(!act->isChecked());
    QString reason;
    QVERIFY2(d->trigger(QStringLiteral("data.list.unresolved"), d->currentContext(), {}, &reason), qPrintable(reason));
    QVERIFY(act->isChecked()); // 从关到开（原先缺 checked 当 false：无效果）
    QVERIFY(d->trigger(QStringLiteral("data.list.unresolved"), d->currentContext(), {}));
    QVERIFY(!act->isChecked()); // 再选一次 = 关
    const QVariantMap on{{QStringLiteral("checked"), true}};
    QVERIFY(d->trigger(QStringLiteral("data.list.unresolved"), d->currentContext(), on));
    QVERIFY(d->trigger(QStringLiteral("data.list.unresolved"), d->currentContext(), on));
    QVERIFY(act->isChecked()); // 显式目标态幂等
    QVERIFY(d->trigger(QStringLiteral("data.list.unresolved"), d->currentContext(), {{QStringLiteral("checked"), false}}));
    QVERIFY(!act->isChecked());
  }

  void previewMaximizeTogglesAndGatesFromLocator()
  {
    CommandDispatcher *d = m_win->commandDispatcher();
    auto *act = m_win->findChild<QAction *>(QStringLiteral("ribbonPreviewMaximize"));
    auto *btn = m_win->findChild<QToolButton *>(QStringLiteral("previewMaxButton"));
    QVERIFY(act && btn);
    // 没有打开的预览：动作禁用 → 命令也禁用且带同一原因（不再「看着可用、选了没反应」）。
    act->setEnabled(false);
    const CommandState off = d->state(QStringLiteral("data.preview.maximize"), d->currentContext());
    QVERIFY(!off.enabled);
    QCOMPARE(off.reason, act->toolTip());
    act->setEnabled(true);
    const bool before = btn->isChecked();
    QVERIFY(d->trigger(QStringLiteral("data.preview.maximize"), d->currentContext(), {}));
    QCOMPARE(btn->isChecked(), !before);
    QVERIFY(d->trigger(QStringLiteral("data.preview.maximize"), d->currentContext(), {}));
    QCOMPARE(btn->isChecked(), before);
  }

  // 镜像型可勾选命令：显式 checked 不再一律 click 翻转；缺 checked = 翻转。
  void checkableMirrorHonoursTargetState()
  {
    CommandDispatcher d;
    CommandActionBinder binder(&d);
    QWidget host;
    auto *button = new QPushButton(QStringLiteral("src"), &host);
    button->setCheckable(true);
    auto *action = new QAction(QStringLiteral("mirror"), &host);
    PaleoRibbon::mirror(action, button, false, false);
    binder.bindMirror(action, QStringLiteral("constraint.breakLine"));
    const QVariantMap on{{QStringLiteral("checked"), true}};
    QVERIFY(d.trigger(QStringLiteral("constraint.breakLine"), CommandContext{}, on));
    QVERIFY(button->isChecked());
    QVERIFY(d.trigger(QStringLiteral("constraint.breakLine"), CommandContext{}, on));
    QVERIFY(button->isChecked());
    QVERIFY(d.trigger(QStringLiteral("constraint.breakLine"), CommandContext{}, {}));
    QVERIFY(!button->isChecked());
    action->trigger(); // ribbon 点击：动作先翻转，再按新态驱动源按钮
    QVERIFY(button->isChecked());
    QCOMPARE(action->isChecked(), button->isChecked());
  }

  // 审查 P2#2：子菜单型父动作（添加要素 / 相面分区操作 / 更多）从 Ctrl+K 选中 = 弹出同一子菜单；
  // 没有可用子项时命令禁用并带原因——不再「看着可用、选了没反应」。子项本身另有独立命令。
  void menuParentCommandsPopTheirMenu()
  {
    CommandDispatcher *d = m_win->commandDispatcher();
    for (const char *id : {"edit.addFeature", "edit.faciesOperations", "view.more.constraint"})
    {
      const QString cid = QLatin1String(id);
      QVERIFY2(locatorIds(cid).contains(cid), id);
      QAction *parent = nullptr;
      for (QAction *a : m_win->findChildren<QAction *>())
        if (CommandActionBinder::commandIdOf(a) == cid && a->menu())
          parent = a;
      QVERIFY2(parent, id);
      QMenu *menu = parent->menu();
      const CommandState st = d->state(cid, d->currentContext());
      QString reason;
      if (!st.enabled)
      {
        QVERIFY2(!st.reason.isEmpty(), id);
        QVERIFY(!d->trigger(cid, d->currentContext(), {}, &reason));
        QVERIFY(!menu->isVisible());
        // 强制一个可用子项，验证弹出路径本身。
        parent->setEnabled(true);
        QAction *child = nullptr;
        for (QAction *c : menu->actions())
          if (!c->isSeparator() && !child)
            child = c;
        QVERIFY2(child, id);
        child->setVisible(true);
        child->setEnabled(true);
      }
      QVERIFY2(d->trigger(cid, d->currentContext(), {}, &reason), qPrintable(cid + QLatin1Char(' ') + reason));
      QTRY_VERIFY2(menu->isVisible(), id);
      menu->hide();
    }
    // 子项也是独立命令（Ctrl+K 可直达）。
    for (const char *id : {"edit.addPoint", "edit.addLine", "edit.addPolygon", "edit.facies.split",
                           "edit.facies.paint", "edit.facies.merge"})
      QVERIFY2(d->isBound(QLatin1String(id)), id);
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
#ifdef PALEO_TEST_HAS_TRANSLATIONS
  Q_INIT_RESOURCE(translations);
#endif
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
