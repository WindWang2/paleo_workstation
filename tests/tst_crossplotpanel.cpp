#include "services/crossplotsamples.h"
#include "services/faciestraining.h"
#include "ui/crossplot/crossplotpanel.h"
#include "ui/paleotheme.h"
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>
#include <limits>
using namespace paleo::crossplot;
class TestPanel : public QObject {
  Q_OBJECT
private slots:
  void intents();
  void density();
  void methodComboOrder();
  void trainingFlow();
};
void TestPanel::intents() {
  PaleoTheme::pinRenderEnvironment();
  CrossplotPanel panel;
  panel.resize(900, 480);
  panel.show();
  panel.setSources(
      {{"a", "GR", "well"}, {"b", "RHOB", "well"}, {"c", "PHI", "well"}});
  panel.setDimensions({"GR", "RHOB", "PHI"});
  QSignalSpy axes(&panel, &CrossplotPanel::axesRequested),
      load(&panel, &CrossplotPanel::samplesRequested),
      run(&panel, &CrossplotPanel::classifyRequested),
      lasso(&panel, &CrossplotPanel::lassoRequested),
      point(&panel, &CrossplotPanel::pointRequested);
  panel.findChild<QComboBox *>("crossplotX")->setCurrentIndex(2);
  QCOMPARE(axes.count(), 1);
  QCOMPARE(panel.axes().x, 2);
  auto *sources = panel.findChild<QListWidget *>("crossplotSources");
  sources->item(0)->setSelected(true);
  sources->item(1)->setSelected(true);
  panel.findChild<QPushButton *>("crossplotLoad")->click();
  QCOMPARE(load.count(), 1);
  QCOMPARE(load[0][0].toStringList().size(), 2);
  panel.findChild<QPushButton *>("crossplotRun")->click();
  QCOMPARE(run.count(), 1);
  panel.setClassified(true, {3, 4});
  QVERIFY(panel.findChild<QPushButton *>("crossplotWrite")->isEnabled());
  panel.setBusy(true);
  QVERIFY(!panel.findChild<QPushButton *>("crossplotWrite")->isEnabled());
  panel.setBusy(false);
  SampleSet samples;
  samples.names = {"a", "b"};
  samples.units = {"", ""};
  samples.values = {0, 0, 1, 1, .5, .5};
  samples.locations.resize(3);
  auto frame = CrossplotSamples::project(samples, {});
  panel.setFrame(frame);
  auto *canvas = panel.canvas();
  auto r = canvas->plotRect();
  QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier,
                    r.center().toPoint());
  QCOMPARE(point.count(), 1);
  const QPoint start = (r.topLeft() + QPointF(5, 5)).toPoint(),
               end = (r.bottomRight() - QPointF(5, 5)).toPoint();
  QTest::mousePress(canvas, Qt::LeftButton, Qt::ShiftModifier, start);
  QTest::mouseMove(canvas, end);
  QTest::mouseRelease(canvas, Qt::LeftButton, Qt::ShiftModifier, end);
  QCOMPARE(lasso.count(), 1);
  auto vertices = qvariant_cast<QVector<QPointF>>(lasso[0][0]);
  auto selected = CrossplotSamples::select(samples, frame, vertices);
  QCOMPARE(selected.indices.size(), 1);
  QCOMPARE(selected.means[0], .5);
  panel.setSelection(selected, samples.names);
  panel.setFrame(frame);
  panel.findChild<QComboBox *>("crossplotMethod")->setCurrentIndex(2);
  panel.findChild<QPushButton *>("crossplotRun")->click();
  QVERIFY(
      qvariant_cast<ClassificationOptions>(run.last()[0]).selection.isEmpty());
}
void TestPanel::density() {
  CrossplotPanel panel;
  panel.resize(1000, 550);
  panel.show();
  SampleSet samples;
  samples.names = {"X", "Y", "Z"};
  samples.units = {"", "", ""};
  samples.values.reserve(300000);
  samples.locations.resize(100000);
  for (int i = 0; i < 100000; ++i) {
    samples.values.push_back(i % 1000);
    samples.values.push_back(i / 1000);
    samples.values.push_back(i % 23);
  }
  panel.setDimensions(samples.names);
  panel.setSources(
      {{"a", "VSH", "well"}, {"b", "PHI", "well"}, {"c", "RMS", "raster"}});
  QElapsedTimer timer;
  timer.start();
  auto frame = CrossplotSamples::project(samples, {});
  panel.setFrame(frame);
  panel.canvas()->grab();
  double ms = timer.nsecsElapsed() / 1e6;
  QCOMPARE(frame.points.size(), 100000);
  QVERIFY(!frame.density.isEmpty());
  qint64 total = 0;
  for (int n : frame.density)
    total += n;
  QCOMPARE(total, 100000);
  qInfo("BASELINE crossplot_100k_project_paint_ms = %.3f", ms);
  timer.restart();
  Axes a{2, 0, 1, 70, 30};
  panel.findChild<QComboBox *>("crossplotX")->setCurrentIndex(a.x);
  panel.findChild<QComboBox *>("crossplotY")->setCurrentIndex(a.y);
  panel.findChild<QComboBox *>("crossplotZ")->setCurrentIndex(a.z + 1);
  auto spins = panel.findChildren<QDoubleSpinBox *>();
  QCOMPARE(spins.size(), 2);
  spins[0]->setValue(a.yaw);
  spins[1]->setValue(a.pitch);
  QCoreApplication::processEvents();
  frame = CrossplotSamples::project(samples, a);
  panel.setFrame(frame);
  panel.canvas()->grab();
  qInfo("BASELINE crossplot_100k_axis_switch_3d_ms = %.3f",
        timer.nsecsElapsed() / 1e6);
  QVERIFY(frame.is3d);
  std::vector<int> labels(samples.rows());
  for (std::size_t i = 0; i < labels.size(); ++i)
    labels[i] = int(i % 8);
  frame = CrossplotSamples::project(samples, a, labels);
  QVERIFY(!frame.densityClass.isEmpty());
  panel.setFrame(frame);
  panel.setClassified(true, QVector<qint64>(8, 12500));
  QCoreApplication::processEvents();

  if (auto path = qEnvironmentVariable("PALEO_CROSSPLOT_SCREENSHOT");
      !path.isEmpty())
    QVERIFY(panel.grab().save(path));
}
// 方法下拉项序 == Classifier 枚举序（索引耦合护栏回归）：新增分类器只许
// 追加在枚举与 combo 末尾（Som=4…Knn=7），插入中间即红。
void TestPanel::methodComboOrder() {
  CrossplotPanel panel;
  auto *combo = panel.findChild<QComboBox *>("crossplotMethod");
  QVERIFY(combo);
  // 项数锚在枚举末项：新增分类器只许追加（Knn 之后），插入中间即红。
  QCOMPARE(combo->count(), int(Classifier::Knn) + 1);
  QCOMPARE(combo->currentIndex(), 0); // 缺省 k-means
  const QStringList expected{QString::fromUtf8("k-means++"),
                             QString::fromUtf8("高斯混合（对角 EM）"),
                             QString::fromUtf8("手选凸包规则"),
                             QString::fromUtf8("手选多维盒规则"),
                             QString::fromUtf8("SOM 自组织图"),
                             QString::fromUtf8("LDA 线性判别"),
                             QString::fromUtf8("QDA 二次判别"),
                             QString::fromUtf8("kNN 近邻")};
  for (int i = 0; i < expected.size(); ++i)
    QCOMPARE(combo->itemText(i), expected[i]);
  // 索引耦合：面板 options() 的方法必须等于枚举第 i 项。
  for (int i = 0; i < combo->count(); ++i) {
    combo->setCurrentIndex(i);
    QCOMPARE(int(panel.options().method), i);
  }
  // 判据一致性（域枚举双判据防漂移）：面板的监督族判据（CV 折数可见性）
  // 必须逐族等于 services/faciestraining.h isSupervisedClassifier——跨层同
  // 口径，任何一侧漂移都会让训练按钮/运行门禁与 validate 分派错位。
  auto *cvFolds = panel.findChild<QSpinBox *>("crossplotCvFolds");
  QVERIFY(cvFolds);
  for (int i = 0; i < combo->count(); ++i) {
    combo->setCurrentIndex(i);
    QCOMPARE(!cvFolds->isHidden(), isSupervisedClassifier(Classifier(i)));
  }
  combo->setCurrentIndex(0);
  // 新参数控件缺省值（SOM 2-16/4；kNN 1-50/5；CV 2-10/5；掩膜 0.05-0.95/0.5）。
  auto *somW = panel.findChild<QSpinBox *>("crossplotSomWidth");
  auto *somH = panel.findChild<QSpinBox *>("crossplotSomHeight");
  auto *knn = panel.findChild<QSpinBox *>("crossplotKnnK");
  auto *cv = panel.findChild<QSpinBox *>("crossplotCvFolds");
  auto *mask = panel.findChild<QSpinBox *>("crossplotMaskThreshold");
  QVERIFY(somW && somH && knn && cv && mask);
  QCOMPARE(somW->minimum(), 2);
  QCOMPARE(somW->maximum(), 16);
  QCOMPARE(somW->value(), 4);
  QCOMPARE(somH->value(), 4);
  QCOMPARE(knn->minimum(), 1);
  QCOMPARE(knn->maximum(), 50);
  QCOMPARE(knn->value(), 5);
  QCOMPARE(cv->minimum(), 2);
  QCOMPARE(cv->maximum(), 10);
  QCOMPARE(cv->value(), 5);
  // 掩膜阈值百分数整型 spin：5–95 ↔ 0.05–0.95，步 5 ↔ 0.05，默认 50 ↔ 0.5。
  QCOMPARE(mask->minimum(), 5);
  QCOMPARE(mask->maximum(), 95);
  QCOMPARE(mask->singleStep(), 5);
  QCOMPARE(mask->value(), 50);
  somW->setValue(6);
  knn->setValue(7);
  cv->setValue(3);
  const auto o = panel.options();
  QCOMPARE(o.somWidth, 6);
  QCOMPARE(o.somHeight, 4);
  QCOMPARE(o.knnNeighbors, 7);
  QCOMPARE(o.cvFolds, 3);
  QCOMPARE(o.confidenceMaskThreshold, 0.5); // 百分数 / 100 → 域口径 [0,1]
  mask->setValue(95);
  QCOMPARE(panel.options().confidenceMaskThreshold, 0.95);
  mask->setValue(5);
  QCOMPARE(panel.options().confidenceMaskThreshold, 0.05);
  mask->setValue(50);
}
// Oracle 7（UI 级）+ 显隐 + 质量渲染：视图只发信号、只反射状态，门禁判断
// 全在 controller/workflow；这里只钉面板契约。
void TestPanel::trainingFlow() {
  PaleoTheme::pinRenderEnvironment();
  CrossplotPanel panel;
  panel.resize(900, 480);
  panel.show();
  panel.setDimensions({"GR", "RHOB"});
  auto *train = panel.findChild<QPushButton *>("crossplotTrain");
  auto *run = panel.findChild<QPushButton *>("crossplotRun");
  auto *method = panel.findChild<QComboBox *>("crossplotMethod");
  auto *summary = panel.findChild<QLabel *>("crossplotTrainingSummary");
  auto *quality = panel.findChild<QLabel *>("crossplotQuality");
  QVERIFY(train && run && method && summary && quality);
  // 摘要单一出处 = controller setTrainingSummary（workflow trainingSummary）；
  // 面板自身不持有「未标注」等工作流口径文案（裸面板 = 空）。
  QVERIFY(summary->text().isEmpty());
  // 训练禁用原因单一出处 = controller 首次 refreshTrainingState；面板构造期
  // 不 hardcode 文案（无 controller 的裸面板：禁用 + 空 tooltip）。
  QVERIFY(!train->isEnabled());
  QVERIFY(train->toolTip().isEmpty());
  auto hidden = [&panel](const char *name) {
    auto *w = panel.findChild<QWidget *>(QString::fromUtf8(name));
    return w && w->isHidden();
  };
  // 参数显隐：k-means 下 k 可见、SOM/kNN/CV 隐藏、掩膜阈值始终可见。
  QVERIFY(!hidden("crossplotK"));
  QVERIFY(hidden("crossplotSomWidth"));
  QVERIFY(hidden("crossplotSomHeight"));
  QVERIFY(hidden("crossplotKnnK"));
  QVERIFY(hidden("crossplotCvFolds"));
  QVERIFY(!hidden("crossplotMaskThreshold"));
  method->setCurrentIndex(int(Classifier::Som));
  QVERIFY(hidden("crossplotK"));
  QVERIFY(!hidden("crossplotSomWidth"));
  QVERIFY(!hidden("crossplotSomHeight"));
  QVERIFY(!hidden("crossplotMaskThreshold"));
  method->setCurrentIndex(int(Classifier::Knn));
  QVERIFY(!hidden("crossplotKnnK"));
  QVERIFY(!hidden("crossplotCvFolds"));
  QVERIFY(hidden("crossplotSomWidth"));
  method->setCurrentIndex(int(Classifier::Lda));
  QVERIFY(!hidden("crossplotCvFolds"));
  QVERIFY(hidden("crossplotKnnK"));
  QVERIFY(hidden("crossplotK"));
  // 显隐矩阵往返：切回 k-means 后 k 复可见、监督控件复隐藏。
  method->setCurrentIndex(int(Classifier::KMeans));
  QVERIFY(!hidden("crossplotK"));
  QVERIFY(hidden("crossplotCvFolds"));
  QVERIFY(hidden("crossplotSomWidth"));
  QVERIFY(hidden("crossplotKnnK"));
  // 训练按钮三态：禁用带原因 tooltip；启用即 reason 清空。
  const QString reason = QString::fromUtf8("尚无标注样本——先用套索选区并赋予类名");
  panel.setTrainingState(false, reason);
  QVERIFY(!train->isEnabled());
  QVERIFY(train->toolTip().contains(reason));
  panel.setTrainingState(true, QString());
  QVERIFY(train->isEnabled());
  QVERIFY(train->toolTip().isEmpty());
  // 第三态：启用 + 非空 reason（warnings 摘要）——按钮可用且 tooltip 提示。
  panel.setTrainingState(true, QString::fromUtf8("可以训练（注意：类样本数比 11.0:1 失衡）"));
  QVERIFY(train->isEnabled());
  QVERIFY(!train->toolTip().isEmpty());
  panel.setTrainingState(true, QString());
  // busy 对偶文案：任务进行中（与「当前没有运行中的任务」成对）。
  panel.setBusy(true);
  QVERIFY(!train->isEnabled());
  QCOMPARE(train->toolTip(), QString::fromUtf8("任务进行中"));
  QCOMPARE(run->toolTip(), QString::fromUtf8("任务进行中")); // Low-2：同排同口径
  // 新控件同口径（Low-4 轮 2）：忙 = 「任务进行中」。
  for (auto *w : QList<QWidget *>{
           panel.findChild<QWidget *>("crossplotClassName"),
           panel.findChild<QWidget *>("crossplotAssignLabel"),
           panel.findChild<QWidget *>("crossplotClearTraining"),
           panel.findChild<QWidget *>("crossplotSomWidth"),
           panel.findChild<QWidget *>("crossplotSomHeight"),
           panel.findChild<QWidget *>("crossplotKnnK"),
           panel.findChild<QWidget *>("crossplotCvFolds"),
           panel.findChild<QWidget *>("crossplotMaskThreshold")}) {
    QVERIFY(w);
    QCOMPARE(w->toolTip(), QString::fromUtf8("任务进行中"));
  }
  // Low-1（轮 3）幂等短路：busy 期重入 setBusy(true)（setTrainingState 内部
  // 即如此）不得把对偶文案误还原。
  panel.setTrainingState(true, QString());
  QCOMPARE(train->toolTip(), QString::fromUtf8("任务进行中"));
  QCOMPARE(panel.findChild<QPushButton *>("crossplotAssignLabel")->toolTip(),
           QString::fromUtf8("任务进行中"));
  panel.setBusy(false);
  QVERIFY(train->isEnabled());
  QCOMPARE(panel.findChild<QPushButton *>("crossplotAssignLabel")->toolTip(),
           QString::fromUtf8("把当前套索选区标注为指定类名（重复标注覆盖）"));
  QCOMPARE(panel.findChild<QPushButton *>("crossplotClearTraining")->toolTip(),
           QString::fromUtf8("清空全部标注与已训练模型"));
  // Low-3（轮 3）：kNN 近邻 spin 常态 tooltip 提示「重新训练方生效」。
  QVERIFY(panel.findChild<QSpinBox *>("crossplotKnnK")
              ->toolTip()
              .contains(QString::fromUtf8("重新训练")));
  // 无监督族运行按钮不受训练态约束。
  QVERIFY(run->isEnabled());
  // 监督族运行门禁：训练态可用且已训练（LDA 视角）。
  method->setCurrentIndex(int(Classifier::Lda));
  QVERIFY(!run->isEnabled());
  panel.setTrainingState(true, QString());
  QVERIFY(!run->isEnabled()); // 未训练
  panel.setModelTrained(true);
  QVERIFY(run->isEnabled());
  panel.setTrainingState(false, reason);
  QVERIFY(!run->isEnabled()); // 训练态回退即禁
  QVERIFY(run->toolTip().contains(reason));
  panel.setTrainingState(true, QString());
  panel.setModelTrained(false);
  QVERIFY(!run->isEnabled());
  // Medium-1（轮 2）：启用态 + 非空 reason（warnings 摘要）时 run 禁用，
  // tooltip 必须是固定文案，不得拿非阻断 warnings 冒充禁用原因。
  const QString warnSummary =
      QString::fromUtf8("可以训练（注意：类样本数比 11.0:1 失衡）");
  panel.setTrainingState(true, warnSummary);
  QVERIFY(!run->isEnabled());
  QCOMPARE(run->toolTip(), QString::fromUtf8("请先完成标注并训练监督模型"));
  QVERIFY(!run->toolTip().contains(warnSummary));
  // 对照：未启用态才取 controller 下发的真实禁用原因。
  panel.setModelTrained(true);
  QVERIFY(run->isEnabled());
  panel.setTrainingState(false, reason);
  QVERIFY(!run->isEnabled());
  QCOMPARE(run->toolTip(), reason);
  panel.setTrainingState(true, QString());
  // 信号面：标注/清除/训练/方法切换都只发信号不带参到 workflow。
  QSignalSpy assign(&panel, &CrossplotPanel::assignLabelRequested),
      clear(&panel, &CrossplotPanel::clearTrainingRequested),
      trained(&panel, &CrossplotPanel::trainRequested),
      changed(&panel, &CrossplotPanel::methodChanged),
      params(&panel, &CrossplotPanel::paramsChanged);
  auto *name = panel.findChild<QComboBox *>("crossplotClassName");
  QVERIFY(name);
  name->setCurrentText(QString::fromUtf8("砂岩"));
  panel.findChild<QPushButton *>("crossplotAssignLabel")->click();
  QCOMPARE(assign.count(), 1);
  QCOMPARE(assign[0][0].toString(), QString::fromUtf8("砂岩"));
  panel.findChild<QPushButton *>("crossplotClearTraining")->click();
  QCOMPARE(clear.count(), 1);
  panel.setTrainingState(true, QString());
  train->click();
  QCOMPARE(trained.count(), 1);
  method->setCurrentIndex(int(Classifier::Qda));
  QCOMPARE(changed.count(), 1);
  // 训练相关参数变更 → paramsChanged（controller 据此重算训练可用态；
  // CV 折数/kNN 近邻范围与 validate 下限耦合）。
  auto *cv = panel.findChild<QSpinBox *>("crossplotCvFolds");
  QVERIFY(cv);
  cv->setValue(3);
  QCOMPARE(params.count(), 1);
  QCOMPARE(panel.options().cvFolds, 3);
  auto *knn = panel.findChild<QSpinBox *>("crossplotKnnK");
  QVERIFY(knn);
  method->setCurrentIndex(int(Classifier::Knn));
  knn->setValue(9);
  QCOMPARE(params.count(), 2);
  QCOMPARE(panel.options().knnNeighbors, 9);
  // 质量展示：CV 混淆矩阵 + 每类查准/查全百分数 + warnings；禁「准确率」。
  QVariantMap report;
  report.insert("method", "lda");
  report.insert("supervisedMethod", int(Classifier::Lda));
  report.insert("classNames", QStringList{"砂岩", "泥岩"});
  report.insert("classIds", QVariantList{0, 1});
  report.insert("folds", 5);
  report.insert("labeledCount", 40);
  report.insert("confusionCells",
                QVariantList{qint64(19), qint64(1), qint64(2), qint64(18)});
  report.insert("precision", QVariantList{0.95, 0.9474});
  report.insert("recall", QVariantList{0.95, 0.9});
  report.insert("warnings",
                QStringList{QString::fromUtf8("类「泥岩」仅有 3 个样本，"
                                              "交叉验证波动可能偏大")});
  panel.setTrainingQuality(report);
  const QString text = quality->text();
  QVERIFY(text.contains(QString::fromUtf8("交叉验证")));
  QVERIFY(text.contains(QString::fromUtf8("砂岩")));
  QVERIFY(text.contains(QString::fromUtf8("泥岩")));
  // 强断言：逐类指标行整串 + 混淆矩阵单元格标记（弱「19」子串易误命中）。
  QVERIFY(text.contains(QString::fromUtf8("砂岩：查准率 95.0%，查全率 95.0%")));
  QVERIFY(text.contains(QString::fromUtf8("泥岩：查准率 94.7%，查全率 90.0%")));
  QVERIFY(text.contains(QString::fromUtf8("<td>19</td>")));
  QVERIFY(text.contains(QString::fromUtf8("<td>1</td>")));
  QVERIFY(text.contains(QString::fromUtf8("<td>2</td>")));
  QVERIFY(text.contains(QString::fromUtf8("<td>18</td>")));
  QVERIFY(text.contains(QString::fromUtf8("提示")));
  QVERIFY(!text.contains(QString::fromUtf8("准确率")));
  panel.setTrainingQuality({});
  QVERIFY(quality->text().isEmpty());
  // NaN precision（无预测的类）：破折号如实呈现，不渲染 nan 字样。
  QVariantMap nanReport = report;
  nanReport.insert("precision", QVariantList{std::numeric_limits<double>::quiet_NaN(), 0.9});
  nanReport.insert("recall", QVariantList{0.95, 0.9});
  panel.setTrainingQuality(nanReport);
  const QString nanText = quality->text();
  QVERIFY(nanText.contains(QString::fromUtf8("—")));
  QVERIFY(!nanText.contains(QString::fromUtf8("nan")));
  QVERIFY(!nanText.contains(QString::fromUtf8("准确率")));
  panel.setTrainingQuality({});
}
QTEST_MAIN(TestPanel)
#include "tst_crossplotpanel.moc"
