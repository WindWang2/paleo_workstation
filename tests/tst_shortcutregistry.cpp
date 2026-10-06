// 方向63：services/shortcutregistry 单测——注册校验 / 查重（歧义·同上下文·遮蔽）
// / 上下文包含 / 上下文优先级解析。纯数据层，无 QtWidgets、无主窗。
#include <QtTest>

#include "../src/services/shortcutregistry.h"

using paleo::shortcuts::Binding;
using paleo::shortcuts::ShortcutConflict;
using paleo::shortcuts::ShortcutEntry;
using paleo::shortcuts::ShortcutRegistry;

namespace
{
ShortcutEntry entry(const QString &id, const QKeySequence &key, const QString &context,
                    Binding binding = Binding::Shortcut,
                    Qt::ShortcutContext qtContext = Qt::WindowShortcut)
{
  ShortcutEntry e;
  e.id = id;
  e.key = key;
  e.context = context;
  e.binding = binding;
  e.qtContext = qtContext;
  e.group = QStringLiteral("g");
  e.description = QStringLiteral("描述");
  e.sourcePanel = QStringLiteral("面板");
  return e;
}

QStringList conflictPairs(const QList<ShortcutConflict> &list)
{
  QStringList out;
  for (const ShortcutConflict &c : list)
    out << c.first.id + QLatin1Char('|') + c.second.id;
  return out;
}
} // namespace

class TestShortcutRegistry : public QObject
{
  Q_OBJECT
private slots:
  void registerValidates()
  {
    ShortcutRegistry reg;
    QString err;
    QVERIFY(reg.registerEntry(entry("a", QKeySequence("Ctrl+A"), "main"), &err));
    QCOMPARE(reg.size(), 1);
    QVERIFY(!reg.registerEntry(entry("a", QKeySequence("Ctrl+B"), "main"), &err));
    QVERIFY(err.contains(QStringLiteral("duplicate id")));
    QVERIFY(!reg.registerEntry(entry("", QKeySequence("Ctrl+B"), "main"), &err));
    QVERIFY(!reg.registerEntry(entry("b", QKeySequence(), "main"), &err));
    QVERIFY(err.contains(QStringLiteral("empty key")));
    QVERIFY(!reg.registerEntry(entry("c", QKeySequence("Ctrl+C"), ""), &err));
    ShortcutEntry noGroup = entry("d", QKeySequence("Ctrl+D"), "main");
    noGroup.group.clear();
    QVERIFY(!reg.registerEntry(noGroup, &err));
    ShortcutEntry noDesc = entry("e", QKeySequence("Ctrl+E"), "main");
    noDesc.description = QStringLiteral("  ");
    QVERIFY(!reg.registerEntry(noDesc, &err));
    QCOMPARE(reg.size(), 1);
    QVERIFY(reg.contains("a"));
    QCOMPARE(reg.key("a"), QKeySequence("Ctrl+A"));
    QVERIFY(reg.entry("missing").id.isEmpty());
    QVERIFY(reg.key("missing").isEmpty());
  }

  void contextCoversBySegment()
  {
    QVERIFY(ShortcutRegistry::contextCovers("main", "main"));
    QVERIFY(ShortcutRegistry::contextCovers("main", "main/data"));
    QVERIFY(ShortcutRegistry::contextCovers("main", "main/data/assets"));
    QVERIFY(!ShortcutRegistry::contextCovers("main/data", "main"));
    QVERIFY(!ShortcutRegistry::contextCovers("main", "mainx"));        // 按段，不按前缀串
    QVERIFY(!ShortcutRegistry::contextCovers("main/data", "main/database"));
    QVERIFY(!ShortcutRegistry::contextCovers("", "main"));
    QVERIFY(ShortcutRegistry::contextsRelated("main/data/assets", "main"));
    QVERIFY(!ShortcutRegistry::contextsRelated("main/data", "main/map"));
    QCOMPARE(ShortcutRegistry::contextDepth("main/data/assets"), 3);
    QCOMPARE(ShortcutRegistry::contextDepth(""), 0);
  }

  // 同上下文同键序 = 冲突；作用域重叠的两条 Qt 快捷键（Ctrl+K 历史事故：主窗
  // 定位器 + 数据页命令面板）= 歧义冲突；兄弟作用域/不同窗口不冲突。
  void conflictsDetected()
  {
    ShortcutRegistry reg;
    QVERIFY(reg.registerEntry(entry("locator", QKeySequence("Ctrl+K"), "main")));
    QVERIFY(reg.registerEntry(entry("palette", QKeySequence("Ctrl+K"), "main/data")));
    QVERIFY(reg.registerEntry(entry("same1", QKeySequence("F5"), "main/map")));
    QVERIFY(reg.registerEntry(entry("same2", QKeySequence("F5"), "main/map")));
    QVERIFY(reg.registerEntry(entry("sib1", QKeySequence("F6"), "main/data")));
    QVERIFY(reg.registerEntry(entry("sib2", QKeySequence("F6"), "main/map")));
    QVERIFY(reg.registerEntry(entry("win1", QKeySequence("Ctrl+Z"), "main/data")));
    QVERIFY(reg.registerEntry(entry("win2", QKeySequence("Ctrl+Z"), "layout-designer")));
    QVERIFY(reg.registerEntry(entry("kh1", QKeySequence("Esc"), "main/map/draw", Binding::KeyHandler)));
    QVERIFY(reg.registerEntry(entry("kh2", QKeySequence("Esc"), "main/map/draw", Binding::KeyHandler)));
    QVERIFY(reg.registerEntry(entry("kh3", QKeySequence("Esc"), "main/map/edit", Binding::KeyHandler)));
    QVERIFY(reg.registerEntry(entry("mixS", QKeySequence("F7"), "main/data")));
    QVERIFY(reg.registerEntry(entry("mixK", QKeySequence("F7"), "main/data", Binding::KeyHandler)));

    const QList<ShortcutConflict> found = reg.conflicts();
    const QStringList pairs = conflictPairs(found);
    QCOMPARE(pairs.size(), 4);
    QVERIFY(pairs.contains("locator|palette"));
    QVERIFY(pairs.contains("same1|same2"));
    QVERIFY(pairs.contains("kh1|kh2"));
    QVERIFY(pairs.contains("mixS|mixK"));
    for (const ShortcutConflict &c : found)
    {
      if (c.first.id == QLatin1String("locator"))
        QCOMPARE(c.kind, ShortcutConflict::Kind::Ambiguous);
      if (c.first.id == QLatin1String("kh1"))
        QCOMPARE(c.kind, ShortcutConflict::Kind::Duplicate);
      QVERIFY(!c.describe().isEmpty());
    }
  }

  void applicationShortcutCoversEveryWindow()
  {
    ShortcutRegistry reg;
    QVERIFY(reg.registerEntry(entry("app", QKeySequence("Ctrl+Q"), "main", Binding::Shortcut,
                                    Qt::ApplicationShortcut)));
    QVERIFY(reg.registerEntry(entry("designer", QKeySequence("Ctrl+Q"), "layout-designer")));
    QCOMPARE(conflictPairs(reg.conflicts()), QStringList{"app|designer"});
  }

  // 外层快捷键 vs 内层焦点内处理器：只告警（遮蔽），不计冲突。
  void shadowsAreWarningsNotConflicts()
  {
    ShortcutRegistry reg;
    QVERIFY(reg.registerEntry(entry("layers.remove", QKeySequence(Qt::Key_Delete), "main")));
    QVERIFY(reg.registerEntry(entry("assets.remove", QKeySequence(Qt::Key_Delete), "main/data/assets",
                                    Binding::KeyHandler)));
    QVERIFY(reg.registerEntry(entry("designer.delete", QKeySequence(Qt::Key_Delete), "layout-designer")));
    QVERIFY(reg.conflicts().isEmpty());
    const QList<ShortcutConflict> sh = reg.shadows();
    QCOMPARE(conflictPairs(sh), QStringList{"layers.remove|assets.remove"});
    QCOMPARE(sh.front().kind, ShortcutConflict::Kind::Shadowed);
  }

  void resolveHonoursContextPriority()
  {
    ShortcutRegistry reg;
    // 焦点内处理器：深者优先
    QVERIFY(reg.registerEntry(entry("outerK", QKeySequence(Qt::Key_Up), "main/section", Binding::KeyHandler)));
    QVERIFY(reg.registerEntry(entry("innerK", QKeySequence(Qt::Key_Up), "main/section/canvas",
                                    Binding::KeyHandler)));
    QCOMPARE(reg.resolve(QKeySequence(Qt::Key_Up), "main/section/canvas")->id, QStringLiteral("innerK"));
    QCOMPARE(reg.resolve(QKeySequence(Qt::Key_Up), "main/section")->id, QStringLiteral("outerK"));
    QVERIFY(!reg.resolve(QKeySequence(Qt::Key_Up), "main/map").has_value()); // 兄弟作用域不参与
    QVERIFY(!reg.resolve(QKeySequence(Qt::Key_Up), "main").has_value());     // 内层不向外生效

    // Qt 快捷键先于任何焦点内处理器（事件顺序：ShortcutOverride → 快捷键映射 → keyPress）
    QVERIFY(reg.registerEntry(entry("winS", QKeySequence(Qt::Key_Delete), "main")));
    QVERIFY(reg.registerEntry(entry("deepK", QKeySequence(Qt::Key_Delete), "main/data/assets",
                                    Binding::KeyHandler)));
    QCOMPARE(reg.resolve(QKeySequence(Qt::Key_Delete), "main/data/assets")->id, QStringLiteral("winS"));

    // 重叠的两条快捷键 → 歧义，一条都不响应
    QVERIFY(reg.registerEntry(entry("s1", QKeySequence("Ctrl+K"), "main")));
    QVERIFY(reg.registerEntry(entry("s2", QKeySequence("Ctrl+K"), "main/data")));
    QVERIFY(!reg.resolve(QKeySequence("Ctrl+K"), "main/data").has_value());
    QCOMPARE(reg.resolve(QKeySequence("Ctrl+K"), "main")->id, QStringLiteral("s1"));

    // 同深度焦点内处理器并列 → 无法判定
    QVERIFY(reg.registerEntry(entry("t1", QKeySequence(Qt::Key_F9), "main/a", Binding::KeyHandler)));
    QVERIFY(reg.registerEntry(entry("t2", QKeySequence(Qt::Key_F9), "main/a", Binding::KeyHandler)));
    QVERIFY(!reg.resolve(QKeySequence(Qt::Key_F9), "main/a/b").has_value());
  }

  void groupsKeepFirstRegistrationOrder()
  {
    ShortcutRegistry reg;
    ShortcutEntry a = entry("a", QKeySequence("F1"), "main");
    a.group = QStringLiteral("help");
    ShortcutEntry b = entry("b", QKeySequence("F2"), "main");
    b.group = QStringLiteral("data");
    ShortcutEntry c = entry("c", QKeySequence("F3"), "main", Binding::KeyHandler);
    c.group = QStringLiteral("help");
    QVERIFY(reg.registerEntry(a));
    QVERIFY(reg.registerEntry(b));
    QVERIFY(reg.registerEntry(c));
    QCOMPARE(reg.groups(), (QStringList{"help", "data"}));
    QCOMPARE(reg.entriesInGroup("help").size(), 2);
    QCOMPARE(reg.entriesIn(Binding::KeyHandler).size(), 1);
  }
};

QTEST_GUILESS_MAIN(TestShortcutRegistry)
#include "tst_shortcutregistry.moc"
