#include <QtTest>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QFile>
#include <QCryptographicHash>
#include <gdal.h>
#include <ogr_api.h>
#include <cpl_error.h>
#include "../src/io/constraintstore.h"

namespace {
bool execute(const QString &path, const char *query) {
  auto ds = GDALOpenEx(path.toUtf8().constData(), GDAL_OF_UPDATE | GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
  if (!ds) return false;
  CPLErrorReset(); auto rows = GDALDatasetExecuteSQL(ds, query, nullptr, "SQLITE");
  const bool success = CPLGetLastErrorType() < CE_Failure;
  if (rows) GDALDatasetReleaseResultSet(ds, rows);
  GDALClose(ds); return success;
}
QByteArray hashFile(const QString &path) {
  QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {};
  return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}
}
class TestConstraintEvidence : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() { GDALAllRegister(); }
  void semanticChangesAndDeletionHaveDurableHistory() {
    QTemporaryDir dir; ConstraintStore store(dir.filePath("constraints.gpkg")); QString error;
    QVERIFY(!store.setAuditContext({"authenticated:someone", "reason", {}, {}}, &error));
    QVERIFY(!store.setAuditContext({"local:geologist", {}, {}, {}}, &error));
    QVERIFY(store.setAuditContext({"local:geologist", "登记物源方向线", "operation-create", {}}, &error));
    QVERIFY2(store.appendExtended("H1", "line", "LINESTRING(0 0,10 0)", "direction_line", 7,
                                  "{\"ratio\":8,\"semantic\":\"direction_guide\"}", 1, &error, 0.7), qPrintable(error));
    auto events = store.history("line"); QCOMPARE(events.size(), 1);
    QCOMPARE(events.first().value("actor_id").toString(), QString("local:geologist"));
    QCOMPARE(events.first().value("identity_kind").toString(), QString("local_operator"));
    QCOMPARE(events.first().value("operation_id").toString(), QString("operation-create"));
    QVERIFY(events.first().value("before").toMap().isEmpty());
    QCOMPARE(events.first().value("after").toMap().value("type").toString(), QString("direction_line"));
    QVERIFY(!events.first().value("after_params_fingerprint").toString().isEmpty());
    const auto plain = store.load("H1"); QVERIFY(!plain.first().contains("recent_events"));
    auto audited = store.load("H1", true); QCOMPARE(audited.first().value("revision").toLongLong(), 1);
    QCOMPARE(audited.first().value("recent_events").toList().size(), 1);
    QVERIFY(store.updateParameters("line", plain.first().value("params_json").toString(), 1, "direction_line", &error));
    QCOMPARE(store.history("line").size(), 1); // no-op is not an interpretation edit
    QVERIFY(store.updateParameters("line", "{ \"semantic\": \"direction_guide\", \"ratio\": 8 }", 1, "direction_line", &error));
    QCOMPARE(store.history("line").size(), 1); QCOMPARE(store.load("H1"), plain);
    QVERIFY(store.replaceHorizon("H1", store.load("H1"), &error)); QCOMPARE(store.history("line").size(), 1);
    QVERIFY(store.setAuditContext({"local:geologist", "改为硬屏障", "operation-semantic", "edit"}, &error));
    QVERIFY(store.updateParameters("line", "{\"blockMode\":\"full_block\",\"semantic\":\"hard_barrier\"}", 1, "break_line", &error));
    events = store.history("line"); QCOMPARE(events.size(), 2);
    QCOMPARE(events.first().value("before_revision").toLongLong(), 1);
    QCOMPARE(events.first().value("after_revision").toLongLong(), 2);
    QCOMPARE(events.first().value("before").toMap().value("type").toString(), QString("direction_line"));
    QCOMPARE(events.first().value("after").toMap().value("type").toString(), QString("break_line"));
    QVERIFY(events.first().value("before_params_fingerprint") != events.first().value("after_params_fingerprint"));
    QVERIFY(store.remove("line", &error)); QVERIFY(store.load("H1").isEmpty());
    events = store.history("line"); QCOMPARE(events.size(), 3); QVERIFY(events.first().value("deleted").toBool());
    QVERIFY(!events.first().value("before").toMap().value("wkt").toString().isEmpty());
    QVERIFY(events.first().value("after").toMap().isEmpty());
    QVERIFY(store.remove("missing", &error)); QCOMPARE(store.history("line").size(), 3);
    ConstraintStore reopened(store.gpkgPath()); QCOMPARE(reopened.history("line"), events);
    QCOMPARE(reopened.history("line", 1, 1).first(), events[1]);
    QCOMPARE(reopened.historyForHorizon("H1", 200).size(), 3);
    QVERIFY(reopened.historyForHorizon("H2").isEmpty());
    // Restore a deleted stable ID as a compensating event, never rewrite its history.
    QVERIFY(reopened.setAuditContext({"local:geologist", "恢复误删约束", {}, "undo"}, &error));
    QVERIFY(reopened.replaceHorizon("H1", plain, &error));
    QCOMPARE(reopened.load("H1"), plain);
    QCOMPARE(reopened.load("H1", true).first().value("revision").toLongLong(), 4);
    QCOMPARE(reopened.load("H1", true).first().value("recent_events").toList().size(), 3);
    QCOMPARE(reopened.history("line").first().value("action").toString(), QString("undo"));
  }
  void auditFailureRollsBackEveryBusinessMutation() {
    QTemporaryDir dir; ConstraintStore store(dir.filePath("constraints.gpkg")); QString error;
    QVERIFY(store.append("H1", "a", "LINESTRING(0 0,10 0)", "direction_line", 1, &error));
    const auto before = store.load(); const auto events = store.history("a");
    QVERIFY(execute(store.gpkgPath(), "CREATE TRIGGER reject_constraint_audit BEFORE INSERT ON constraint_audit BEGIN SELECT RAISE(ABORT,'injected audit failure'); END"));
    QVERIFY(!store.append("H1", "b", "POINT(1 1)", "point", 2, &error)); QCOMPARE(store.load(), before);
    QVERIFY(!store.updateParameters("a", "{\"ratio\":3}", 1, "break_line", &error)); QCOMPARE(store.load(), before);
    QVERIFY(!store.remove("a", &error)); QCOMPARE(store.load(), before);
    auto rows = before; rows[0]["wkt"] = "LINESTRING(0 0,10 4)";
    QVERIFY(!store.replaceHorizon("H1", rows, &error)); QCOMPARE(store.load(), before);
    QCOMPARE(store.history("a"), events); QVERIFY(store.history("b").isEmpty());
    QVERIFY(execute(store.gpkgPath(), "DROP TRIGGER reject_constraint_audit"));
    QVERIFY(store.replaceHorizon("H1", rows, &error)); QCOMPARE(store.history("a").size(), 2);
  }
  void legacyReadAndNoOpDoNotUpgradeButRealWriteDoes() {
    QTemporaryDir dir; const auto path = dir.filePath("legacy.gpkg");
    auto ds = GDALCreate(GDALGetDriverByName("GPKG"), path.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr); QVERIFY(ds);
    auto layer = GDALDatasetCreateLayer(ds, "constraints", nullptr, wkbUnknown, nullptr); QVERIFY(layer);
    for (const auto name : {"id", "horizon", "type"}) {
      auto field = OGR_Fld_Create(name, OFTString); QCOMPARE(OGR_L_CreateField(layer, field, TRUE), OGRERR_NONE); OGR_Fld_Destroy(field);
    }
    auto feature = OGR_F_Create(OGR_L_GetLayerDefn(layer));
    OGR_F_SetFieldString(feature, 0, "legacy-line"); OGR_F_SetFieldString(feature, 1, "H1"); OGR_F_SetFieldString(feature, 2, "direction_line");
    QByteArray wkt("LINESTRING(0 0,10 0)"); char *text = wkt.data(); OGRGeometryH geometry = nullptr;
    QCOMPARE(OGR_G_CreateFromWkt(&text, nullptr, &geometry), OGRERR_NONE); OGR_F_SetGeometryDirectly(feature, geometry);
    QCOMPARE(OGR_L_CreateFeature(layer, feature), OGRERR_NONE); OGR_F_Destroy(feature); GDALClose(ds);
    const auto hash = hashFile(path); ConstraintStore store(path); QString error;
    QCOMPARE(store.load("H1").size(), 1); QVERIFY(store.history("legacy-line").isEmpty());
    QCOMPARE(store.load("H1", true).first().value("revision").toInt(), 0);
    QCOMPARE(hashFile(path), hash);
    QVERIFY(store.replaceHorizon("H1", store.load("H1"), &error));
    ds = GDALOpenEx(path.toUtf8().constData(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr); QVERIFY(ds);
    QVERIFY(!GDALDatasetGetLayerByName(ds, "constraint_audit")); GDALClose(ds);
    QVERIFY(store.updateParameters("legacy-line", "{\"ratio\":5}", 1, "direction_line", &error));
    QCOMPARE(store.history("legacy-line").size(), 1);
    QCOMPARE(store.history("legacy-line").first().value("before_revision").toInt(), 0);
    store.setReadOnly(true); const auto rows = store.load(); const auto events = store.history("legacy-line");
    QVERIFY(!store.remove("legacy-line", &error)); QVERIFY(!store.replaceHorizon("H1", {}, &error));
    QCOMPARE(store.load(), rows); QCOMPARE(store.history("legacy-line"), events);
  }
  void queueRejectionAndDuplicateSnapshotAreAtomic() {
    QTemporaryDir dir; const auto path = dir.filePath("constraints.gpkg"); QString error; ConstraintStore store(path);
    QVERIFY(store.append("H1", "a", "POINT(0 0)", "point", 1, &error));
    const auto rows = store.load(); const auto events = store.history("a");
    ConstraintStore refused(path, [](const ConstraintStore::WriteFn &) { return PaleoProjectStore::WriteResult{false, "queue rejected"}; });
    QVERIFY(!refused.remove("a", &error)); QCOMPARE(store.load(), rows); QCOMPARE(store.history("a"), events);
    auto duplicate = rows; duplicate.append(rows.first());
    QVERIFY(!store.replaceHorizon("H1", duplicate, &error)); QCOMPARE(store.load(), rows); QCOMPARE(store.history("a"), events);
    QVERIFY(!store.append("H2", "a", "POINT(1 1)", "point", 1, &error)); QCOMPARE(store.load(), rows);
    auto malformed = rows; malformed[0]["wkt"] = "invalid";
    QVERIFY(!store.replaceHorizon("H1", malformed, &error)); QCOMPARE(store.load(), rows); QCOMPARE(store.history("a"), events);
  }
  void snapshotFileKeepsDeletionHistory() {
    QTemporaryDir dir; ConstraintStore store(dir.filePath("constraints.gpkg")); QString error;
    QVERIFY(store.setAuditContext({"local:geologist", "登记后删除", "operation-delete", "edit"}, &error));
    QVERIFY2(store.append("H1", "gone", "LINESTRING(0 0,4 0)", "direction_line", 3, &error), qPrintable(error));
    QVERIFY2(store.remove("gone", &error), qPrintable(error));
    const auto events = store.history("gone"); QCOMPARE(events.size(), 2);
    QVERIFY(events.first().value("deleted").toBool());
    const auto snapshot = dir.filePath("snapshot.gpkg");
    int copied = 0;
    QVERIFY2(store.copyAudit(snapshot, "H1", &error, &copied), qPrintable(error));
    QCOMPARE(copied, 2);
    ConstraintStore carried(snapshot);
    QCOMPARE(carried.history("gone"), events);
    QCOMPARE(carried.historyForHorizon("H1").size(), 2);
    QVERIFY(!store.copyAudit(store.gpkgPath(), "H1", &error, &copied));
    QCOMPARE(store.history("gone"), events);
  }

  // F-07（full-audit-20261028）：审计快照翻页判终不受损坏行干扰——
  // readHistory 跳过损坏 JSON 行使页变短，「页短即终页」在损坏行落页内时
  // 静默截断其后全部事件（快照用途是带走删除历史，静默缺失与用途相悖）。
  void copyAuditSurvivesCorruptedRowsMidPagination() {
    QTemporaryDir dir; ConstraintStore store(dir.filePath("constraints.gpkg")); QString error;
    const int total = 205; // 超过 200/页 → 触发游标翻页
    for (int i = 0; i < total; ++i)
      QVERIFY2(store.append("H1", QStringLiteral("c%1").arg(i), "POINT(0 0)", "point", 1, &error),
               qPrintable(error));
    const QString path = store.gpkgPath();
    // 损坏 DESC 首页内一行（最高 fid）：该页解析 199 行——旧判终在页短即
    // break，其后 5 行全丢；新判终继续游标翻页。
    QVERIFY(execute(path, "UPDATE constraint_audit SET event_json='{bad json' "
                          "WHERE fid=(SELECT MAX(fid) FROM constraint_audit)"));
    const auto snapshot = dir.filePath("snapshot-corrupt.gpkg");
    int copied = -1;
    QVERIFY2(store.copyAudit(snapshot, "H1", &error, &copied), qPrintable(error));
    QCOMPARE(copied, total - 1); // 损坏 1 行如实丢弃，其余全数带走
    // historyForHorizon 单页上限 200 读不回全量——直查快照行数验证完整性
    //（旧判终只带 199 行：损坏行使首页解析 199 < 200 即 break）。
    int rows = -1;
    {
      auto ds = GDALOpenEx(snapshot.toUtf8().constData(), GDAL_OF_VECTOR, nullptr, nullptr, nullptr);
      QVERIFY(ds);
      auto result = GDALDatasetExecuteSQL(ds, "SELECT COUNT(*) AS n FROM constraint_audit", nullptr, "SQLITE");
      QVERIFY(result);
      if (auto f = OGR_L_GetNextFeature(result)) {
        const int idx = OGR_FD_GetFieldIndex(OGR_L_GetLayerDefn(result), "n");
        rows = int(OGR_F_GetFieldAsInteger64(f, idx));
        OGR_F_Destroy(f);
      }
      GDALDatasetReleaseResultSet(ds, result);
      GDALClose(ds);
    }
    QCOMPARE(rows, total - 1);
  }
};
QTEST_GUILESS_MAIN(TestConstraintEvidence)
#include "tst_constraintevidence.moc"
