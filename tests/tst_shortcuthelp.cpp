// 方向63：快捷键中央登记处 + 总表对话框 + 帮助面（offscreen，纯 Qt Widgets）。
//   - 目录健康：零被拒条目、零冲突、条目数 ≥44、遮蔽清单钉死
//   - 变异：把命令面板改回 Ctrl+K（历史事故）/ 同上下文重复登记 → 必红
//   - R0 收编：f130fb2 散装 27 处键位逐条比对（键序/上下文/绑定类型不变）
//   - 源码闸：src/ 内除 ui/shortcuts 外零 `new QShortcut` / `setShortcut(`
//   - 绑定行为：经注册表建的 QShortcut/QAction 在 offscreen 下真实触发
//   - 总表：条目数 = 注册数、搜索、复制单条、「当前位置」过滤
//   - 帮助面：三入口可达、F1 开总表、Shift+F1 进出「这是什么？」
#include <QtTest>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDirIterator>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QShortcut>
#include <QSignalSpy>
#include <QTreeWidget>
#include <QWhatsThis>

#include "../src/ui/help/helpsurface.h"
#include "../src/ui/help/whatsthiscatalog.h"
#include "../src/ui/pages/pageshared.h"
#include "../src/ui/shortcuts/shortcutcatalog.h"
#include "../src/ui/shortcuts/shortcutsheetdialog.h"

using namespace paleo::shortcuts;

namespace
{
QStringList describeAll(const QList<ShortcutConflict> &list)
{
  QStringList out;
  for (const ShortcutConflict &c : list)
    out << c.describe();
  return out;
}

ShortcutRegistry registryFrom(const QList<ShortcutEntry> &entries)
{
  ShortcutRegistry reg;
  for (const ShortcutEntry &e : entries)
    reg.registerEntry(e);
  return reg;
}

void showActive(QWidget *w)
{
  w->resize(320, 200);
  w->show();
  w->activateWindow();
  QVERIFY(QTest::qWaitForWindowActive(w));
}
} // namespace

class TestShortcutHelp : public QObject
{
  Q_OBJECT
private slots:
  void cleanup()
  {
    if (QWhatsThis::inWhatsThisMode())
      QWhatsThis::leaveWhatsThisMode();
  }

  // ---- 目录健康 ----
  void catalogIsHealthy()
  {
    QVERIFY2(catalogErrors().isEmpty(), qPrintable(catalogErrors().join('\n')));
    QCOMPARE(registry().size(), catalogEntries().size());
    // Oracle 1：收编完整——条目数 ≥44（f130fb2 散装 27 处绑定 + 本方向新增 3 处 + 焦点内按键说明）。
    QVERIFY2(registry().size() >= 44, qPrintable(QString::number(registry().size())));
    QVERIFY(registry().entriesIn(Binding::Shortcut).size() >= 30); // 27 + F1 / Shift+F1 / 总表复制
    for (const ShortcutEntry &e : registry().entries())
    {
      QVERIFY2(!displayGroup(e.group).isEmpty() && displayGroup(e.group) != e.group,
               qPrintable(QStringLiteral("group without title: ") + e.group));
      QVERIFY2(!e.sourcePanel.isEmpty(), qPrintable(e.id));
      QVERIFY2(!e.origin.isEmpty(), qPrintable(e.id));
    }
    qInfo("registry: %d entries (%d bound, %d key-handler)", int(registry().size()),
          int(registry().entriesIn(Binding::Shortcut).size()),
          int(registry().entriesIn(Binding::KeyHandler).size()));
  }

  // Oracle 2：当前注册零冲突。
  void catalogHasZeroConflicts()
  {
    const QList<ShortcutConflict> found = registry().conflicts();
    QVERIFY2(found.isEmpty(), qPrintable(describeAll(found).join('\n')));
    QCOMPARE(logConflictsOnce(), 0);
  }

  // 遮蔽（外层窗口快捷键 vs 内层焦点内处理器）只告警；清单钉死——新增遮蔽
  // 必须进 review（ledger「遮蔽告警」节 + TODOS 递延核实）。
  void knownShadowsArePinned()
  {
    QStringList pairs;
    for (const ShortcutConflict &c : registry().shadows())
      pairs << c.first.id + QLatin1Char('|') + c.second.id;
    pairs.sort();
    QCOMPARE(pairs, (QStringList{QStringLiteral("layers.remove|data.assets.remove"),
                                 QStringLiteral("layers.remove|map.vertex.delete")}));
  }

  // Oracle 2 变异：故意注册重复键能被打红。
  void mutationDuplicateKeyGoesRed()
  {
    // (a) 历史事故复现：数据页命令面板改回 Ctrl+K → 与主窗定位器歧义。
    QList<ShortcutEntry> entries = catalogEntries();
    bool mutated = false;
    for (ShortcutEntry &e : entries)
      if (e.id == QLatin1String("data.palette.open"))
      {
        e.key = QKeySequence(QStringLiteral("Ctrl+K"));
        mutated = true;
      }
    QVERIFY(mutated);
    const QList<ShortcutConflict> a = registryFrom(entries).conflicts();
    QCOMPARE(a.size(), 1);
    QCOMPARE(a.front().kind, ShortcutConflict::Kind::Ambiguous);
    QCOMPARE(QSet<QString>({a.front().first.id, a.front().second.id}),
             QSet<QString>({QStringLiteral("main.locator.focus"), QStringLiteral("data.palette.open")}));

    // (b) 同上下文再登记一条焦点内 Esc → Duplicate。
    entries = catalogEntries();
    ShortcutEntry dup = registry().entry(QStringLiteral("map.draw.cancel"));
    dup.id = QStringLiteral("mutation.draw.cancel2");
    entries << dup;
    const QList<ShortcutConflict> b = registryFrom(entries).conflicts();
    QCOMPARE(b.size(), 1);
    QCOMPARE(b.front().kind, ShortcutConflict::Kind::Duplicate);

    // (c) 主窗再挂一条 F1 → 与帮助总表歧义。
    entries = catalogEntries();
    ShortcutEntry f1 = registry().entry(QStringLiteral("main.locator.focus"));
    f1.id = QStringLiteral("mutation.f1");
    f1.key = registry().key(QStringLiteral("main.help.shortcuts"));
    f1.context = QStringLiteral("main/seismic-section");
    entries << f1;
    QCOMPARE(registryFrom(entries).conflicts().size(), 1);
  }

  // ---- R0 收编：f130fb2 散装键位逐条比对（行为红线：键序/上下文不变）----
  void r0SitesPreserved_data()
  {
    QTest::addColumn<QString>("id");
    QTest::addColumn<QKeySequence>("key");
    // 每行 = f130fb2 上的原字面量（文件:行见 ledger R0 清单）。
    const QStringList pages = paleo::pagesinternal::kPageIds;
    for (int i = 0; i < pages.size(); ++i)
      QTest::newRow(qPrintable(QStringLiteral("main.page.") + pages.at(i)))
          << QStringLiteral("main.page.") + pages.at(i)
          << QKeySequence(QStringLiteral("Ctrl+%1").arg(i + 1));
    QTest::newRow("next") << "main.page.next" << QKeySequence(QStringLiteral("Ctrl+Tab"));
    QTest::newRow("prev") << "main.page.prev" << QKeySequence(QStringLiteral("Ctrl+Shift+Tab"));
    QTest::newRow("locator") << "main.locator.focus" << QKeySequence(QStringLiteral("Ctrl+K"));
    QTest::newRow("save") << "main.project.save" << QKeySequence(QKeySequence::Save);
    QTest::newRow("crossplot") << "main.crossplot.open" << QKeySequence(QStringLiteral("Ctrl+Alt+X"));
    QTest::newRow("palette") << "data.palette.open" << QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P);
    QTest::newRow("paletteUp") << "data.palette.up" << QKeySequence(Qt::Key_Up);
    QTest::newRow("paletteDown") << "data.palette.down" << QKeySequence(Qt::Key_Down);
    QTest::newRow("undo") << "data.list.undo" << QKeySequence(Qt::CTRL | Qt::Key_Z);
    QTest::newRow("redo") << "data.list.redo" << QKeySequence(Qt::CTRL | Qt::Key_Y);
    QTest::newRow("invert") << "data.list.invert" << QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A);
    QTest::newRow("selFiltered") << "data.list.selectFiltered"
                                 << QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F);
    QTest::newRow("rename") << "data.list.rename" << QKeySequence(Qt::Key_F2);
    QTest::newRow("shortcuts") << "data.list.shortcuts" << QKeySequence(Qt::Key_Question);
    QTest::newRow("focusSearch") << "data.list.focusSearch" << QKeySequence(Qt::CTRL | Qt::Key_F);
    QTest::newRow("vim") << "data.list.vimToggle" << QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_V);
    QTest::newRow("layoutUndo") << "layout.undo" << QKeySequence(QKeySequence::StandardKey::Undo);
    QTest::newRow("layoutRedo") << "layout.redo" << QKeySequence(QKeySequence::StandardKey::Redo);
    QTest::newRow("layoutDelete") << "layout.delete" << QKeySequence(Qt::Key_Delete);
    QTest::newRow("gotoDepth") << "wellcomposite.gotoDepth" << QKeySequence(QStringLiteral("Ctrl+G"));
    QTest::newRow("layersRemove") << "layers.remove" << QKeySequence(Qt::Key_Delete);
  }

  void r0SitesPreserved()
  {
    QFETCH(QString, id);
    QFETCH(QKeySequence, key);
    const ShortcutEntry e = registry().entry(id);
    QVERIFY2(!e.id.isEmpty(), qPrintable(id));
    QCOMPARE(e.key, key);
    QCOMPARE(e.binding, Binding::Shortcut);
    QCOMPARE(e.qtContext, Qt::WindowShortcut); // 散装时全是 QShortcut/QAction 缺省上下文
  }

  // 源码闸：全仓 src/ 只有 ui/shortcuts 能落键序（防散装回潮）。
  void noScatteredBindingsInSource()
  {
    const QString root = QStringLiteral(PALEO_SOURCE_DIR) + QStringLiteral("/src");
    QVERIFY2(QDir(root).exists(), qPrintable(root));
    const QRegularExpression bad(
        QStringLiteral(R"((\bnew\s+QShortcut\b|(->|\.)\s*setShortcuts?\s*\())"));
    QStringList hits;
    QDirIterator it(root, {QStringLiteral("*.cpp"), QStringLiteral("*.h")}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext())
    {
      const QString path = it.next();
      if (path.contains(QStringLiteral("/src/ui/shortcuts/")))
        continue;
      QFile f(path);
      QVERIFY(f.open(QIODevice::ReadOnly));
      int lineNo = 0;
      for (const QByteArray &raw : f.readAll().split('\n'))
      {
        ++lineNo;
        QString line = QString::fromUtf8(raw);
        const int comment = line.indexOf(QStringLiteral("//"));
        if (comment >= 0)
          line.truncate(comment);
        if (bad.match(line).hasMatch())
          hits << QStringLiteral("%1:%2: %3").arg(path.mid(root.size() + 1)).arg(lineNo).arg(line.trimmed());
      }
    }
    QVERIFY2(hits.isEmpty(), qPrintable(hits.join('\n')));
  }

  // ---- 绑定：键序/上下文/登记 id 落到对象上 ----
  void bindersCarryRegistryKey()
  {
    QWidget host;
    QShortcut *sc = bindShortcut(QStringLiteral("main.locator.focus"), &host);
    QCOMPARE(sc->parent(), &host);
    QCOMPARE(sc->key(), QKeySequence(QStringLiteral("Ctrl+K")));
    QCOMPARE(sc->context(), Qt::WindowShortcut);
    QCOMPARE(sc->property(kShortcutIdProperty).toString(), QStringLiteral("main.locator.focus"));

    QAction save(&host);
    bindAction(QStringLiteral("main.project.save"), &save);
    QCOMPARE(save.shortcut(), QKeySequence(QKeySequence::Save));
    QCOMPARE(save.property(kShortcutIdProperty).toString(), QStringLiteral("main.project.save"));
    setActionShortcutActive(QStringLiteral("main.project.save"), &save, false); // 地层对比页让出
    QVERIFY(save.shortcut().isEmpty());
    setActionShortcutActive(QStringLiteral("main.project.save"), &save, true);
    QCOMPARE(save.shortcut(), QKeySequence(QKeySequence::Save));
  }

  // Oracle 1 抽查：经注册表建的绑定在 offscreen 下按键真实触发（主窗级回归在
  // tst_shortcuts_shell，这里钉住「注册表 → Qt 绑定」这一环）。
  void boundShortcutsFireOffscreen_data()
  {
    QTest::addColumn<QString>("id");
    QTest::addColumn<int>("key");
    QTest::addColumn<int>("modifiers");
    QTest::newRow("Ctrl+1") << "main.page.data" << int(Qt::Key_1) << int(Qt::ControlModifier);
    QTest::newRow("Ctrl+K") << "main.locator.focus" << int(Qt::Key_K) << int(Qt::ControlModifier);
    QTest::newRow("Ctrl+Tab") << "main.page.next" << int(Qt::Key_Tab) << int(Qt::ControlModifier);
    QTest::newRow("Ctrl+Shift+P") << "data.palette.open" << int(Qt::Key_P)
                                  << int(Qt::ControlModifier | Qt::ShiftModifier);
    QTest::newRow("Ctrl+G") << "wellcomposite.gotoDepth" << int(Qt::Key_G) << int(Qt::ControlModifier);
  }

  void boundShortcutsFireOffscreen()
  {
    QFETCH(QString, id);
    QFETCH(int, key);
    QFETCH(int, modifiers);
    QWidget win;
    QShortcut *sc = bindShortcut(id, &win);
    QSignalSpy spy(sc, &QShortcut::activated);
    showActive(&win);
    QTest::keyClick(&win, Qt::Key(key), Qt::KeyboardModifiers(modifiers));
    QCOMPARE(spy.count(), 1);
  }

  void boundActionFiresOffscreen()
  {
    QWidget win;
    auto *remove = new QAction(QStringLiteral("remove"), &win);
    bindAction(QStringLiteral("layers.remove"), remove);
    win.addAction(remove);
    QSignalSpy spy(remove, &QAction::triggered);
    showActive(&win);
    QTest::keyClick(&win, Qt::Key_Delete);
    QCOMPARE(spy.count(), 1);
  }

  // ---- 总表（Oracle 3）----
  void sheetListsEveryRegisteredEntry()
  {
    ShortcutSheetDialog dlg(registry());
    QCOMPARE(dlg.entryCount(), registry().size());
    QCOMPARE(dlg.visibleEntryCount(), registry().size());
    QCOMPARE(dlg.tree()->topLevelItemCount(), registry().groups().size());
    auto *count = dlg.findChild<QLabel *>(QStringLiteral("shortcutCountLabel"));
    QVERIFY(count && count->text().contains(QString::number(registry().size())));
    QVERIFY(!dlg.isModal());
  }

  void sheetSearchFilters()
  {
    ShortcutSheetDialog dlg(registry());
    dlg.setFilterText(QStringLiteral("ctrl+k"));
    QVERIFY(dlg.visibleEntryCount() >= 1);
    QVERIFY(dlg.visibleEntryCount() < dlg.entryCount());
    QVERIFY(dlg.selectEntry(QStringLiteral("main.locator.focus")));
    // 中文功能描述同样可搜
    dlg.setFilterText(QStringLiteral("综合柱状图"));
    QVERIFY(dlg.visibleEntryCount() >= 8);
    dlg.setFilterText(QStringLiteral("zz-no-such-key"));
    QCOMPARE(dlg.visibleEntryCount(), 0);
    dlg.setFilterText(QString());
    QCOMPARE(dlg.visibleEntryCount(), registry().size());
  }

  void sheetCopiesSingleEntry()
  {
    ShortcutSheetDialog dlg(registry());
    QVERIFY(dlg.selectEntry(QStringLiteral("main.locator.focus")));
    const QString text = dlg.copySelected();
    const QStringList cols = text.split(QLatin1Char('\t'));
    QCOMPARE(cols.size(), 3);
    QCOMPARE(cols.at(0), displayKey(QKeySequence(QStringLiteral("Ctrl+K"))));
    QCOMPARE(cols.at(1), displayDescription(registry().entry(QStringLiteral("main.locator.focus"))));
    QCOMPARE(QGuiApplication::clipboard()->text(), text);

    // 复制按钮与总表自身的复制键（也经注册表：sheet.copy）走同一路径。
    QVERIFY(dlg.selectEntry(QStringLiteral("wellcomposite.gotoDepth")));
    auto *button = dlg.findChild<QPushButton *>(QStringLiteral("shortcutCopyButton"));
    QVERIFY(button && button->isEnabled());
    button->click();
    QVERIFY(QGuiApplication::clipboard()->text().startsWith(displayKey(QKeySequence("Ctrl+G"))));
    QGuiApplication::clipboard()->clear();
    showActive(&dlg);
    dlg.tree()->setFocus();
    QTest::keyClick(dlg.tree(), Qt::Key_C, Qt::ControlModifier);
    QVERIFY(QGuiApplication::clipboard()->text().startsWith(displayKey(QKeySequence("Ctrl+G"))));
  }

  void sheetActiveContextFilter()
  {
    ShortcutSheetDialog dlg(registry());
    dlg.setActiveContext(QStringLiteral("main/data/wellcomposite"));
    const QString selected = dlg.selectedEntryId();
    QCOMPARE(selected, QStringLiteral("wellcomposite.gotoDepth")); // 覆盖该位置的最深条目
    dlg.setContextFilterEnabled(true);
    QVERIFY(dlg.visibleEntryCount() < dlg.entryCount());
    // 可见的全是「相关」条目：外层全局键 + 本面板键 + 画布焦点内键
    int composite = 0;
    for (const ShortcutEntry &e : registry().entries())
      if (ShortcutRegistry::contextsRelated(e.context, QStringLiteral("main/data/wellcomposite")))
        ++composite;
    QCOMPARE(dlg.visibleEntryCount(), composite);
    dlg.setActiveContext(QString());
    QCOMPARE(dlg.visibleEntryCount(), dlg.entryCount());
  }

  // ---- 帮助面（Oracle 3/4/5）----
  void helpMenuHasThreeEntries()
  {
    QWidget win;
    paleo::help::HelpSurface help(&win);
    QStringList names;
    for (QAction *a : help.menu()->actions())
      if (!a->isSeparator())
        names << a->objectName();
    QCOMPARE(names, (QStringList{QStringLiteral("helpShortcutsAction"),
                                 QStringLiteral("helpWhatsThisAction"),
                                 QStringLiteral("helpAboutAction")}));
    QCOMPARE(help.shortcutsAction()->shortcut(), registry().key(QStringLiteral("main.help.shortcuts")));
    QCOMPARE(help.whatsThisAction()->shortcut(), registry().key(QStringLiteral("main.help.whatsThis")));
    QVERIFY(win.actions().contains(help.shortcutsAction()));
    QVERIFY(win.actions().contains(help.whatsThisAction()));

    help.aboutAction()->trigger();
    QVERIFY(help.aboutBox() && help.aboutBox()->isVisible());
    QVERIFY(!help.aboutBox()->isModal());
    help.aboutBox()->close();
  }

  void f1OpensSheet()
  {
    QWidget win;
    paleo::help::HelpSurface help(&win);
    showActive(&win);
    QTest::keyClick(&win, Qt::Key_F1);
    QVERIFY(help.shortcutSheet());
    QTRY_VERIFY(help.shortcutSheet()->isVisible());
    QCOMPARE(help.shortcutSheet()->entryCount(), registry().size());
    help.shortcutSheet()->close();
    // 菜单入口同一路径、同一单例
    auto *first = help.shortcutSheet();
    help.shortcutsAction()->trigger();
    QCOMPARE(help.shortcutSheet(), first);
    QVERIFY(first->isVisible());
    first->close();
  }

  void whatsThisModeEntersAndLeaves()
  {
    QWidget win;
    auto *button = new QPushButton(QStringLiteral("x"), &win);
    paleo::help::HelpSurface help(&win);
    showActive(&win);
    QVERIFY(!QWhatsThis::inWhatsThisMode());
    QTest::keyClick(&win, Qt::Key_F1, Qt::ShiftModifier); // Shift+F1
    QVERIFY(QWhatsThis::inWhatsThisMode());
    QTest::keyClick(button, Qt::Key_Escape); // Esc 退出
    QVERIFY(!QWhatsThis::inWhatsThisMode());
    help.whatsThisAction()->trigger(); // 菜单入口进入
    QVERIFY(QWhatsThis::inWhatsThisMode());
    help.whatsThisAction()->trigger(); // 再触发 = 退出
    QVERIFY(!QWhatsThis::inWhatsThisMode());
  }

  // whatsThis 清单自洽（实际回填在 tst_shortcuts_shell 用真面板核对）。
  void whatsThisCatalogIsWellFormed()
  {
    const QList<paleo::help::WhatsThisEntry> entries = paleo::help::whatsThisEntries();
    QVERIFY(entries.size() >= 30);
    QSet<QString> seen;
    for (const auto &e : entries)
    {
      const QString key = QString::fromLatin1(e.scopeClass) + QLatin1Char('/') +
                          QString::fromLatin1(e.objectName);
      QVERIFY2(!seen.contains(key), qPrintable(key));
      seen.insert(key);
      const QString text = paleo::help::whatsThisText(e);
      QVERIFY2(text.endsWith(QChar(0x3002)) || text.endsWith(QChar(0xFF09)), qPrintable(key));
      if (e.shortcutId)
        QVERIFY2(registry().contains(QString::fromLatin1(e.shortcutId)), e.shortcutId);
    }
  }

  void whatsThisRespectsScope()
  {
    // 同名控件不在所属面板子树内 → 不回填（各页都有 runButton）。
    QWidget plain;
    auto *run = new QPushButton(&plain);
    run->setObjectName(QStringLiteral("runButton"));
    QCOMPARE(paleo::help::applyWhatsThis(&plain), 0);
    QVERIFY(run->whatsThis().isEmpty());
  }
};

int main(int argc, char *argv[])
{
  if (qgetenv("QT_QPA_PLATFORM").isEmpty())
    qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  TestShortcutHelp tc;
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_shortcuthelp.moc"
