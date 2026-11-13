#include <QtTest>
#include <QTemporaryDir>
#include <QTimer>
#include <QThread>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QSaveFile>
#include <algorithm>
#include "../src/services/storagegovernance.h"
#include "../src/workflow/storagegovernancecontroller.h"
#include "../src/io/perffixtures.h"
#include "../src/catalog/purgelease.h"
using namespace paleo::storage;

class TestStorageGovernance : public QObject {
  Q_OBJECT
  static bool write(const QString &path, int size) {
    QDir().mkpath(QFileInfo(path).absolutePath()); QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(QByteArray(size, 'x')) == size;
  }
  static bool operations(const QString &root, const QJsonArray &rows) {
    const auto path=QDir(root).filePath(".paleo/capability-ops/operations.json");QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);const auto bytes=QJsonDocument(rows).toJson(QJsonDocument::Compact);
    return file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size()&&file.commit();
  }
  static QJsonObject seal(QJsonObject object) {
    object["planHash"]=operationPlanHash(object.value("frozenRequest").toObject(),object.value("stagePlan").toObject(),
      object.value("presentationIntent").toObject(),object.value("externalRequest").toObject());return object;
  }
  static QJsonObject operation(const QString &path, const QString &head={}) {
    return seal({{"schemaVersion",2},{"operationId","op-retained"},
      {"documentId","doc"},{"callerScope","scope"},{"clientRequestId","request"},{"stateRevision","1"},{"events",QJsonArray{}},
      {"key",QString::fromUtf8(QJsonDocument(QJsonArray{"doc","scope","request"}).toJson(QJsonDocument::Compact))},
      {"stagePlan",QJsonObject{{"targets",QJsonArray{QJsonObject{{"staging",QJsonObject{{"path",path},{"expectedHead",head}}}}}},
        {"parents",QJsonArray{"raw"}},{"outputSetId","set"},{"outputSpec",QJsonObject{}}}},
      {"preparedManifest",QJsonObject{}},{"artifactManifest",QJsonObject{}},
      {"presentationIntent",QJsonObject{}},{"externalRequest",QJsonObject{}},{"frozenRequest",QJsonObject{{"plan",QJsonObject{}}}}});
  }
  static bool seed(DataCatalog &cat, const QString &dir) {
    CatalogEntity e; e.id = "well"; e.name = "A1"; e.entityType = "well";
    if (!cat.addEntity(e)) return false;
    CatalogAsset a; a.id = "asset"; a.type = "well_log"; a.displayName = "A1.las";
    if (!cat.addAsset(a)) return false;
    CatalogVersion raw; raw.id = "raw"; raw.assetId = a.id; raw.stage = "RAW";
    raw.path = DataCatalog::managedPath(raw.stage, raw.assetId, raw.id, "a.las");
    if (!write(QDir(dir).filePath(raw.path), 13) || !cat.addVersion(raw)) return false;
    CatalogVersion derived = raw; derived.id = "derived"; derived.versionNumber = 2;
    derived.stage = "DERIVED"; derived.path = DataCatalog::managedPath(derived.stage, derived.assetId, derived.id, "a.las");
    derived.parentVersionIds = {raw.id};
    if (!write(QDir(dir).filePath(derived.path), 29) || !cat.addVersion(derived)) return false;
    CatalogVersion child = derived; child.id = "child"; child.versionNumber = 3;
    child.path = DataCatalog::managedPath(child.stage, child.assetId, child.id, "a.las"); child.parentVersionIds = {derived.id};
    if (!write(QDir(dir).filePath(child.path), 37) || !cat.addVersion(child)) return false;
    EntityAssetLink link; link.assetId = a.id; link.entityType = "well"; link.entityId = e.id; link.role = "well_log";
    if (!cat.addLink(link)) return false;
    link.role = "reference"; if (!cat.addLink(link)) return false; // duplicate entity attribution dedup
    return cat.markDownstreamStale(raw.id, "上游源字节改变");
  }
private slots:
  void missingConsumedVersionRowsRefuseEveryCleanupProjection() {
    QTemporaryDir root;DataCatalog cat;QVERIFY(cat.open(root.path()));QVERIFY(seed(cat,root.path()));
    const QString orphan="artifacts/derived/unrelated/unreferenced.bin";QVERIFY(write(root.filePath(orphan),23));
    QVERIFY(operations(root.path(),{operation("artifacts/derived/agent/new/output.bin")}));
    auto source=snapshot(&cat);const auto healthy=scan(source);QVERIFY(healthy.complete);source.retention=healthy.source.retention;
    auto choice=preview(healthy,{}, {orphan});QVERIFY(choice.valid);QString error;QVERIFY(validate(choice,{},&error));
    source.versions.erase(std::remove_if(source.versions.begin(),source.versions.end(),[](const CatalogVersion &v){return v.id=="raw";}),source.versions.end());
    const auto unresolved=scan(source);QVERIFY(!unresolved.complete);QCOMPARE(unresolved.source.retention.code,QString("operation_retention_version_missing"));
    auto stale=healthy;stale.source=source;QVERIFY(!preview(stale,{}, {orphan}).valid);
    choice.source.versions=source.versions;QVERIFY(!validate(choice,{},&error));QVERIFY(QFileInfo::exists(root.filePath(orphan)));
  }
  void storedInputManifestProtectsBodiesAndCompletedResponseCache() {
    QTemporaryDir root;const QString directory=".paleo/capability-ops/well-inputs/frozen-input";
    const auto manifest=root.filePath(directory+"/manifest.json"),body=root.filePath(directory+"/body-0.json"),cache=root.filePath(directory+"/result-0-sha.json");
    QVERIFY(write(manifest,2));QVERIFY(write(body,12));QVERIFY(write(cache,16));
    const auto rowFor=[&](const QJsonValue &ref){auto row=operation("artifacts/derived/a/v/out.bin");
      row["frozenRequest"]=QJsonObject{{"plan",QJsonObject{{"storedInput",ref}}}};return seal(row);};
    const QJsonObject ref{{"directory",directory},{"manifestHash",QString(64,'a')}};
    QVERIFY(operations(root.path(),{rowFor(ref)}));const auto retained=operationRetention(root.path());
    QVERIFY2(retained.complete,qPrintable(retained.code));QVERIFY(retained.paths.contains(directory+"/manifest.json"));
    QVERIFY(retained.protectsPath(body));QVERIFY(retained.protectsPath(cache));
    for(const QJsonValue invalid:{QJsonValue("path-only"),QJsonValue(QJsonObject{{"directory",directory},{"manifestHash","bad"}}),
        QJsonValue(QJsonObject{{"directory","../escape"},{"manifestHash",QString(64,'a')}}),
        QJsonValue(QJsonObject{{"directory",".paleo/capability-ops/missing"},{"manifestHash",QString(64,'a')}})}) {
      QVERIFY(operations(root.path(),{rowFor(invalid)}));QVERIFY(!operationRetention(root.path()).complete);
    }
    QVERIFY(operations(root.path(),{rowFor(ref)}));QVERIFY(QFile::remove(manifest));QVERIFY(!operationRetention(root.path()).complete);
  }
  void operationRootsProtectPreparedSidecarsSourcesAndPreviewRevisions() {
    QTemporaryDir dir;DataCatalog cat;QVERIFY(cat.open(dir.path()));QVERIFY(seed(cat,dir.path()));
    const QString target="artifacts/derived/agent/v1/output.tif", prepared="artifacts/derived/agent2/v2/output.gpkg";
    const QString artifact=".paleo/capability-ops/op-retained/private/part.bin", orphan="artifacts/derived/orphan/unreferenced.bin";
    const QString source="raw/legacy-model/input.sgy";
    const QString raster="raw/legacy-tile/input.tif";
    for(const auto &path:QStringList{target,target+".aux.xml",prepared,artifact,orphan,source,source+".sidecar",raster,raster+".aux.xml"})
      QVERIFY(write(dir.filePath(path),11));
    auto row=operation(target,"child");
    row["preparedManifest"]=QJsonObject{{"schema",2},{"members",QJsonArray{QJsonObject{{"staging",QJsonObject{{"path",prepared},{"expectedHead",""}}}}}}};
    row["artifactManifest"]=QJsonObject{{"schema",1},{"ports",QJsonArray{QJsonObject{{"path",artifact}}}}};
    row["frozenRequest"]=QJsonObject{{"plan",QJsonObject{{"sourceVerification",QJsonObject{{"path",dir.filePath(source)},{"versionId",""}}},
      {"raster",QJsonObject{{"path",dir.filePath(raster)}}}}}};
    row=seal(row);
    QVERIFY(operations(dir.path(),{row}));const auto roots=operationRetention(dir.path());QVERIFY2(roots.complete,qPrintable(roots.code));
    QCOMPARE(roots.versionIds,QStringList({"child","raw"}));QCOMPARE(roots.paths.size(),5);
    QVERIFY(roots.protectsPath(dir.filePath(target+".aux.xml")));QVERIFY(roots.protectsPath(dir.filePath(source+".sidecar")));
    QVERIFY(roots.protectsPath(dir.filePath(raster+".aux.xml")));
    QVERIFY(roots.protectsPath(dir.filePath(artifact)));QVERIFY(!roots.protectsPath(dir.filePath(orphan)));
    const auto report=scan(snapshot(&cat));QVERIFY(report.complete);QCOMPARE(report.orphans.size(),1);
    QCOMPARE(report.orphans.first().relativePath,orphan);QVERIFY(!preview(report,{"child"},{}).valid);
    const auto selected=preview(report,{}, {orphan});QVERIFY(selected.valid);QString error;QVERIFY(validate(selected,{},&error));
    row["execution"]="succeeded";row["commitState"]="committed";QVERIFY(operations(dir.path(),{row}));
    QCOMPARE(operationRetention(dir.path()).revision,roots.revision);QVERIFY(validate(selected,{},&error));
    auto stage=row["stagePlan"].toObject();stage["parents"]=QJsonArray{"raw","derived"};row["stagePlan"]=stage;row=seal(row);
    QVERIFY(operations(dir.path(),{row}));QVERIFY(!preview(report,{}, {orphan}).valid);QVERIFY(!validate(selected,{},&error));
    QVERIFY(QFileInfo::exists(dir.filePath(orphan)));
  }
  void operationRetentionRejectsUnknownCorruptUnsafeAndOversizedJournals() {
    QTemporaryDir dir;DataCatalog cat;QVERIFY(cat.open(dir.path()));
    const auto absent=operationRetention(dir.path());QVERIFY(absent.complete);QVERIFY(absent.revision.isEmpty());
    QVERIFY(!operationRetention(dir.path(),[]{return true;}).complete);
    auto row=operation("artifacts/derived/a/v/file.bin");QVERIFY(operations(dir.path(),{row}));QVERIFY(operationRetention(dir.path()).complete);
    auto corrupt=row;corrupt.remove("documentId");QVERIFY(operations(dir.path(),{corrupt}));QVERIFY(!operationRetention(dir.path()).complete);
    QVERIFY(operations(dir.path(),{row,row}));QVERIFY(!operationRetention(dir.path()).complete);
    auto missingStage=row;missingStage["stagePlan"]=QJsonObject{};missingStage=seal(missingStage);
    QVERIFY(operations(dir.path(),{missingStage}));QVERIFY(!operationRetention(dir.path()).complete);
    auto edited=row;auto changed=edited["stagePlan"].toObject();
    changed["targets"]=QJsonArray{QJsonObject{{"staging",QJsonObject{{"path","artifacts/derived/a/v/secretly-moved.bin"},{"expectedHead",""}}}}};edited["stagePlan"]=changed;
    QVERIFY(operations(dir.path(),{edited}));QCOMPARE(operationRetention(dir.path()).code,QStringLiteral("operation_retention_plan_mismatch"));
    row["schemaVersion"]=1;QVERIFY(operations(dir.path(),{row}));QVERIFY(!operationRetention(dir.path()).complete);QVERIFY(!scan(snapshot(&cat)).complete);
    row["schemaVersion"]=2;auto stage=row["stagePlan"].toObject();
    stage["targets"]=QJsonArray{QJsonObject{{"staging",QJsonObject{{"path","../outside.bin"},{"expectedHead",""}}}}};row["stagePlan"]=stage;row=seal(row);
    QVERIFY(operations(dir.path(),{row}));QVERIFY(!operationRetention(dir.path()).complete);
    const auto path=dir.filePath(".paleo/capability-ops/operations.json");QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));QCOMPARE(file.write("{broken"),qint64(7));file.close();
    QVERIFY(!operationRetention(dir.path()).complete);
    QVERIFY(file.open(QIODevice::WriteOnly));QVERIFY(file.resize(128LL*1024*1024+1));file.close();
    QCOMPARE(operationRetention(dir.path()).code,QStringLiteral("operation_retention_budget_exceeded"));
  }
  // #283：shapefile 族附属文件（.shx/.dbf/.prj/.cpg）无 catalog 记录但与主
  // .shp 同生共死——scan 不得列为孤儿；purge 过期版本时成员一并回收、目录
  // 无残留；已单独登记为其他版本的成员不被连带删。
  void shapefileSidecarsStayReferencedAndPurgeTogether() {
    QTemporaryDir dir; DataCatalog cat; QVERIFY(cat.open(dir.path()));
    CatalogAsset shp; shp.id = "shp-asset"; shp.type = "horizon"; shp.displayName = "boundary.shp";
    QVERIFY(cat.addAsset(shp));
    CatalogVersion raw; raw.id = "shp-raw"; raw.assetId = shp.id; raw.stage = "RAW";
    raw.path = DataCatalog::managedPath(raw.stage, raw.assetId, raw.id, "boundary.shp");
    QVERIFY(!raw.path.isEmpty());
    QVERIFY(write(QDir(dir.path()).filePath(raw.path), 11));
    QVERIFY(cat.addVersion(raw));
    CatalogVersion derived = raw; derived.id = "shp-derived"; derived.versionNumber = 2;
    derived.stage = "DERIVED";
    derived.path = DataCatalog::managedPath(derived.stage, derived.assetId, derived.id, "boundary.shp");
    derived.parentVersionIds = {raw.id};
    QVERIFY(!derived.path.isEmpty());
    QVERIFY(write(QDir(dir.path()).filePath(derived.path), 23));
    QVERIFY(cat.addVersion(derived));
    // 族附属文件落位（模拟 copyBundleMembersIntoVersion 拷进受管版本目录）。
    const QString derivedAbs = QDir(dir.path()).filePath(derived.path);
    const QDir vdir = QFileInfo(derivedAbs).absoluteDir();
    for (const char *ext : {"shx", "dbf", "prj", "cpg"})
      QVERIFY(write(vdir.filePath(QStringLiteral("boundary.%1").arg(QLatin1String(ext))), 7));
    // boundary.dbf 另有人单独登记为版本字节：是别人的主件，不能连带删。
    CatalogAsset dbf; dbf.id = "dbf-asset"; dbf.type = "table"; QVERIFY(cat.addAsset(dbf));
    CatalogVersion dbfVer; dbfVer.id = "dbf-ver"; dbfVer.assetId = dbf.id; dbfVer.stage = "RAW";
    dbfVer.managed = false; // 外链版本：绝对源路径
    dbfVer.path = vdir.filePath(QStringLiteral("boundary.dbf"));
    QString addErr; QVERIFY2(cat.addVersion(dbfVer, &addErr), qPrintable(addErr));
    // 大写族：ingestplan 归组大小写不敏感（.SHP/.SHX/.DBF 同主名即成族），
    // 成员按原名落位受管版本目录——探测也必须大小写不敏感，否则 AREA.SHX
    // 等仍是孤儿、仍会被误删。
    CatalogAsset area; area.id = "area-asset"; area.type = "horizon";
    area.displayName = QStringLiteral("AREA.SHP"); QVERIFY(cat.addAsset(area));
    CatalogVersion areaVer; areaVer.id = "area-ver"; areaVer.assetId = area.id;
    areaVer.stage = "RAW";
    areaVer.path = DataCatalog::managedPath(areaVer.stage, areaVer.assetId, areaVer.id,
                                            QStringLiteral("AREA.SHP"));
    QVERIFY(!areaVer.path.isEmpty());
    const QString areaAbs = QDir(dir.path()).filePath(areaVer.path);
    QVERIFY(write(areaAbs, 31));
    QVERIFY(cat.addVersion(areaVer));
    const QDir areaDir = QFileInfo(areaAbs).absoluteDir();
    for (const char *ext : {"SHX", "DBF", "PRJ", "CPG"})
      QVERIFY(write(areaDir.filePath(QStringLiteral("AREA.%1").arg(QLatin1String(ext))), 5));
    QVERIFY(cat.markDownstreamStale(raw.id, "上游源字节改变"));
    const auto r = scan(snapshot(&cat)); QVERIFY(r.complete);
    QCOMPARE(r.orphans.size(), 0);
    QVERIFY(r.versionSidecars.contains("shp-derived"));
    QCOMPARE(r.versionSidecars.value("shp-derived").size(), 4);
    QCOMPARE(r.versionSidecars.value("area-ver").size(), 4); // 大写族同样随主件
    // 主 .shp + shx/prj/cpg 三个附属；dbf 已登记为别人版本，不进待删清单。
    const auto plan = preview(r, {"shp-derived"}, {});
    QVERIFY(plan.valid); QCOMPARE(plan.affectedVersions, 1);
    QCOMPARE(plan.files.size(), 4); QCOMPARE(plan.bytes, 23 + 7 * 3);
    QString err; QVERIFY(validate(plan, {}, &err));
    // 控制器全链路：确认回收后成员一并消失、保留者无损、复扫无残留孤儿。
    StorageGovernanceController controller; controller.setCatalog(&cat);
    QSignalSpy scanned(&controller, &StorageGovernanceController::reportReady);
    QSignalSpy planned(&controller, &StorageGovernanceController::previewReady);
    QSignalSpy cleaned(&controller, &StorageGovernanceController::cleanupFinished);
    controller.scan(); QTRY_COMPARE(scanned.size(), 1);
    controller.requestPreview({"shp-derived"}, {}); QCOMPARE(planned.size(), 1);
    const auto live = qvariant_cast<Preview>(planned.first().first());
    QVERIFY(live.valid); QCOMPARE(live.files.size(), 4);
    controller.confirmPreview(); QTRY_COMPARE(cleaned.size(), 1);
    const auto out = qvariant_cast<paleo::assetops::PurgeOutcome>(cleaned.first().first());
    QVERIFY(out.leftoverFiles.isEmpty());
    QVERIFY(!QFileInfo::exists(derivedAbs));
    for (const char *ext : {"shx", "prj", "cpg"})
      QVERIFY(!QFileInfo::exists(vdir.filePath(QStringLiteral("boundary.%1").arg(QLatin1String(ext)))));
    QVERIFY(QFileInfo::exists(vdir.filePath(QStringLiteral("boundary.dbf"))));
    QCOMPARE(cat.versions().size(), 3);
    controller.scan(); QTRY_COMPARE(scanned.size(), 2);
    const auto after = qvariant_cast<Report>(scanned.last().first());
    QVERIFY(after.complete); QCOMPARE(after.orphans.size(), 0);
  }
  // B-03/A-14（full-audit-20261020）：bundle 与非 bundle 版本同权族扫描——
  // 导入后由 QGIS/GDAL 生成的 .aux.xml/.qix/.qml（shp）与 .tfw/.aux.xml/.qml
  // （栅格）此前被误报孤儿（bundle 版本以 bundle_members.isEmpty() 为门完全
  // 跳过族扫描），用户确认回收即删样式/统计文件。
  void generatedSidecarsAndBundleVersionsStayReferenced() {
    QTemporaryDir dir; DataCatalog cat; QVERIFY(cat.open(dir.path()));
    // bundle 版本（paleomap 导入形态：bundle_members 已登记 shx/dbf）
    CatalogAsset a; a.id = "pm"; a.type = "paleogeographic_map";
    a.displayName = "pm.shp"; QVERIFY(cat.addAsset(a));
    CatalogVersion v; v.id = "pm-raw"; v.assetId = a.id; v.stage = "RAW";
    v.path = DataCatalog::managedPath(v.stage, v.assetId, v.id, "pm.shp");
    QVERIFY(!v.path.isEmpty());
    QVERIFY(write(QDir(dir.path()).filePath(v.path), 11));
    const QString vAbs = QDir(dir.path()).filePath(v.path);
    const QDir vdir = QFileInfo(vAbs).absoluteDir();
    QVERIFY(write(vdir.filePath(QStringLiteral("pm.shx")), 5));
    QVERIFY(write(vdir.filePath(QStringLiteral("pm.dbf")), 9));
    v.extra.insert(QStringLiteral("bundle_members"),
                   QVariantList{QVariantMap{{"file", "pm.shx"}, {"sha256", "x"}},
                                QVariantMap{{"file", "pm.dbf"}, {"sha256", "y"}}});
    QVERIFY(cat.addVersion(v));
    // 导入后 QGIS/GDAL 在受管版本目录旁生成：统计/直方图、空间索引、样式。
    QVERIFY(write(vdir.filePath(QStringLiteral("pm.shp.aux.xml")), 3));
    QVERIFY(write(vdir.filePath(QStringLiteral("pm.qix")), 3));
    QVERIFY(write(vdir.filePath(QStringLiteral("pm.qml")), 3));
    // 非 bundle 栅格版本：tif 主件 + world 文件 + 统计 + 样式。
    CatalogAsset r; r.id = "rs"; r.type = "paleogeographic_map";
    r.displayName = "rs.tif"; QVERIFY(cat.addAsset(r));
    CatalogVersion rv; rv.id = "rs-raw"; rv.assetId = r.id; rv.stage = "RAW";
    rv.path = DataCatalog::managedPath(rv.stage, rv.assetId, rv.id, "rs.tif");
    QVERIFY(!rv.path.isEmpty());
    QVERIFY(write(QDir(dir.path()).filePath(rv.path), 17));
    const QDir rdir = QFileInfo(QDir(dir.path()).filePath(rv.path)).absoluteDir();
    QVERIFY(write(rdir.filePath(QStringLiteral("rs.tfw")), 5));
    QVERIFY(write(rdir.filePath(QStringLiteral("rs.tif.aux.xml")), 5));
    QVERIFY(write(rdir.filePath(QStringLiteral("rs.qml")), 4));
    QVERIFY(cat.addVersion(rv));
    const auto report = scan(snapshot(&cat)); QVERIFY(report.complete);
    QCOMPARE(report.orphans.size(), 0);
    // bundle 成员（2）+ 族命中（aux.xml/qix/qml，3）；栅格族命中（tfw/aux.xml/qml，3）。
    QCOMPARE(report.versionSidecars.value(QStringLiteral("pm-raw")).size(), 5);
    QCOMPARE(report.versionSidecars.value(QStringLiteral("rs-raw")).size(), 3);
  }
  void exactAggregationOrphansAndReasons() {
    QTemporaryDir dir; DataCatalog cat; QVERIFY(cat.open(dir.path())); QVERIFY(seed(cat, dir.path()));
    const QString orphan = "raw/unregistered/file.dat";
    QVERIFY(write(QDir(dir.path()).filePath(orphan), 17));
    // External alias inside project references registered bytes with lexical . /
    // .. form. Canonical identity keeps it out of the orphan list.
    CatalogAsset alias; alias.id = "alias"; alias.type = "seismic"; QVERIFY(cat.addAsset(alias));
    CatalogVersion v; v.id = "alias-version"; v.assetId = alias.id; v.managed = false; v.stage = "RAW";
    v.path = QDir(dir.path()).filePath("raw/asset/raw/../raw/a.las"); QVERIFY(cat.addVersion(v));
    const auto r = scan(snapshot(&cat)); QVERIFY(r.complete);
    QCOMPARE(r.versionBytes, 92); QCOMPARE(r.bytesByType.value("well_log"), 79);
    QCOMPARE(r.bytesByType.value("seismic"), 13); QCOMPARE(r.bytesByEntity.value("well"), 79);
    QCOMPARE(r.bytesByEntity.value(QString()), 13); QCOMPARE(r.orphans.size(), 1);
    QCOMPARE(r.orphans.first().relativePath, orphan); QCOMPARE(r.orphans.first().sizeBytes, 17);
    QCOMPARE(r.stale.size(), 2); QCOMPARE(r.stale.first().reason, QString("上游源字节改变"));
    qint64 sum = 0;
    for (const auto &v : cat.versions()) sum += QFileInfo(DataCatalog::resolvedVersionPath(dir.path(), v)).size();
    QCOMPARE(r.versionBytes, sum);
    auto plan = preview(r, {"derived"}, {}); QVERIFY(!plan.valid); QVERIFY(!plan.blocked.isEmpty());
    plan = preview(r, {"derived", "child"}, {orphan}); QVERIFY(plan.valid);
    QCOMPARE(plan.affectedVersions, 2); QCOMPARE(plan.bytes, 83); QCOMPARE(plan.files.size(), 3);
    QString err; QVERIFY(validate(plan, {}, &err));
    QVERIFY(write(QDir(dir.path()).filePath(orphan), 18)); QVERIFY(!validate(plan, {}, &err));
  }
  void exactMutatorAndAbort() {
    QTemporaryDir dir; DataCatalog cat; QVERIFY(cat.open(dir.path())); QVERIFY(seed(cat, dir.path()));
    QString err; QVERIFY(!cat.removeStaleVersions({"derived"}, &err));
    QCOMPARE(cat.versions().size(), 3);
    const int revision = cat.catalogRevision();
    { DataCatalog::BatchSave batch(&cat); QVERIFY(cat.removeStaleVersions({"child", "derived"})); batch.abort(); }
    QCOMPARE(cat.versions().size(), 3); QCOMPARE(cat.catalogRevision(), revision);
    QVERIFY(cat.indexHealthy());
    QVERIFY(!cat.removeStaleVersions({"raw"}));
    QVERIFY(cat.removeStaleVersions({"derived", "child"}));
    QCOMPARE(cat.currentVersion("asset").id, QString("raw"));
    QCOMPARE(cat.links().size(), 2); QVERIFY(cat.indexHealthy());
    QVERIFY(cat.open(dir.path())); QCOMPARE(cat.versions().size(), 1);
  }
  void previewAndExecutionAgree() {
    QTemporaryDir dir; DataCatalog cat; QVERIFY(cat.open(dir.path())); QVERIFY(seed(cat, dir.path()));
    const QString orphan = "raw/unknown.dat"; QVERIFY(write(QDir(dir.path()).filePath(orphan), 17));
    StorageGovernanceController controller; controller.setCatalog(&cat);
    QSignalSpy scanned(&controller, &StorageGovernanceController::reportReady);
    QSignalSpy planned(&controller, &StorageGovernanceController::previewReady);
    QSignalSpy cleaned(&controller, &StorageGovernanceController::cleanupFinished);
    controller.scan(); QTRY_COMPARE(scanned.size(), 1);
    controller.requestPreview({"derived", "child"}, {orphan}); QCOMPARE(planned.size(), 1);
    const auto plan = qvariant_cast<Preview>(planned.first().first()); QVERIFY(plan.valid); QCOMPARE(plan.bytes, 83);
    QCOMPARE(cat.versions().size(), 3); QVERIFY(QFileInfo(QDir(dir.path()).filePath(orphan)).exists());
    controller.confirmPreview(); QTRY_COMPARE(cleaned.size(), 1);
    const auto out = qvariant_cast<paleo::assetops::PurgeOutcome>(cleaned.first().first());
    QCOMPARE(out.bytesFreed, plan.bytes); QVERIFY(out.leftoverFiles.isEmpty());
    QCOMPARE(cat.versions().size(), 1); QCOMPARE(cat.assets().size(), 1); QVERIFY(cat.indexHealthy());
    QVERIFY(!QFileInfo(QDir(dir.path()).filePath(orphan)).exists());
    QVERIFY(QFileInfo(QDir(dir.path()).filePath(cat.currentVersion("asset").path)).exists());
  }
  void cancellationAndChangedBaselineMakeNoActions() {
    QTemporaryDir dir; DataCatalog cat; QVERIFY(cat.open(dir.path())); QVERIFY(seed(cat, dir.path()));
    StorageGovernanceController controller; controller.setCatalog(&cat);
    QSignalSpy scanned(&controller, &StorageGovernanceController::reportReady);
    QSignalSpy cleaned(&controller, &StorageGovernanceController::cleanupFinished);
    controller.scan(); controller.cancel(); QTRY_COMPARE(scanned.size(), 1);
    QVERIFY(!qvariant_cast<Report>(scanned.first().first()).complete);
    controller.requestPreview({"child"}, {}); controller.confirmPreview(); QCOMPARE(cleaned.size(), 0);
    controller.scan(); QTRY_COMPARE(scanned.size(), 2);
    controller.requestPreview({"child"}, {});
    CatalogAsset a; a.id = "new"; a.type = "unknown"; QVERIFY(cat.addAsset(a));
    controller.confirmPreview(); QCOMPARE(cleaned.size(), 0); QCOMPARE(cat.versions().size(), 3);
    controller.scan(); QTRY_COMPARE(scanned.size(), 3);
    controller.requestPreview({"child"}, {}); controller.confirmPreview(); controller.cancel();
    QTRY_VERIFY(!controller.busy()); QCOMPARE(cleaned.size(), 0); QCOMPARE(cat.versions().size(), 3);
  }
  void sharedFileAndUncoveredSymlink() {
    QTemporaryDir dir; DataCatalog cat; QVERIFY(cat.open(dir.path())); QVERIFY(seed(cat, dir.path()));
    CatalogAsset a; a.id = "keeper"; a.type = "horizon"; QVERIFY(cat.addAsset(a));
    auto v = cat.versionById("child"); v.id = "kept-version"; v.assetId = a.id; v.stage = "RAW";
    v.parentVersionIds.clear(); v.extra.clear(); QVERIFY(cat.addVersion(v));
    const auto r = scan(snapshot(&cat)); const auto p = preview(r, {"child"}, {});
    QVERIFY(p.valid); QCOMPARE(p.bytes, 0); QCOMPARE(p.affectedVersions, 1);
    QVERIFY(cat.removeStaleVersions({"child"}));
    const QString shared = QDir(dir.path()).filePath(v.path);
    const auto out = paleo::assetops::purgeManagedFiles(dir.path(), {shared}, cat.versions());
    QCOMPARE(out.bytesFreed, 0); QVERIFY(QFileInfo(shared).exists());
#ifndef Q_OS_WIN
    const QString link = QDir(dir.path()).filePath("raw/link");
    QVERIFY(QFile::link(QDir(dir.path()).filePath("derived"), link));
    const auto incomplete = scan(snapshot(&cat)); QVERIFY(!incomplete.complete); QVERIFY(!incomplete.uncovered.isEmpty());
    QVERIFY(!preview(incomplete, {"derived"}, {}).valid);
#endif
  }
  void shaMidFileCancellationAndUiHeartbeat() {
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).filePath("external.sgy"); QVERIFY(write(path, 32 * 1024 * 1024));
    DataCatalog cat; QVERIFY(cat.open(dir.path()));
    CatalogAsset a; a.id = "ext"; a.type = "seismic"; QVERIFY(cat.addAsset(a));
    CatalogVersion v; v.id = "ext-ver"; v.assetId = a.id; v.managed = false; v.stage = "RAW"; v.path = path;
    v.sha256 = DataCatalog::sha256FileHex(path); QVERIFY(cat.addVersion(v));
    int chunks = 0; QString error;
    QVERIFY(DataCatalog::sha256FileHex(path, &error, [&] { return ++chunks > 2; }).isEmpty());
    QCOMPARE(chunks, 3); QVERIFY(!error.isEmpty());
    StorageGovernanceController controller; controller.setCatalog(&cat);
    QSignalSpy done(&controller, &StorageGovernanceController::shaReady);
    int beats = 0; QTimer timer; timer.setInterval(0); connect(&timer, &QTimer::timeout, this, [&] { ++beats; });
    timer.start(); controller.verifySha(); QTRY_COMPARE(done.size(), 1); timer.stop();
    QVERIFY(beats > 0); QCOMPARE(done.first().at(1).toBool(), true);
    controller.verifySha(); controller.cancel(); QTRY_COMPARE(done.size(), 2);
    QCOMPARE(done.last().at(1).toBool(), false); QCOMPARE(cat.versions().size(), 1);
  }
  void physicalPurgeLeaseBlocksNewReferences() {
    QTemporaryDir dir; DataCatalog cat; QVERIFY(cat.open(dir.path())); QVERIFY(seed(cat, dir.path()));
    auto lease = CatalogPurgeLease::acquire(QDir::current().relativeFilePath(cat.catalogPath())); QVERIFY(lease);
    QVERIFY(CatalogPurgeLease::isHeld(cat.catalogPath()));
    QVERIFY(!CatalogPurgeLease::acquire(cat.catalogPath()));
    CatalogAsset a; a.id = "new"; QString error;
    QVERIFY(!cat.addAsset(a, &error)); QVERIFY(!error.isEmpty());
    QVERIFY(!cat.open(dir.path(), &error));
    lease.reset(); QVERIFY(cat.addAsset(a, &error)); QVERIFY(cat.open(dir.path()));
  }
  void scanScaleHeartbeat() {
    const int count = qEnvironmentVariableIntValue("PALEO_CATALOG_SCALE");
    if (count < 10000) QSKIP("PALEO_CATALOG_SCALE=10000 enables directory-scale worker evidence");
    QTemporaryDir dir; QVERIFY(PerfFixtures::makeSyntheticCatalogDir(dir.path(), count));
    DataCatalog cat; QVERIFY(cat.open(dir.path()));
    StorageGovernanceController controller; controller.setCatalog(&cat);
    QSignalSpy scanned(&controller, &StorageGovernanceController::reportReady);
    QSignalSpy progress(&controller, &StorageGovernanceController::progress);
    int beats = 0; QTimer timer; timer.setInterval(0); connect(&timer, &QTimer::timeout, this, [&] { ++beats; });
    const auto source = snapshot(&cat);
    QElapsedTimer clock; clock.start(); const auto baseline = paleo::storage::scan(source);
    const qint64 scanCost = clock.nsecsElapsed(); QVERIFY(baseline.complete);
    clock.restart(); timer.start(); controller.scan(); const qint64 submitCost = clock.nsecsElapsed();
    QVERIFY2(double(submitCost) / scanCost < 0.5, "scan submission must remain cheaper than full directory stat/scan");
    QTRY_COMPARE_WITH_TIMEOUT(scanned.size(), 1, 120000); timer.stop();
    const auto report = qvariant_cast<Report>(scanned.first().first()); QVERIFY(report.complete); QCOMPARE(report.orphans.size(), 0);
    QCOMPARE(report.versionFiles.size(), count); QVERIFY(beats > 0); QVERIFY(progress.size() > 0);
    qInfo() << "SCALE storage submit/scan" << double(submitCost)/scanCost << "heartbeat" << beats << "progress" << progress.size() << "versions" << count;
  }
};
QTEST_MAIN(TestStorageGovernance)
#include "tst_storagegovernance.moc"
