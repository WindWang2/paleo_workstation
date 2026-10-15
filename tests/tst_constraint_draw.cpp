#include <QtTest>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QListWidget>
#include <QTemporaryDir>
#include <QSignalSpy>

#include <qgsmapcanvas.h>
#include <qgssnappingconfig.h>
#include <qgssnappingutils.h>

#include "../src/metadata/layermanifest.h"
#include "../src/qgis/qgiscanvascontroller.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/ui/maptools/paleomaptools.h"
#include "../src/ui/typedconstraintdrawcontroller.h"
#include "../src/ui/pages/constraintpage.h"
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

  // ---- U6：约束 reason 词表与主链显示完整 ----------------------------------

  // 真实栈（wf + store）+ 约束页夹具：ctor 绑 wf（wireWorkflowFeedback 写
  // paleo.page.wf），层位下拉置 T1——refreshConstraintList 从 store 滤载 T1 行。
  struct PageStack
  {
    std::unique_ptr<Stack> stack;
    std::unique_ptr<ConstraintPage> page;
  };
  static std::unique_ptr<PageStack> makePageStack(const QString &dir)
  {
    auto ps = std::make_unique<PageStack>();
    ps->stack = makeStack(dir);
    if (!ps->stack)
      return nullptr;
    ps->page = std::make_unique<ConstraintPage>(ps->stack->wf.get());
    auto *horizons = ps->page->findChild<QComboBox *>(QStringLiteral("horizonCombo"));
    if (horizons->findText(QStringLiteral("T1")) < 0)
      horizons->addItem(QStringLiteral("T1"));
    horizons->setCurrentIndex(horizons->findText(QStringLiteral("T1")));
    return ps;
  }

  static QString constraintItemText(const ConstraintPage *page, const QString &id)
  {
    auto *list = page->findChild<QListWidget *>(QStringLiteral("constraintList"));
    for (int i = 0; list && i < list->count(); ++i)
      if (list->item(i)->data(Qt::UserRole).toString() == id)
        return list->item(i)->text();
    return QString();
  }

private slots:
  void initTestCase() { QVERIFY(QgisRuntime::isInitialized()); }

  // 方向23：捕获工具激活时画布吸附已开（AllLayers、顶点+段）——绘制可吸
  // 井位/已有约束线端点；断层棒是域模型非地图图层，不参与 QGIS 吸附。
  void snappingActiveDuringCapture()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto s = makeStack(dir.filePath(QStringLiteral("p")));
    QVERIFY(s != nullptr);
    auto &ctl = *s->ctl;
    ctl.startCapture(QStringLiteral("T1"), QStringLiteral("line"),
                     QStringLiteral("direction_line"), 0);
    const QgsSnappingConfig config = s->canvasCtl.canvas()->snappingUtils()->config();
    QVERIFY(config.enabled());
    QCOMPARE(config.mode(), Qgis::SnappingMode::AllLayers);
    QVERIFY(config.typeFlag() & Qgis::SnappingType::Vertex);
    QVERIFY(config.typeFlag() & Qgis::SnappingType::Segment);
    ctl.cancel();
  }

  void familyParametersPersistOnCapture() {
    QTemporaryDir dir; QVERIFY(dir.isValid()); auto s = makeStack(dir.filePath("p")); QVERIFY(s);
    s->ctl->startCapture("T1", "line", "direction_line", 0, {{"ratio", 6.5}, {"influenceRadius", 50.0}});
    s->ctl->onDrawn("LineString (0 0, 1 1)");
    s->ctl->startCapture("T1", "line", "break_line", 0, {{"blockMode", "display_only"}});
    s->ctl->onDrawn("LineString (0 1, 1 2)");
    const auto rows = s->wf->loadConstraints("T1"); QCOMPARE(rows.size(), 2);
    const auto first = QJsonDocument::fromJson(rows[0].value("params_json").toString().toUtf8()).object();
    QCOMPARE(first.value("ratio").toDouble(), 6.5); QCOMPARE(first.value("influenceRadius").toDouble(), 50.0);
    const auto second = QJsonDocument::fromJson(rows[1].value("params_json").toString().toUtf8()).object();
    QCOMPARE(second.value("blockMode").toString(), QStringLiteral("display_only"));
  }

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
    QVERIFY(s);
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

  // ---- U6 用例（夹具见上方 makePageStack/constraintItemText）----------------

  // 主链：通用「绘制约束」入口以裸 type=line 入库（无 params_json，与
  // ConstraintDrawController::onDrawn → drawConstraint 同口径）——默认方法
  // local_direction_idw 下该行被拒 unknown_type:line。列表/tooltip 文案必须是
  // 中文原因（不露裸 token），且提示区把它计入「未类型化」桶（计数盲区）。
  void untypedConstraintRowShowsChineseReason()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto ps = makePageStack(dir.filePath(QStringLiteral("p")));
    QVERIFY(ps != nullptr);
    QString err;
    QVERIFY(ps->stack->wf->addConstraint(QStringLiteral("T1"), QStringLiteral("LineString (0 0, 1 1)"),
                                         QStringLiteral("line"), 0, &err));
    ps->page->refreshConstraintList();
    auto *list = ps->page->findChild<QListWidget *>(QStringLiteral("constraintList"));
    QCOMPARE(list->count(), 1);
    const QString text = list->item(0)->text();
    QVERIFY2(!text.contains(QStringLiteral("unknown_type")),
             qPrintable(QStringLiteral("bare token leaked: %1").arg(text)));
    QVERIFY2(text.contains(QStringLiteral("未识别的约束类型：line")), qPrintable(text));
    QVERIFY2(list->item(0)->toolTip().contains(QStringLiteral("未识别的约束类型")),
             qPrintable(list->item(0)->toolTip()));
    auto *hint = ps->page->findChild<QLabel *>(QStringLiteral("factorConstraintHint"));
    QVERIFY2(hint->text().contains(QStringLiteral("1 条未类型化")), qPrintable(hint->text()));
  }

  // 参数化：constraintsemantics 实际发出、页面可达/可注入的原因 token 全走
  // 同一中文映射（other_horizon / directions_disabled_by_request /
  // interpretive_strength_zero_or_barriers_disabled / unknown_method）。
  // 行集用 paleo.page.constraintrows 覆盖、结构面开关用 paleo.page.structural*
  // 注入，触发点都是既有联动（成图方法切换 → applyConstraintUsePlan）。
  void reasonVocabularyCoversDomainTokens()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto ps = makePageStack(dir.filePath(QStringLiteral("p")));
    QVERIFY(ps != nullptr);
    auto *wf = ps->stack->wf.get();
    QString err, idBarrier, idDirection;
    QVERIFY(wf->addConstraint(QStringLiteral("T1"), QStringLiteral("LineString (0 0, 1 1)"),
                              QStringLiteral("break_line"), 0, &err, &idBarrier));
    QVERIFY(wf->addConstraint(QStringLiteral("T1"), QStringLiteral("LineString (0 2, 1 3)"),
                              QStringLiteral("direction_line"), 0, &err, &idDirection));
    ps->page->refreshConstraintList();
    auto *method = ps->page->findChild<QComboBox *>(QStringLiteral("factorMethodCombo"));
    const int structural = method->findData(QStringLiteral("structural_idw"));
    QVERIFY(structural >= 0);

    // other_horizon：硬屏障行覆盖成 T2（当前层位 T1）→ 切方法触发重算。
    QVariantList overrideRows;
    for (const QVariantMap &row : wf->loadConstraints(QStringLiteral("T1")))
    {
      QVariantMap mutated = row;
      if (mutated.value(QStringLiteral("id")).toString() == idBarrier)
        mutated.insert(QStringLiteral("horizon"), QStringLiteral("T2"));
      overrideRows << mutated;
    }
    ps->page->setProperty("paleo.page.constraintrows", overrideRows);
    method->setCurrentIndex(structural);
    QString text = constraintItemText(ps->page.get(), idBarrier);
    QVERIFY2(!text.contains(QStringLiteral("other_horizon")),
             qPrintable(QStringLiteral("bare token leaked: %1").arg(text)));
    QVERIFY2(text.contains(QStringLiteral("属于其他层位")), qPrintable(text));

    // 结构面开关（页面暂无专属控件，读 paleo.page.* 属性，缺省与引擎参数一致）：
    // 停用方向 → directions_disabled_by_request；停用屏障 →
    // interpretive_strength_zero_or_barriers_disabled。
    ps->page->refreshConstraintList(); // 恢复 store 真实行（去掉 T2 覆盖）
    ps->page->setProperty("paleo.page.structuralEnableDirections", false);
    method->setCurrentIndex(method->findData(QStringLiteral("local_direction_idw")));
    ps->page->setProperty("paleo.page.structuralEnableBarriers", false);
    method->setCurrentIndex(structural);
    text = constraintItemText(ps->page.get(), idDirection);
    QVERIFY2(!text.contains(QStringLiteral("directions_disabled_by_request")),
             qPrintable(QStringLiteral("bare token leaked: %1").arg(text)));
    QVERIFY2(text.contains(QStringLiteral("已停用方向线")), qPrintable(text));
    text = constraintItemText(ps->page.get(), idBarrier);
    QVERIFY2(!text.contains(QStringLiteral("interpretive_strength_zero_or_barriers_disabled")),
             qPrintable(QStringLiteral("bare token leaked: %1").arg(text)));
    QVERIFY2(text.contains(QStringLiteral("屏障已停用")), qPrintable(text));

    // unknown_method：词表外方法 id（下拉可注入）→ 同一映射兜底。
    method->addItem(QStringLiteral("未知方法"), QStringLiteral("martian_idw"));
    method->setCurrentIndex(method->findData(QStringLiteral("martian_idw")));
    text = constraintItemText(ps->page.get(), idBarrier);
    QVERIFY2(!text.contains(QStringLiteral("unknown_method")),
             qPrintable(QStringLiteral("bare token leaked: %1").arg(text)));
    QVERIFY2(text.contains(QStringLiteral("未知的成图方法")), qPrintable(text));
  }
};

int main(int argc, char *argv[])
{
  // 页面用例构造 QWidget：QgsApplication（即 QApplication）构造前定好平台。
  if (qgetenv("QT_QPA_PLATFORM").isEmpty())
    qputenv("QT_QPA_PLATFORM", "offscreen");
  if (!QgisRuntime::initialize(QStringLiteral("/usr"))) // 自持 QgsApplication
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