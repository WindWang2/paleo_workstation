#include <QtTest>
#include <QDir>
#include <QTemporaryDir>
#include <QSignalSpy>

#include <qgsmapcanvas.h>

#include "../src/metadata/layermanifest.h"
#include "../src/qgis/qgiscanvascontroller.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/ui/maptools/paleomaptools.h"
#include "../src/ui/typedconstraintdrawcontroller.h"
#include "../src/workflow/workflows.h"

// 方向23 验收——类型化约束绘制链闭环：五个语义入口 → 捕获工具 →
// TypedConstraintDrawController::onDrawn 确定性提交 → ConstraintStore
// type 列与 params_json.semantic 回读一致 → 删除/语义切换走同一持久化通道。
class TestConstraintDraw : public QObject
{
  Q_OBJECT
  struct Stack
  {
    QgisProjectService projectSvc;
    std::unique_ptr<LayerManifest> manifest;
    std::unique_ptr<QgisLayerService> layerSvc;
    std::unique_ptr<ConstraintWorkflow> wf;
    QgisCanvasController canvasCtl;
    std::unique_ptr<TypedConstraintDrawController> ctl;
  };

  static std::unique_ptr<Stack> makeStack(const QString &dir)
  {
    QDir().mkpath(dir);
    auto s = std::make_unique<Stack>();
    if (!s->projectSvc.createProject(QDir(dir).filePath(QStringLiteral("proj.qgz"))))
      return nullptr;
    s->manifest = std::make_unique<LayerManifest>(QDir(dir).filePath(QStringLiteral("m.sqlite")));
    if (!s->manifest->open())
      return nullptr;
    s->layerSvc = std::make_unique<QgisLayerService>(&s->projectSvc, s->manifest.get());
    s->wf = std::make_unique<ConstraintWorkflow>(nullptr, s->layerSvc.get());
    s->ctl = std::make_unique<TypedConstraintDrawController>(&s->canvasCtl, s->wf.get());
    return s;
  }

  // 五个语义入口 → ConstraintStore type 列词表（hard_barrier 存 break_line 同义词）。
  static QString storedTypeFor(const QString &entryType)
  {
    if (entryType == QLatin1String("break_line") || entryType == QLatin1String("hard_barrier"))
      return QStringLiteral("break_line");
    if (entryType == QLatin1String("direction_line") || entryType == QLatin1String("direction_guide"))
      return QStringLiteral("direction_line");
    return entryType;
  }

private slots:
  void initTestCase() { QVERIFY(QgisRuntime::isInitialized()); }

  // 五种语义全部可画：每个入口走 startCapture → onDrawn → 入库 → 回读一致。
  void fiveSemanticEntriesDrawAndRoundTrip()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto s = makeStack(dir.filePath(QStringLiteral("p")));
    QVERIFY(s != nullptr);
    auto &ctl = *s->ctl;

    const QStringList entries{
        QStringLiteral("break_line"),        QStringLiteral("direction_line"),
        QStringLiteral("interpretive_boundary"), QStringLiteral("contour_stop"),
        QStringLiteral("cartographic_detour"),
    };
    const QStringList semantics{
        QStringLiteral("hard_barrier"),      QStringLiteral("direction_guide"),
        QStringLiteral("interpretive_boundary"), QStringLiteral("contour_stop"),
        QStringLiteral("cartographic_detour"),
    };
    QSignalSpy addedSpy(s->wf.get(), &ConstraintWorkflow::constraintAdded);
    QSignalSpy finSpy(&ctl, &TypedConstraintDrawController::captureFinished);
    for (int i = 0; i < entries.size(); ++i)
    {
      ctl.startCapture(QStringLiteral("T1"), QStringLiteral("line"), entries.at(i), 7);
      QVERIFY2(ctl.active(), qPrintable(entries.at(i)));
      QVERIFY(qobject_cast<PaleoDrawConstraintTool *>(ctl.currentTool()));
      ctl.onDrawn(QStringLiteral("LineString (%1 0, %1 1)").arg(i));
      QVERIFY(!ctl.active());
    }
    QCOMPARE(addedSpy.count(), 5);
    QCOMPARE(finSpy.count(), 5);

    // 约束页声明已就位。
    const QVector<LayerDeclaration> decls = s->layerSvc->declared();
    QCOMPARE(decls.size(), 1);
    QCOMPARE(decls.at(0).layerId, QStringLiteral("constraints.T1"));

    // 入库回读：type 列与 params_json.semantic 逐行对号。
    const QVector<QVariantMap> rows = s->wf->loadConstraints(QStringLiteral("T1"));
    QCOMPARE(rows.size(), 5);
    for (int i = 0; i < rows.size(); ++i)
    {
      const QString expectedType = storedTypeFor(entries.at(i));
      QCOMPARE(rows.at(i).value(QStringLiteral("type")).toString(), expectedType);
      const QString paramsText = rows.at(i).value(QStringLiteral("params_json")).toString();
      QVERIFY2(!paramsText.isEmpty(), qPrintable(expectedType));
      const auto doc = QJsonDocument::fromJson(paramsText.toUtf8());
      QVERIFY(doc.isObject());
      QCOMPARE(doc.object().value(QStringLiteral("semantic")).toString(), semantics.at(i));
    }
  }

  // 语义切换：硬屏障 → 制图绕行，type 列与 params_json 同步改写。
  void semanticSwitchRewritesTypeColumn()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto s = makeStack(dir.filePath(QStringLiteral("p")));
    QVERIFY(s != nullptr);
    QString id;
    QString err;
    QVERIFY(s->wf->addConstraint(QStringLiteral("T1"), QStringLiteral("LineString (0 0, 1 1)"),
                                 QStringLiteral("break_line"), 0, &err, &id));
    QVERIFY(s->wf->switchConstraintSemantic(id, QStringLiteral("cartographic_detour"), &err));
    const QVector<QVariantMap> rows = s->wf->loadConstraints(QStringLiteral("T1"));
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.at(0).value(QStringLiteral("type")).toString(), QStringLiteral("cartographic_detour"));
    const auto doc = QJsonDocument::fromJson(
        rows.at(0).value(QStringLiteral("params_json")).toString().toUtf8());
    QCOMPARE(doc.object().value(QStringLiteral("semantic")).toString(),
             QStringLiteral("cartographic_detour"));
    // 冻结语义词表：第六词拒绝。
    QVERIFY(!s->wf->switchConstraintSemantic(id, QStringLiteral("wormhole"), &err));
    QVERIFY(err.contains(QStringLiteral("未知约束语义")));
  }

  // 删除：store 行消失、constraintRemoved 发出、未知 id 拒绝。
  void removeConstraintDropsRow()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto s = makeStack(dir.filePath(QStringLiteral("p")));
    QVERIFY(s != nullptr);
    QString id;
    QString err;
    QVERIFY(s->wf->addConstraint(QStringLiteral("T1"), QStringLiteral("LineString (0 0, 1 1)"),
                                 QStringLiteral("direction_line"), 0, &err, &id));
    QSignalSpy removedSpy(s->wf.get(), &ConstraintWorkflow::constraintRemoved);
    QVERIFY(s->wf->removeConstraint(id, &err));
    QCOMPARE(removedSpy.count(), 1);
    QCOMPARE(removedSpy.at(0).at(0).toString(), id);
    QVERIFY(s->wf->loadConstraints(QStringLiteral("T1")).isEmpty());
    QVERIFY(!s->wf->removeConstraint(id, &err));
    QVERIFY(err.contains(QStringLiteral("找不到约束")));
  }
};

int main(int argc, char *argv[])
{
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
  {
    qFatal("QgisRuntime::initialize failed");
    return 1;
  }
  TestConstraintDraw tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_constraint_draw.moc"