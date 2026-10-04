#include <QtTest>

#include "../src/catalog/datacatalog.h"
#include "../src/domain/welltopsedit.h"
#include "../src/io/wellfileparsers.h"
#include "../src/ui/welltops/welltopseditordialog.h"
#include "../src/ui/welltops/welltopsmergedialog.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>

// 方向 32 UI 面（offscreen）：编辑器对话框的 CRUD/脏行标记/校验阻断/保存
// round-trip、合并对话框默认取舍。模态 QMessageBox 用预置定时器自动关掉。
class TestWellTopsEditorUi : public QObject
{
  Q_OBJECT

private slots:
  void dialogOpensAndFillsPerWell();
  void editMarksRowDirtyAndSaveRoundTrips();
  void validationBlocksSaveOnErrorThenPasses();
  void insertDeleteSortRows();
  void zWithoutXYBlocksSave();
  void precisionAndZSurviveEditorRoundTrip();
  void mergeDialogDefaultsAndResolutions();

private:
  struct Fixture
  {
    QTemporaryDir dir;
    DataCatalog cat;
    QString assetId;

    static WellTopRecord rec(const QString &well, const QString &top, double md, double tvd)
    {
      WellTopRecord r;
      r.wellName = well;
      r.topName = top;
      r.md = md;
      r.hasMd = true;
      r.tvd = tvd;
      r.hasTvd = true;
      r.x = 5288.67;
      r.y = 8219.94;
      r.hasX = r.hasY = true;
      r.z = -tvd; // DC.dat 常态：X/Y/Z 同列组（评审 H1 路径）
      r.hasTime = false;
      return r;
    }

    bool build()
    {
      if (!cat.open(dir.path()))
        return false;
      QVector<WellTopRecord> rows;
      rows << rec(QStringLiteral("A1"), QStringLiteral("X"), 850.0, 850.0)
           << rec(QStringLiteral("A1"), QStringLiteral("A"), 942.5, 942.5)
           << rec(QStringLiteral("A1"), QStringLiteral("B"), 1146.0, 1146.0)
           << rec(QStringLiteral("A2"), QStringLiteral("X"), 800.0, 800.0);
      CatalogEntity e;
      e.id = cat.nextEntityId(QStringLiteral("well"));
      e.entityType = QStringLiteral("well");
      e.name = QStringLiteral("A1");
      e.td = 2500.0;
      if (!cat.addEntity(e))
        return false;
      CatalogAsset a;
      a.id = cat.nextAssetId();
      a.type = QStringLiteral("well_stratification");
      a.format = QStringLiteral("dat");
      a.displayName = QStringLiteral("DC.dat");
      if (!cat.addAsset(a))
        return false;
      assetId = a.id;
      CatalogVersion v;
      v.id = cat.nextVersionId();
      v.assetId = a.id;
      v.stage = QStringLiteral("RAW");
      v.versionNumber = 1;
      v.managed = true;
      v.fileName = QStringLiteral("DC.dat");
      v.path = QStringLiteral("artifacts/") +
               DataCatalog::managedPath(QStringLiteral("raw"), a.id, v.id,
                                        QStringLiteral("DC.dat"));
      const QString abs = QDir(dir.path()).absoluteFilePath(v.path);
      if (!QDir().mkpath(QFileInfo(abs).absolutePath()))
        return false;
      QFile f(abs);
      if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
      f.write(writeWellTopsText(rows));
      f.close();
      v.sha256 = DataCatalog::sha256FileHex(abs);
      if (!cat.addVersion(v))
        return false;
      EntityAssetLink l;
      l.entityType = QStringLiteral("well");
      l.entityId = e.id;
      l.assetId = a.id;
      l.role = QStringLiteral("tops");
      l.isPrimary = true;
      return cat.addLink(l);
    }
  };

  // 预置自动关闭模态 QMessageBox（offscreen 不挂起）。
  static void autoDismissModalBoxes()
  {
    QTimer::singleShot(0, []
                       {
    for (QWidget *w : QApplication::topLevelWidgets())
      if (auto *mb = qobject_cast<QMessageBox *>(w))
        mb->reject(); });
  }

  static WellTopsEditorDialog *openDialog(Fixture &fx)
  {
    auto *dlg = new WellTopsEditorDialog(&fx.cat, fx.dir.path(), fx.assetId);
    return dlg;
  }
};

void TestWellTopsEditorUi::dialogOpensAndFillsPerWell()
{
  Fixture fx;
  QVERIFY(fx.build());
  auto *dlg = openDialog(fx);
  auto *combo = dlg->findChild<QComboBox *>(QStringLiteral("topsWellCombo"));
  QVERIFY(combo);
  QCOMPARE(combo->count(), 2);
  QCOMPARE(combo->itemText(0), QStringLiteral("A1"));
  auto *table = dlg->findChild<QTableWidget *>(QStringLiteral("topsEditTable"));
  QVERIFY(table);
  QCOMPARE(table->rowCount(), 3);
  QCOMPARE(table->columnCount(), 8);
  QCOMPARE(table->item(0, 1)->text(), QStringLiteral("X"));
  QCOMPARE(table->item(0, 2)->text(), QStringLiteral("850.000"));
  // 切井（无脏改动）→ 表切换。
  combo->setCurrentText(QStringLiteral("A2"));
  QCOMPARE(table->rowCount(), 1);
  delete dlg;
}

void TestWellTopsEditorUi::editMarksRowDirtyAndSaveRoundTrips()
{
  Fixture fx;
  QVERIFY(fx.build());
  auto *dlg = openDialog(fx);
  auto *table = dlg->findChild<QTableWidget *>(QStringLiteral("topsEditTable"));
  QVERIFY(table);
  // 改 A1 X 的 MD。
  table->item(0, 2)->setText(QStringLiteral("855.500"));
  QCOMPARE(table->item(0, 0)->text(), QStringLiteral("已改"));
  QCOMPARE(table->item(1, 0)->text(), QStringLiteral("—"));

  auto *save = dlg->findChild<QPushButton *>(QStringLiteral("topsSaveButton"));
  QVERIFY(save);
  save->click();
  QCOMPARE(fx.cat.versionsForAsset(fx.assetId).size(), 2);
  const CatalogVersion v2 = fx.cat.currentVersion(fx.assetId);
  QCOMPARE(v2.stage, QStringLiteral("DERIVED"));
  delete dlg;

  // 重开对话框：改动经盘上版本 round-trip。
  auto *reopened = openDialog(fx);
  auto *table2 = reopened->findChild<QTableWidget *>(QStringLiteral("topsEditTable"));
  QCOMPARE(table2->item(0, 2)->text(), QStringLiteral("855.500"));
  QCOMPARE(table2->rowCount(), 3);
  delete reopened;
}

void TestWellTopsEditorUi::validationBlocksSaveOnErrorThenPasses()
{
  Fixture fx;
  QVERIFY(fx.build());
  auto *dlg = openDialog(fx);
  auto *table = dlg->findChild<QTableWidget *>(QStringLiteral("topsEditTable"));
  auto *save = dlg->findChild<QPushButton *>(QStringLiteral("topsSaveButton"));
  // 植入 TVD > MD（域冲突，错误级）。
  table->item(0, 3)->setText(QStringLiteral("9000.000"));
  autoDismissModalBoxes();
  save->click();
  QCOMPARE(fx.cat.versionsForAsset(fx.assetId).size(), 1); // 阻断
  auto *issues = dlg->findChild<QListWidget *>(QStringLiteral("topsIssueList"));
  QVERIFY(!issues->isHidden()); // 对话框未 show()——按 hidden 标志断言
  QVERIFY(issues->count() > 0);
  // 修正后保存通过（TVD 须 ≤ MD=850）。
  table->item(0, 3)->setText(QStringLiteral("845.000"));
  autoDismissModalBoxes(); // 任何路径再弹框都自动关——测试只关心版本计数
  save->click();
  QCOMPARE(fx.cat.versionsForAsset(fx.assetId).size(), 2);
  delete dlg;
}

void TestWellTopsEditorUi::precisionAndZSurviveEditorRoundTrip()
{
  Fixture fx;
  QVERIFY(fx.build());
  auto *dlg = openDialog(fx);
  auto *table = dlg->findChild<QTableWidget *>(QStringLiteral("topsEditTable"));
  QVERIFY(table);
  // Z 列随 X/Y 组显示且保存后不丢（评审 H1）。
  QCOMPARE(table->item(0, 6)->text(), QStringLiteral("-850.000"));
  // 打开即干净：无任何行标「已改/新行」（z/x/y round-trip 一致）。
  for (int r = 0; r < table->rowCount(); ++r)
    QCOMPARE(table->item(r, 0)->text(), QStringLiteral("—"));

  // 高精度 MD：显示自适应精度，保存往返不截断（评审 H2）。
  table->item(0, 2)->setText(QStringLiteral("1234.5678"));
  auto *save = dlg->findChild<QPushButton *>(QStringLiteral("topsSaveButton"));
  save->click();
  QCOMPARE(fx.cat.versionsForAsset(fx.assetId).size(), 2);
  delete dlg;

  auto *reopened = openDialog(fx);
  auto *table2 = reopened->findChild<QTableWidget *>(QStringLiteral("topsEditTable"));
  QCOMPARE(table2->item(0, 2)->text(), QStringLiteral("1234.5678"));
  QCOMPARE(table2->item(0, 6)->text(), QStringLiteral("-850.000"));
  // 重开后亦无假脏。
  for (int r = 0; r < table2->rowCount(); ++r)
    QCOMPARE(table2->item(r, 0)->text(), QStringLiteral("—"));
  delete reopened;
}

void TestWellTopsEditorUi::zWithoutXYBlocksSave()
{
  Fixture fx;
  QVERIFY(fx.build());
  auto *dlg = openDialog(fx);
  auto *table = dlg->findChild<QTableWidget *>(QStringLiteral("topsEditTable"));
  // 清空 X/Y 但留 Z——同列组静默丢值不可接受，保存被拦。
  table->item(0, 4)->setText(QString());
  table->item(0, 5)->setText(QString());
  autoDismissModalBoxes();
  auto *save = dlg->findChild<QPushButton *>(QStringLiteral("topsSaveButton"));
  save->click();
  QCOMPARE(fx.cat.versionsForAsset(fx.assetId).size(), 1); // 未发版本
  delete dlg;
}

void TestWellTopsEditorUi::insertDeleteSortRows()
{
  Fixture fx;
  QVERIFY(fx.build());
  auto *dlg = openDialog(fx);
  auto *table = dlg->findChild<QTableWidget *>(QStringLiteral("topsEditTable"));
  auto *insert = dlg->findChild<QPushButton *>(QStringLiteral("topsInsertRowButton"));
  insert->click();
  QCOMPARE(table->rowCount(), 4);
  QCOMPARE(table->item(3, 0)->text(), QStringLiteral("新行"));

  // 填一行无 MD 的新行，验证排序把它放末尾、MD 行按浅→深。
  table->item(3, 1)->setText(QStringLiteral("Z"));
  table->item(0, 2)->setText(QStringLiteral("9999.000"));
  auto *sort = dlg->findChild<QPushButton *>(QStringLiteral("topsSortButton"));
  sort->click();
  QCOMPARE(table->item(0, 1)->text(), QStringLiteral("A"));
  QCOMPARE(table->item(1, 1)->text(), QStringLiteral("B"));
  QCOMPARE(table->item(2, 1)->text(), QStringLiteral("X"));
  QCOMPARE(table->item(3, 1)->text(), QStringLiteral("Z"));

  auto *del = dlg->findChild<QPushButton *>(QStringLiteral("topsDeleteRowButton"));
  table->setCurrentCell(3, 1);
  del->click();
  QCOMPARE(table->rowCount(), 3);
  delete dlg;
}

void TestWellTopsEditorUi::mergeDialogDefaultsAndResolutions()
{
  using namespace WellTopsEdit;
  QVector<MergeRow> rows;
  {
    WellTopRecord oldRec = Fixture::rec(QStringLiteral("A1"), QStringLiteral("X"), 850.0, 850.0);
    WellTopRecord newRec = Fixture::rec(QStringLiteral("A1"), QStringLiteral("X"), 900.0, 900.0);
    MergeRow conflict;
    conflict.topName = QStringLiteral("X");
    conflict.inOld = conflict.inNew = true;
    conflict.oldRec = oldRec;
    conflict.newRec = newRec;
    rows.append(conflict);

    MergeRow onlyOld;
    onlyOld.topName = QStringLiteral("OLD");
    onlyOld.inOld = true;
    onlyOld.oldRec = Fixture::rec(QStringLiteral("A1"), QStringLiteral("OLD"), 700.0, 700.0);
    rows.append(onlyOld);

    MergeRow onlyNew;
    onlyNew.topName = QStringLiteral("NEW");
    onlyNew.inNew = true;
    onlyNew.newRec = Fixture::rec(QStringLiteral("A1"), QStringLiteral("NEW"), 1000.0, 1000.0);
    rows.append(onlyNew);
  }

  WellTopsMergeDialog dlg(rows);
  // 默认：冲突保留旧值。
  auto *combo0 = dlg.findChild<QComboBox *>(QStringLiteral("topsMergeChoice0"));
  auto *combo2 = dlg.findChild<QComboBox *>(QStringLiteral("topsMergeChoice2"));
  QVERIFY(combo0);
  QVERIFY(combo2);
  QCOMPARE(combo0->currentText(), QStringLiteral("保留旧值"));
  QCOMPARE(combo2->currentText(), QStringLiteral("新增"));
  QVector<WellTopRecord> applied = WellTopsEdit::applyMerge(dlg.resolvedRows());
  QCOMPARE(applied.size(), 3);
  QCOMPARE(applied.at(0).md, 850.0);

  // 逐行改取舍。
  combo0->setCurrentIndex(1); // 采用新值
  combo2->setCurrentIndex(1); // 跳过新增
  applied = WellTopsEdit::applyMerge(dlg.resolvedRows());
  QCOMPARE(applied.size(), 2);
  QCOMPARE(applied.at(0).md, 900.0);
  QVERIFY(applied.at(1).topName == QLatin1String("OLD"));
  auto *summary = dlg.findChild<QLabel *>(QStringLiteral("topsMergeSummary"));
  QVERIFY(summary->text().contains(QStringLiteral("删除旧行 0")));
}

int main(int argc, char *argv[])
{
  if (qgetenv("QT_QPA_PLATFORM").isEmpty())
    qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  TestWellTopsEditorUi tc;
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_welltopseditor_ui.moc"
