#include <QtTest>
#include <QTemporaryDir>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QDir>
#include <QFile>

#include "../src/catalog/datacatalog.h"
#include "../src/catalog/frameworkstore.h"
#include "../src/domain/sequenceframework.h"

// goal/core-deepening（A-R1）：catalog 契约完整性——
//  1) FrameworkStore::save 失败路径不留「资产入库、零版本」幽灵（BatchSave
//     abort + 孤儿文件清理）；
//  2) 装载跳过的坏段版本行仍吸收 ver-N 序号（nextVersionId 不重发坏行 id，
//     血缘 parentVersionIds 不被静默改接）；
//  3) 装载侧 JSON 列（extra/parents/corners）损坏 qWarning 如实可见，
//     不再静默吞掉；
//  4) updateVersionExtra 批量 map 重载与单键重载同口径拒绝 '#' 保留字。
class TestCatalogIntegrity : public QObject
{
  Q_OBJECT

private slots:
  void frameworkSaveFailureLeavesNoGhostAsset();
  void frameworkSaveRecoversAfterFailure();
  void skippedUnsafeVersionRowHoldsItsSeq();
  void corruptJsonColumnsWarnAndDrop();
  void updateVersionExtraMapRejectsReservedKeys();

private:
  static SequenceFramework::Framework makeFramework();
  static bool corruptSqlite(const QString &projectDir, const QStringList &statements,
                            QString *error);
};

namespace
{

QString sqlitePath(const QString &projectDir)
{
  return QDir(projectDir).filePath(QStringLiteral("artifacts/metadata/catalog.sqlite"));
}

QString uniqueConn(const char *prefix)
{
  static int n = 0;
  return QStringLiteral("tst_catalog_integrity_%1_%2").arg(QLatin1String(prefix)).arg(++n);
}

int countFrameworkAssets(const DataCatalog &cat)
{
  int n = 0;
  for (const CatalogAsset &a : cat.assets())
    if (a.type == SequenceFramework::FrameworkStore::assetType())
      ++n;
  return n;
}

} // namespace

SequenceFramework::Framework TestCatalogIntegrity::makeFramework()
{
  SequenceFramework::Framework fw;
  fw.name = QStringLiteral("完整性夹具");
  SequenceFramework::FrameworkUnit sq1;
  sq1.id = QStringLiteral("sfu-1");
  sq1.name = QStringLiteral("SQ1");
  sq1.level = SequenceFramework::UnitLevel::Sequence;
  sq1.ordinal = 0;
  sq1.topBoundary = QStringLiteral("C3");
  sq1.baseBoundary = QStringLiteral("D53");
  sq1.thickness = 200.0;
  fw.units.append(sq1);
  return fw;
}

bool TestCatalogIntegrity::corruptSqlite(const QString &projectDir,
                                         const QStringList &statements, QString *error)
{
  const QString conn = uniqueConn("corrupt");
  {
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
    db.setDatabaseName(sqlitePath(projectDir));
    if (!db.open())
    {
      if (error)
        *error = db.lastError().text();
      return false;
    }
    for (const QString &stmt : statements)
    {
      QSqlQuery q(db);
      if (!q.exec(stmt))
      {
        if (error)
          *error = q.lastError().text();
        db.close();
        return false;
      }
    }
    db.close();
  }
  QSqlDatabase::removeDatabase(conn);
  return true;
}

// ---- 1. 格架保存失败：批次回滚，不留零版本幽灵资产/孤儿文件 ----
void TestCatalogIntegrity::frameworkSaveFailureLeavesNoGhostAsset()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QString err;
  QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));

  // 注入 mkpath 失败：把受管根 output/ 占成普通文件——save 走到
  //「格架目录创建失败」失败分支（addAsset 已进批）。格架受管路径 =
  // <projectDir>/output/<aid>/<ver>/framework.json（managedPath 小写 stage、
  // 无 artifacts 前缀）。
  QVERIFY(QFile(dir.path() + QStringLiteral("/output")).open(QIODevice::WriteOnly));

  SequenceFramework::FrameworkStore store(&cat, dir.path());
  QString saveErr;
  QVERIFY(!store.save(makeFramework(), &saveErr));
  QVERIFY(!saveErr.isEmpty());
  QVERIFY(saveErr.contains(QStringLiteral("目录创建失败")));

  // 同一目录重开：库里不得出现 sequence_framework 资产（旧实现经
  // BatchSave 析构 flush 把零版本资产提交成了幽灵）。
  DataCatalog reloaded;
  QVERIFY2(reloaded.open(dir.path(), &err), qPrintable(err));
  QCOMPARE(countFrameworkAssets(reloaded), 0);
  QCOMPARE(reloaded.assets().size(), 0);
}

// ---- 1b. 失败后现场可恢复：清障后同一 store 正常保存，一条资产一个版本 ----
void TestCatalogIntegrity::frameworkSaveRecoversAfterFailure()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QString err;
  QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));

  QVERIFY(QFile(dir.path() + QStringLiteral("/output")).open(QIODevice::WriteOnly));

  SequenceFramework::FrameworkStore store(&cat, dir.path());
  QString saveErr;
  QVERIFY(!store.save(makeFramework(), &saveErr));

  // 清障重试：成功路径行为不变（资产 1、版本 1、可 load 回来）。
  QFile::remove(dir.path() + QStringLiteral("/output"));
  QVERIFY2(store.save(makeFramework(), &saveErr), qPrintable(saveErr));
  QCOMPARE(store.versionCount(), 1);

  SequenceFramework::Framework loaded;
  QString loadErr;
  QVERIFY(store.load(&loaded, &loadErr));
  QCOMPARE(loaded.name, QStringLiteral("完整性夹具"));
  QCOMPARE(loaded.units.size(), 1);

  DataCatalog reloaded;
  QVERIFY2(reloaded.open(dir.path(), &err), qPrintable(err));
  QCOMPARE(countFrameworkAssets(reloaded), 1);
}

// ---- 2. 坏段版本行：id 序号吸收，nextVersionId 不重发 ----
void TestCatalogIntegrity::skippedUnsafeVersionRowHoldsItsSeq()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QString err;
  {
    DataCatalog cat;
    QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));
    CatalogAsset a;
    a.id = QStringLiteral("ast-1");
    a.type = QStringLiteral("well_log");
    a.format = QStringLiteral("las");
    a.displayName = QStringLiteral("w1.las");
    QVERIFY(cat.addAsset(a, &err));
    CatalogVersion v;
    v.id = QStringLiteral("ver-1");
    v.assetId = a.id;
    v.stage = QStringLiteral("RAW");
    v.versionNumber = 1;
    v.managed = false;
    v.path = QStringLiteral("/nowhere/w1.las");
    v.fileName = QStringLiteral("w1.las");
    QVERIFY(cat.addVersion(v, &err));
  }

  // 直改 sqlite：ver-1 → ver-900 且 file_name 带非法段（装载将被跳过），
  // 并把 meta version_seq 压回陈旧值 1——模拟遗留/被改库。
  QVERIFY2(corruptSqlite(dir.path(),
                         {
                             QStringLiteral(
                                 "UPDATE versions SET id='ver-900', file_name='../evil' WHERE id='ver-1'"),
                             QStringLiteral(
                                 "UPDATE catalog_meta SET value='1' WHERE key='version_seq'"),
                         },
                         &err),
           qPrintable(err));

  DataCatalog cat;
  QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));
  // 坏行确实被跳过（不在内存）。
  QVERIFY(cat.versionById(QStringLiteral("ver-900")).id.isEmpty());
  // 但序号必须被吸收：下一个 id 越过坏行占用的 900（旧行为重发 ver-2，
  // 随后 upsert 静默覆盖盘上坏行——指向 ver-900 的血缘会被改接）。
  QCOMPARE(cat.nextVersionId(), QStringLiteral("ver-901"));

  CatalogVersion fresh;
  fresh.id = QStringLiteral("ver-901");
  fresh.assetId = QStringLiteral("ast-1");
  fresh.stage = QStringLiteral("RAW");
  fresh.versionNumber = 2;
  fresh.managed = false;
  fresh.path = QStringLiteral("/nowhere/w2.las");
  fresh.fileName = QStringLiteral("w2.las");
  QVERIFY2(cat.addVersion(fresh, &err), qPrintable(err));

  // 重开对拍：ver-901 落库，坏行 ver-900 原样留在库里（不被覆盖）。
  DataCatalog reloaded;
  QVERIFY2(reloaded.open(dir.path(), &err), qPrintable(err));
  QVERIFY(!reloaded.versionById(QStringLiteral("ver-901")).id.isEmpty());
  QString probeErr;
  QVERIFY2(TestCatalogIntegrity::corruptSqlite(
               dir.path(), {QStringLiteral("SELECT 1 FROM versions WHERE id='ver-900' AND file_name='../evil'")},
               &probeErr),
           qPrintable(probeErr));
}

// ---- 3. JSON 列损坏：qWarning 可见、其余行照常装载 ----
void TestCatalogIntegrity::corruptJsonColumnsWarnAndDrop()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QString err;
  {
    DataCatalog cat;
    QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));
    CatalogEntity e;
    e.id = QStringLiteral("well-1");
    e.entityType = QStringLiteral("well");
    e.name = QStringLiteral("W1");
    e.extra.insert(QStringLiteral("uwi"), QStringLiteral("UWI-1"));
    QVERIFY(cat.addEntity(e, &err));
    CatalogAsset a;
    a.id = QStringLiteral("ast-1");
    a.type = QStringLiteral("well_log");
    a.format = QStringLiteral("las");
    a.displayName = QStringLiteral("w1.las");
    QVERIFY(cat.addAsset(a, &err));
    CatalogVersion v;
    v.id = QStringLiteral("ver-1");
    v.assetId = a.id;
    v.stage = QStringLiteral("DERIVED");
    v.versionNumber = 1;
    v.managed = false;
    v.path = QStringLiteral("/nowhere/d.tif");
    v.parentVersionIds = QStringList{QStringLiteral("ver-0")};
    v.extra.insert(QStringLiteral("method"), QStringLiteral("idw"));
    QVERIFY(cat.addVersion(v, &err));
  }

  QVERIFY2(corruptSqlite(dir.path(),
                         {
                             QStringLiteral("UPDATE entities SET extra_json='{oops' WHERE id='well-1'"),
                             QStringLiteral(
                                 "UPDATE versions SET parent_version_ids_json='not-json', extra_json='[1,2]' WHERE id='ver-1'"),
                         },
                         &err),
           qPrintable(err));

  DataCatalog cat;
  // 三个损坏列各一条告警：extra（实体）、parents（血缘）、extra（版本）。
  QTest::ignoreMessage(QtWarningMsg, QRegularExpression("corrupt JSON object column.*"));
  QTest::ignoreMessage(QtWarningMsg, QRegularExpression("corrupt JSON parents column.*"));
  QTest::ignoreMessage(QtWarningMsg, QRegularExpression("corrupt JSON object column.*"));
  QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));

  // 值如实丢弃（空 extra / 无父版本），实体/版本本身仍在——不静默整行消失。
  QCOMPARE(cat.entityById(QStringLiteral("well-1")).extra.size(), 0);
  const CatalogVersion v = cat.versionById(QStringLiteral("ver-1"));
  QVERIFY(!v.id.isEmpty());
  QCOMPARE(v.parentVersionIds.size(), 0);
  QCOMPARE(v.extra.size(), 0);
}

// ---- 4. updateVersionExtra(map)：保留字/空键与单键重载同口径拒收 ----
void TestCatalogIntegrity::updateVersionExtraMapRejectsReservedKeys()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QString err;
  QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));

  CatalogAsset a;
  a.id = QStringLiteral("ast-1");
  a.type = QStringLiteral("image_reference");
  a.format = QStringLiteral("jpg");
  a.displayName = QStringLiteral("a1.jpg");
  QVERIFY(cat.addAsset(a, &err));
  CatalogVersion v;
  v.id = QStringLiteral("ver-1");
  v.assetId = a.id;
  v.stage = QStringLiteral("RAW");
  v.versionNumber = 1;
  v.managed = false;
  v.path = QStringLiteral("/nowhere/a1.jpg");
  QVERIFY(cat.addVersion(v, &err));
  const int revision0 = cat.catalogRevision();

  // 良性键照常合并。
  QVariantMap patch;
  patch.insert(QStringLiteral("cuttings_selection"), QStringLiteral("unselected"));
  QVERIFY2(cat.updateVersionExtra(QStringLiteral("ver-1"), patch, &err), qPrintable(err));
  QCOMPARE(cat.versionById(QStringLiteral("ver-1")).extra.value(
               QStringLiteral("cuttings_selection")).toString(),
           QStringLiteral("unselected"));

  // '#' 保留字 / 空键：拒收、有 error、不动 revision 不改 extra。
  QVariantMap bad;
  bad.insert(QStringLiteral("depthMd#history"), 1.0);
  QVERIFY(!cat.updateVersionExtra(QStringLiteral("ver-1"), bad, &err));
  QVERIFY(!err.isEmpty());
  err.clear();
  QVariantMap badEmpty;
  badEmpty.insert(QString(), 1.0);
  QVERIFY(!cat.updateVersionExtra(QStringLiteral("ver-1"), badEmpty, &err));
  QVERIFY(!err.isEmpty());
  QVERIFY(!cat.versionById(QStringLiteral("ver-1")).extra.contains(QStringLiteral("depthMd#history")));
  QVERIFY(cat.versionById(QStringLiteral("ver-1")).extra.contains(
      QStringLiteral("cuttings_selection")));
  Q_UNUSED(revision0);
}

QTEST_MAIN(TestCatalogIntegrity)
#include "tst_catalog_integrity.moc"
