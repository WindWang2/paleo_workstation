// goal7/ui-shell：主窗 chrome（dock 策略 / ribbon / 帮助）真实壳回归。
//   - 页面 dock 策略（数据页藏图层树、编图页按用户意愿恢复）必须在保存/恢复/
//     重置布局之后保持成立：布局命令不得把「页面策略隐藏」当成「用户想藏」。
// 每个用例自建窗口 + 自带画布控制器（同 tst_ui windowStateAndExtentPersist 惯例），
// 彼此不共享 dock/QSettings 状态。
#include <QtTest>

#include <iterator>

#include <QAbstractButton>
#include <QAction>
#include <QDockWidget>
#include <QLineEdit>
#include <QMenu>
#include <QSettings>
#include <QTemporaryDir>
#include <QToolButton>

#include "../src/app/appcontext.h"
#include "../src/qgis/qgiscanvascontroller.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/ui/paleodockmanager.h"
#include "../src/ui/paleomainwindow.h"
#include "../src/ui/pages/pageshared.h"

class TestShellChrome : public QObject
{
  Q_OBJECT
public:
  explicit TestShellChrome(AppContext *ctx) : m_ctx(ctx) {}

private:
  AppContext *m_ctx;

  static void clearSettings()
  {
    QSettings(QStringLiteral("paleo"), QStringLiteral("paleo")).clear();
  }

  static QDockWidget *dock(PaleoMainWindow &w, const char *name)
  {
    return w.findChild<QDockWidget *>(QString::fromLatin1(name));
  }

  // 「布局与面板」菜单里按文案取动作（与用户点击同一条路径）。
  static QAction *layoutAction(PaleoMainWindow &w, const QString &text, QScopedPointer<QMenu> &menu)
  {
    auto *manager = w.findChild<PaleoDockManager *>();
    menu.reset(manager ? manager->createMenu() : nullptr);
    if (!menu)
      return nullptr;
    for (QAction *a : menu->actions())
      if (a->text() == text)
        return a;
    return nullptr;
  }

private slots:
  void initTestCase() { QVERIFY2(m_ctx->ready(), "AppContext failed to bring up QgisRuntime"); }
  void init() { clearSettings(); }
  void cleanup() { clearSettings(); }

  // 基线（非回归场景）：纯切页时左 dock 随页策略显隐，且不丢「用户想看」。
  void pagePolicyHidesLayerTreeOnDataOnly()
  {
    QgisCanvasController canvas(m_ctx);
    PaleoMainWindow win(&canvas, m_ctx->projectSvc(), m_ctx->layerSvc(), m_ctx->toolSvc(),
                        m_ctx->selection());
    win.show();
    QDockWidget *left = dock(win, "layerTreeDock");
    QVERIFY(left);
    win.showPage(QStringLiteral("predict"));
    QVERIFY(!left->isHidden());
    win.showPage(QStringLiteral("data"));
    QVERIFY(left->isHidden());
    win.showPage(QStringLiteral("predict"));
    QVERIFY(!left->isHidden());
  }

  // R-1：在数据页退出（左 dock 被页策略藏起）→ 下次启动恢复窗口状态 → 切到编图页，
  // 图层树必须照常出现。
  void exitOnDataPageDoesNotPinLayerTreeHidden()
  {
    {
      QgisCanvasController canvas(m_ctx);
      PaleoMainWindow win(&canvas, m_ctx->projectSvc(), m_ctx->layerSvc(), m_ctx->toolSvc(),
                          m_ctx->selection());
      win.show();
      win.showPage(QStringLiteral("data"));
      QVERIFY(dock(win, "layerTreeDock")->isHidden());
      win.saveWindowState();
    }
    QgisCanvasController canvas(m_ctx);
    PaleoMainWindow win(&canvas, m_ctx->projectSvc(), m_ctx->layerSvc(), m_ctx->toolSvc(),
                        m_ctx->selection());
    win.show();
    win.showPage(QStringLiteral("predict"));
    QVERIFY2(!dock(win, "layerTreeDock")->isHidden(),
             "layer tree dock stayed hidden on a mapping page after restoring a window state "
             "saved while the data page had hidden it by page policy");
  }

  // R-2：在数据页点「恢复默认布局」——数据页纯三栏策略（图层树藏、数据列表显）不得被破坏。
  void resetLayoutOnDataPageKeepsPagePolicy()
  {
    QgisCanvasController canvas(m_ctx);
    PaleoMainWindow win(&canvas, m_ctx->projectSvc(), m_ctx->layerSvc(), m_ctx->toolSvc(),
                        m_ctx->selection());
    win.show();
    win.showPage(QStringLiteral("data"));
    QScopedPointer<QMenu> menu;
    QAction *reset = layoutAction(win, tr("恢复默认布局"), menu);
    QVERIFY(reset);
    reset->trigger();
    QTest::qWait(20);
    QVERIFY2(dock(win, "layerTreeDock")->isHidden(), "layer tree reappeared on the data page");
    QVERIFY2(!dock(win, "dataListDock")->isHidden(), "data list vanished on the data page");
  }

  // R-3：在编图页重置布局后来回切页，图层树仍按页策略出现。
  void resetLayoutThenPageRoundTripKeepsLayerTree()
  {
    QgisCanvasController canvas(m_ctx);
    PaleoMainWindow win(&canvas, m_ctx->projectSvc(), m_ctx->layerSvc(), m_ctx->toolSvc(),
                        m_ctx->selection());
    win.show();
    win.showPage(QStringLiteral("predict"));
    QScopedPointer<QMenu> menu;
    QAction *reset = layoutAction(win, tr("恢复默认布局"), menu);
    QVERIFY(reset);
    reset->trigger();
    QTest::qWait(20);
    win.showPage(QStringLiteral("data"));
    win.showPage(QStringLiteral("predict"));
    QVERIFY2(!dock(win, "layerTreeDock")->isHidden(),
             "layer tree did not come back on a mapping page after reset + page round trip");
  }

  // R-4：数据页「保存当前布局」→ 编图页「恢复保存的布局」→ 来回切页，图层树仍出现。
  void restoreSavedLayoutSavedOnDataPageKeepsLayerTree()
  {
    QgisCanvasController canvas(m_ctx);
    PaleoMainWindow win(&canvas, m_ctx->projectSvc(), m_ctx->layerSvc(), m_ctx->toolSvc(),
                        m_ctx->selection());
    win.show();
    win.showPage(QStringLiteral("data"));
    QScopedPointer<QMenu> menu;
    QAction *save = layoutAction(win, tr("保存当前布局"), menu);
    QVERIFY(save);
    save->trigger();
    win.showPage(QStringLiteral("predict"));
    QAction *restore = layoutAction(win, tr("恢复保存的布局"), menu);
    QVERIFY(restore && restore->isEnabled());
    restore->trigger();
    QTest::qWait(20);
    win.showPage(QStringLiteral("data"));
    win.showPage(QStringLiteral("predict"));
    QVERIFY2(!dock(win, "layerTreeDock")->isHidden(),
             "layer tree stayed hidden on a mapping page after restoring a layout saved on the data page");
  }

  // 用户主动藏图层树（与页策略无关）：意图跨会话保留——恢复后切到编图页也不重显。
  void userHiddenLayerTreeStaysHiddenAcrossSessions()
  {
    {
      QgisCanvasController canvas(m_ctx);
      PaleoMainWindow win(&canvas, m_ctx->projectSvc(), m_ctx->layerSvc(), m_ctx->toolSvc(),
                          m_ctx->selection());
      win.show();
      win.showPage(QStringLiteral("predict"));
      auto *left = qobject_cast<PaleoDockWidget *>(dock(win, "layerTreeDock"));
      QVERIFY(left);
      left->hide(); // 用户手势：非程序化 setVisible
      QVERIFY(!left->userWantsVisible());
      win.saveWindowState();
    }
    QgisCanvasController canvas(m_ctx);
    PaleoMainWindow win(&canvas, m_ctx->projectSvc(), m_ctx->layerSvc(), m_ctx->toolSvc(),
                        m_ctx->selection());
    win.show();
    win.showPage(QStringLiteral("predict"));
    auto *left = qobject_cast<PaleoDockWidget *>(dock(win, "layerTreeDock"));
    QVERIFY(left);
    QVERIFY2(left->isHidden(), "user-hidden layer tree reappeared after session restore");
    QVERIFY2(!left->userWantsVisible(), "user-hide intent was lost by the restore");
  }

  // 旧版设置（只有 windowState/windowGeometry、没有 windowDockIntent）：照常恢复不崩，
  // 图层树意愿保持默认 true，编图页照常显示。
  void legacySettingsWithoutIntentStillRestore()
  {
    {
      QgisCanvasController canvas(m_ctx);
      PaleoMainWindow win(&canvas, m_ctx->projectSvc(), m_ctx->layerSvc(), m_ctx->toolSvc(),
                          m_ctx->selection());
      win.show();
      QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
      s.setValue(QStringLiteral("windowState"), win.saveState());
      s.setValue(QStringLiteral("windowGeometry"), win.saveGeometry());
    }
    QgisCanvasController canvas(m_ctx);
    PaleoMainWindow win(&canvas, m_ctx->projectSvc(), m_ctx->layerSvc(), m_ctx->toolSvc(),
                        m_ctx->selection());
    win.show();
    auto *left = qobject_cast<PaleoDockWidget *>(dock(win, "layerTreeDock"));
    QVERIFY(left);
    QVERIFY2(left->userWantsVisible(), "legacy restore clobbered the default show intent");
    win.showPage(QStringLiteral("predict"));
    QVERIFY2(!left->isHidden(), "layer tree missing on a mapping page after legacy restore");
  }
  // C（反复触发）：六页来回切三轮，dock 策略每轮一致——图层树只在数据页/地层对比页隐藏，
  // 数据列表只在数据页显示；切页不得累积意愿漂移。非回归场景。
  void repeatedPageCyclingKeepsDockPolicy()
  {
    QgisCanvasController canvas(m_ctx);
    PaleoMainWindow win(&canvas, m_ctx->projectSvc(), m_ctx->layerSvc(), m_ctx->toolSvc(),
                        m_ctx->selection());
    win.show();
    QDockWidget *left = dock(win, "layerTreeDock");
    QDockWidget *list = dock(win, "dataListDock");
    QVERIFY(left && list);
    for (int round = 0; round < 3; ++round)
      for (const QString &id : paleo::pagesinternal::kPageIds)
      {
        win.showPage(id);
        const bool hideLeft = id == QLatin1String("data") || id == QLatin1String("correlation");
        const bool showList = id == QLatin1String("data");
        QVERIFY2(left->isHidden() == hideLeft,
                 qPrintable(QStringLiteral("round %1 page %2: layerTreeDock hidden=%3")
                                .arg(round).arg(id).arg(left->isHidden())));
        QVERIFY2(list->isHidden() == !showList,
                 qPrintable(QStringLiteral("round %1 page %2: dataListDock hidden=%3")
                                .arg(round).arg(id).arg(list->isHidden())));
      }
  }

  // F/B：窗口停在最小尺寸时 ribbon 内容放不下，SARibbon 的左右滚动钮出现；它们是纯图标按钮，
  // 必须有无障碍名称和悬停提示，且左右两个不同名。
  void ribbonScrollButtonsHaveNamesAndTips()
  {
    QgisCanvasController canvas(m_ctx);
    PaleoMainWindow win(&canvas, m_ctx->projectSvc(), m_ctx->layerSvc(), m_ctx->toolSvc(),
                        m_ctx->selection());
    win.show();
    QStringList bad;
    QSet<QString> leftNames, rightNames;
    int total = 0;
    for (auto *b : win.findChildren<QToolButton *>())
    {
      if (!b->inherits("SARibbonCategoryScrollButton"))
        continue;
      ++total;
      if (b->accessibleName().isEmpty() || b->toolTip().isEmpty())
        bad << QStringLiteral("arrow=%1 name='%2' tip='%3'")
                   .arg(int(b->arrowType())).arg(b->accessibleName(), b->toolTip());
      (b->arrowType() == Qt::LeftArrow ? leftNames : rightNames).insert(b->accessibleName());
    }
    QVERIFY2(total >= 2, "expected the ribbon category scroll buttons to exist");
    QVERIFY2(bad.isEmpty(), qPrintable(bad.join(QLatin1Char('\n'))));
    QVERIFY2(leftNames.size() == 1 && rightNames.size() == 1 && leftNames != rightNames,
             "left and right scroll buttons must carry one distinct name each");
  }
  // D：ribbon 按钮是页内按钮的镜像（ButtonMirror），点的是 ribbon 这一份——页内按钮在目录里有
  // 「这是什么」帮助时，ribbon 上同名按钮必须带同一段帮助，否则 Shift+F1 点 ribbon 什么也不显示。
  void ribbonButtonsAdoptHelpOfTheirSourceButtons()
  {
    QgisCanvasController canvas(m_ctx);
    PaleoMainWindow win(&canvas, m_ctx->projectSvc(), m_ctx->layerSvc(), m_ctx->toolSvc(),
                        m_ctx->selection());
    win.attachWorkflows(m_ctx->predictionWf(), m_ctx->constraintWf(), m_ctx->compositionWf(),
                        m_ctx->validationWf(), m_ctx->importSvc(), m_ctx->seismicLink(),
                        m_ctx->processingSvc(), m_ctx->store(), m_ctx->editingSvc(),
                        m_ctx->layoutSvc(), m_ctx->taskSvc());
    win.show();
    const auto inRibbon = [](QWidget *w) {
      for (QWidget *p = w; p; p = p->parentWidget())
        if (p->inherits("SARibbonBar"))
          return true;
      return false;
    };
    // 同名源按钮的帮助不唯一的键（如「撤销」：数据列表撤销 vs 图层编辑撤销是两件事）不做文案配对，
    // 由下面的专门断言覆盖；其余键必须逐字一致。
    QHash<QString, QSet<QString>> helpByText;
    for (auto *b : win.findChildren<QAbstractButton *>())
      if (!inRibbon(b) && !b->whatsThis().isEmpty())
        helpByText[b->text().simplified()].insert(b->whatsThis());
    for (auto it = helpByText.begin(); it != helpByText.end();)
      it = it->size() == 1 ? std::next(it) : helpByText.erase(it);
    int matched = 0;
    QStringList missing;
    for (const QString &id : paleo::pagesinternal::kPageIds)
    {
      win.showPage(id);
      QWidget *cat = win.categoryForPage(id);
      if (!cat)
        continue;
      for (auto *b : cat->findChildren<QToolButton *>())
      {
        if (b->inherits("SARibbonCategoryScrollButton"))
          continue;
        const QAction *a = b->defaultAction();
        const QString key = (a ? a->text() : b->text()).simplified();
        if (!helpByText.contains(key))
          continue;
        ++matched;
        if (!helpByText.value(key).contains(b->whatsThis()))
          missing << QStringLiteral("%1/%2 (%3)").arg(id, key, b->objectName());
      }
    }
    QVERIFY2(matched >= 6, qPrintable(QStringLiteral("only %1 ribbon buttons matched a helped source").arg(matched)));
    QVERIFY2(missing.isEmpty(), qPrintable(QStringLiteral("%1 of %2 ribbon buttons lack their source's help:\n%3")
                                               .arg(missing.size()).arg(matched).arg(missing.join(QLatin1Char('\n')))));
  }

  // D：编图页三个地图页的 ribbon「撤销/重做」是图层编辑的撤销重做（直挂 QAction，不经镜像），
  // 必须带帮助，且不是数据列表撤销的那段话。
  void ribbonUndoRedoCarryEditingHelp()
  {
    QgisCanvasController canvas(m_ctx);
    PaleoMainWindow win(&canvas, m_ctx->projectSvc(), m_ctx->layerSvc(), m_ctx->toolSvc(),
                        m_ctx->selection());
    win.attachWorkflows(m_ctx->predictionWf(), m_ctx->constraintWf(), m_ctx->compositionWf(),
                        m_ctx->validationWf(), m_ctx->importSvc(), m_ctx->seismicLink(),
                        m_ctx->processingSvc(), m_ctx->store(), m_ctx->editingSvc(),
                        m_ctx->layoutSvc(), m_ctx->taskSvc());
    win.show();
    int seen = 0;
    QStringList bad;
    for (const QString &id : {QStringLiteral("predict"), QStringLiteral("constraint"), QStringLiteral("compose")})
    {
      win.showPage(id);
      QWidget *cat = win.categoryForPage(id);
      QVERIFY(cat);
      for (auto *b : cat->findChildren<QToolButton *>())
      {
        const QAction *a = b->defaultAction();
        const QString key = (a ? a->text() : b->text()).simplified();
        if (key != QStringLiteral("撤销") && key != QStringLiteral("重做"))
          continue;
        ++seen;
        if (b->whatsThis().isEmpty() || b->whatsThis().contains(QStringLiteral("数据列表")))
          bad << QStringLiteral("%1/%2: '%3'").arg(id, key, b->whatsThis());
      }
    }
    QVERIFY2(seen >= 6, qPrintable(QStringLiteral("found only %1 undo/redo ribbon buttons").arg(seen)));
    QVERIFY2(bad.isEmpty(), qPrintable(bad.join(QLatin1Char('\n'))));
  }

  // F：ribbon 命令按钮默认 NoFocus（vendor SARibbon），Tab 到不了——键盘用户只能靠快捷键。
  // 当前页分类里所有可见且启用的命令按钮必须在 Tab 链上。
  void ribbonCommandButtonsAreKeyboardReachable()
  {
    QgisCanvasController canvas(m_ctx);
    PaleoMainWindow win(&canvas, m_ctx->projectSvc(), m_ctx->layerSvc(), m_ctx->toolSvc(),
                        m_ctx->selection());
    win.attachWorkflows(m_ctx->predictionWf(), m_ctx->constraintWf(), m_ctx->compositionWf(),
                        m_ctx->validationWf(), m_ctx->importSvc(), m_ctx->seismicLink(),
                        m_ctx->processingSvc(), m_ctx->store(), m_ctx->editingSvc(),
                        m_ctx->layoutSvc(), m_ctx->taskSvc());
    win.show();
    int total = 0;
    QStringList unreachable;
    for (const QString &id : paleo::pagesinternal::kPageIds)
    {
      win.showPage(id);
      QWidget *cat = win.categoryForPage(id);
      QVERIFY(cat);
      for (auto *b : cat->findChildren<QToolButton *>())
      {
        if (!b->inherits("SARibbonToolButton") || !b->isVisibleTo(cat) || !b->isEnabled())
          continue;
        ++total;
        if (!(b->focusPolicy() & Qt::TabFocus))
          unreachable << QStringLiteral("%1/%2 (%3)").arg(id, b->text().simplified(), b->objectName());
      }
    }
    QVERIFY2(total >= 20, qPrintable(QStringLiteral("only %1 enabled ribbon buttons seen").arg(total)));
    QVERIFY2(unreachable.isEmpty(), qPrintable(QStringLiteral("%1 of %2 enabled ribbon buttons are not in the Tab chain:\n%3")
                                                   .arg(unreachable.size()).arg(total).arg(unreachable.join(QLatin1Char('\n')))));
  }

  // F/B：窗口停在最小宽度时，predict/constraint/compose 的 ribbon 放不下（出现滚动钮）；键盘聚焦到被
  // 裁掉的按钮时必须把它滚进可视区，否则焦点落在看不见的按钮上。
  void ribbonFocusScrollsClippedButtonIntoView()
  {
    QgisCanvasController canvas(m_ctx);
    PaleoMainWindow win(&canvas, m_ctx->projectSvc(), m_ctx->layerSvc(), m_ctx->toolSvc(),
                        m_ctx->selection());
    win.attachWorkflows(m_ctx->predictionWf(), m_ctx->constraintWf(), m_ctx->compositionWf(),
                        m_ctx->validationWf(), m_ctx->importSvc(), m_ctx->seismicLink(),
                        m_ctx->processingSvc(), m_ctx->store(), m_ctx->editingSvc(),
                        m_ctx->layoutSvc(), m_ctx->taskSvc());
    win.show();
    win.activateWindow();
    QTest::qWait(80);
    int clippedCases = 0;
    QStringList stillClipped;
    for (const QString &id : {QStringLiteral("predict"), QStringLiteral("constraint"), QStringLiteral("compose")})
    {
      win.showPage(id);
      QTest::qWait(60);
      QWidget *cat = win.categoryForPage(id);
      QVERIFY(cat);
      QToolButton *rightmost = nullptr;
      int rightmostX = -1;
      for (auto *b : cat->findChildren<QToolButton *>())
      {
        if (!b->inherits("SARibbonToolButton") || !b->isVisibleTo(cat) || !b->isEnabled())
          continue;
        const int x = b->mapTo(cat, QPoint(0, 0)).x();
        if (x > rightmostX)
        {
          rightmostX = x;
          rightmost = b;
        }
      }
      QVERIFY2(rightmost, qPrintable(QStringLiteral("no enabled ribbon button on %1").arg(id)));
      if (rightmostX + rightmost->width() <= cat->width())
        continue; // 这一页没有被裁掉的启用按钮
      ++clippedCases;
      rightmost->setFocus(Qt::TabFocusReason);
      QTest::qWait(80);
      const QRect r(rightmost->mapTo(cat, QPoint(0, 0)), rightmost->size());
      if (r.right() > cat->width() || r.left() < 0)
        stillClipped << QStringLiteral("%1/%2: x=%3..%4 of %5").arg(id, rightmost->text().simplified())
                            .arg(r.left()).arg(r.right()).arg(cat->width());
    }
    QVERIFY2(clippedCases >= 1, "precondition failed: no enabled ribbon button is clipped at the minimum window size");
    QVERIFY2(stillClipped.isEmpty(), qPrintable(stillClipped.join(QLatin1Char('\n'))));
  }
  // F：QGIS 定位器用 Tab 做补全/选下一条，Esc 只收起结果列表——键盘用户 Tab 进了搜索框
  // 就出不来（探针实测：进入后连按上百次 Tab 焦点不动）。要求：
  //   - 有补全可做时（文本非空）Tab 仍归 QGIS，不被劫持；
  //   - 空闲时（文本为空、结果列表未显示）Tab / Shift+Tab 放行到前后控件；
  //   - Esc（结果列表已收起时）把焦点还给进入前的控件；
  //   - 搜索框有无障碍名称。
  void locatorDoesNotTrapKeyboardFocus()
  {
    QgisCanvasController canvas(m_ctx);
    PaleoMainWindow win(&canvas, m_ctx->projectSvc(), m_ctx->layerSvc(), m_ctx->toolSvc(),
                        m_ctx->selection());
    win.attachWorkflows(m_ctx->predictionWf(), m_ctx->constraintWf(), m_ctx->compositionWf(),
                        m_ctx->validationWf(), m_ctx->importSvc(), m_ctx->seismicLink(),
                        m_ctx->processingSvc(), m_ctx->store(), m_ctx->editingSvc(),
                        m_ctx->layoutSvc(), m_ctx->taskSvc());
    win.show();
    win.activateWindow();
    QTest::qWait(80);
    QLineEdit *edit = nullptr;
    for (auto *w : win.findChildren<QLineEdit *>())
      if (w->inherits("QgsLocatorLineEdit"))
        edit = w;
    QVERIFY2(edit, "locator line edit not found");
    QWidget *prev = win.findChild<QToolButton *>(QStringLiteral("helpMenuButton"));
    QVERIFY(prev);
    prev->setFocus();
    QTest::qWait(30);
    QVERIFY2(prev->hasFocus(), "offscreen window did not take focus; test would be vacuous");
    QVERIFY2(!edit->accessibleName().isEmpty(), "locator line edit has no accessible name");

    // 有补全可做：Tab 留在搜索框（QGIS 原生补全/下一条结果）。
    edit->setFocus();
    QTest::qWait(300); // 越过 QGIS 聚焦定时器(110 ms)，覆盖「空浮窗已弹出」的真实时序
    QVERIFY(edit->hasFocus());
    edit->setText(QStringLiteral("abc"));
    QTest::keyClick(edit, Qt::Key_Tab);
    QTest::qWait(30);
    QVERIFY2(edit->hasFocus(), "Tab with text typed must stay with the locator (native QGIS completion)");
    edit->clear();

    // 空闲：Tab 向前放行。
    edit->setFocus();
    QTest::qWait(300); // 越过 QGIS 聚焦定时器(110 ms)，覆盖「空浮窗已弹出」的真实时序
    QTest::keyClick(edit, Qt::Key_Tab);
    QTest::qWait(30);
    QVERIFY2(!edit->hasFocus() && QApplication::focusWidget(), "Tab left keyboard focus trapped in the idle locator");

    // 空闲：Shift+Tab 向后放行。
    edit->setFocus();
    QTest::qWait(300); // 越过 QGIS 聚焦定时器(110 ms)，覆盖「空浮窗已弹出」的真实时序
    QTest::keyClick(edit, Qt::Key_Backtab, Qt::ShiftModifier);
    QTest::qWait(30);
    QVERIFY2(!edit->hasFocus() && QApplication::focusWidget(), "Shift+Tab left keyboard focus trapped in the idle locator");

    // Esc：还给进入前的控件。
    prev->setFocus();
    QTest::qWait(30);
    edit->setFocus();
    QTest::qWait(300); // 越过 QGIS 聚焦定时器(110 ms)，覆盖「空浮窗已弹出」的真实时序
    QVERIFY(edit->hasFocus());
    QTest::keyClick(edit, Qt::Key_Escape);
    QTest::qWait(30);
    QVERIFY2(!edit->hasFocus(), "Esc left keyboard focus trapped in the locator");
    QCOMPARE(QApplication::focusWidget(), prev);
  }

  // D：每个可见的 ribbon 命令钮都要有「这是什么」帮助（Shift+F1 点哪个都有说法）。帮助文本都取自
  // 命令自己的作者写下的用途串（动作 tip / 编辑条 hint / 页内按钮目录条目），不另编。
  void everyVisibleRibbonCommandHasHelp()
  {
    QgisCanvasController canvas(m_ctx);
    PaleoMainWindow win(&canvas, m_ctx->projectSvc(), m_ctx->layerSvc(), m_ctx->toolSvc(),
                        m_ctx->selection());
    win.attachWorkflows(m_ctx->predictionWf(), m_ctx->constraintWf(), m_ctx->compositionWf(),
                        m_ctx->validationWf(), m_ctx->importSvc(), m_ctx->seismicLink(),
                        m_ctx->processingSvc(), m_ctx->store(), m_ctx->editingSvc(),
                        m_ctx->layoutSvc(), m_ctx->taskSvc());
    win.attachWorkbench(m_ctx->mappingWorkbench());
    win.resize(1440, 900);
    win.show();
    int total = 0;
    QStringList missing;
    for (const QString &id : paleo::pagesinternal::kPageIds)
    {
      win.showPage(id);
      QWidget *cat = win.categoryForPage(id);
      QVERIFY(cat);
      for (auto *b : cat->findChildren<QToolButton *>())
      {
        if (!b->inherits("SARibbonToolButton") || !b->isVisibleTo(cat))
          continue;
        ++total;
        if (b->whatsThis().simplified().isEmpty())
          missing << QStringLiteral("%1/%2 (%3)").arg(id, b->objectName(), b->text().simplified());
      }
    }
    QVERIFY2(total >= 90, qPrintable(QStringLiteral("only %1 visible ribbon buttons seen").arg(total)));
    QVERIFY2(missing.isEmpty(), qPrintable(QStringLiteral("%1 of %2 ribbon buttons have no help:\n%3")
                                               .arg(missing.size()).arg(total).arg(missing.join(QLatin1Char('\n')))));
  }

  // C（通用门控接入既有 QAction，必须是本文件最后一个用例：它会开关 AppContext 里的工程）：
  // 没有打开工程时导入必然失败（DataImportService「project dir is not set」），界面却先弹文件
  // 对话框、选完文件才报错。ToolAvailabilityService 的全局门控把这五个导入命令置灰并写明原因；
  // 工程打开后放开，关闭后再置灰。其余命令（刷新列表）不受影响。
  void importCommandsAreGatedUntilProjectOpens()
  {
    QgisCanvasController canvas(m_ctx);
    PaleoMainWindow win(&canvas, m_ctx->projectSvc(), m_ctx->layerSvc(), m_ctx->toolSvc(),
                        m_ctx->selection());
    win.attachWorkflows(m_ctx->predictionWf(), m_ctx->constraintWf(), m_ctx->compositionWf(),
                        m_ctx->validationWf(), m_ctx->importSvc(), m_ctx->seismicLink(),
                        m_ctx->processingSvc(), m_ctx->store(), m_ctx->editingSvc(),
                        m_ctx->layoutSvc(), m_ctx->taskSvc());
    win.show();
    win.showPage(QStringLiteral("data"));
    QWidget *cat = win.categoryForPage(QStringLiteral("data"));
    QVERIFY(cat);
    const QStringList names{QStringLiteral("ribbonImportFolder"), QStringLiteral("ribbonImportWells"),
                            QStringLiteral("ribbonImportWellLogs"), QStringLiteral("ribbonImportSeismic"),
                            QStringLiteral("ribbonImportBoundary")};
    const auto button = [cat](const QString &name) -> QToolButton * {
      for (auto *b : cat->findChildren<QToolButton *>())
        if (b->objectName() == name)
          return b;
      return nullptr;
    };
    const QStringList pageNames{QStringLiteral("importFolder"), QStringLiteral("importWells"),
                                QStringLiteral("importWellLogs"), QStringLiteral("importSeismic"),
                                QStringLiteral("importBoundary")};
    const auto expectGated = [&](bool gated, const char *phase) {
      for (const QString &n : names)
      {
        QToolButton *b = button(n);
        QVERIFY2(b, qPrintable(QStringLiteral("%1: ribbon button %2 not found").arg(QLatin1String(phase), n)));
        if (gated)
        {
          QVERIFY2(!b->isEnabled(), qPrintable(QStringLiteral("%1: %2 must be disabled").arg(QLatin1String(phase), n)));
          QVERIFY2(b->toolTip().contains(QStringLiteral("工程")),
                   qPrintable(QStringLiteral("%1: %2 tooltip must name the reason, got '%3'")
                                  .arg(QLatin1String(phase), n, b->toolTip())));
        }
        else
        {
          QVERIFY2(b->isEnabled(), qPrintable(QStringLiteral("%1: %2 must be enabled").arg(QLatin1String(phase), n)));
          QVERIFY2(!b->toolTip().contains(QStringLiteral("先新建或打开工程")),
                   qPrintable(QStringLiteral("%1: %2 still shows the gate reason").arg(QLatin1String(phase), n)));
        }
      }
      // 页内导入按钮是同一入口的另一份，必须与 ribbon 一起置灰/放开
      for (const QString &n : pageNames)
      {
        QAbstractButton *b = win.findChild<QAbstractButton *>(n);
        QVERIFY2(b, qPrintable(QStringLiteral("%1: page button %2 not found").arg(QLatin1String(phase), n)));
        QVERIFY2(b->isEnabled() != gated,
                 qPrintable(QStringLiteral("%1: page button %2 enabled=%3, expected %4")
                                .arg(QLatin1String(phase), n).arg(b->isEnabled()).arg(!gated)));
        if (gated)
          QVERIFY2(b->toolTip().contains(QStringLiteral("工程")),
                   qPrintable(QStringLiteral("%1: page %2 tooltip must name the reason, got '%3'")
                                  .arg(QLatin1String(phase), n, b->toolTip())));
      }
    };
    QToolButton *refresh = button(QStringLiteral("ribbonRefreshList"));
    QVERIFY(refresh);

    expectGated(true, "no project");
    QVERIFY2(refresh->isEnabled(), "unrelated command must stay enabled without a project");

    QTemporaryDir dir;
    QVERIFY(m_ctx->projectSvc()->createProject(dir.filePath(QStringLiteral("gate.qgz"))));
    QTest::qWait(60);
    expectGated(false, "project open");
    QVERIFY(refresh->isEnabled());

    m_ctx->projectSvc()->closeProject();
    QTest::qWait(60);
    expectGated(true, "project closed");
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
  TestShellChrome tc(&ctx);
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_shell_chrome.moc"
