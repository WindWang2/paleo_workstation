// G08/G12 证据反向索引（services/evidenceindex）契约回归：
// 索引是可重建检索结构而非第二事实源——同步幂等性、sha 变化替换、目录
// 删除传播、损坏记录跳过计数、查询子集正确性与 limit/offset 有界性。
// 纯 services 层：OGR 读写最小 GPKG 装置，DataCatalog 直接手搓版本。
#include <QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <cpl_error.h>
#include <gdal.h>
#include <ogr_api.h>

#include "../src/catalog/datacatalog.h"
#include "../src/services/evidenceindex.h"

using paleo::evidenceindex::Entry;
using paleo::evidenceindex::Index;

namespace {

struct FixtureFeature {
  QString uid;
  QString evidenceJson;
  QString interpretationJson;
  bool identityReview = false;
};

QString encodeJson(const QVariant &value) {
  return QString::fromUtf8(QJsonDocument::fromVariant(value).toJson(QJsonDocument::Compact));
}

// OGR 手写最小成果 GPKG：字段面与 mappingfeatureidentity/现写入面一致。
bool writeGpkg(const QString &path, const QVector<FixtureFeature> &features,
               const QString &layerName = QStringLiteral("facies_polygons")) {
  QFile::remove(path);
  QFile::remove(path + QStringLiteral("-wal")); // 残留 WAL 副本会污染重建库
  QFile::remove(path + QStringLiteral("-shm"));
  CPLPushErrorHandler(CPLQuietErrorHandler);
  GDALDriverH driver = GDALGetDriverByName("GPKG");
  if (!driver) {
    CPLPopErrorHandler();
    return false;
  }
  GDALDatasetH ds = GDALCreate(driver, path.toUtf8().constData(), 0, 0, 0,
                               GDT_Unknown, nullptr);
  if (!ds) {
    CPLPopErrorHandler();
    return false;
  }
  OGRLayerH layer = GDALDatasetCreateLayer(ds, layerName.toUtf8().constData(),
                                           nullptr, wkbPolygon, nullptr);
  bool ok = true;
  const auto addStringField = [&layer, &ok](const char *name) {
    OGRFieldDefnH field = OGR_Fld_Create(name, OFTString);
    ok = OGR_L_CreateField(layer, field, true) == OGRERR_NONE && ok;
    OGR_Fld_Destroy(field);
  };
  addStringField("facies_code");
  addStringField("paleo_feature_uid");
  addStringField("facies_evidence");
  addStringField("paleo_interpretation");
  {
    OGRFieldDefnH field = OGR_Fld_Create("paleo_identity_review", OFTInteger);
    OGR_Fld_SetSubType(field, OFSTBoolean);
    ok = OGR_L_CreateField(layer, field, true) == OGRERR_NONE && ok;
    OGR_Fld_Destroy(field);
  }
  OGRFeatureDefnH defn = OGR_L_GetLayerDefn(layer);
  const int codeIndex = OGR_FD_GetFieldIndex(defn, "facies_code");
  const int uidIndex = OGR_FD_GetFieldIndex(defn, "paleo_feature_uid");
  const int evidenceIndex = OGR_FD_GetFieldIndex(defn, "facies_evidence");
  const int interpretationIndex = OGR_FD_GetFieldIndex(defn, "paleo_interpretation");
  const int reviewIndex = OGR_FD_GetFieldIndex(defn, "paleo_identity_review");
  int ordinal = 0;
  for (const auto &fixture : features) {
    OGRFeatureH feature = OGR_F_Create(defn);
    OGRGeometryH ring = OGR_G_CreateGeometry(wkbLinearRing);
    const double x = 100.0 + 10.0 * ordinal, y = 200.0 + 10.0 * ordinal;
    OGR_G_AddPoint_2D(ring, x, y);
    OGR_G_AddPoint_2D(ring, x + 5, y);
    OGR_G_AddPoint_2D(ring, x + 5, y + 5);
    OGR_G_AddPoint_2D(ring, x, y + 5);
    OGR_G_AddPoint_2D(ring, x, y);
    OGRGeometryH polygon = OGR_G_CreateGeometry(wkbPolygon);
    OGR_G_AddGeometry(polygon, ring);
    OGR_F_SetGeometry(feature, polygon);
    OGR_F_SetFieldString(feature, codeIndex, "Fs");
    OGR_F_SetFieldString(feature, uidIndex, fixture.uid.toUtf8().constData());
    OGR_F_SetFieldString(feature, evidenceIndex, fixture.evidenceJson.toUtf8().constData());
    OGR_F_SetFieldString(feature, interpretationIndex,
                         fixture.interpretationJson.toUtf8().constData());
    OGR_F_SetFieldInteger(feature, reviewIndex, fixture.identityReview ? 1 : 0);
    ok = OGR_L_CreateFeature(layer, feature) == OGRERR_NONE && ok;
    OGR_F_Destroy(feature);
    OGR_G_DestroyGeometry(ring);
    OGR_G_DestroyGeometry(polygon);
    ++ordinal;
  }
  GDALClose(ds);
  CPLPopErrorHandler();
  return ok;
}

// 登记一个矢量成果版本（外链绝对路径；extra 与 mappingworkbench record()
// 的成果面一致）。
bool addMappingVersion(DataCatalog *catalog, const QString &versionId,
                       const QString &filePath,
                       const QString &horizon = QStringLiteral("D61"),
                       const QString &layerName = QStringLiteral("facies_polygons")) {
  const QString assetId = QStringLiteral("a-%1").arg(versionId);
  CatalogAsset a;
  a.id = assetId;
  a.type = QStringLiteral("map_product");
  a.format = QStringLiteral("gpkg");
  a.displayName = versionId + QStringLiteral(".gpkg");
  CatalogVersion v;
  v.id = versionId;
  v.assetId = assetId;
  v.stage = QStringLiteral("OUTPUT");
  v.versionNumber = 1;
  v.managed = false;
  v.path = filePath;
  QVariantMap extra;
  extra.insert(QStringLiteral("mapping_product"), true);
  extra.insert(QStringLiteral("layer_type"), QStringLiteral("vector"));
  extra.insert(QStringLiteral("kind"), QStringLiteral("facies_polygons"));
  extra.insert(QStringLiteral("horizon"), horizon);
  extra.insert(QStringLiteral("layer_id"), QStringLiteral("product.%1").arg(versionId));
  extra.insert(QStringLiteral("title"), QStringLiteral("沉积相图 v1"));
  extra.insert(QStringLiteral("source_suffix"),
               QStringLiteral("|layername=%1").arg(layerName));
  v.extra = extra;
  QString error;
  return catalog->addAsset(a, &error) && catalog->addVersion(v, &error);
}

QVariantMap record(const QString &id, const QVariantMap &overrides = {}) {
  QVariantMap r;
  r.insert(QStringLiteral("id"), id);
  r.insert(QStringLiteral("text"), QStringLiteral("record %1").arg(id));
  r.insert(QStringLiteral("role"), QStringLiteral("REFERENCES"));
  r.insert(QStringLiteral("type"), QStringLiteral("manual"));
  r.insert(QStringLiteral("verification"), QStringLiteral("Unverified"));
  r.insert(QStringLiteral("revision"), 1);
  r.insert(QStringLiteral("origin"), QStringLiteral("manual_interpretation"));
  r.insert(QStringLiteral("actor_id"), QStringLiteral("local:alice"));
  for (auto it = overrides.cbegin(); it != overrides.cend(); ++it)
    r.insert(it.key(), it.value());
  return r;
}

QVariantMap interpretation(const QString &id, const QString &status,
                           const QString &uid) {
  QVariantMap i;
  i.insert(QStringLiteral("id"), id);
  i.insert(QStringLiteral("assertion_id"), QStringLiteral("assert-%1").arg(id));
  i.insert(QStringLiteral("feature_uid"), uid);
  i.insert(QStringLiteral("horizon"), QStringLiteral("D61"));
  i.insert(QStringLiteral("status"), status);
  i.insert(QStringLiteral("revision"), 1);
  return i;
}

// 装置：A（3 条记录，NeedsReview，含 unlinked 历史与被反驳关联）、
// B（2 条记录，Confirmed + identityReview，含无来源钉定的约束记录）、
// C（facies_evidence 损坏 JSON）、D（无记录，仅 Confirmed 解释）。
QVector<FixtureFeature> baseFeatures() {
  QVector<FixtureFeature> out;
  {
    QVariantList records;
    records << record(QStringLiteral("ev-1"), QVariantMap{
                        {"role", QStringLiteral("SUPPORTED_BY")},
                        {"type", QStringLiteral("well")},
                        {"text", QStringLiteral("bioturbation indicates shoreface")},
                        {"source_version", QStringLiteral("v-src-1")},
                        {"source_feature_uid", QStringLiteral("uid-src-1")}});
    records << record(QStringLiteral("ev-2"), QVariantMap{
                        {"role", QStringLiteral("CONTRADICTED_BY")},
                        {"type", QStringLiteral("seismic")},
                        {"text", QStringLiteral("amplitude anomaly contradicts")},
                        {"source_version", QStringLiteral("v-src-2")}});
    records << record(QStringLiteral("ev-3"), QVariantMap{
                        {"text", QStringLiteral("legacy tie to old version")},
                        {"unlinked", true}});
    FixtureFeature f;
    f.uid = QStringLiteral("uid-a");
    f.evidenceJson = encodeJson(records);
    f.interpretationJson = encodeJson(
        interpretation(QStringLiteral("interp-1"), QStringLiteral("NeedsReview"), f.uid));
    out << f;
  }
  {
    QVariantList records;
    records << record(QStringLiteral("ev-4"), QVariantMap{
                        {"text", QStringLiteral("manual pick note")},
                        {"actor_id", QStringLiteral("local:bob")}});
    records << record(QStringLiteral("ev-5"), QVariantMap{
                        {"role", QStringLiteral("CONSTRAINED_BY")},
                        {"type", QStringLiteral("constraint")},
                        {"text", QStringLiteral("constraint C-9 applied")}});
    FixtureFeature f;
    f.uid = QStringLiteral("uid-b");
    f.identityReview = true;
    f.evidenceJson = encodeJson(records);
    f.interpretationJson = encodeJson(
        interpretation(QStringLiteral("interp-2"), QStringLiteral("Confirmed"), f.uid));
    out << f;
  }
  {
    FixtureFeature f;
    f.uid = QStringLiteral("uid-c");
    f.evidenceJson = QStringLiteral("{oops not json");
    out << f;
  }
  {
    FixtureFeature f;
    f.uid = QStringLiteral("uid-d");
    f.evidenceJson = QStringLiteral("[]");
    f.interpretationJson = encodeJson(
        interpretation(QStringLiteral("interp-3"), QStringLiteral("Confirmed"), f.uid));
    out << f;
  }
  return out;
}

bool uidIn(const QVector<Entry> &entries, const QString &uid) {
  for (const auto &e : entries)
    if (e.featureUid == uid) return true;
  return false;
}
} // namespace

class TestEvidenceIndex : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() { GDALAllRegister(); }
  void syncIndexesEntriesIncludingUnlinked();
  void synchronizeIsIdempotentAndRescansOnShaChange();
  void subsetQueries();
  void searchMatchesTextActorAndEvidenceId();
  void removedVersionClearsEntries();
  void corruptAndMissingVersionsDoNotAbort();
  void limitAndOffsetAreBounded();
  void reopenKeepsIndex();
  void cancelAbortsSynchronize();
  void contradictedExcludesUnlinkedAndPendingCoversDisputed();
  void truncationIsRecordedAndRescanned();
  void schemaUpgradeFromV1Rebuilds();
  void failedWriteRollsBackAndStaysUsable();
};

void TestEvidenceIndex::syncIndexesEntriesIncludingUnlinked() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  const QString gpkg = dir.filePath(QStringLiteral("result.gpkg"));
  QVERIFY(writeGpkg(gpkg, baseFeatures()));
  QVERIFY(addMappingVersion(&cat, QStringLiteral("vx1"), gpkg));

  Index index;
  QString error;
  QVERIFY(index.open(dir.path(), &error));
  QCOMPARE(index.synchronize(&cat, dir.path(), {}, &error), 1);
  QVERIFY(QFile::exists(dir.filePath(QStringLiteral("artifacts/evidence-index.sqlite"))));

  // A 3 条 + B 2 条 + D 图斑级 1 条；C 损坏字段跳过并计数。
  const auto stats = index.stats();
  QCOMPARE(stats.value(QStringLiteral("versions_indexed")).toInt(), 1);
  QCOMPARE(stats.value(QStringLiteral("entries")).toInt(), 6);
  QCOMPARE(stats.value(QStringLiteral("corrupt_records")).toInt(), 1);
  QCOMPARE(stats.value(QStringLiteral("errored_versions")).toInt(), 0);
  QVERIFY(!stats.value(QStringLiteral("last_sync")).toString().isEmpty());

  const auto shoreface = index.search(QStringLiteral("shoreface"));
  QCOMPARE(shoreface.size(), 1);
  const Entry &first = shoreface.first();
  QCOMPARE(first.versionId, QStringLiteral("vx1"));
  QCOMPARE(first.assetId, QStringLiteral("a-vx1"));
  QCOMPARE(first.layerId, QStringLiteral("product.vx1"));
  QCOMPARE(first.title, QStringLiteral("沉积相图 v1"));
  QCOMPARE(first.horizon, QStringLiteral("D61"));
  QCOMPARE(first.featureUid, QStringLiteral("uid-a"));
  QCOMPARE(first.evidenceId, QStringLiteral("ev-1"));
  QCOMPARE(first.evidenceType, QStringLiteral("well"));
  QCOMPARE(first.role, QStringLiteral("SUPPORTED_BY"));
  QCOMPARE(first.sourceVersion, QStringLiteral("v-src-1"));
  QCOMPARE(first.sourceFeatureUid, QStringLiteral("uid-src-1"));
  QCOMPARE(first.interpretationId, QStringLiteral("interp-1"));
  QCOMPARE(first.assertionId, QStringLiteral("assert-interp-1"));
  QCOMPARE(first.reviewState, QStringLiteral("NeedsReview"));
  QCOMPARE(first.actorId, QStringLiteral("local:alice"));
  QVERIFY(!first.identityReview);
  QVERIFY(!first.unlinked);

  // unlinked 历史记录入库：可检索，但标记如实。
  const auto legacy = index.featuresForEvidence(QStringLiteral("ev-3"));
  QCOMPARE(legacy.size(), 1);
  QVERIFY(legacy.first().unlinked);
}

void TestEvidenceIndex::synchronizeIsIdempotentAndRescansOnShaChange() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  const QString gpkg = dir.filePath(QStringLiteral("result.gpkg"));
  QVERIFY(writeGpkg(gpkg, baseFeatures()));
  QVERIFY(addMappingVersion(&cat, QStringLiteral("vx1"), gpkg));

  Index index;
  QVERIFY(index.open(dir.path(), nullptr));
  QString error;
  QCOMPARE(index.synchronize(&cat, dir.path(), {}, &error), 1);
  // 幂等：sha 未变 → 0 变更，条目不重复。
  QCOMPARE(index.synchronize(&cat, dir.path(), {}, &error), 0);
  QCOMPARE(index.stats().value(QStringLiteral("entries")).toInt(), 6);

  // 文件字节变化（新增特征 E）→ 整版本替换。
  auto extended = baseFeatures();
  {
    FixtureFeature f;
    f.uid = QStringLiteral("uid-e");
    QVariantList records;
    records << record(QStringLiteral("ev-6"));
    f.evidenceJson = encodeJson(records);
    f.interpretationJson = encodeJson(
        interpretation(QStringLiteral("interp-4"), QStringLiteral("Proposed"), f.uid));
    extended << f;
  }
  QVERIFY(writeGpkg(gpkg, extended));
  QCOMPARE(index.synchronize(&cat, dir.path(), {}, &error), 1);
  QCOMPARE(index.stats().value(QStringLiteral("entries")).toInt(), 7);
  QCOMPARE(index.featuresForEvidence(QStringLiteral("ev-6")).size(), 1);
  QCOMPARE(index.featuresForEvidence(QStringLiteral("ev-1")).size(), 1); // 旧条目未翻倍
  // 再次幂等。
  QCOMPARE(index.synchronize(&cat, dir.path(), {}, &error), 0);
  QCOMPARE(index.stats().value(QStringLiteral("entries")).toInt(), 7);
}

void TestEvidenceIndex::subsetQueries() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  const QString gpkg = dir.filePath(QStringLiteral("result.gpkg"));
  QVERIFY(writeGpkg(gpkg, baseFeatures()));
  QVERIFY(addMappingVersion(&cat, QStringLiteral("vx1"), gpkg));

  Index index;
  QVERIFY(index.open(dir.path(), nullptr));
  QString error;
  QCOMPARE(index.synchronize(&cat, dir.path(), {}, &error), 1);

  // 证据 id → 图斑：含 unlinked 历史行（Entry.unlinked 可区分）。
  QCOMPARE(index.featuresForEvidence(QStringLiteral("ev-1")).size(), 1);
  QCOMPARE(index.featuresForEvidence(QStringLiteral("ev-3")).size(), 1);
  QCOMPARE(index.featuresForEvidence(QStringLiteral("nope")).size(), 0);

  // 解释 id → 所在图斑的全部条目（每条记录一行）+ 图斑级行。
  const auto interp1 = index.featuresForInterpretation(QStringLiteral("interp-1"));
  QCOMPARE(interp1.size(), 3);
  QVERIFY(uidIn(interp1, QStringLiteral("uid-a")));
  const auto interp3 = index.featuresForInterpretation(QStringLiteral("interp-3"));
  QCOMPARE(interp3.size(), 1); // D：无记录的图斑级行
  QCOMPARE(interp3.first().featureUid, QStringLiteral("uid-d"));
  QVERIFY(interp3.first().evidenceId.isEmpty());

  // 来源版本反查。
  QCOMPARE(index.featuresReferencingSource(QStringLiteral("v-src-1")).size(), 1);
  QCOMPARE(index.featuresReferencingSource(QStringLiteral("v-src-2")).size(), 1);
  QCOMPARE(index.featuresReferencingSource(QStringLiteral("v-ghost")).size(), 0);

  // 待复核：NeedsReview/Proposed（A 3 行）或 identityReview（B 2 行）；
  // Confirmed 且无标记的 D 不在其中。
  const auto pending = index.pendingReview(QStringLiteral("D61"));
  QCOMPARE(pending.size(), 5);
  QVERIFY(uidIn(pending, QStringLiteral("uid-a")));
  QVERIFY(uidIn(pending, QStringLiteral("uid-b")));
  QVERIFY(!uidIn(pending, QStringLiteral("uid-d")));
  QCOMPARE(index.pendingReview(QStringLiteral("D99")).size(), 0);

  // 被反驳关联：role 含 CONTRADICTED。
  const auto contradicted = index.contradicted(QStringLiteral("D61"));
  QCOMPARE(contradicted.size(), 1);
  QCOMPARE(contradicted.first().evidenceId, QStringLiteral("ev-2"));
  QCOMPARE(contradicted.first().role, QStringLiteral("CONTRADICTED_BY"));

  // 过期/缺失来源：要求钉定 source_version 的类型缺来源（ev-5 constraint）。
  const auto stale = index.staleSources(QStringLiteral("D61"));
  QCOMPARE(stale.size(), 1);
  QCOMPARE(stale.first().evidenceId, QStringLiteral("ev-5"));

  // 同类计数与查询同口径。
  QCOMPARE(index.countFor(QStringLiteral("pendingReview"), QStringLiteral("D61")), 5);
  QCOMPARE(index.countFor(QStringLiteral("contradicted"), QStringLiteral("D61")), 1);
  QCOMPARE(index.countFor(QStringLiteral("featuresForEvidence"), QStringLiteral("ev-1")), 1);
  QCOMPARE(index.countFor(QStringLiteral("bogus"), QString()), -1);
  QVERIFY(!index.lastError().isEmpty());
}

void TestEvidenceIndex::searchMatchesTextActorAndEvidenceId() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  const QString gpkg = dir.filePath(QStringLiteral("result.gpkg"));
  QVERIFY(writeGpkg(gpkg, baseFeatures()));
  QVERIFY(addMappingVersion(&cat, QStringLiteral("vx1"), gpkg));

  Index index;
  QVERIFY(index.open(dir.path(), nullptr));
  QString error;
  QCOMPARE(index.synchronize(&cat, dir.path(), {}, &error), 1);

  QCOMPARE(index.search(QStringLiteral("shoreface")).size(), 1); // text 命中
  QCOMPARE(index.search(QStringLiteral("SHOREFACE")).size(), 1); // ASCII 大小写不敏感
  QCOMPARE(index.search(QStringLiteral("local:bob")).size(), 1); // actorId 命中
  QCOMPARE(index.search(QStringLiteral("ev-1")).size(), 1);      // evidenceId 命中
  QCOMPARE(index.search(QStringLiteral("no-such-token")).size(), 0);
  QCOMPARE(index.search(QString()).size(), 0);                    // 空词不匹配全库
  QCOMPARE(index.search(QStringLiteral("%")).size(), 0);          // LIKE 通配符被转义
  QCOMPARE(index.countFor(QStringLiteral("search"), QStringLiteral("shoreface")), 1);
}

void TestEvidenceIndex::removedVersionClearsEntries() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  const QString gpkg1 = dir.filePath(QStringLiteral("one.gpkg"));
  const QString gpkg2 = dir.filePath(QStringLiteral("two.gpkg"));
  QVERIFY(writeGpkg(gpkg1, baseFeatures()));
  QVERIFY(writeGpkg(gpkg2, baseFeatures()));
  QVERIFY(addMappingVersion(&cat, QStringLiteral("vx1"), gpkg1));
  QVERIFY(addMappingVersion(&cat, QStringLiteral("vx2"), gpkg2));

  Index index;
  QVERIFY(index.open(dir.path(), nullptr));
  QString error;
  QCOMPARE(index.synchronize(&cat, dir.path(), {}, &error), 2);
  QCOMPARE(index.stats().value(QStringLiteral("entries")).toInt(), 12);

  // 目录删除资产（含版本）→ 索引整版本清除；索引不是事实源。
  QVERIFY(cat.removeAsset(QStringLiteral("a-vx1"), &error));
  QCOMPARE(index.synchronize(&cat, dir.path(), {}, &error), 1);
  QCOMPARE(index.stats().value(QStringLiteral("entries")).toInt(), 6);
  QCOMPARE(index.countFor(QStringLiteral("featuresForEvidence"), QStringLiteral("ev-1")), 1);
  const auto remaining = index.search(QStringLiteral("shoreface"));
  QCOMPARE(remaining.size(), 1);
  QCOMPARE(remaining.first().versionId, QStringLiteral("vx2"));
}

void TestEvidenceIndex::corruptAndMissingVersionsDoNotAbort() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  const QString gpkg = dir.filePath(QStringLiteral("result.gpkg"));
  QVERIFY(writeGpkg(gpkg, baseFeatures(),
                    QStringLiteral("features"))); // 图层名与后缀不同
  QVERIFY(addMappingVersion(&cat, QStringLiteral("vx1"), gpkg));
  // 文件缺失的版本：sha 算不出 → 登记 error、不中断、可重试。
  QVERIFY(addMappingVersion(&cat, QStringLiteral("vmissing"),
                            dir.filePath(QStringLiteral("ghost.gpkg"))));

  Index index;
  QVERIFY(index.open(dir.path(), nullptr));
  QString error;
  QCOMPARE(index.synchronize(&cat, dir.path(), {}, &error), 2);
  const auto stats = index.stats();
  QCOMPARE(stats.value(QStringLiteral("versions_indexed")).toInt(), 2);
  QCOMPARE(stats.value(QStringLiteral("entries")).toInt(), 6);
  QCOMPARE(stats.value(QStringLiteral("corrupt_records")).toInt(), 1);
  QCOMPARE(stats.value(QStringLiteral("errored_versions")).toInt(), 1);
  QVERIFY(index.lastError().isEmpty());
  // 后缀 layername 对不上 → 回退首层仍能索引。
  QCOMPARE(index.featuresForEvidence(QStringLiteral("ev-1")).size(), 1);
  // 文件随后出现 → 空不留底 sha 下一轮重试成功。
  QVERIFY(writeGpkg(dir.filePath(QStringLiteral("ghost.gpkg")), baseFeatures()));
  QCOMPARE(index.synchronize(&cat, dir.path(), {}, &error), 1);
  QCOMPARE(index.stats().value(QStringLiteral("entries")).toInt(), 12);
  QCOMPARE(index.stats().value(QStringLiteral("errored_versions")).toInt(), 0);
}

void TestEvidenceIndex::limitAndOffsetAreBounded() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  QVariantList records;
  for (int n = 0; n < 30; ++n) {
    const QString id = QStringLiteral("ev-b-%1").arg(n, 2, 10, QLatin1Char('0'));
    records << record(id, QVariantMap{{"text", QStringLiteral("bulk evidence %1").arg(n)}});
  }
  const QString gpkg = dir.filePath(QStringLiteral("result.gpkg"));
  {
    FixtureFeature f;
    f.uid = QStringLiteral("uid-many");
    f.evidenceJson = encodeJson(records);
    f.interpretationJson = encodeJson(
        interpretation(QStringLiteral("interp-many"), QStringLiteral("Confirmed"), f.uid));
    QVERIFY(writeGpkg(gpkg, {f}));
  }
  QVERIFY(addMappingVersion(&cat, QStringLiteral("vx1"), gpkg));

  Index index;
  QVERIFY(index.open(dir.path(), nullptr));
  QString error;
  QCOMPARE(index.synchronize(&cat, dir.path(), {}, &error), 1);
  QCOMPARE(index.countFor(QStringLiteral("search"), QStringLiteral("bulk")), 30);

  const auto page1 = index.search(QStringLiteral("bulk"), 10);
  QCOMPARE(page1.size(), 10);
  const auto page2 = index.search(QStringLiteral("bulk"), 10, 10);
  QCOMPARE(page2.size(), 10);
  const auto page4 = index.search(QStringLiteral("bulk"), 10, 30);
  QCOMPARE(page4.size(), 0); // offset 越界如实为空
  QSet<QString> ids;
  for (const auto &e : page1 + page2) ids.insert(e.evidenceId);
  QCOMPARE(ids.size(), 20); // 分页不重叠

  // limit 超上界被钳制（30 条全量 < 1000，验证钳制不吞行）。
  QCOMPARE(index.search(QStringLiteral("bulk"), 5000).size(), 30);
}

void TestEvidenceIndex::reopenKeepsIndex() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  const QString gpkg = dir.filePath(QStringLiteral("result.gpkg"));
  QVERIFY(writeGpkg(gpkg, baseFeatures()));
  QVERIFY(addMappingVersion(&cat, QStringLiteral("vx1"), gpkg));

  {
    Index index;
    QString error;
    QVERIFY(index.open(dir.path(), &error));
    QCOMPARE(index.synchronize(&cat, dir.path(), {}, &error), 1);
    QCOMPARE(index.stats().value(QStringLiteral("entries")).toInt(), 6);
  }
  // 工程重开：索引仍在（可重建 ≠ 必重建），且增量基线（sha）不丢。
  Index reopened;
  QString error;
  QVERIFY(reopened.open(dir.path(), &error));
  QCOMPARE(reopened.stats().value(QStringLiteral("entries")).toInt(), 6);
  QCOMPARE(reopened.featuresForEvidence(QStringLiteral("ev-2")).size(), 1);
  QCOMPARE(reopened.synchronize(&cat, dir.path(), {}, &error), 0);
  QVERIFY(!reopened.stats().value(QStringLiteral("last_sync")).toString().isEmpty());
}

void TestEvidenceIndex::cancelAbortsSynchronize() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  const QString gpkg = dir.filePath(QStringLiteral("result.gpkg"));
  QVERIFY(writeGpkg(gpkg, baseFeatures()));
  QVERIFY(addMappingVersion(&cat, QStringLiteral("vx1"), gpkg));

  Index index;
  QVERIFY(index.open(dir.path(), nullptr));
  QString error;
  const auto cancel = [] { return true; };
  QCOMPARE(index.synchronize(&cat, dir.path(), cancel, &error), -1);
  QVERIFY(index.lastError().contains(QStringLiteral("取消")));
  // 取消不产出半截索引。
  QCOMPARE(index.stats().value(QStringLiteral("entries")).toInt(), 0);
  QCOMPARE(index.synchronize(&cat, dir.path(), {}, &error), 1);
  QCOMPARE(index.stats().value(QStringLiteral("entries")).toInt(), 6);
}

// X8：unlinked 断链历史行不进「被反驳」结论口径；Disputed 计入待复核。
void TestEvidenceIndex::contradictedExcludesUnlinkedAndPendingCoversDisputed() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  const QString gpkg = dir.filePath(QStringLiteral("result.gpkg"));
  auto features = baseFeatures();
  {
    FixtureFeature f;
    f.uid = QStringLiteral("uid-hist");
    f.evidenceJson = encodeJson(QVariantList{
        record(QStringLiteral("ev-hist"),
               QVariantMap{{"role", QStringLiteral("CONTRADICTED_BY")},
                           {"type", QStringLiteral("well")},
                           {"unlinked", true},
                           {"source_version", QStringLiteral("v-src-ghost")},
                           {"text", QStringLiteral("unlinked contradiction")}})});
    f.interpretationJson = encodeJson(
        interpretation(QStringLiteral("interp-hist"),
                       QStringLiteral("Disputed"), f.uid));
    features << f;
  }
  QVERIFY(writeGpkg(gpkg, features));
  QVERIFY(addMappingVersion(&cat, QStringLiteral("vx1"), gpkg));

  Index index;
  QVERIFY(index.open(dir.path(), nullptr));
  QString error;
  QCOMPARE(index.synchronize(&cat, dir.path(), {}, &error), 1);

  // 结论口径：unlinked 反驳行被排除（只有 ev-2）。
  const auto contradicted = index.contradicted(QStringLiteral("D61"));
  QCOMPARE(contradicted.size(), 1);
  QCOMPARE(contradicted.first().evidenceId, QStringLiteral("ev-2"));
  QCOMPARE(index.countFor(QStringLiteral("contradicted"),
                          QStringLiteral("D61")),
           1);
  // 历史行仍可检索（按证据 id 反查保留 tombstone 事实）。
  const auto history = index.featuresForEvidence(QStringLiteral("ev-hist"));
  QCOMPARE(history.size(), 1);
  QVERIFY(history.first().unlinked);
  // 复核口径：Disputed 计入待复核（uid-hist 的记录行）。
  const auto pending = index.pendingReview(QStringLiteral("D61"));
  QVERIFY(uidIn(pending, QStringLiteral("uid-hist")));
  QCOMPARE(index.countFor(QStringLiteral("pendingReview"),
                          QStringLiteral("D61")),
           pending.size());
  // distinctSourceVersions：只列有效（unlinked=0）钉定来源——
  // v-src-ghost 是断链记录的引用，不进列表。
  const auto sources = index.distinctSourceVersions(QStringLiteral("D61"));
  QCOMPARE(sources.size(), 2);
  QVERIFY(sources.contains(QStringLiteral("v-src-1")));
  QVERIFY(sources.contains(QStringLiteral("v-src-2")));
  QVERIFY(!sources.contains(QStringLiteral("v-src-ghost")));
}

// X12：读取上限命中如实登记 truncated——不是静默的部分索引；且截断版本
// 不算已索引完成，默认上限的下轮同步会重灌补齐。
void TestEvidenceIndex::truncationIsRecordedAndRescanned() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  const QString gpkg = dir.filePath(QStringLiteral("result.gpkg"));
  QVERIFY(writeGpkg(gpkg, baseFeatures()));
  QVERIFY(addMappingVersion(&cat, QStringLiteral("vx1"), gpkg));

  Index index;
  QVERIFY(index.open(dir.path(), nullptr));
  QString error;
  paleo::evidenceindex::SyncOptions tiny;
  tiny.maxFeaturesPerVersion = 2;
  tiny.maxRowsPerVersion = 3;
  QCOMPARE(index.synchronize(&cat, dir.path(), {}, &error, tiny), 1);
  QCOMPARE(index.stats().value(QStringLiteral("truncated_versions")).toInt(), 1);
  QVERIFY(index.stats().value(QStringLiteral("entries")).toInt() < 6);
  // 截断不顶包「已完成」：默认上限再同步 → 整版本重灌、标记清零。
  QCOMPARE(index.synchronize(&cat, dir.path(), {}, &error), 1);
  QCOMPARE(index.stats().value(QStringLiteral("truncated_versions")).toInt(), 0);
  QCOMPARE(index.stats().value(QStringLiteral("entries")).toInt(), 6);
}

// v1→v2：旧库整库重建（drop + 重建 schema），不迁移行——索引是可重建
// 检索结构，下一轮同步全量重灌。
void TestEvidenceIndex::schemaUpgradeFromV1Rebuilds() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  const QString gpkg = dir.filePath(QStringLiteral("result.gpkg"));
  QVERIFY(writeGpkg(gpkg, baseFeatures()));
  QVERIFY(addMappingVersion(&cat, QStringLiteral("vx1"), gpkg));

  QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("artifacts"))));
  const QString dbPath =
      dir.filePath(QStringLiteral("artifacts/evidence-index.sqlite"));
  const QString legacyConn = QStringLiteral("v1-legacy-fixture");
  {
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", legacyConn);
    db.setDatabaseName(dbPath);
    QVERIFY(db.open());
    QSqlQuery q(db);
    QVERIFY(q.exec(QStringLiteral(
        "CREATE TABLE evidence_entries (version_id TEXT)")));
    QVERIFY(q.exec(QStringLiteral(
        "CREATE TABLE indexed_versions (version_id TEXT PRIMARY KEY,"
        " sha256 TEXT, indexed_at TEXT, corrupt_records INTEGER, error TEXT)")));
    QVERIFY(q.exec(QStringLiteral(
        "INSERT INTO indexed_versions VALUES ('stale-v1','x','',0,'')")));
    QVERIFY(q.exec(QStringLiteral("PRAGMA user_version = 1")));
    db.close();
  }
  QSqlDatabase::removeDatabase(legacyConn);

  Index index;
  QString error;
  QVERIFY2(index.open(dir.path(), &error), qPrintable(error));
  // 升级后 schema 是 v2：indexed_versions 有 truncated 列、user_version=2。
  const QString checkConn = QStringLiteral("v1-check-fixture");
  {
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", checkConn);
    db.setDatabaseName(dbPath);
    QVERIFY(db.open());
    QSqlQuery q(db);
    QVERIFY(q.exec(QStringLiteral("PRAGMA user_version")) && q.next());
    QCOMPARE(q.value(0).toInt(), 2);
    QVERIFY(q.exec(QStringLiteral(
        "SELECT truncated FROM indexed_versions"))); // v2 列存在
    QVERIFY(!q.next()); // 旧行未迁移——整库重建
    db.close();
  }
  QSqlDatabase::removeDatabase(checkConn);
  // 重建后的索引照常全量同步。
  QCOMPARE(index.synchronize(&cat, dir.path(), {}, &error), 1);
  QCOMPARE(index.stats().value(QStringLiteral("entries")).toInt(), 6);
}

// X12：写事务中途失败（外连接持写锁 → SQLITE_BUSY）必须 rollback——
// 连接不残留未提交事务，锁释放后同一 Index 照常完成同步。
void TestEvidenceIndex::failedWriteRollsBackAndStaysUsable() {
  QTemporaryDir dir;
  DataCatalog cat;
  QVERIFY(cat.open(dir.path(), nullptr));
  const QString gpkg = dir.filePath(QStringLiteral("result.gpkg"));
  QVERIFY(writeGpkg(gpkg, baseFeatures()));
  QVERIFY(addMappingVersion(&cat, QStringLiteral("vx1"), gpkg));

  Index index;
  QVERIFY(index.open(dir.path(), nullptr));
  const QString dbPath =
      dir.filePath(QStringLiteral("artifacts/evidence-index.sqlite"));
  QVERIFY(QFile::exists(dbPath));
  const QString holdConn = QStringLiteral("hold-lock-fixture");
  {
    QSqlDatabase hold = QSqlDatabase::addDatabase("QSQLITE", holdConn);
    hold.setDatabaseName(dbPath);
    QVERIFY(hold.open());
    {
      QSqlQuery q(hold);
      QVERIFY(q.exec(QStringLiteral("BEGIN IMMEDIATE")));
      QVERIFY(q.exec(QStringLiteral(
          "INSERT INTO index_meta (key, value) VALUES ('lock','1')")));
    }
    QString error;
    QCOMPARE(index.synchronize(&cat, dir.path(), {}, &error), -1);
    QVERIFY(!index.lastError().isEmpty());
    {
      QSqlQuery q(hold);
      QVERIFY(q.exec(QStringLiteral("ROLLBACK")));
    }
    hold.close();
  }
  QSqlDatabase::removeDatabase(holdConn);
  // 若失败路径留了未提交事务，这里会在「无法开始索引写入事务」处再败；
  // 成功即证明回滚干净、连接可用。
  QString error;
  QCOMPARE(index.synchronize(&cat, dir.path(), {}, &error), 1);
  QCOMPARE(index.stats().value(QStringLiteral("entries")).toInt(), 6);
}

QTEST_MAIN(TestEvidenceIndex)
#include "tst_evidenceindex.moc"
