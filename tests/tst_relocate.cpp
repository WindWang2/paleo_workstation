#include <QtTest>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "../src/catalog/datacatalog.h"
#include "../src/io/dataimportservice.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisruntime.h"

// wave4/runtime-resilience：外链源文件「重新定位」恢复路径（TODOS P3 autoplan
// pass-2 递延项）。场景：RAW 外链版本（managed=false，SEG-Y 一律外链）源文件
// 被移动/重命名 → 预览「找不到源文件」死胡同。契约：
//   relocateVersionSource(versionId, newPath) 流式重算新文件 SHA-256，与该版本
//   入库时留底一致才接受——版本记录不可变，追加一条同内容、指向新路径的外链
//   RAW 版本（extra.relocatedFrom 留血统），currentVersion 从此解析到新路径；
//   不一致 → 拒解，catalog 一字不动。新路径落在工程目录内也仍按 external 记。
class TestRelocate : public QObject
{
  Q_OBJECT

  struct Stack
  {
    QgisProjectService projectSvc;
    std::unique_ptr<LayerManifest> manifest;
    std::unique_ptr<QgisLayerService> layerSvc;
    std::unique_ptr<PaleoProjectStore> store;
    std::unique_ptr<DataImportService> importSvc;
    QString metaPath;
  };

  static std::unique_ptr<Stack> makeStack(const QString &projectDir)
  {
    if (!QDir().mkpath(projectDir))
      return nullptr;
    auto s = std::make_unique<Stack>();
    s->metaPath = QDir(projectDir).filePath(QStringLiteral("metadata/project.sqlite"));
    if (!s->projectSvc.createProject(QDir(projectDir).filePath(QStringLiteral("proj.qgz"))))
      return nullptr;
    s->manifest = std::make_unique<LayerManifest>(s->metaPath);
    if (!s->manifest->open())
      return nullptr;
    s->layerSvc = std::make_unique<QgisLayerService>(&s->projectSvc, s->manifest.get());
    s->store = std::make_unique<PaleoProjectStore>();
    s->importSvc = std::make_unique<DataImportService>(s->layerSvc.get(), s->store.get());
    s->importSvc->setProjectDir(projectDir);
    return s;
  }

  static QString fixture(const QString &name)
  {
    return QStringLiteral(PROJECT_FIXTURE_DIR) + QLatin1Char('/') + name;
  }

private slots:
  void initTestCase() { QVERIFY(QgisRuntime::isInitialized()); }

  void relocateAfterMoveSucceeds();
  void shaMismatchIsRefused();
  void relocationSurvivesReopen();
  void managedVersionIsNotRelocatable();
  void newPathInsideProjectStaysExternal();
  void missingNewPathIsRefused();

private:
  // 搭一台外链地震资产：源从 tmp 里导入（SEG-Y 一律外链），返回 (assetId, verId, 源路径)。
  struct External
  {
    QString assetId, versionId, source;
  };
  static External makeExternal(DataImportService &svc, const QTemporaryDir &tmp,
                               const QString &subdir = QStringLiteral("src"))
  {
    External out;
    const QString dir = tmp.filePath(subdir);
    if (!QDir().mkpath(dir))
      return out;
    out.source = QDir(dir).filePath(QStringLiteral("vol.sgy"));
    if (!QFile::copy(fixture(QStringLiteral("mini_seismic.sgy")), out.source))
      return out;
    QString err;
    out.assetId = svc.importProjectFile(out.source, &err);
    if (out.assetId.isEmpty())
      return out;
    const CatalogVersion v = svc.catalog()->currentVersion(out.assetId);
    out.versionId = v.id;
    return out;
  }
};

void TestRelocate::relocateAfterMoveSucceeds()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st != nullptr);
  const External ex = makeExternal(*st->importSvc, tmp);
  QVERIFY(!ex.assetId.isEmpty());
  const CatalogVersion orig = st->importSvc->catalog()->versionById(ex.versionId);
  QVERIFY(!orig.managed);
  QVERIFY(!orig.sha256.isEmpty());

  // 源文件被移动：旧路径消失，新路径是同一批字节。
  const QString moved = tmp.filePath(QStringLiteral("elsewhere/vol.sgy"));
  QVERIFY(QDir().mkpath(QFileInfo(moved).absolutePath()));
  QVERIFY(QFile::rename(ex.source, moved));
  QVERIFY(!QFile::exists(st->importSvc->absolutePath(ex.assetId))); // 死胡同成立

  QString err;
  const QString newVer = st->importSvc->relocateVersionSource(ex.versionId, moved, &err);
  QVERIFY2(!newVer.isEmpty(), qPrintable(err));
  QVERIFY(newVer != ex.versionId);

  // currentVersion 从此解析到新路径（预览/打开恢复）。
  QCOMPARE(st->importSvc->absolutePath(ex.assetId), moved);
  const CatalogVersion relocated = st->importSvc->catalog()->versionById(newVer);
  QCOMPARE(relocated.path, moved);
  QCOMPARE(relocated.sha256, orig.sha256); // 同内容：SHA 沿用留底值
  QVERIFY(!relocated.managed);
  QCOMPARE(relocated.extra.value(QStringLiteral("relocatedFrom")).toString(),
           ex.versionId);
  // 旧版本记录原样保留（不可变血统），dedup 只认文件仍在的版本。
  QCOMPARE(st->importSvc->catalog()->versionById(ex.versionId).path, orig.path);
  const CatalogVersion dedupHit =
      st->importSvc->catalog()->versionBySha256(orig.sha256);
  QCOMPARE(dedupHit.id, newVer);
}

void TestRelocate::shaMismatchIsRefused()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st != nullptr);
  const External ex = makeExternal(*st->importSvc, tmp);
  QVERIFY(!ex.assetId.isEmpty());

  // 「移动」后内容被动过：SHA 不一致 → 拒解，catalog 不动。
  const QString moved = tmp.filePath(QStringLiteral("elsewhere/vol.sgy"));
  QVERIFY(QDir().mkpath(QFileInfo(moved).absolutePath()));
  QVERIFY(QFile::rename(ex.source, moved));
  {
    QFile f(moved);
    QVERIFY(f.open(QIODevice::Append));
    f.write("tampered");
  }
  const int versionCountBefore =
      st->importSvc->catalog()->versionsForAsset(ex.assetId).size();

  QString err;
  const QString newVer = st->importSvc->relocateVersionSource(ex.versionId, moved, &err);
  QVERIFY(newVer.isEmpty());
  QVERIFY2(err.contains(QStringLiteral("不符")), qPrintable(err));
  QCOMPARE(st->importSvc->catalog()->versionsForAsset(ex.assetId).size(),
           versionCountBefore);
  // 死胡同保持如实：currentVersion 仍指旧（已消失）路径。
  QCOMPARE(st->importSvc->absolutePath(ex.assetId), ex.source);
}

void TestRelocate::relocationSurvivesReopen()
{
  QTemporaryDir tmp;
  const QString projDir = tmp.filePath(QStringLiteral("proj"));
  {
    auto st = makeStack(projDir);
    QVERIFY(st != nullptr);
    const External ex = makeExternal(*st->importSvc, tmp);
    QVERIFY(!ex.assetId.isEmpty());
    const QString moved = tmp.filePath(QStringLiteral("elsewhere/vol.sgy"));
    QVERIFY(QDir().mkpath(QFileInfo(moved).absolutePath()));
    QVERIFY(QFile::rename(ex.source, moved));
    QString err;
    QVERIFY2(!st->importSvc->relocateVersionSource(ex.versionId, moved, &err).isEmpty(),
             qPrintable(err));
  }
  // 重开工程（新服务实例、catalog 从盘上装载）：路径存活。
  auto st2 = makeStack(projDir);
  QVERIFY(st2 != nullptr);
  const QString path = st2->importSvc->absolutePath(
      st2->importSvc->catalog()->assets().front().id);
  QVERIFY(path.endsWith(QStringLiteral("vol.sgy")));
  QVERIFY(QFile::exists(path));
}

void TestRelocate::managedVersionIsNotRelocatable()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st != nullptr);
  // 受管资产（默认复制入库）：缺失不是重定位该管的事——如实拒绝。
  QString err;
  const QString assetId =
      st->importSvc->importProjectFile(fixture(QStringLiteral("tiny.png")), &err);
  QVERIFY(!assetId.isEmpty());
  const CatalogVersion v = st->importSvc->catalog()->currentVersion(assetId);
  QVERIFY(v.managed);
  const QString r = st->importSvc->relocateVersionSource(
      v.id, fixture(QStringLiteral("tiny.png")), &err);
  QVERIFY(r.isEmpty());
  QVERIFY2(err.contains(QStringLiteral("受管")), qPrintable(err));
}

void TestRelocate::newPathInsideProjectStaysExternal()
{
  QTemporaryDir tmp;
  const QString projDir = tmp.filePath(QStringLiteral("proj"));
  auto st = makeStack(projDir);
  QVERIFY(st != nullptr);
  const External ex = makeExternal(*st->importSvc, tmp);
  QVERIFY(!ex.assetId.isEmpty());
  // 用户把源挪进了工程目录：仍按 external 记，不升级为 managed。
  const QString inProject = QDir(projDir).filePath(QStringLiteral("brought-back.sgy"));
  QVERIFY(QFile::rename(ex.source, inProject));
  QString err;
  const QString newVer =
      st->importSvc->relocateVersionSource(ex.versionId, inProject, &err);
  QVERIFY2(!newVer.isEmpty(), qPrintable(err));
  QVERIFY(!st->importSvc->catalog()->versionById(newVer).managed);
  QCOMPARE(st->importSvc->absolutePath(ex.assetId), inProject);
}

void TestRelocate::missingNewPathIsRefused()
{
  QTemporaryDir tmp;
  auto st = makeStack(tmp.filePath(QStringLiteral("proj")));
  QVERIFY(st != nullptr);
  const External ex = makeExternal(*st->importSvc, tmp);
  QVERIFY(!ex.assetId.isEmpty());
  QString err;
  const QString r = st->importSvc->relocateVersionSource(
      ex.versionId, tmp.filePath(QStringLiteral("nope/vol.sgy")), &err);
  QVERIFY(r.isEmpty());
  QVERIFY2(err.contains(QStringLiteral("找不到")), qPrintable(err));
}

int main(int argc, char *argv[])
{
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
  {
    qFatal("QgisRuntime::initialize failed");
    return 1;
  }
  TestRelocate tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_relocate.moc"
