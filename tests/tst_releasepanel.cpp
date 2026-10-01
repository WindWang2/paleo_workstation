#include <QtTest>
#include <QTemporaryDir>
#include <QComboBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTreeWidget>

#include "../src/ui/releasepanel.h"
#include "uipolish_capture.h"

// ReleasePanel is a pure-Qt shell over ReleaseStore — providers inject the
// project store path and the current manifest snapshot, so the panel is fully
// testable without a live QgsProject.
class TestReleasePanel : public QObject
{
  Q_OBJECT

private:
  static LayerDeclaration decl(const QString &id)
  {
    LayerDeclaration d;
    d.layerId = id;
    d.horizon = QStringLiteral("T1");
    d.type = QStringLiteral("vector");
    d.source = QStringLiteral("memory|%1").arg(id);
    d.group = QStringLiteral("04_SingleFactor");
    return d;
  }

private slots:

  void createReleaseSnapshotsManifest()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString db = dir.filePath(QStringLiteral("meta.sqlite"));

    ReleasePanel panel;
    QVector<LayerDeclaration> manifest = {decl(QStringLiteral("facies.T1"))};
    panel.setProviders([db] { return db; },
                       [&manifest] { return manifest; });

    QSignalSpy createdSpy(&panel, &ReleasePanel::releaseCreated);
    panel.findChild<QLineEdit *>(QStringLiteral("releaseNameEdit"))
        ->setText(QStringLiteral("v1"));
    panel.findChild<QPushButton *>(QStringLiteral("createReleaseButton"))->click();

    QCOMPARE(createdSpy.count(), 1);
    QCOMPARE(createdSpy.at(0).at(0).toString(), QStringLiteral("rel-1"));

    auto *list = panel.findChild<QTreeWidget *>(QStringLiteral("releaseList"));
    QCOMPARE(list->topLevelItemCount(), 1);
    QCOMPARE(list->topLevelItem(0)->text(0), QStringLiteral("rel-1"));
    QCOMPARE(list->topLevelItem(0)->text(1), QStringLiteral("v1"));
    QCOMPARE(list->topLevelItem(0)->text(3), QStringLiteral("1"));

    // Diff combos repopulated with the new release.
    QCOMPARE(panel.findChild<QComboBox *>(QStringLiteral("diffA"))->count(), 1);
  }

  void diffFlow()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString db = dir.filePath(QStringLiteral("meta.sqlite"));

    ReleasePanel panel;
    QVector<LayerDeclaration> manifest;
    panel.setProviders([db] { return db; },
                       [&manifest] { return manifest; });

    auto *createBtn = panel.findChild<QPushButton *>(QStringLiteral("createReleaseButton"));
    auto *nameEdit = panel.findChild<QLineEdit *>(QStringLiteral("releaseNameEdit"));
    manifest = {decl(QStringLiteral("facies.T1")), decl(QStringLiteral("faults.T1"))};
    nameEdit->setText(QStringLiteral("v1"));
    createBtn->click();
    manifest = {decl(QStringLiteral("facies.T1")), decl(QStringLiteral("grid.T1"))};
    nameEdit->setText(QStringLiteral("v2")); // 空名称被拒（防呆）——测试给真名
    createBtn->click();

    auto *comboA = panel.findChild<QComboBox *>(QStringLiteral("diffA"));
    auto *comboB = panel.findChild<QComboBox *>(QStringLiteral("diffB"));
    QCOMPARE(comboA->count(), 2);
    QCOMPARE(comboB->count(), 2);
    comboA->setCurrentIndex(0);
    comboB->setCurrentIndex(1);
    panel.findChild<QPushButton *>(QStringLiteral("diffButton"))->click();

    auto *out = panel.findChild<QListWidget *>(QStringLiteral("diffOutput"));
    QStringList rows;
    for (int i = 0; i < out->count(); ++i)
      rows << out->item(i)->text();
    QVERIFY(rows.contains(QStringLiteral("+ grid.T1")));
    QVERIFY(rows.contains(QStringLiteral("- faults.T1")));
  }

  void emptyAndNoProjectGuards()
  {
    ReleasePanel panel;
    panel.refresh(); // providers unset — must not crash

    QSignalSpy statusSpy(&panel, &ReleasePanel::statusMessage);
    // Empty db path → create reports via statusMessage, no release emitted.
    panel.setProviders([] { return QString(); },
                       [] { return QVector<LayerDeclaration>(); });
    panel.findChild<QPushButton *>(QStringLiteral("createReleaseButton"))->click();
    QCOMPARE(statusSpy.count(), 1);

    // 无工程时列表是空态指引行（不可交互），不是白板。
    auto *list = panel.findChild<QTreeWidget *>(QStringLiteral("releaseList"));
    QCOMPARE(list->topLevelItemCount(), 1);
    QCOMPARE(list->topLevelItem(0)->flags(), Qt::NoItemFlags);
    QVERIFY(list->topLevelItem(0)->text(0).contains(QString::fromUtf8("打开工程")));
  }

  void emptyNameRejected()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString db = dir.filePath(QStringLiteral("meta.sqlite"));

    ReleasePanel panel;
    QVector<LayerDeclaration> manifest = {decl(QStringLiteral("facies.T1"))};
    panel.setProviders([db] { return db; },
                       [&manifest] { return manifest; });

    // 空名称 → 拒绝 + statusMessage 提示，不创建任何发布。
    QSignalSpy statusSpy(&panel, &ReleasePanel::statusMessage);
    QSignalSpy createdSpy(&panel, &ReleasePanel::releaseCreated);
    panel.findChild<QPushButton *>(QStringLiteral("createReleaseButton"))->click();
    QCOMPARE(createdSpy.count(), 0);
    QCOMPARE(statusSpy.count(), 1);
    QVERIFY(statusSpy.at(0).at(0).toString().contains(QString::fromUtf8("名称不能为空")));
  }

  // goal/ui-experience-polish：发布树键盘可达（↓ 选中首行——键盘也能走
  // 发布/对比流）。
  void keyboardNavigationOnReleaseList()
  {
    QTemporaryDir dir;
    const QString db = dir.filePath(QStringLiteral("meta.sqlite"));
    ReleasePanel panel;
    QVector<LayerDeclaration> manifest = {decl(QStringLiteral("facies.T1"))};
    panel.setProviders([db] { return db; },
                       [&manifest] { return manifest; });
    panel.findChild<QLineEdit *>(QStringLiteral("releaseNameEdit"))
        ->setText(QStringLiteral("v1"));
    panel.findChild<QPushButton *>(QStringLiteral("createReleaseButton"))->click();
    auto *list = panel.findChild<QTreeWidget *>(QStringLiteral("releaseList"));
    QVERIFY(list && list->topLevelItemCount() == 1);
    panel.show();
    QTest::qWaitForWindowExposed(&panel);
    list->setFocus();
    // 显式清起点（窗口激活时序下树会自动选首行——offscreen 不定）。
    list->setCurrentItem(nullptr);
    QCOMPARE(list->currentIndex().row(), -1);
    QTest::keyClick(list, Qt::Key_Down);
    QCOMPARE(list->currentIndex().row(), 0);
  }

  // goal/ui-experience-polish：发布树 + 表单的修前/修后截图证据。
  void captureEvidence()
  {
    QTemporaryDir dir;
    const QString db = dir.filePath(QStringLiteral("meta.sqlite"));
    ReleasePanel panel;
    QVector<LayerDeclaration> manifest = {decl(QStringLiteral("facies.T1"))};
    panel.setProviders([db] { return db; },
                       [&manifest] { return manifest; });
    panel.findChild<QLineEdit *>(QStringLiteral("releaseNameEdit"))
        ->setText(QStringLiteral("v1"));
    panel.findChild<QPushButton *>(QStringLiteral("createReleaseButton"))->click();
    uipolish::capturePanel(&panel, QStringLiteral("releasepanel"));
  }
};

QTEST_MAIN(TestReleasePanel)
#include "tst_releasepanel.moc"
