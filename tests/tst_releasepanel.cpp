#include <QtTest>
#include <QTemporaryDir>
#include <QComboBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTreeWidget>

#include "../src/ui/releasepanel.h"

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
    manifest = {decl(QStringLiteral("facies.T1")), decl(QStringLiteral("faults.T1"))};
    createBtn->click();
    manifest = {decl(QStringLiteral("facies.T1")), decl(QStringLiteral("grid.T1"))};
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

    auto *list = panel.findChild<QTreeWidget *>(QStringLiteral("releaseList"));
    QCOMPARE(list->topLevelItemCount(), 0);
  }
};

QTEST_MAIN(TestReleasePanel)
#include "tst_releasepanel.moc"
