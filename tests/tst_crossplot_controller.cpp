#include "app/appcontext.h"
#include "app/crossplotcontroller.h"
#include "io/dataimportservice.h"
#include "io/perffixtures.h"
#include "linkage/selectioncontext.h"
#include "qgis/qgiscanvascontroller.h"
#include "qgis/qgislayerservice.h"
#include "qgis/qgisprojectservice.h"
#include "services/crossplotsamples.h"
#include "ui/crossplot/crossplotpanel.h"
#include "ui/pages/composepage.h"
#include "ui/paleomainwindow.h"
#include "workflow/faciesclassify.h"
#include "workflow/workflows.h"
#include <QAction>
#include <QComboBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>
#include <cmath>
#include <limits>
#include <qgsapplication.h>
#include <qgsmapcanvas.h>
using namespace paleo::crossplot;
class TestController : public QObject {
  Q_OBJECT
private slots:
  void endToEnd();
  void supervisedChain();
  void oracleSampleFloor();
};
void TestController::endToEnd() {
  QTemporaryDir dir;
  AppContext context(qEnvironmentVariable("QGIS_PREFIX_PATH", "/usr"));
  QVERIFY(context.ready());
  QVERIFY(context.projectSvc()->createProject(dir.filePath("test.qgz")));
  auto *cat = context.importSvc()->catalog();
  QVERIFY(cat->isOpen());
  for (int i = 0; i < 2; ++i) {
    const auto path = dir.filePath(QString("input%1.tif").arg(i));
    QVERIFY(PerfFixtures::makeSyntheticGeoTiff(path, 32, 32, false));
    CatalogAsset a;
    a.id = cat->nextAssetId();
    a.type = "horizon";
    a.displayName = QString("axis%1").arg(i);
    a.format = "tif";
    QVERIFY(cat->addAsset(a));
    CatalogVersion v;
    v.id = cat->nextVersionId();
    v.assetId = a.id;
    v.stage = "DERIVED";
    v.managed = false;
    v.path = path;
    QVERIFY(cat->addVersion(v));
  }
  PaleoMainWindow window(context.canvasCtl(), context.projectSvc(),
                         context.layerSvc(), context.toolSvc(),
                         context.selection());
  window.attachWorkflows(
      context.predictionWf(), context.constraintWf(), context.compositionWf(),
      context.validationWf(), context.importSvc(), context.seismicLink(),
      context.processingSvc(), context.store(), context.editingSvc(),
      context.layoutSvc(), context.taskSvc());
  new CrossplotController(&context, &window);
  window.show();
  context.selection()->setActiveHorizon("D53");
  auto *action = window.findChild<QAction *>("openCrossplot");
  QVERIFY(action);
  action->trigger();
  auto *panel = window.findChild<CrossplotPanel *>();
  QVERIFY(panel);
  auto *sources = panel->findChild<QListWidget *>("crossplotSources");
  QCOMPARE(sources->count(), 2);
  QStringList sourceIds;
  for (int i = 0; i < sources->count(); ++i) {
    sources->item(i)->setSelected(true);
    sourceIds << sources->item(i)->data(Qt::UserRole).toString();
  }
  panel->findChild<QPushButton *>("crossplotLoad")->click();
  auto *run = panel->findChild<QPushButton *>("crossplotRun");
  QTRY_VERIFY_WITH_TIMEOUT(run->isEnabled(), 10000);
  auto *workflow = window.findChild<FaciesClassifyWorkflow *>();
  QVERIFY(workflow);
  QCOMPARE(workflow->samples()->rows(), std::size_t(1024));
  panel->findChild<QComboBox *>("crossplotX")->setCurrentIndex(1);
  run->click();
  auto *write = panel->findChild<QPushButton *>("crossplotWrite");
  QTRY_VERIFY_WITH_TIMEOUT(write->isEnabled(), 10000);
  write->click();
  QVERIFY(context.layerSvc()->layer("predict.D53.crossplot"));
  auto *combo = window.findChild<QComboBox *>("faciesRasterCombo");
  QVERIFY(combo);
  QVERIFY(combo->findData("predict.D53.crossplot") >= 0);
  panel->pointRequested(
      {.5, .5}); // actual picked sample must be in the raster footprint
  auto center = context.canvasCtl()->canvas()->extent().center();
  QVERIFY(center.x() >= 500000 && center.x() <= 500800);
  QVERIFY(center.y() <= 4000000 && center.y() >= 3999200);
  panel->lassoRequested({{0, 0}, {.7, 0}, {.7, .7}, {0, .7}});
  QVERIFY(!panel->findChildren<QLabel *>().isEmpty());
  // An invalid intent leaves valid samples intact; a failed replacement clears
  // them.
  panel->samplesRequested({"missing0", "missing1"});
  QVERIFY(
      workflow
          ->samples()); // invalid intent rejected before replacing valid data
  QVERIFY(QFile::remove(dir.filePath("input0.tif")));
  panel->samplesRequested(sourceIds);
  QTRY_COMPARE_WITH_TIMEOUT(context.taskSvc()->runningCount(), 0, 10000);
  QVERIFY(!workflow->samples());
  QVERIFY(!run->isEnabled());
  QVERIFY(!write->isEnabled());
  // Cancellation before the sampler runs still reports an explicit status.
  context.taskSvc()->setMaxWorkerThreads(1);
  auto *blocker = context.taskSvc()->start("block", [](PaleoTask *t) {
    while (!t->cancelRequested())
      QThread::msleep(1);
    return QString();
  });
  panel->samplesRequested(sourceIds);
  panel->cancelRequested();
  blocker->requestCancel();
  QTRY_COMPARE_WITH_TIMEOUT(context.taskSvc()->runningCount(), 0, 10000);
  const auto labels = panel->findChildren<QLabel *>();
  QVERIFY(std::any_of(labels.begin(), labels.end(), [](QLabel *label) {
    return label->text() == QString::fromUtf8("抽样已取消");
  }));
}
// 方向46 批 3：交会页「标注 → 训练 → 推理 → 成图」端到端（UI 信号 →
// controller → workflow）。夹具照 endToEnd：两通道合成 GeoTIFF（同值 →
// 样本落在对角线，两个对角套索区各自选中一批行），经面板 lassoRequested
// 走 controller::select，再由面板标注按钮经 assignLabelRequested 落到
// workflow 训练集。
void TestController::supervisedChain() {
  QTemporaryDir dir;
  AppContext context(qEnvironmentVariable("QGIS_PREFIX_PATH", "/usr"));
  QVERIFY(context.ready());
  QVERIFY(context.projectSvc()->createProject(dir.filePath("test.qgz")));
  auto *cat = context.importSvc()->catalog();
  QVERIFY(cat->isOpen());
  for (int i = 0; i < 2; ++i) {
    const auto path = dir.filePath(QString("input%1.tif").arg(i));
    QVERIFY(PerfFixtures::makeSyntheticGeoTiff(path, 32, 32, false));
    CatalogAsset a;
    a.id = cat->nextAssetId();
    a.type = "horizon";
    a.displayName = QString("axis%1").arg(i);
    a.format = "tif";
    QVERIFY(cat->addAsset(a));
    CatalogVersion v;
    v.id = cat->nextVersionId();
    v.assetId = a.id;
    v.stage = "DERIVED";
    v.managed = false;
    v.path = path;
    QVERIFY(cat->addVersion(v));
  }
  PaleoMainWindow window(context.canvasCtl(), context.projectSvc(),
                         context.layerSvc(), context.toolSvc(),
                         context.selection());
  window.attachWorkflows(
      context.predictionWf(), context.constraintWf(), context.compositionWf(),
      context.validationWf(), context.importSvc(), context.seismicLink(),
      context.processingSvc(), context.store(), context.editingSvc(),
      context.layoutSvc(), context.taskSvc());
  new CrossplotController(&context, &window);
  window.show();
  context.selection()->setActiveHorizon("D53");
  auto *action = window.findChild<QAction *>("openCrossplot");
  QVERIFY(action);
  action->trigger();
  auto *panel = window.findChild<CrossplotPanel *>();
  QVERIFY(panel);
  auto *sources = panel->findChild<QListWidget *>("crossplotSources");
  QCOMPARE(sources->count(), 2);
  for (int i = 0; i < sources->count(); ++i)
    sources->item(i)->setSelected(true);
  panel->findChild<QPushButton *>("crossplotLoad")->click();
  auto *workflow = window.findChild<FaciesClassifyWorkflow *>();
  QVERIFY(workflow);
  auto *run = panel->findChild<QPushButton *>("crossplotRun");
  auto *train = panel->findChild<QPushButton *>("crossplotTrain");
  auto *write = panel->findChild<QPushButton *>("crossplotWrite");
  auto *method = panel->findChild<QComboBox *>("crossplotMethod");
  auto *name = panel->findChild<QComboBox *>("crossplotClassName");
  auto *summary = panel->findChild<QLabel *>("crossplotTrainingSummary");
  QVERIFY(run && train && write && method && name && summary);
  QTRY_VERIFY_WITH_TIMEOUT(run->isEnabled(), 10000);
  QCOMPARE(workflow->samples()->rows(), std::size_t(1024));
  // Medium-1（轮 3）：setDimensions 先、setSamples 后——load 完成链末尾
  // 「未标注」摘要不得被 setDimensions 的清空抹掉。
  QVERIFY(summary->text().contains(QString::fromUtf8("未标注")));
  // zero-annotation 初始态：训练按钮禁用且 tooltip 带中文原因。
  // Medium-2 根因修（setSamples 先落样本再清训练集）后，load 完成链上
  // trainingChanged 已按新样本重算：k-means 缺省 → 原因是「方法非监督」，
  // 不再停留在陈旧的「请先读取至少两个通道」。
  QVERIFY(!train->isEnabled());
  QVERIFY(train->toolTip().contains(QString::fromUtf8("请选择监督分类方法")));
  QVERIFY(!train->toolTip().contains(QString::fromUtf8("请先读取")));
  method->setCurrentIndex(int(Classifier::Lda));
  QVERIFY(!train->isEnabled());
  // 样本就绪 + 零标注 → 精确文案。
  QVERIFY(train->toolTip().contains(QString::fromUtf8("尚无标注样本")));
  // 无选区点「标注选区」：只提示，不发空标注。
  panel->findChild<QPushButton *>("crossplotAssignLabel")->click();
  const auto labels = panel->findChildren<QLabel *>();
  QVERIFY(std::any_of(labels.begin(), labels.end(), [](QLabel *label) {
    return label->text() == QString::fromUtf8("先在图上框选样本");
  }));
  QVERIFY(workflow->trainingSet().labels.empty());
  // 两次套索标注两类（经面板 → controller::select → assignTrainingLabel）。
  name->setCurrentText(QString::fromUtf8("砂岩"));
  panel->lassoRequested({{0, 0}, {.5, 0}, {.5, .5}, {0, .5}});
  panel->findChild<QPushButton *>("crossplotAssignLabel")->click();
  // 单类态（Medium-3）：validate 拒「监督分类至少需要两个类」——UI 级
  // 禁用 + 中文原因，按钮不放行。
  QVERIFY(!train->isEnabled());
  QVERIFY(train->toolTip().contains(QString::fromUtf8("至少需要两个类")));
  name->setCurrentText(QString::fromUtf8("泥岩"));
  panel->lassoRequested({{.5, .5}, {1, .5}, {1, 1}, {.5, 1}});
  panel->findChild<QPushButton *>("crossplotAssignLabel")->click();
  QCOMPARE(workflow->trainingSet().classNames,
           (QStringList{"砂岩", "泥岩"}));
  QVERIFY(summary->text().contains(QString::fromUtf8("已标注")));
  // 标注两类后训练按钮启用（validate 放行）。
  QVERIFY(train->isEnabled());
  // trainRequested 链路 → trainingReady，report 键齐全。
  QSignalSpy trained(workflow, &FaciesClassifyWorkflow::trainingReady);
  QSignalSpy trainFailed(workflow, &FaciesClassifyWorkflow::failed);
  train->click();
  QVERIFY2(trained.wait(10000),
           qPrintable(trainFailed.isEmpty()
                          ? QString()
                          : trainFailed.first().first().toString()));
  const auto report = trained.first().first().toMap();
  QCOMPARE(report.value("classNames").toStringList(),
           (QStringList{"砂岩", "泥岩"}));
  QCOMPARE(report.value("supervisedMethod").toInt(), int(Classifier::Lda));
  QCOMPARE(report.value("confusionCells").toList().size(), qsizetype(4));
  QCOMPARE(report.value("precision").toList().size(), qsizetype(2));
  QCOMPARE(report.value("recall").toList().size(), qsizetype(2));
  QCOMPARE(report.value("folds").toInt(), 5); // 面板 CV 折数缺省 5
  QVERIFY(report.value("labeledCount").toInt() > 0);
  auto *quality = panel->findChild<QLabel *>("crossplotQuality");
  QVERIFY(quality);
  QVERIFY(quality->text().contains(QString::fromUtf8("砂岩")));
  QVERIFY(quality->text().contains(QString::fromUtf8("泥岩")));
  QVERIFY(!quality->text().contains(QString::fromUtf8("准确率")));
  // 方法切换映射（Medium-4）：切 QDA → 模型方法不匹配，run 禁用但 train
  // 可用（同一训练集过 QDA 每类维度+1 下限）；切回 LDA → run 恢复。
  method->setCurrentIndex(int(Classifier::Qda));
  QVERIFY(!run->isEnabled());
  QVERIFY(train->isEnabled());
  QVERIFY(quality->text().isEmpty()); // Medium-1：旧方法 CV 质量不得残留
  method->setCurrentIndex(int(Classifier::Lda));
  QVERIFY(run->isEnabled());
  // 质量区回填（Low-1 轮 2）：切回已训练方法 → 恢复该次训练的 CV 质量。
  QVERIFY(!quality->text().isEmpty());
  QVERIFY(quality->text().contains(QString::fromUtf8("砂岩")));
  // 监督族推理：模型新鲜 + 训练态可用 → run 启用；classificationReady。
  QVERIFY(run->isEnabled());
  QSignalSpy classified(workflow,
                        &FaciesClassifyWorkflow::classificationReady);
  run->click();
  QVERIFY2(classified.wait(10000),
           qPrintable(workflow->classification().error));
  QVERIFY(workflow->classification().ok);
  // 成图：主图 instantiate；置信度/掩膜两件只 declare 进图层树。
  write->click();
  QVERIFY(context.layerSvc()->layer("predict.D53.crossplot"));
  QVERIFY(!context.layerSvc()->isInstantiated("confidence.D53.crossplot"));
  QVERIFY(!context.layerSvc()->isInstantiated(
      "predict.D53.crossplot.masked"));
  QStringList declaredIds;
  for (const auto &d : context.layerSvc()->declared())
    declaredIds << d.layerId;
  QVERIFY(declaredIds.contains("confidence.D53.crossplot"));
  QVERIFY(declaredIds.contains("predict.D53.crossplot.masked"));
  // 清除标注 → 模型作废：训练与运行按钮都回落禁用并带原因；质量区清空
  // （Medium-1：陈旧 CV 混淆矩阵不得残留）。
  panel->findChild<QPushButton *>("crossplotClearTraining")->click();
  QVERIFY(workflow->trainingSet().labels.empty());
  QVERIFY(!train->isEnabled());
  QVERIFY(train->toolTip().contains(QString::fromUtf8("尚无标注样本")));
  QVERIFY(!run->isEnabled());
  QVERIFY(quality->text().isEmpty());
  // clearTraining 在途补偿（批 2 遗留 Low，批 3 C 项修复）：占住唯一工作
  // 线程让 train 排队，此时清标注——finished 回调被 generation 早退，面板
  // 必须脱离 busy，且模型不得被在途任务复活。
  context.taskSvc()->setMaxWorkerThreads(1);
  auto *blocker = context.taskSvc()->start("block", [](PaleoTask *t) {
    while (!t->cancelRequested())
      QThread::msleep(1);
    return QString();
  });
  QVERIFY(blocker);
  name->setCurrentText(QString::fromUtf8("砂岩"));
  panel->lassoRequested({{0, 0}, {.5, 0}, {.5, .5}, {0, .5}});
  panel->findChild<QPushButton *>("crossplotAssignLabel")->click();
  name->setCurrentText(QString::fromUtf8("泥岩"));
  panel->lassoRequested({{.5, .5}, {1, .5}, {1, 1}, {.5, 1}});
  panel->findChild<QPushButton *>("crossplotAssignLabel")->click();
  QVERIFY(train->isEnabled());
  train->click(); // 排队：工作线程被 blocker 占住 → busy 常驻
  auto *cancel = panel->findChild<QPushButton *>("crossplotCancel");
  QVERIFY(cancel && cancel->isEnabled()); // busy 中：取消可点
  // busy 期间面板控件按 setBusy 设计全禁用，在途 clearTraining 是程序化
  // 路径（如换样本前清训练集）——直发信号触发，同 endToEnd 直发信号惯例。
  panel->clearTrainingRequested();
  QVERIFY(!cancel->isEnabled()); // C 项：busy 补偿 emit 已抵达面板
  QVERIFY(!train->isEnabled());
  blocker->requestCancel();
  QTRY_COMPARE_WITH_TIMEOUT(context.taskSvc()->runningCount(), 0, 10000);
  // 在途 train 的 finished 回调被 generation 早退：不复活 busy、不回填模型。
  QVERIFY(!cancel->isEnabled());
  QVERIFY(!train->isEnabled());
  QVERIFY(!run->isEnabled());
  QVERIFY(workflow->trainingSet().labels.empty());
  // clearTraining 只作废训练集/模型，不碰已生成的分类成果。
  QVERIFY(workflow->classification().ok);
}
// Oracle 7 样本不足环（UI 级）：两类中一类仅 1 样本 → validate 的 CV 每类
// 下限中文拒答，训练按钮禁用并带原因。单样本选区的圈法必须确定性地只圈住
// 一行：在实测投影帧里取近邻间距最大的点当锚，矩形半宽 = 1/4 间距——其余
// 任一点到锚的距离 ≥ gap，若落在闭矩形内则距离 ≤ 0.354·gap < gap，矛盾，
// 故闭矩形内有且仅有一个采样点。
void TestController::oracleSampleFloor() {
  QTemporaryDir dir;
  AppContext context(qEnvironmentVariable("QGIS_PREFIX_PATH", "/usr"));
  QVERIFY(context.ready());
  QVERIFY(context.projectSvc()->createProject(dir.filePath("test.qgz")));
  auto *cat = context.importSvc()->catalog();
  QVERIFY(cat->isOpen());
  for (int i = 0; i < 2; ++i) {
    const auto path = dir.filePath(QString("input%1.tif").arg(i));
    QVERIFY(PerfFixtures::makeSyntheticGeoTiff(path, 32, 32, false));
    CatalogAsset a;
    a.id = cat->nextAssetId();
    a.type = "horizon";
    a.displayName = QString("axis%1").arg(i);
    a.format = "tif";
    QVERIFY(cat->addAsset(a));
    CatalogVersion v;
    v.id = cat->nextVersionId();
    v.assetId = a.id;
    v.stage = "DERIVED";
    v.managed = false;
    v.path = path;
    QVERIFY(cat->addVersion(v));
  }
  PaleoMainWindow window(context.canvasCtl(), context.projectSvc(),
                         context.layerSvc(), context.toolSvc(),
                         context.selection());
  window.attachWorkflows(
      context.predictionWf(), context.constraintWf(), context.compositionWf(),
      context.validationWf(), context.importSvc(), context.seismicLink(),
      context.processingSvc(), context.store(), context.editingSvc(),
      context.layoutSvc(), context.taskSvc());
  new CrossplotController(&context, &window);
  window.show();
  context.selection()->setActiveHorizon("D53");
  auto *action = window.findChild<QAction *>("openCrossplot");
  QVERIFY(action);
  action->trigger();
  auto *panel = window.findChild<CrossplotPanel *>();
  QVERIFY(panel);
  auto *sources = panel->findChild<QListWidget *>("crossplotSources");
  QCOMPARE(sources->count(), 2);
  for (int i = 0; i < sources->count(); ++i)
    sources->item(i)->setSelected(true);
  panel->findChild<QPushButton *>("crossplotLoad")->click();
  auto *workflow = window.findChild<FaciesClassifyWorkflow *>();
  auto *run = panel->findChild<QPushButton *>("crossplotRun");
  auto *train = panel->findChild<QPushButton *>("crossplotTrain");
  auto *method = panel->findChild<QComboBox *>("crossplotMethod");
  auto *name = panel->findChild<QComboBox *>("crossplotClassName");
  QVERIFY(workflow && run && train && method && name);
  // samples 就绪前 samples() 为空——先等运行按钮可用（面板维度已落），再取行数
  // （QTRY_COMPARE 表达式内直接解引用空 shared_ptr 会段错误）。
  QTRY_VERIFY_WITH_TIMEOUT(run->isEnabled(), 10000);
  QCOMPARE(workflow->samples()->rows(), std::size_t(1024));
  method->setCurrentIndex(int(Classifier::Lda));
  // 第一类：左下对角半区（多行）。
  name->setCurrentText(QString::fromUtf8("砂岩"));
  panel->lassoRequested({{0, 0}, {.45, 0}, {.45, .45}, {0, .45}});
  panel->findChild<QPushButton *>("crossplotAssignLabel")->click();
  // 第二类：单行。锚点取实测帧内 x > 0.55（避开第一类半区）中近邻间距最大
  // 的点；复刻 controller 的投影口径（同一样本、同一 axes）。
  const auto frame =
      CrossplotSamples::project(*workflow->samples(), panel->axes());
  QVERIFY(!frame.points.isEmpty());
  int pick = -1;
  double gap = 0;
  for (int i = 0; i < frame.points.size(); ++i) {
    if (frame.points[i].x <= 0.55)
      continue;
    double nearest = std::numeric_limits<double>::infinity();
    for (int j = 0; j < frame.points.size(); ++j) {
      if (i == j)
        continue;
      const double dx = frame.points[i].x - frame.points[j].x;
      const double dy = frame.points[i].y - frame.points[j].y;
      nearest = std::min(nearest, std::sqrt(dx * dx + dy * dy));
    }
    if (nearest > gap) {
      gap = nearest;
      pick = i;
    }
  }
  QVERIFY(pick >= 0);
  QVERIFY(gap > 0);
  const double eps = gap * 0.25;
  const QPointF anchor{frame.points[pick].x, frame.points[pick].y};
  name->setCurrentText(QString::fromUtf8("泥岩"));
  panel->lassoRequested({{anchor.x() - eps, anchor.y() - eps},
                         {anchor.x() + eps, anchor.y() - eps},
                         {anchor.x() + eps, anchor.y() + eps},
                         {anchor.x() - eps, anchor.y() + eps}});
  panel->findChild<QPushButton *>("crossplotAssignLabel")->click();
  // 标注计数如实：砂岩多行、泥岩恰一行（圈法确定性）。
  const auto &training = workflow->trainingSet();
  QCOMPARE(training.classNames, (QStringList{"砂岩", "泥岩"}));
  int sand = 0, shale = 0;
  for (int code : training.labels) {
    if (code == 0)
      ++sand;
    else if (code == 1)
      ++shale;
  }
  QVERIFY(sand > 2);
  QCOMPARE(shale, 1);
  // CV 每类下限拒答（faciestraining.cpp 实际文案）：训练按钮禁用 + 原因。
  QVERIFY(!train->isEnabled());
  QVERIFY(train->toolTip().contains(QString::fromUtf8("交叉验证要求每类至少")));
  // 补足样本后放行：把泥岩半区也标上 → 两类都过下限。
  name->setCurrentText(QString::fromUtf8("泥岩"));
  panel->lassoRequested({{.55, .55}, {1, .55}, {1, 1}, {.55, 1}});
  panel->findChild<QPushButton *>("crossplotAssignLabel")->click();
  QVERIFY(train->isEnabled());
}
int main(int argc, char **argv) {
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", "/usr"), true);
  app.initQgis();
  TestController test;
  const int rc = QTest::qExec(&test, argc, argv);
  QgsApplication::exitQgis();
  return rc;
}
#include "tst_crossplot_controller.moc"
