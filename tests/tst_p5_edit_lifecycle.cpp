// 层：测试壳
// P5 专属回归：约束编辑生命周期、单一撤销归属、批量导入有界性、复制身份
// 映射与编辑器一次绑定（#1009/#1013/#1015/#1017/#990）。
// 既有覆盖在 tst_constraintediting / tst_constraintstore / tst_featureidentity /
// tst_mappingworkbench——本文件只补 P5 新语义，不重复旧断言。
#include <QtTest>
#include <gdal.h>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QJsonDocument>
#include <QUndoStack>
#include <qgsapplication.h>
#include <qgsvectorlayer.h>
#include <qgsfeature.h>
#include <qgsgeometry.h>
#include <qgsproject.h>

#include "../src/catalog/datacatalog.h"
#include "../src/io/constraintstore.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/faciespartitioneditor.h"
#include "../src/qgis/faciespartitionstate.h"
#include "../src/qgis/mappingfeatureidentity.h"
#include "../src/qgis/qgiseditingservice.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/mappingartifactwriter.h"
#include "../src/workflow/mappingworkbench.h"
#include "../src/workflow/workflows.h"
#include "../src/workflow/closureeditstack.h"

namespace {
QVariantList hierarchySchema() {
  QVariantList schema;
  const QStringList parents{"河流", "河流", "三角洲", "三角洲", "湖泊"};
  const QStringList children{"河道", "河道", "河道", "河道", "浅湖"};
  const QStringList leaves{"主槽", "边滩", "主槽", "边滩", "湖泥"};
  for (int i = 0; i < 5; ++i)
    schema << QVariantMap{{"code", i + 1},
                          {"name", parents[i] + leaves[i]},
                          {"facies", parents[i]},
                          {"subfacies", children[i]},
                          {"microfacies", leaves[i]},
                          {"color", i < 2 ? "#97B4CE" : "#F2D28B"}};
  return schema;
}
} // namespace

class TestP5EditLifecycle : public QObject {
  Q_OBJECT
  struct Fixture {
    QTemporaryDir dir;
    DataCatalog catalog;
    QgisProjectService project;
    PaleoProjectStore store;
    LayerManifest manifest{dir.filePath("project.sqlite")};
    QgisLayerService layers{&project, &manifest};
    QgisProcessingService processing{&store};
    ConstraintWorkflow constraints{&processing, &layers};
    MappingWorkbench work{&layers, &processing, &project, &constraints};
    paleo::workflow::ClosureEditStack closure;
    QgisEditingService edits{&store};
    bool init() {
      if (!catalog.open(dir.path()) ||
          !project.createProject(dir.filePath("project.qgz")) ||
          !manifest.open())
        return false;
      store.setProjectPaths(project.projectPath(), dir.filePath("project.gpkg"),
                            dir.filePath("project.sqlite"));
      constraints.setStore(&store);
      constraints.setCatalog(&catalog, dir.path());
      constraints.setPropertyEditStack(&closure);
      constraints.setEditingService(&edits);
      project.setDeclarationProvider(
          [this](QVector<LayerDeclaration> *out, QString *error) {
            return layers.tryDeclared(out, error);
          });
      processing.setProject(project.project());
      work.bindCatalog(&catalog, dir.path());
      return true;
    }
    // 生产同款 session starter：实例化 constraints.<horizon> 并经编辑服务开会话
    //（对齐 paleomainwindow_attach_constraint 的 beginConstraintEdit 语义）。
    void installProductionStarter() {
      constraints.setDrawSessionStarter(
          [this](const QString &horizon, QString *error) {
            auto *layer = qobject_cast<QgsVectorLayer *>(layers.instantiate(
                QStringLiteral("constraints.%1").arg(horizon), error));
            return layer && edits.beginEdit(layer, error);
          });
    }
    int rowCount(const QString &horizon = {}) {
      return constraints.loadConstraints(horizon).size();
    }
  };
  static QVector<paleo::ImportedConstraintRecord> lineRecords(int count,
                                                              int ratio = 8) {
    QVector<paleo::ImportedConstraintRecord> records;
    for (int i = 0; i < count; ++i)
      records.append({QStringLiteral("LINESTRING(%1 0, %1 10)").arg(i),
                      QStringLiteral("direction_line"),
                      QVariantMap{{"semantic", "direction_guide"},
                                  {"ratio", ratio}}});
    return records;
  }

private slots:
  // W2（#1009）：已安装真实 starter 时，会话外绘制的 undo/redo 重放不得开启
  // 另一种编辑会话或产生第二份（原生）撤销历史。
  void outOfSessionDrawUndoKeepsSingleOwner() {
    Fixture f;
    QVERIFY(f.init());
    f.installProductionStarter();
    QString error;
    QString id;
    QVERIFY2(f.constraints.drawConstraint("T70", "LINESTRING(0 0, 10 5)",
                                          "break_line", 0, &error, &id),
             qPrintable(error));
    QVERIFY(!id.isEmpty());
    QCOMPARE(f.rowCount("T70"), 1);
    QCOMPARE(f.closure.count(), 1);
    // 会话外：重放前后图层都不可编辑（重放绝不 ensure 会话）。
    auto *layer = qobject_cast<QgsVectorLayer *>(
        f.layers.layer(QStringLiteral("constraints.T70")));
    QVERIFY(layer);
    const int nativeIndex = layer->undoStack()->index();
    QVERIFY(!layer->isEditable());
    QVERIFY(f.closure.undo());
    QVERIFY(!layer->isEditable());
    QCOMPARE(f.rowCount("T70"), 0);
    // 原生栈未被闭包重放推进（没有第二份历史）。
    QCOMPARE(layer->undoStack()->index(), nativeIndex);
    QCOMPARE(layer->undoStack()->count(), 0);
    QVERIFY(f.closure.redo());
    QCOMPARE(f.rowCount("T70"), 1);
    // redo 合法重分配 id；条目内共享 id 已更新（再 undo 删的是新 id）。
    const auto rows = f.constraints.loadConstraints("T70");
    QCOMPARE(rows.size(), 1);
    const QString redoId = rows.first().value("id").toString();
    QVERIFY(redoId != id);
    QVERIFY(f.closure.undo());
    QCOMPARE(f.rowCount("T70"), 0);
    // 审计如实记录完整序列（不删除历史事件伪装未发生）：create c-1 →
    // delete（undo）→ create c-2（redo）→ delete（undo）。readHistory 按
    // fid 降序（最新在前）。
    ConstraintStore verify(f.store.gpkgPath());
    const auto history = verify.historyForHorizon("T70");
    QCOMPARE(history.size(), 4);
    QCOMPARE(history.first().value("change").toString(),
             QStringLiteral("delete"));
    int creates = 0, deletes = 0;
    for (const auto &event : history) {
      if (event.value("change").toString() == QLatin1String("create"))
        ++creates;
      else if (event.value("change").toString() == QLatin1String("delete"))
        ++deletes;
    }
    QCOMPARE(creates, 2);
    QCOMPARE(deletes, 2);
  }

  // W2：闭包重放遇同层位活会话 → 如实拒绝，条目原地保留；会话结束后可重放。
  void closureReplayRefusedDuringLiveSession() {
    Fixture f;
    QVERIFY(f.init());
    f.installProductionStarter();
    QString error;
    QVERIFY2(f.constraints.drawConstraint("T70", "LINESTRING(0 0, 10 5)",
                                          "break_line", 0, &error),
             qPrintable(error));
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(
        QStringLiteral("constraints.T70"), &error));
    QVERIFY(layer);
    QVERIFY(f.edits.beginEdit(layer, &error));
    QVERIFY(layer->isEditable());
    QCOMPARE(f.closure.count(), 1);
    QVERIFY(f.rowCount("T70") == 1);
    QVERIFY(!f.closure.undo());
    QCOMPARE(f.closure.count(), 1);
    QCOMPARE(f.rowCount("T70"), 1);
    f.edits.rollbackEdit(layer); // 取消会话（恢复起点）
    QVERIFY(!layer->isEditable());
    QVERIFY(f.closure.undo());
    QCOMPARE(f.rowCount("T70"), 0);
  }

  // W1：闭包携带归属存储身份——工程切换后重放拒绝，条目保留。
  void closureGuardRejectsStoreSwitch() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    QVERIFY2(f.constraints.drawConstraint("T70", "LINESTRING(0 0, 1 1)",
                                          "break_line", 0, &error),
             qPrintable(error));
    // 模拟切工程：同一 workflow 换到另一个 GPKG 存储。
    PaleoProjectStore other;
    other.setProjectPaths(f.dir.filePath("other.qgz"),
                          f.dir.filePath("other.gpkg"),
                          f.dir.filePath("other.sqlite"));
    f.constraints.setStore(&other);
    QVERIFY(!f.closure.undo());
    QCOMPARE(f.closure.count(), 1);
    // 切回原工程后重放可用。
    f.constraints.setStore(&f.store);
    QVERIFY(f.closure.undo());
    QCOMPARE(f.rowCount("T70"), 0);
  }

  // W3（#1017）：可提前判定的拒绝（层位进不了 subset URI）发生在持久写入前，
  // 无 durable 残留、无成功信号；重试不产生重复。
  void addConstraintPrevalidatesUnrepresentableHorizon() {
    Fixture f;
    QVERIFY(f.init());
    QSignalSpy added(&f.constraints, &ConstraintWorkflow::constraintAdded);
    QString error;
    QVERIFY(!f.constraints.addConstraint("A|B", "LINESTRING(0 0, 1 1)",
                                         "break_line", 0, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(added.count(), 0);
    QCOMPARE(f.rowCount(), 0);
    QCOMPARE(f.rowCount("A|B"), 0);
    // 干净重试（合法层位）恰好成功一次。
    error.clear();
    QVERIFY2(f.constraints.addConstraint("T70", "LINESTRING(0 0, 1 1)",
                                         "break_line", 0, &error),
             qPrintable(error));
    QCOMPARE(f.rowCount("T70"), 1);
    QCOMPARE(added.count(), 1);
  }

  // W4（#1013）：单条删除的有界读取（旧实现 4 次全量 load）。
  void removeConstraintBoundedLoads() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    QStringList created;
    QVERIFY2(f.constraints.importConstraintBatch("T70", lineRecords(40), &error,
                                                 &created),
             qPrintable(error));
    QCOMPARE(created.size(), 40);
    const QString victim = created.at(17);
    const quint64 before = ConstraintStore::debugLoadCount();
    QVERIFY2(f.constraints.removeConstraint(victim, &error), qPrintable(error));
    const quint64 loads = ConstraintStore::debugLoadCount() - before;
    QVERIFY2(loads <= 3, qPrintable(QStringLiteral("remove loads=%1").arg(loads)));
    QCOMPARE(f.rowCount("T70"), 39);
  }

  // W4（#1013）：批量导入一次事务 + 有界全量读取；规模翻 10 倍读取不随之线性
  // 增长；预验证失败（自相交几何）整批拒绝不写行；他人写入（revision 变化）
  // 后编号仍全局唯一。
  void importBatchIsBoundedAndAtomic() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    QStringList created;
    quint64 before = ConstraintStore::debugLoadCount();
    QVERIFY2(f.constraints.importConstraintBatch("T70", lineRecords(100), &error,
                                                 &created),
             qPrintable(error));
    quint64 loads100 = ConstraintStore::debugLoadCount() - before;
    QCOMPARE(created.size(), 100);
    QCOMPARE(f.rowCount("T70"), 100);
    QVERIFY2(loads100 <= 6,
             qPrintable(QStringLiteral("import(100) loads=%1").arg(loads100)));
    // 1000 条的读取开销与 100 条同数量级（不随 N 线性增长）。
    before = ConstraintStore::debugLoadCount();
    QStringList more;
    QVERIFY2(f.constraints.importConstraintBatch("T71", lineRecords(1000),
                                                 &error, &more),
             qPrintable(error));
    const quint64 loads1000 = ConstraintStore::debugLoadCount() - before;
    QCOMPARE(f.rowCount("T71"), 1000);
    QVERIFY2(loads1000 <= 6,
             qPrintable(QStringLiteral("import(1000) loads=%1").arg(loads1000)));
    // 重开存储核对持久事实（含参数与审计）。historyForHorizon 每页上限 200
    //（qBound 语义），1000 条 create 以满页 + 首事件特征核对。
    ConstraintStore verify(f.store.gpkgPath());
    const auto rows = verify.load("T71");
    QCOMPARE(rows.size(), 1000);
    QVERIFY(rows.first().value("params_json").toString().contains(
        "direction_guide"));
    const auto audit = verify.historyForHorizon("T71");
    QCOMPARE(audit.size(), 200); // 满页：批量审计逐条在档
    QCOMPARE(audit.first().value("change").toString(),
             QStringLiteral("create"));
    // 整批原子：一条自相交多边形毁掉整批，不留半成品。
    auto bad = lineRecords(5);
    bad.append({"POLYGON((0 0, 10 0, 0 10, 10 10, 0 0))",
                QStringLiteral("break_line"), {}});
    bad.append({"LINESTRING(50 0, 60 0)", QStringLiteral("break_line"), {}});
    QVERIFY(!f.constraints.importConstraintBatch("T72", bad, &error));
    QCOMPARE(f.rowCount("T72"), 0);
    // 他人写入（另一实例越过 workflow 落一条 c-N）后，编号索引经 revision
    // 失效重扫，不撞身份。
    ConstraintStore outsider(f.store.gpkgPath());
    QString otherError;
    QVERIFY(outsider.append("T70", "c-9999", "LINESTRING(0 0, 1 0)",
                            "break_line", 0, &otherError));
    QStringList after;
    QVERIFY2(f.constraints.importConstraintBatch("T70", lineRecords(3), &error,
                                                 &after),
             qPrintable(error));
    QCOMPARE(f.rowCount("T70"), 100 + 1 + 3);
    QVERIFY(!after.contains("c-9999"));
    // 会话进行中拒绝导入（会话快照会把批外写入整层覆盖）。
    f.edits.beginEdit(qobject_cast<QgsVectorLayer *>(f.layers.instantiate(
                          QStringLiteral("constraints.T70"), &error)),
                      &error);
    QVERIFY(!f.constraints.importConstraintBatch("T73", lineRecords(2), &error));
    QCOMPARE(f.rowCount("T73"), 0);
  }

  // W5（#990）：重复几何 + 重复 facies_code 的来源——复制后按身份映射改到
  // 被选中的那个业务对象，来源不被写入。
  void copyIdentityDisambiguatesDuplicateGeometries() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    QVERIFY(f.work.saveFacies("D61", hierarchySchema(), &error));
    // 两个 WKB 与 facies_code 完全一致的点要素（旧首匹配路径必改错）。
    QgsVectorLayer source(
        QStringLiteral("Point?field=facies_code:integer"), QStringLiteral("dup"),
        QStringLiteral("memory"));
    source.setCrs(
        QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt()));
    QgsFeatureList features;
    for (int i = 0; i < 2; ++i) {
      QgsFeature feature(source.fields());
      feature.setAttribute(QStringLiteral("facies_code"), 1);
      feature.setGeometry(QgsGeometry::fromWkt(QStringLiteral("POINT(30 30)")));
      features << feature;
    }
    QVERIFY(source.dataProvider()->addFeatures(features));
    const QString path = f.dir.filePath("dup-source.gpkg");
    QVERIFY(MappingArtifactWriter::vectorSnapshot(&source, path, &error));
    LayerDeclaration d;
    d.layerId = QStringLiteral("product.dup");
    d.horizon = QStringLiteral("D61");
    d.type = QStringLiteral("vector");
    d.group = QStringLiteral("05_Composition");
    d.title = QStringLiteral("duplicate points");
    d.source = path + QStringLiteral("|layername=features");
    QVERIFY(f.layers.declare(d, &error));
    const QByteArray sourceHash = QFile::exists(path)
                                      ? [&] {
                                          QFile file(path);
                                          file.open(QIODevice::ReadOnly);
                                          return file.readAll();
                                        }()
                                      : QByteArray();
    // 选中第二个要素（模拟右键目标）。
    auto *layer = qobject_cast<QgsVectorLayer *>(f.layers.instantiate(d.layerId, &error));
    QVERIFY(layer);
    QVector<QgsFeature> ordered;
    QgsFeature sf;
    auto it = layer->getFeatures();
    while (it.nextFeature(sf))
      ordered.append(sf);
    QCOMPARE(ordered.size(), 2);
    const QgsFeature selected = ordered.at(1);
    QVERIFY(MappingFeatureIdentity::uid(selected).isEmpty()); // 无 UID 历史来源
    // 复制并取身份映射。
    MappingWorkbench::CopyFeatureIdentity identity;
    const QString draft =
        f.work.copyForEditing(d.layerId, {}, &error, false, &identity);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));
    QVERIFY(identity.ambiguousSourceUids.isEmpty());
    const qint64 targetFid =
        identity.copyFidBySourceFid.value(selected.id(), FID_NULL);
    QVERIFY(targetFid != FID_NULL);
    QVERIFY(identity.copyFidBySourceFid.value(ordered.at(0).id()) !=
            targetFid); // 两个源要素映射到不同副本要素
    // 经映射修改：被改的恰是被选中的那个副本要素。
    QVERIFY2(f.work.assignFacies(draft, {targetFid}, 3, &error),
             qPrintable(error));
    auto *copy = qobject_cast<QgsVectorLayer *>(f.layers.layer(draft));
    QVERIFY(copy);
    QCOMPARE(copy->getFeature(identity.copyFidBySourceFid.value(ordered.at(0).id()))
                 .attribute(QStringLiteral("facies_code")).toInt(), 1);
    QCOMPARE(copy->getFeature(targetFid)
                 .attribute(QStringLiteral("facies_code")).toInt(), 3);
    // 来源未被写入（RAW/历史来源只读语义）。
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), sourceHash);
  }

  // W6（#1015）：连续编辑命令不重复 install 编辑器、不重复读取 footprint
  // sidecar；版本变化（新登记）后按签名重建一次。
  void styleLayerBindsOnceAcrossEditCommands() {
    Fixture f;
    QVERIFY(f.init());
    QString error;
    QVERIFY(f.work.saveFacies("D61", hierarchySchema(), &error));
    QgsVectorLayer grid(QStringLiteral("Polygon?field=facies_code:integer"),
                        QStringLiteral("grid"), QStringLiteral("memory"));
    grid.setCrs(
        QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt()));
    QgsFeatureList features;
    for (int i = 0; i < 5; ++i) {
      QgsFeature feature(grid.fields());
      feature.setAttribute(QStringLiteral("facies_code"), i + 1);
      feature.setGeometry(QgsGeometry::fromRect(
          QgsRectangle(i * 100, 0, (i + 1) * 100, 100)));
      features << feature;
    }
    QVERIFY(grid.dataProvider()->addFeatures(features));
    const QString path = f.dir.filePath("grid.gpkg");
    QVERIFY(MappingArtifactWriter::vectorSnapshot(&grid, path, &error));
    LayerDeclaration d;
    d.layerId = QStringLiteral("input.grid");
    d.horizon = QStringLiteral("D61");
    d.type = QStringLiteral("vector");
    d.group = QStringLiteral("05_Composition");
    d.title = QStringLiteral("grid");
    d.source = path + QStringLiteral("|layername=features");
    QVERIFY(f.layers.declare(d, &error));
    const QString draft = f.work.copyForEditing(d.layerId, {}, &error);
    QVERIFY2(!draft.isEmpty(), qPrintable(error));
    f.work.styleLayer(draft); // 首次绑定
    auto *copy = qobject_cast<QgsVectorLayer *>(f.layers.layer(draft));
    QVERIFY(copy);
    QVERIFY(FaciesPartitionState::hasFrozenFootprint(copy));
    const int installsBefore = FaciesPartitionEditor::installCount();
    const int readsBefore = FaciesPartitionState::footprintReadCount();
    QVERIFY(f.edits.beginEdit(copy, &error));
    // 50 次属性编辑命令（editCommandEnded → styleLayer 每次触发）。
    const QList<QgsFeatureId> fids = copy->allFeatureIds().values();
    QVERIFY(fids.size() >= 2);
    for (int i = 0; i < 50; ++i) {
      const auto fid = fids.at(i % fids.size());
      const int code = 1 + (i % 5);
      copy->beginEditCommand(QStringLiteral("edit %1").arg(i));
      QVERIFY(copy->changeAttributeValue(
          fid, copy->fields().indexOf(QStringLiteral("facies_code")), code));
      copy->endEditCommand();
    }
    QCOMPARE(FaciesPartitionEditor::installCount(), installsBefore);
    QCOMPARE(FaciesPartitionState::footprintReadCount(), readsBefore);
    // 轻量刷新仍在工作：样式属性与绑定没有丢。
    QVERIFY(FaciesPartitionState::hasFrozenFootprint(copy));
    QVERIFY(FaciesPartitionEditor::isPartition(copy));
    // undo/redo 亦不重装。
    while (copy->undoStack()->canUndo())
      copy->undoStack()->undo();
    QCOMPARE(FaciesPartitionEditor::installCount(), installsBefore);
    QCOMPARE(FaciesPartitionState::footprintReadCount(), readsBefore);
    f.edits.rollbackEdit(copy);
  }
};
int main(int argc, char **argv) {
  QTemporaryDir profile;
  if (!profile.isValid())
    return 1;
  QgsApplication app(argc, argv, true, profile.path());
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH"), true);
  QgsApplication::init(profile.path());
  QCoreApplication::setOrganizationName(QStringLiteral("paleo-tests"));
  QCoreApplication::setApplicationName(QStringLiteral("tst_p5_edit_lifecycle"));
  QString databaseError;
  if (!QgsApplication::createDatabase(&databaseError)) {
    qCritical().noquote() << databaseError;
    return 1;
  }
  app.initQgis();
  QgsCoordinateReferenceSystem::invalidateCache(true);
  GDALAllRegister();
  TestP5EditLifecycle test;
  const int rc = QTest::qExec(&test, argc, argv);
  QgsApplication::exitQgis();
  return rc;
}
#include "tst_p5_edit_lifecycle.moc"
