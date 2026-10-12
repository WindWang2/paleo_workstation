#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "../src/catalog/datacatalog.h"
#include "../src/workflow/derivedassets.h"

// wave3/derived-publish T26 — 派生产物登记：厚度/ONNX/processing 输出不再写
// QDir::temp()（重启即死链，autoplan pass-2 H4），全部落 catalog 受管的
// <projectDir>/artifacts/derived/{asset_id}/{version_id}/{filename} 并登记
// DERIVED 版本（父版本 + sha256 + provenance extra）。覆盖：
//   · stage 产出受管路径（资产按 type+displayName 复用，版本号递增）；
//   · commit 登记 DERIVED 版本且 sha256 可复验、文件只读；
//   · 文件缺失时 commit 失败（不登记空版本）；
//   · 绝对路径反查父版本 id（厚度栅格父版本 = D61 时间栅格版本）；
//   · 未绑定 catalog 时明确失败；
//   · 重启存活：新会话重开 catalog，同一相对路径解析出的文件仍在、sha 一致。
class TestDerivedAssets : public QObject
{
  Q_OBJECT

private slots:
  void stageLandsUnderArtifactsDerived();
  void stageReusesAssetAndBumpsVersion();
  void commitRegistersDerivedVersionWithSha();
  void commitFailsWhenFileMissing();
  void parentVersionIdsForResolvesPaths();
  void unboundRegistrarFailsReadable();
  void restartKeepsPathAliveAndShaVerifiable();
  // goal/core-deepening A-R4：延迟建资产——失败/取消不留幽灵、归并不分叉。
  void abandonedStageLeavesNoAsset();
  void rerunAfterAbandonedStageYieldsSingleAsset();
  void commitMergesIntoAssetCreatedMeanwhile();
  // A5 review P1：任务中心 prepare/publish 分属不同 registrar 实例——预留
  // 信息必须随 staging 走，跨实例 commit 能结算（constraintfactorjobs_
  // localdirection/_geostat 生产形态）。
  void crossRegistrarCommitSettlesReservation();

private:
  static bool writeBytes(const QString &path, const QByteArray &bytes)
  {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
      return false;
    f.write(bytes);
    f.close();
    return true;
  }
};

void TestDerivedAssets::stageLandsUnderArtifactsDerived()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));

  DerivedAssetRegistrar registrar(&cat, dir.path());
  DerivedStaging st = registrar.stage(QStringLiteral("thickness_raster"),
                                      QStringLiteral("D61–D62 等厚"),
                                      QStringLiteral("THICKNESS_D61.tif"));
  QVERIFY2(st.isValid(), "staging should succeed with an open catalog");

  QVERIFY(st.absolutePath.startsWith(
      QDir(dir.path()).absoluteFilePath(QStringLiteral("artifacts/derived/"))));
  QVERIFY(st.relativePath.startsWith(QStringLiteral("artifacts/derived/")));
  QVERIFY(st.relativePath.contains(st.assetId));
  QVERIFY(st.relativePath.contains(st.versionId));
  QVERIFY(st.relativePath.endsWith(QStringLiteral("THICKNESS_D61.tif")));
  QCOMPARE(st.versionNumber, 1);
  QVERIFY(QFileInfo::exists(QFileInfo(st.absolutePath).absolutePath())); // 目录已建
}

void TestDerivedAssets::stageReusesAssetAndBumpsVersion()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));

  DerivedAssetRegistrar registrar(&cat, dir.path());
  const DerivedStaging a = registrar.stage(QStringLiteral("thickness_raster"),
                                           QStringLiteral("D61–D62 等厚"),
                                           QStringLiteral("THICKNESS_D61.tif"));
  const DerivedStaging b = registrar.stage(QStringLiteral("thickness_raster"),
                                           QStringLiteral("D61–D62 等厚"),
                                           QStringLiteral("THICKNESS_D61.tif"));
  QVERIFY(a.isValid() && b.isValid());
  QCOMPARE(b.assetId, a.assetId);      // 同 (type, displayName) → 同一资产
  QCOMPARE(b.versionNumber, 2);        // 版本号在该资产上递增
  QVERIFY(b.versionId != a.versionId);
  QVERIFY(b.absolutePath != a.absolutePath); // 每版本独立目录，不覆盖

  // 不同 displayName → 新资产，版本号从 1 起。
  const DerivedStaging c = registrar.stage(QStringLiteral("thickness_raster"),
                                           QStringLiteral("D62–D63 等厚"),
                                           QStringLiteral("THICKNESS_D62.tif"));
  QVERIFY(c.assetId != a.assetId);
  QCOMPARE(c.versionNumber, 1);
}

void TestDerivedAssets::commitRegistersDerivedVersionWithSha()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));

  // 父版本：D61 时间栅格（RAW 资产 + 版本，受管路径）。
  CatalogAsset parent;
  parent.id = QStringLiteral("ast-1");
  parent.type = QStringLiteral("horizon");
  parent.format = QStringLiteral("dat");
  parent.displayName = QStringLiteral("D61.dat");
  QVERIFY(cat.addAsset(parent));
  CatalogVersion parentVer;
  parentVer.id = QStringLiteral("ver-1");
  parentVer.assetId = parent.id;
  parentVer.stage = QStringLiteral("RAW");
  parentVer.versionNumber = 1;
  parentVer.path = DataCatalog::managedPath(QStringLiteral("raw"), parent.id,
                                            parentVer.id, QStringLiteral("D61.dat"));
  QVERIFY(cat.addVersion(parentVer));

  DerivedAssetRegistrar registrar(&cat, dir.path());
  DerivedStaging st = registrar.stage(QStringLiteral("thickness_raster"),
                                      QStringLiteral("D61–D62 等厚"),
                                      QStringLiteral("THICKNESS_D61.tif"));
  QVERIFY(st.isValid());
  const QByteArray payload = "fake geotiff bytes";
  QVERIFY(writeBytes(st.absolutePath, payload));

  QVariantMap extra;
  extra.insert(QStringLiteral("rows"), 411);
  extra.insert(QStringLiteral("cols"), 641);
  QString err;
  QVERIFY2(registrar.commit(st, QStringList{parentVer.id},
                            QStringLiteral("mappingworkflow/thickness"), extra, &err),
           qPrintable(err));

  const CatalogVersion v = cat.versionById(st.versionId);
  QCOMPARE(v.assetId, st.assetId);
  QCOMPARE(v.stage, QStringLiteral("DERIVED"));
  QCOMPARE(v.versionNumber, 1);
  QCOMPARE(v.path, st.relativePath);
  QCOMPARE(v.fileName, QStringLiteral("THICKNESS_D61.tif"));
  QCOMPARE(v.parentVersionIds, QStringList{parentVer.id});
  QCOMPARE(v.extra.value(QStringLiteral("rows")).toInt(), 411);
  QCOMPARE(v.sourceUri, QStringLiteral("mappingworkflow/thickness"));

  // sha 入库且可复验（与文件现场重算一致）。
  QString shaErr;
  const QString recomputed = DataCatalog::sha256FileHex(st.absolutePath, &shaErr);
  QVERIFY2(!recomputed.isEmpty(), qPrintable(shaErr));
  QCOMPARE(v.sha256, recomputed);
  QVERIFY(!v.sha256.isEmpty());

  // 登记后文件只读（版本不可变纪律，同 import 的 DERIVED 栅格）。
  QVERIFY(!QFileInfo(st.absolutePath).isWritable());
}

void TestDerivedAssets::commitFailsWhenFileMissing()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));

  DerivedAssetRegistrar registrar(&cat, dir.path());
  const DerivedStaging st = registrar.stage(QStringLiteral("onnx_prediction"),
                                            QStringLiteral("pred T1 onnx grid"),
                                            QStringLiteral("ONNX_T1_grid.tif"));
  QVERIFY(st.isValid());
  // 不写文件直接 commit → 失败 + 可读原因，且不产生版本行。
  QString err;
  QVERIFY(!registrar.commit(st, {}, QStringLiteral("onnx"), {}, &err));
  QVERIFY2(!err.isEmpty(), "commit 必须给出失败原因");
  QVERIFY(cat.versionById(st.versionId).id.isEmpty());
}

void TestDerivedAssets::parentVersionIdsForResolvesPaths()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QVERIFY(cat.open(dir.path()));

  CatalogAsset horizon;
  horizon.id = QStringLiteral("ast-1");
  horizon.type = QStringLiteral("horizon");
  horizon.format = QStringLiteral("tif");
  horizon.displayName = QStringLiteral("D61.tif");
  QVERIFY(cat.addAsset(horizon));
  CatalogVersion derivedVer;
  derivedVer.id = QStringLiteral("ver-1");
  derivedVer.assetId = horizon.id;
  derivedVer.stage = QStringLiteral("DERIVED");
  derivedVer.versionNumber = 1;
  derivedVer.path = QStringLiteral("artifacts/derived/ast-1/ver-1/D61.tif");
  QVERIFY(cat.addVersion(derivedVer));
  QVERIFY(writeBytes(QDir(dir.path()).filePath(derivedVer.path), "x"));

  DerivedAssetRegistrar registrar(&cat, dir.path());
  const QString abs = QDir(dir.path()).filePath(derivedVer.path);
  const QStringList parents = registrar.parentVersionIdsFor(QStringList{abs});
  QCOMPARE(parents, QStringList{derivedVer.id});

  // 未知路径 → 空列表（provenance 缺父版本不伪造）。
  QVERIFY(registrar.parentVersionIdsFor(QStringList{
              QDir(dir.path()).filePath(QStringLiteral("nope.tif"))})
      .isEmpty());
}

void TestDerivedAssets::unboundRegistrarFailsReadable()
{
  DerivedAssetRegistrar registrar; // 未绑定
  QString err;
  const DerivedStaging st = registrar.stage(QStringLiteral("thickness_raster"),
                                            QStringLiteral("x"),
                                            QStringLiteral("X.tif"), &err);
  QVERIFY(!st.isValid());
  QVERIFY2(!err.isEmpty(), "未绑定 catalog 必须有可读错误");
}

void TestDerivedAssets::restartKeepsPathAliveAndShaVerifiable()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QString committedPath;
  QString committedVersionId;
  QString committedSha;
  {
    DataCatalog cat;
    QVERIFY(cat.open(dir.path()));
    DerivedAssetRegistrar registrar(&cat, dir.path());
    DerivedStaging st = registrar.stage(QStringLiteral("facies_polygons"),
                                        QStringLiteral("facies D61"),
                                        QStringLiteral("FACIES_D61.gpkg"));
    QVERIFY(st.isValid());
    QVERIFY(writeBytes(st.absolutePath, QByteArray(4096, '\x7')));
    QVERIFY(registrar.commit(st, {}, QStringLiteral("polygonize"), {}, nullptr));
    committedPath = st.absolutePath;
    committedVersionId = st.versionId;
    committedSha = cat.versionById(st.versionId).sha256;
    QVERIFY(!committedSha.isEmpty());
  } // 会话结束（catalog 析构）

  // 新会话：重开同一工程目录的 catalog，版本行里的相对路径仍指向活文件。
  DataCatalog reopened;
  QString err;
  QVERIFY2(reopened.open(dir.path(), &err), qPrintable(err));
  const CatalogVersion v = reopened.versionById(committedVersionId);
  QVERIFY(!v.id.isEmpty());
  const QString resolved = QDir(dir.path()).absoluteFilePath(v.path);
  QCOMPARE(resolved, committedPath);
  QVERIFY2(QFile::exists(resolved), "派生产物在重启后必须是活路径（非 /tmp 死链）");
  QVERIFY(resolved.contains(QStringLiteral("artifacts/derived/")));
  QCOMPARE(DataCatalog::sha256FileHex(resolved, &err), committedSha);

  // dedup 面：sha 检索在重开的 catalog 里同样命中。
  QCOMPARE(reopened.versionBySha256(committedSha).id, committedVersionId);
}

// ---- goal/core-deepening A-R4：stage 只 find-or-reserve，commit 才入库 ----

namespace
{
int countAssetsOfType(const DataCatalog &cat, const QString &type)
{
  int n = 0;
  for (const CatalogAsset &a : cat.assets())
    if (a.type == type)
      ++n;
  return n;
}
} // namespace

// stage 被放弃（计算取消/失败、注册器随请求作用域消亡）：零 catalog 副作用。
// 旧实现 stage 即 addAsset——重开工程会看到零版本幽灵资产。
void TestDerivedAssets::abandonedStageLeavesNoAsset()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QString err;
  {
    DataCatalog cat;
    QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));
    DerivedAssetRegistrar registrar(&cat, dir.path());
    const DerivedStaging st = registrar.stage(QStringLiteral("single_factor_raster"),
                                              QStringLiteral("D61 砂厚"),
                                              QStringLiteral("factor.tif"), &err);
    QVERIFY2(st.isValid(), qPrintable(err));
    QVERIFY(!st.assetId.isEmpty());
    QCOMPARE(countAssetsOfType(cat, QStringLiteral("single_factor_raster")), 0);
  }
  DataCatalog reloaded;
  QVERIFY2(reloaded.open(dir.path(), &err), qPrintable(err));
  QCOMPARE(countAssetsOfType(reloaded, QStringLiteral("single_factor_raster")), 0);
  QCOMPARE(reloaded.assets().size(), 0);
}

// 放弃后重跑：一次成功 → 一条资产一条版本（不从幽灵起步）。
void TestDerivedAssets::rerunAfterAbandonedStageYieldsSingleAsset()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  QString err;
  {
    DataCatalog cat;
    QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));
    DerivedAssetRegistrar first(&cat, dir.path());
    const DerivedStaging abandoned = first.stage(QStringLiteral("depth_raster"),
                                                 QStringLiteral("D61 深度域"),
                                                 QStringLiteral("depth.tif"), &err);
    QVERIFY(abandoned.isValid());
    // 放弃：不写文件、不 commit。
  }
  {
    DataCatalog cat;
    QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));
    DerivedAssetRegistrar second(&cat, dir.path());
    const DerivedStaging st = second.stage(QStringLiteral("depth_raster"),
                                           QStringLiteral("D61 深度域"),
                                           QStringLiteral("depth.tif"), &err);
    QVERIFY2(st.isValid(), qPrintable(err));
    QVERIFY(writeBytes(st.absolutePath, QByteArrayLiteral("depth-1")));
    QVERIFY2(second.commit(st, {}, QStringLiteral("depthconversion"), {}, &err),
             qPrintable(err));
    QCOMPARE(countAssetsOfType(cat, QStringLiteral("depth_raster")), 1);
    QCOMPARE(cat.currentVersion(st.assetId).versionNumber, 1);
  }
  DataCatalog reloaded;
  QVERIFY2(reloaded.open(dir.path(), &err), qPrintable(err));
  QCOMPARE(countAssetsOfType(reloaded, QStringLiteral("depth_raster")), 1);
  QCOMPARE(reloaded.versions().size(), 1);
}

// stage→commit 延迟窗口内同名资产被外部建立：迁文件归并到既有资产，
// 不分叉第二个同 (type, displayName) 资产。
void TestDerivedAssets::commitMergesIntoAssetCreatedMeanwhile()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QString err;
  QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));

  DerivedAssetRegistrar a(&cat, dir.path());
  const DerivedStaging stA = a.stage(QStringLiteral("velocity_model"),
                                     QStringLiteral("速度模型（层间平均）"),
                                     QStringLiteral("velocity.json"), &err);
  QVERIFY2(stA.isValid(), qPrintable(err));

  DerivedAssetRegistrar b(&cat, dir.path());
  const DerivedStaging stB = b.stage(QStringLiteral("velocity_model"),
                                     QStringLiteral("速度模型（层间平均）"),
                                     QStringLiteral("velocity.json"), &err);
  QVERIFY2(stB.isValid(), qPrintable(err));
  QVERIFY(writeBytes(stB.absolutePath, QByteArrayLiteral("external")));
  QVERIFY2(b.commit(stB, {}, QStringLiteral("depthconversion"), {}, &err), qPrintable(err));

  QVERIFY(writeBytes(stA.absolutePath, QByteArrayLiteral("late")));
  QVERIFY2(a.commit(stA, {}, QStringLiteral("depthconversion"), {}, &err), qPrintable(err));

  QCOMPARE(countAssetsOfType(cat, QStringLiteral("velocity_model")), 1);
  QCOMPARE(cat.versionsForAsset(stB.assetId).size(), 2);
  const CatalogVersion vA = cat.versionById(stA.versionId);
  QVERIFY(!vA.id.isEmpty());
  QCOMPARE(vA.assetId, stB.assetId); // 归并进外部已建资产
  QCOMPARE(vA.versionNumber, 2);     // 取号按既有资产口径
  const QString abs = DataCatalog::resolvedVersionPath(dir.path(), vA);
  QVERIFY(!abs.isEmpty());
  QFile f(abs);
  QVERIFY(f.open(QIODevice::ReadOnly));
  QCOMPARE(f.readAll(), QByteArrayLiteral("late"));
  QVERIFY(!QFile::exists(stA.absolutePath)); // 旧预留路径已让位
}

// 任务中心生产形态：prepare 阶段局部 registrar stage 后消亡，publish 阶段
// 全新 registrar 拿着 staging commit——预留结算只认注册器私有状态时这里
// 会 addVersion 拒绝 "version references unknown asset"（A5 review P1）。
void TestDerivedAssets::crossRegistrarCommitSettlesReservation()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog cat;
  QString err;
  QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));

  DerivedStaging st;
  {
    DerivedAssetRegistrar preparer(&cat, dir.path()); // prepare 侧，随作用域消亡
    st = preparer.stage(QStringLiteral("single_factor_raster"),
                        QStringLiteral("D61 本地方向"),
                        QStringLiteral("factor.tif"), &err);
    QVERIFY2(st.isValid(), qPrintable(err));
    QCOMPARE(countAssetsOfType(cat, QStringLiteral("single_factor_raster")), 0);
  }
  QVERIFY(writeBytes(st.absolutePath, QByteArrayLiteral("computed")));

  DerivedAssetRegistrar publisher(&cat, dir.path()); // publish 侧，全新实例
  QVERIFY2(publisher.commit(st, {}, QStringLiteral("localdirection"), {}, &err),
           qPrintable(err));
  QCOMPARE(countAssetsOfType(cat, QStringLiteral("single_factor_raster")), 1);
  const CatalogVersion v = cat.currentVersion(st.assetId);
  QVERIFY(!v.id.isEmpty());
  QCOMPARE(v.versionNumber, 1);

  DataCatalog reloaded;
  QVERIFY2(reloaded.open(dir.path(), &err), qPrintable(err));
  QCOMPARE(countAssetsOfType(reloaded, QStringLiteral("single_factor_raster")), 1);
}

QTEST_MAIN(TestDerivedAssets)
#include "tst_derivedassets.moc"
