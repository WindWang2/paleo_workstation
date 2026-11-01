#include <QtTest>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTableWidget>
#include <QTreeWidget>
#include <QGroupBox>
#include "../src/ui/propertymodel/propertymodelpanel.h"
#include <QAbstractButton>
#include <QToolButton>
#include <QVariantMap>

#include <QScrollArea>
#include <QDir>
#include <QFile>
#include <QScrollBar>

#include "../src/domain/singlefactorstrategy.h" // 方向67：策略包词表（下拉单一真源对拍）
#include "../src/services/singlefactordef.h"
#include "../src/ui/pages/pagepanels.h"
#include "../src/ui/pages/constraintpage_internal.h" // U9：parseLevels 级别数上限纯函数面
#include "../src/ui/pages/mappingworkbenchpage.h"
#include "../src/ui/paleotheme.h"
#include "../src/workflow/boundarysemantics.h" // C2：相界语义类型词表（单类型首发）
#include "../src/workflow/mappingworkbench.h"

// m2/mapping-pages — 三个编图页（预测/单因素/智能编图）的贯通测试。
// 约定与 tst_panels 相同：面板只发意图信号，测试用 nullptr/null 服务栈 +
// QSignalSpy + objectName 子件查找（plain QApplication，无 QisRuntime）。
// 文件内三个测试类共用一个 main（多类 qExec）；每页一个类，各任务段
// 只改自己的类：
//   PredictPageTests   —— 智能预测页（任务 A）
//   FactorPageTests    —— 单因素图页（任务 B；ConstraintPage 重定位）
//   ComposePageTests   —— 智能编图页（任务 C）

// ---- m2(A) PredictPageTests 专用依赖（只服务本类；manifest/layer service
// 是纯 QtSql/信号面，plain QApplication 可直用，无需 QisRuntime）----
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>

#include "../src/metadata/layermanifest.h"
#include "../src/catalog/datacatalog.h"
#include "../src/workflow/workflows.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/services/algoparamschema.h"

#if PALEO_HAVE_ORT
#include "../src/ai/onnxpredictionservice.h"
#endif

#include <qgsapplication.h>

class PredictPageTests : public QObject
{
  Q_OBJECT
  private slots:
    void constructsWithNullServices(); // 基座冒烟：拆分后类仍可构造
    void predictTypeVocabulary();      // m2(A)：预测类型词表 + stable id
    void schemaRegistryMirrorsAlgorithmTruth(); // m2(A)：schema 注册表对照算法真值
    void noOnnxModelsShowsHonestDegradation(); // ai-assist 范围5：未装模型如实降级
    void schemaFormBuildsPerAlgorithm();        // m2(A)：表单按 schema 动态建控件
    void schemaParamsCollectedIntoRunRequest(); // m2(A)：schema 控件 → QVariantMap
    void predictTypeSwitchRebuildsForm();       // m2(A)：类型切换联动重建（值保留）
    void runBusyLifecycleAndCancel();
    void busyStateSurvivesPageHideShow();
    void schemaValidationFailureRefusesRun();
    void readinessSummaryGatesRun();                    // 预测专项 P0：准备检查
    void onnxRawTensorInputIsCollapsedAdvanced();       // 预测专项 P1：张量入口降级为高级/调试           // m2(A)：任务化忙碌/取消/进度
    void historyListFiltersAndShows();          // m2(A)：历史清单过滤/显示/刷新

  private:
    static LayerDeclaration makeDecl(const QString &layerId, const QString &horizon,
                                     const QString &group);
};

void PredictPageTests::noOnnxModelsShowsHonestDegradation()
{
  // setAlgorithms 是公开面——直接驱动，无需真 workflow 绑定（页面构造期
  // 与工程打开后的刷新走同一条路径）。
  PredictPage page(nullptr, nullptr);
  auto *status = page.findChild<QLabel *>(QStringLiteral("statusLabel"));
  QVERIFY(status);
#if PALEO_HAVE_ORT
  if (!PaleoOnnxService::runtimeAvailable())
    QSKIP("vendored onnxruntime not present");
  page.setAlgorithms({QStringLiteral("paleo:paleo_geological_smoothing")});
  QVERIFY2(status->text().contains(QStringLiteral("未装模型")),
           qPrintable(status->text()));
  // 算法表正常承载 onnx 条目，降级文案随之消失。
  page.setAlgorithms({QStringLiteral("paleo:paleo_geological_smoothing"),
                      QStringLiteral("onnx:seg3")});
  auto *algos = page.findChild<QComboBox *>(QStringLiteral("algoCombo"));
  QVERIFY(algos && algos->count() == 2);
  QCOMPARE(algos->itemText(1), QStringLiteral("seg3 (ONNX)"));
  QVERIFY(!status->text().contains(QStringLiteral("未装模型")));
#else
  QSKIP("build lacks ORT");
#endif
}

void PredictPageTests::constructsWithNullServices()
{
  PredictPage page(nullptr, nullptr);
  QVERIFY(page.findChild<QScrollArea *>(QStringLiteral("predictPageScroll")));
  QVERIFY(page.findChild<QWidget *>(QStringLiteral("predictPageBody")));
  QVERIFY(page.findChild<QComboBox *>(QStringLiteral("horizonCombo")));
  QVERIFY(page.findChild<QComboBox *>(QStringLiteral("algoCombo")));
  // m2(A) 冒烟：新面全部就位，未运行态下取消/进度隐藏、表单隐藏。
  QVERIFY(page.findChild<QComboBox *>(QStringLiteral("predictTypeCombo")));
  QVERIFY(page.findChild<QWidget *>(QStringLiteral("paramsFormArea")));
  QVERIFY(page.findChild<QWidget *>(QStringLiteral("onnxParamsArea")));
  QVERIFY(page.findChild<QPushButton *>(QStringLiteral("runButton")));
  QVERIFY(page.findChild<QPushButton *>(QStringLiteral("cancelRunButton")));
  QVERIFY(page.findChild<QProgressBar *>(QStringLiteral("runProgressBar")));
  QVERIFY(page.findChild<QLabel *>(QStringLiteral("statusLabel")));
  QVERIFY(page.findChild<QListWidget *>(QStringLiteral("historyList")));
  QVERIFY(page.findChild<QWidget *>(QStringLiteral("cancelRunButton"))->isHidden());
  QVERIFY(page.findChild<QWidget *>(QStringLiteral("runProgressBar"))->isHidden());
  QVERIFY(page.findChild<QWidget *>(QStringLiteral("paramsFormArea"))->isHidden());
}

void PredictPageTests::predictTypeVocabulary()
{
  PredictPage page(nullptr, nullptr);
  auto *types = page.findChild<QComboBox *>(QStringLiteral("predictTypeCombo"));
  QVERIFY(types);
  QCOMPARE(types->count(), 3);
  QCOMPARE(types->itemText(0), QStringLiteral("沉积相"));
  QCOMPARE(types->itemText(1), QStringLiteral("地震相"));
  QCOMPARE(types->itemText(2), QStringLiteral("测井相"));
  QCOMPARE(types->itemData(0).toString(), QStringLiteral("sedimentary_facies"));
  QCOMPARE(types->itemData(1).toString(), QStringLiteral("seismic_facies"));
  QCOMPARE(types->itemData(2).toString(), QStringLiteral("well_log_facies"));
  QCOMPARE(types->currentIndex(), 0);
}

void PredictPageTests::schemaRegistryMirrorsAlgorithmTruth()
{
  // 对照 paleoalgorithms/faciespolygonize initAlgorithm 真值（不虚构参数）。
  using F = AlgorithmParamField;
  QVERIFY(AlgorithmParamSchema::knows(QStringLiteral("paleo:paleo_geological_smoothing")));
  QVERIFY(AlgorithmParamSchema::knows(QStringLiteral("paleo:paleo_constraint_idw")));
  QVERIFY(AlgorithmParamSchema::knows(QStringLiteral("paleo:paleo_facies_fusion")));
  QVERIFY(AlgorithmParamSchema::knows(QStringLiteral("paleo:paleo_isopach")));
  QVERIFY(AlgorithmParamSchema::knows(QStringLiteral("paleo:paleo_facies_polygonize")));
  QVERIFY(!AlgorithmParamSchema::knows(QStringLiteral("paleo:x"))); // 未登记桩 id
  QVERIFY(!AlgorithmParamSchema::knows(QStringLiteral("onnx:toy"))); // onnx 不进注册表

  const auto smoothing = AlgorithmParamSchema::fieldsFor(
      QStringLiteral("paleo:paleo_geological_smoothing"));
  QCOMPARE(smoothing.size(), 1);
  QCOMPARE(smoothing.at(0).key, QStringLiteral("PASSES"));
  QCOMPARE(smoothing.at(0).type, F::Int);
  QCOMPARE(smoothing.at(0).defaultValue.toInt(), 1);
  QVERIFY(smoothing.at(0).hasMin);
  QCOMPARE(smoothing.at(0).minValue, 0.0);

  const auto polygonize = AlgorithmParamSchema::fieldsFor(
      QStringLiteral("paleo:paleo_facies_polygonize"));
  QCOMPARE(polygonize.size(), 8);
  bool sawSmooth = false, sawMinCells = false, sawMinFraction = false,
       sawBoundarySmooth = false;
  for (const F &f : polygonize)
  {
    if (f.key == QLatin1String("SMOOTH"))
    {
      sawSmooth = true;
      QCOMPARE(f.type, F::Int);
      QCOMPARE(f.defaultValue.toInt(), 1);
      QVERIFY(f.hasMin);
      QCOMPARE(f.minValue, 0.0);
      QVERIFY(f.hasMax);
      QCOMPARE(f.maxValue, 10.0);
    }
    if (f.key == QLatin1String("MIN_CELLS"))
    {
      sawMinCells = true;
      QCOMPARE(f.type, F::Int);
      QCOMPARE(f.defaultValue.toInt(), 4);
      QVERIFY(f.hasMin);
      QCOMPARE(f.minValue, 0.0);
    }
    if (f.key == QLatin1String("MIN_FRACTION"))
    {
      sawMinFraction = true;
      QCOMPARE(f.type, F::Double);
      QCOMPARE(f.defaultValue.toDouble(), 0.0025);
      QVERIFY(f.hasMin);
      QCOMPARE(f.minValue, 0.0);
      QVERIFY(f.hasMax);
      QCOMPARE(f.maxValue, 1.0);
    }
    if (f.key == QLatin1String("BOUNDARY_SMOOTH"))
    {
      sawBoundarySmooth = true;
      QCOMPARE(f.type, F::Int);
      QCOMPARE(f.defaultValue.toInt(), 2);
      QVERIFY(f.hasMin);
      QCOMPARE(f.minValue, 0.0);
      QVERIFY(f.hasMax);
      QCOMPARE(f.maxValue, 5.0);
    }
  }
  QVERIFY(sawSmooth);
  QVERIFY(sawBoundarySmooth);
  QVERIFY(sawMinCells);
  QVERIFY(sawMinFraction);
  bool sawAngle = false;
  for (const F &f : polygonize)
    if (f.key == QLatin1String("ANGLE_TOLERANCE"))
    {
      sawAngle = true;
      QCOMPARE(f.type, F::Double);
      QCOMPARE(f.defaultValue.toDouble(), 15.0);
      QVERIFY(f.hasMax);
      QCOMPARE(f.maxValue, 90.0);
    }
  QVERIFY(sawAngle);

  // fusion：登记在册但零标量参数（图层型参数由编排解析）。
  QCOMPARE(AlgorithmParamSchema::fieldsFor(QStringLiteral("paleo:paleo_facies_fusion")).size(), 0);
  QVERIFY(AlgorithmParamSchema::registeredAlgorithms().size() >= 5);
}

void PredictPageTests::schemaFormBuildsPerAlgorithm()
{
  PredictPage page(nullptr, nullptr);
  page.setAlgorithms({QStringLiteral("paleo:paleo_geological_smoothing"),
                      QStringLiteral("paleo:paleo_isopach"),
                      QStringLiteral("paleo:paleo_facies_fusion"),
                      QStringLiteral("paleo:x"),
                      QStringLiteral("onnx:toy")});
  auto *algos = page.findChild<QComboBox *>(QStringLiteral("algoCombo"));
  auto *form = page.findChild<QWidget *>(QStringLiteral("paramsFormArea"));
  auto *onnxArea = page.findChild<QWidget *>(QStringLiteral("onnxParamsArea"));
  QVERIFY(algos && form && onnxArea);

  // smoothing：PASSES spin（默认 1），表单可见。
  auto *passes = page.findChild<QSpinBox *>(QStringLiteral("param.PASSES"));
  QVERIFY(passes);
  QCOMPARE(passes->value(), 1);
  QVERIFY(!form->isHidden());

  // isopach：NEGATIVE_TO_NODATA 词表（是/否，默认 否）。
  algos->setCurrentIndex(1);
  auto *neg = page.findChild<QComboBox *>(QStringLiteral("param.NEGATIVE_TO_NODATA"));
  QVERIFY(neg);
  QCOMPARE(neg->count(), 2);
  QCOMPARE(neg->currentData(), QVariant(false));

  // fusion：登记在册零标量参数 → 如实说明，不虚构控件。
  algos->setCurrentIndex(2);
  QVERIFY(page.findChild<QWidget *>(QStringLiteral("schemaNoFieldsNote")));
  QVERIFY(!form->isHidden());

  // 未登记 paleo:x：无表单（params 保持空 map，与拆分前行为一致）。
  algos->setCurrentIndex(3);
  QVERIFY(form->isHidden());
  QVERIFY(onnxArea->isHidden());

  // onnx:toy：退化呈现 = 既有三控件，paramsFormArea 退场。
  algos->setCurrentIndex(4);
  QVERIFY(!onnxArea->isHidden());
  QVERIFY(form->isHidden());
  QVERIFY(page.findChild<QLineEdit *>(QStringLiteral("onnxInputEdit")));
  QVERIFY(page.findChild<QLineEdit *>(QStringLiteral("onnxShapeEdit")));
  QVERIFY(page.findChild<QLineEdit *>(QStringLiteral("onnxInputNameEdit")));
}

void PredictPageTests::schemaParamsCollectedIntoRunRequest()
{
  PredictPage page(nullptr, nullptr);
  page.setHorizons({QStringLiteral("T1")});
  page.setAlgorithms({QStringLiteral("paleo:paleo_geological_smoothing")});
  page.findChild<QComboBox *>(QStringLiteral("horizonCombo"))->setCurrentIndex(0);

  auto *passes = page.findChild<QSpinBox *>(QStringLiteral("param.PASSES"));
  QVERIFY(passes);
  passes->setValue(3);

  QSignalSpy spy(&page, &PredictPage::runRequested);
  page.findChild<QPushButton *>(QStringLiteral("runButton"))->click();
  QCOMPARE(spy.count(), 1);
  QCOMPARE(spy.first().at(0).toString(), QStringLiteral("T1"));
  QCOMPARE(spy.first().at(1).toString(), QStringLiteral("paleo:paleo_geological_smoothing"));
  const QVariantMap params = spy.first().at(2).toMap();
  QCOMPARE(params.value(QStringLiteral("PASSES")), QVariant(3));
  // 预测类型（stable id）随参数下发，且随类型切换更新。
  QCOMPARE(params.value(QStringLiteral("predictType")).toString(),
           QStringLiteral("sedimentary_facies"));

  page.findChild<QComboBox *>(QStringLiteral("predictTypeCombo"))->setCurrentIndex(1);
  spy.clear();
  page.findChild<QPushButton *>(QStringLiteral("runButton"))->click();
  QCOMPARE(spy.count(), 1);
  const QVariantMap params2 = spy.first().at(2).toMap();
  QCOMPARE(params2.value(QStringLiteral("predictType")).toString(),
           QStringLiteral("seismic_facies"));
  QCOMPARE(params2.value(QStringLiteral("PASSES")), QVariant(3)); // 同键值保留

  // 未登记算法：params 为空 map（既有语义）。
  page.setAlgorithms({QStringLiteral("paleo:x")});
  spy.clear();
  page.findChild<QPushButton *>(QStringLiteral("runButton"))->click();
  QCOMPARE(spy.count(), 1);
  QVERIFY(spy.first().at(2).toMap().isEmpty());
}

void PredictPageTests::predictTypeSwitchRebuildsForm()
{
  PredictPage page(nullptr, nullptr);
  page.setAlgorithms({QStringLiteral("paleo:paleo_geological_smoothing")});
  auto *passes = page.findChild<QSpinBox *>(QStringLiteral("param.PASSES"));
  QVERIFY(passes);
  passes->setValue(5);

  // 类型切换触发表单重建（联动）：同键字段值保留，不吞用户输入。
  page.findChild<QComboBox *>(QStringLiteral("predictTypeCombo"))->setCurrentIndex(2);
  auto *passesAfter = page.findChild<QSpinBox *>(QStringLiteral("param.PASSES"));
  QVERIFY(passesAfter);
  QCOMPARE(passesAfter->value(), 5);

  // 算法切换后切回：同键值仍保留（重建快照机制）。setAlgorithms 后当前项
  // 是 isopach（无 PASSES 字段），切回 smoothing 时值从快照恢复。
  page.setAlgorithms({QStringLiteral("paleo:paleo_isopach"),
                      QStringLiteral("paleo:paleo_geological_smoothing")});
  QCOMPARE(page.findChild<QSpinBox *>(QStringLiteral("param.PASSES")), nullptr);
  page.findChild<QComboBox *>(QStringLiteral("algoCombo"))->setCurrentIndex(1);
  auto *passesBack = page.findChild<QSpinBox *>(QStringLiteral("param.PASSES"));
  QVERIFY(passesBack);
  QCOMPARE(passesBack->value(), 5);
}

void PredictPageTests::runBusyLifecycleAndCancel()
{
  PredictPage page(nullptr, nullptr);
  // 预测专项：运行按钮受准备检查约束——给出可运行上下文（层位 + 算法）。
  page.setHorizons({QStringLiteral("T1")});
  page.setAlgorithms({QStringLiteral("paleo:x")});
  auto *run = page.findChild<QPushButton *>(QStringLiteral("runButton"));
  auto *cancel = page.findChild<QPushButton *>(QStringLiteral("cancelRunButton"));
  auto *bar = page.findChild<QProgressBar *>(QStringLiteral("runProgressBar"));
  QVERIFY(run && cancel && bar);

  page.setRunBusy(true);
  QVERIFY(!run->isEnabled());
  QVERIFY(!run->toolTip().isEmpty()); // DESIGN.md：禁用必须带 reason tooltip
  QVERIFY(!cancel->isHidden());
  QVERIFY(!bar->isHidden());
  QCOMPARE(bar->value(), 0);

  page.updateProgress(50);
  QCOMPARE(bar->value(), 50);
  page.updateProgress(120); // 越界钳制，不崩
  QCOMPARE(bar->value(), 100);

  QSignalSpy cancelSpy(&page, &PredictPage::runCancelRequested);
  cancel->click();
  QCOMPARE(cancelSpy.count(), 1);

  page.setRunBusy(false);
  QVERIFY(run->isEnabled());
  QVERIFY(run->toolTip().isEmpty());
  QVERIFY(cancel->isHidden());
  QVERIFY(bar->isHidden());

  page.setRunBusy(true); // 再次进入忙碌态从 0 起
  QCOMPARE(bar->value(), 0);
  page.setRunBusy(false);
}

LayerDeclaration PredictPageTests::makeDecl(const QString &layerId, const QString &horizon,
                                            const QString &group)
{
  LayerDeclaration d;
  d.layerId = layerId;
  d.horizon = horizon;
  d.type = QStringLiteral("raster");
  d.source = QStringLiteral("/tmp/%1.tif").arg(layerId); // 清单行不校验存在性
  d.group = group;
  return d;
}

void PredictPageTests::busyStateSurvivesPageHideShow()
{
  PredictPage page(nullptr, nullptr);
  page.setHorizons({QStringLiteral("T1")});
  page.setAlgorithms({QStringLiteral("paleo:x")});
  auto *run = page.findChild<QPushButton *>(QStringLiteral("runButton"));
  auto *cancel = page.findChild<QPushButton *>(QStringLiteral("cancelRunButton"));
  QVERIFY(run && cancel);

  // 页面切换（壳切页=hide/show）不打断忙碌守卫：状态跨显隐保持，取消路径
  // 完整（主线7「页面切换 dirty 守卫」——忙碌中的预测页被切走再切回）。
  page.setRunBusy(true);
  page.hide();
  page.show();
  QVERIFY(!run->isEnabled());
  QVERIFY(!cancel->isHidden());

  QSignalSpy cancelSpy(&page, &PredictPage::runCancelRequested);
  cancel->click();
  QCOMPARE(cancelSpy.count(), 1);
  page.setRunBusy(false);
  QVERIFY(run->isEnabled());
}

void PredictPageTests::schemaValidationFailureRefusesRun()
{
  PredictPage page(nullptr, nullptr);
  page.setHorizons({QStringLiteral("T1")});
  page.setAlgorithms({QStringLiteral("paleo:paleo_constraint_idw")});
  auto *algos = page.findChild<QComboBox *>(QStringLiteral("algoCombo"));
  auto *run = page.findChild<QPushButton *>(QStringLiteral("runButton"));
  auto *status = page.findChild<QLabel *>(QStringLiteral("statusLabel"));
  QVERIFY(algos && run && status);
  QCOMPARE(algos->currentData().toString(), QStringLiteral("paleo:paleo_constraint_idw"));
  auto *fieldEdit = page.findChild<QLineEdit *>(QStringLiteral("param.FIELD"));
  QVERIFY(fieldEdit != nullptr);

  QSignalSpy runSpy(&page, &PredictPage::runRequested);
  fieldEdit->setText(QString()); // 必填为空
  run->click();
  QCOMPARE(runSpy.count(), 0); // 校验失败：不发意图
  QVERIFY2(status->text().contains(QStringLiteral("不能为空")),
           qPrintable(status->text()));

  // 恢复合法值 → 正常下发
  fieldEdit->setText(QStringLiteral("z"));
  run->click();
  QCOMPARE(runSpy.count(), 1);
  QVERIFY2(status->text().isEmpty(), qPrintable(status->text()));
}

void PredictPageTests::readinessSummaryGatesRun()
{
  PredictPage page(nullptr, nullptr);
  auto *ready = page.findChild<QLabel *>(QStringLiteral("predictReadinessLabel"));
  auto *run = page.findChild<QPushButton *>(QStringLiteral("runButton"));
  auto *jump = page.findChild<QPushButton *>(QStringLiteral("openWellFaciesButton"));
  QVERIFY(ready && run && jump);
  QSignalSpy runSpy(&page, &PredictPage::runRequested);

  // 无层位、无算法：不可运行，按钮禁用且 tooltip 给出真实原因与下一步。
  QVERIFY2(ready->text().contains(QStringLiteral("不可运行")), qPrintable(ready->text()));
  QVERIFY(!run->isEnabled());
  QVERIFY(run->toolTip().contains(QStringLiteral("层位")));
  run->click();
  QCOMPARE(runSpy.count(), 0);

  page.setHorizons({QStringLiteral("T1")});
  QVERIFY2(ready->text().contains(QStringLiteral("算法")), qPrintable(ready->text()));
  QVERIFY(!run->isEnabled());

  page.setAlgorithms({QStringLiteral("paleo:paleo_geological_smoothing")});
  QVERIFY2(ready->text().contains(QStringLiteral("可运行")) &&
               !ready->text().contains(QStringLiteral("不可运行")),
           qPrintable(ready->text()));
  QVERIFY(ready->text().contains(QStringLiteral("T1")));
  QVERIFY(run->isEnabled());
  QVERIFY(jump->isHidden());

  // 测井相：主入口明确转去测井相面板（不在本页假装可跑），提供跳转。
  QSignalSpy jumpSpy(&page, &PredictPage::wellFaciesPanelRequested);
  page.findChild<QComboBox *>(QStringLiteral("predictTypeCombo"))->setCurrentIndex(2);
  QVERIFY(!run->isEnabled());
  QVERIFY(!run->toolTip().isEmpty());
  QVERIFY(!jump->isHidden());
  QVERIFY2(ready->text().contains(QStringLiteral("测井相")), qPrintable(ready->text()));
  jump->click();
  QCOMPARE(jumpSpy.count(), 1);

  // 回到沉积相恢复可运行；忙碌期间禁用，结束后按准备结果恢复。
  page.findChild<QComboBox *>(QStringLiteral("predictTypeCombo"))->setCurrentIndex(0);
  QVERIFY(run->isEnabled());
  page.setRunBusy(true);
  QVERIFY(!run->isEnabled());
  page.setRunBusy(false);
  QVERIFY(run->isEnabled());
  page.setHorizons({});
  QVERIFY(!run->isEnabled());
}

void PredictPageTests::onnxRawTensorInputIsCollapsedAdvanced()
{
  PredictPage page(nullptr, nullptr);
  page.setHorizons({QStringLiteral("T1")});
  page.setAlgorithms({QStringLiteral("onnx:toy")});
  auto *toggle = page.findChild<QToolButton *>(QStringLiteral("onnxAdvancedToggle"));
  auto *body = page.findChild<QWidget *>(QStringLiteral("onnxAdvancedBody"));
  auto *input = page.findChild<QLineEdit *>(QStringLiteral("onnxInputEdit"));
  auto *ready = page.findChild<QLabel *>(QStringLiteral("predictReadinessLabel"));
  auto *status = page.findChild<QLabel *>(QStringLiteral("statusLabel"));
  QVERIFY(toggle && body && input && ready && status);
  // 默认折叠：普通用户不面对张量名/形状/原始浮点数组。
  QVERIFY(body->isHidden());
  QVERIFY(toggle->text().contains(QStringLiteral("高级")));
  QVERIFY(body->isAncestorOf(input));
  // 通用 ONNX 无地质输入映射：如实说明需在高级/调试中手工给张量，不默认填 {0}。
  QVERIFY2(ready->text().contains(QStringLiteral("输入映射")), qPrintable(ready->text()));
  QVERIFY(input->text().isEmpty());

  QSignalSpy spy(&page, &PredictPage::runRequested);
  page.findChild<QPushButton *>(QStringLiteral("runButton"))->click();
  QCOMPARE(spy.count(), 0);
  QVERIFY2(status->text().contains(QStringLiteral("高级")), qPrintable(status->text()));
  QVERIFY(!body->isHidden()); // 校验失败自动展开到需要填写的位置

  // review L1：输入名称不预填、不静默兜底为 "x"；数据与形状齐全但名称为空仍拒绝。
  auto *shape = page.findChild<QLineEdit *>(QStringLiteral("onnxShapeEdit"));
  auto *name = page.findChild<QLineEdit *>(QStringLiteral("onnxInputNameEdit"));
  QVERIFY(shape && name);
  QVERIFY(name->text().isEmpty());
  QVERIFY(!name->placeholderText().isEmpty());
  input->setText(QStringLiteral("1.0"));
  shape->setText(QStringLiteral("1"));
  toggle->click(); // 先折叠，验证拒绝时会重新展开
  QVERIFY(body->isHidden());
  page.findChild<QPushButton *>(QStringLiteral("runButton"))->click();
  QCOMPARE(spy.count(), 0);
  QVERIFY2(status->text().contains(QStringLiteral("名称")), qPrintable(status->text()));
  QVERIFY(!body->isHidden());
  name->setText(QStringLiteral("x"));
  page.findChild<QPushButton *>(QStringLiteral("runButton"))->click();
  QCOMPARE(spy.count(), 1);
  QCOMPARE(spy.first().at(2).toMap().value(QStringLiteral("inputName")).toString(), QStringLiteral("x"));

  toggle->click(); // 折叠
  QVERIFY(body->isHidden());
  toggle->click(); // 展开
  QVERIFY(!body->isHidden());
}

void PredictPageTests::historyListFiltersAndShows()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  LayerManifest manifest(dir.filePath(QStringLiteral("project.sqlite")));
  QVERIFY(manifest.open());
  QgisLayerService layers(nullptr, &manifest);

  QString err;
  QVERIFY(layers.declare(makeDecl(QStringLiteral("predict.T1.run1"), QStringLiteral("T1"),
                                  QStringLiteral("01_Prediction")), &err));
  QVERIFY(layers.declare(makeDecl(QStringLiteral("pred.T1.onnx.m"), QStringLiteral("T1"),
                                  QStringLiteral("03_Predict")), &err));
  QVERIFY(layers.declare(makeDecl(QStringLiteral("confidence.T1"), QStringLiteral("T1"),
                                  QStringLiteral("02_Prediction")), &err));
  QVERIFY(layers.declare(makeDecl(QStringLiteral("factor.T1.idw"), QStringLiteral("T1"),
                                  QStringLiteral("04_SingleFactor")), &err)); // 不入清单
  QVERIFY(layers.declare(makeDecl(QStringLiteral("predict.T2.other"), QStringLiteral("T2"),
                                  QStringLiteral("01_Prediction")), &err)); // 他层位

  PredictPage page(nullptr, &layers);
  page.setHorizons({QStringLiteral("T1"), QStringLiteral("T2")});
  auto *history = page.findChild<QListWidget *>(QStringLiteral("historyList"));
  auto *horizons = page.findChild<QComboBox *>(QStringLiteral("horizonCombo"));
  QVERIFY(history && horizons);

  // manifest 按 layer_id 排序：confidence.* < pred.* < predict.*。
  QCOMPARE(history->count(), 3);
  QCOMPARE(history->item(0)->data(Qt::UserRole).toString(), QStringLiteral("confidence.T1"));
  QCOMPARE(history->item(1)->data(Qt::UserRole).toString(), QStringLiteral("pred.T1.onnx.m"));
  QCOMPARE(history->item(2)->data(Qt::UserRole).toString(), QStringLiteral("predict.T1.run1"));
  QCOMPARE(history->item(2)->text(), QStringLiteral("predict.T1.run1")); // 无 title → layerId

  // 行内「显示」按钮 → showResultRequested(layerId)。
  QSignalSpy showSpy(&page, &PredictPage::showResultRequested);
  const auto buttons = page.findChildren<QPushButton *>(QStringLiteral("historyShowButton"));
  QCOMPARE(buttons.size(), 3);
  buttons.at(2)->click(); // 第三行 = predict.T1.run1
  QCOMPARE(showSpy.count(), 1);
  QCOMPARE(showSpy.first().at(0).toString(), QStringLiteral("predict.T1.run1"));

  // 双击行同样触发。
  emit history->itemActivated(history->item(0));
  QCOMPARE(showSpy.count(), 2);
  QCOMPARE(showSpy.at(1).at(0).toString(), QStringLiteral("confidence.T1"));

  // 换层位 → 只剩该层位声明。
  horizons->setCurrentIndex(1);
  QCOMPARE(history->count(), 1);
  QCOMPARE(history->item(0)->data(Qt::UserRole).toString(), QStringLiteral("predict.T2.other"));

  // 声明落地（layerDeclared）→ 清单即时刷新（回到 T1 后新结果出现）。
  horizons->setCurrentIndex(0);
  QCOMPARE(history->count(), 3);
  QVERIFY(layers.declare(makeDecl(QStringLiteral("predict.T1.run2"), QStringLiteral("T1"),
                                  QStringLiteral("01_Prediction")), &err));
  QCOMPARE(history->count(), 4);
}

// ---- 任务 B（单因素图页）：词表/双区/信号/互斥/折叠区（平面栈）。------
// 真实栈（生成链 declare/样式落盘/等值线）在 tst_factorworkflow.cpp。
class FactorPageTests : public QObject
{
  Q_OBJECT
  private slots:
    void constructsWithNullServices(); // 基座冒烟：拆分后类仍可构造
    void registryVocabularyComplete();
    void dualZoneWidgetsAndLegacyNames();
    void generateFactorPayload();
    void factorCheckIsExclusive();
    void strathickEngineRowsAndPayload();
    void visibilitySignalOnCheckTransitions();
    void contourRowGatingAndSignal();
    void thicknessSamplesInsideCollapsibleSection();
    void typedDrawEntries();
    void nativeMethodContourAndBusyStates();
    void wellFactorExtractionPayload();
    void geologicalAutoModeAndTaskRecommendation();
    void horizonAndFactorSwitchDoNotLeakState();
    void constraintDoubleClickRequestsLocate();
    void methodParameterVisibilityAndFamilies();
    void guidedPreparationAndPinnedActions();
    void ipoStagesAndProgressiveDisclosure();
    void oneClickExtractThenGenerateStopsWithReason();
    void fourStepFlowKeepsAcquisitionVisibleAndLinesOptional();
    void fallbackToolsGroupedUnderMoreTools();
    void strategyPackDrivesMethodCombo(); // 方向67：词表驱动下拉 + 参数预览 + strategy_id 载荷
    void cokrigingCovariateControls();    // 方向91：协变量下拉只在协克里金可见，并写入参数
    void contourIntervalResetsAcrossFactorTypes(); // 方向04 D9：等值距自动态不跨因素类型残留
    void contourIntervalDefaultsToSixthOfFactorRange(); // 有井点值时默认约为极差的 1/6
    void contourIntervalUserValueSurvivesHorizonSwitch(); // #676：换层位保留手改等值距
    void parseLevelsDroppedTokensAreRefused();     // 方向04 D10：脏级别 token 不静默吞
    void staleSuffixSurvivesLayerRedeclare();      // 方向04 D11：「（旧输入）」标记不被重声明抹掉
    void structuralBoundaryComboPrefersPolygonDefaults(); // 成图边界只列面图层；默认值不盲选第 0 个矢量层
    void factorResultsTableShowsLineageColumn();   // P4：成果表第 4 列「来源与质控」+ 明细 tooltip
    void interpretiveResultsShowInFactorResultsTable(); // U7：制图工作场进成果表 + 解释性等值线回执即时刷新
    void wellFactorFieldFetchesAreCached();        // U10：字段清单按（层位,数据世代）缓存，勾选不重取
    void wellFactorCacheInvalidatesOnLayerFileEdit(); // U10/S5：wells 图层文件就地编辑后缓存失效
    void parseLevelsAboveCapCountsDropped();       // U9：固定级别数上限——超限 token 按 dropped 计数（UI 层）
};

void FactorPageTests::constructsWithNullServices()
{
  ConstraintPage page(nullptr);
  QVERIFY(page.findChild<QTableWidget *>(QStringLiteral("thicknessTable")));
}

void FactorPageTests::registryVocabularyComplete()
{
  const QVector<SingleFactorDefinition> defs = SingleFactorRegistry::builtins();
  QCOMPARE(defs.size(), 7);
  // §10 固定顺序 + 稳定 id。
  const QStringList expectedIds = { QStringLiteral( "sandthick" ),
                                    QStringLiteral( "sandratio" ),
                                    QStringLiteral( "strathick" ),
                                    QStringLiteral( "poro" ),
                                    QStringLiteral( "perm" ),
                                    QStringLiteral( "welldist" ),
                                    QStringLiteral( "confidence" ) };
  for ( int i = 0; i < defs.size(); ++i )
  {
    QCOMPARE( defs.at( i ).factorId, expectedIds.at( i ) );
    QVERIFY2( !defs.at( i ).title.isEmpty(), "title must be set" );
    bool hasHan = false; // 标题须含汉字（中显示名）
    for ( const QChar &c : defs.at( i ).title )
    {
      if ( c.unicode() >= 0x4E00 && c.unicode() <= 0x9FA5 )
      {
        hasHan = true;
        break;
      }
    }
    QVERIFY2( hasHan,
              qPrintable( QStringLiteral( "title should be Chinese: %1" ).arg( defs.at( i ).title ) ) );
    QVERIFY2( !defs.at( i ).inputAssetType.isEmpty(), "inputAssetType must be set" );
    QVERIFY2( !defs.at( i ).algorithm.isEmpty(), "algorithm label must be set" );
    QVERIFY2( !defs.at( i ).processingAlgId.isEmpty(), "processingAlgId must be set" );
    QCOMPARE( defs.at( i ).styleRef, QStringLiteral( "factor_%1" ).arg( defs.at( i ).factorId ) );
    QVERIFY( defs.at( i ).defaultParams.contains( QStringLiteral( "field" ) ) );
    QVERIFY( defs.at( i ).defaultParams.contains( QStringLiteral( "cellSize" ) ) );
  }
  QCOMPARE( defs.at( 0 ).title, QStringLiteral( "砂体厚度" ) );
  QCOMPARE( defs.at( 6 ).title, QStringLiteral( "预测置信度" ) );

  // byId 命中/未命中、titleFor 回环。
  bool ok = true;
  const SingleFactorDefinition hit = SingleFactorRegistry::byId( QStringLiteral( "poro" ), &ok );
  QVERIFY( ok );
  QCOMPARE( hit.title, QStringLiteral( "孔隙度" ) );
  SingleFactorRegistry::byId( QStringLiteral( "nope" ), &ok );
  QVERIFY( !ok );
  QCOMPARE( SingleFactorRegistry::titleFor( QStringLiteral( "welldist" ) ),
            QStringLiteral( "距井距离" ) );
  QCOMPARE( SingleFactorRegistry::titleFor( QStringLiteral( "unknown_x" ) ),
            QStringLiteral( "unknown_x" ) );
}

void FactorPageTests::dualZoneWidgetsAndLegacyNames()
{
  ConstraintPage page( nullptr );
  // 双区新件。
  auto *factors = page.findChild<QTableWidget *>(QStringLiteral("factorTable"));
  QVERIFY( factors != nullptr );
  QCOMPARE( factors->rowCount(), 6 ); // Only implemented factors; confidence is a contract.
  QCOMPARE( factors->columnCount(), 4 ); // 勾选/名称/输入/状态
  QCOMPARE( factors->item( 0, 1 )->text(), QStringLiteral( "砂体厚度" ) );
  // #430：输入列改为可读词面（不再暴露 raw inputAssetType）。
  QCOMPARE( factors->item( 0, 2 )->text(), QStringLiteral( "当前地层井点数据" ) );
  QCOMPARE( factors->item( 0, 3 )->text(), QStringLiteral( "未生成" ) );
  auto *field = page.findChild<QLineEdit *>(QStringLiteral("factorFieldEdit"));
  auto *cell = page.findChild<QDoubleSpinBox *>(QStringLiteral("factorCellSizeSpin"));
  auto *generate = page.findChild<QPushButton *>(QStringLiteral("generateFactorButton"));
  auto *interval = page.findChild<QDoubleSpinBox *>(QStringLiteral("contourIntervalSpin"));
  auto *contour = page.findChild<QPushButton *>(QStringLiteral("contourButton"));
  QVERIFY( field && cell && generate && interval && contour );
  QCOMPARE( field->text(), QStringLiteral( "z" ) );
  QCOMPARE( cell->value(), 50.0 );
  QCOMPARE( interval->value(), 20.0 );
  // 禁用态带 reason tooltip（DESIGN.md）。
  QVERIFY( !generate->isEnabled() );
  QVERIFY2( !generate->toolTip().isEmpty(), "disabled generate needs a reason tooltip" );
  QVERIFY( !contour->isEnabled() );
  QVERIFY2( !contour->toolTip().isEmpty(), "disabled contour needs a reason tooltip" );

  // 既有 objectName 全保留（契约：不删不改）。
  for ( const char *name : { "horizonCombo", "constraintList", "faciesCodeSpin",
                             "shapeCombo", "drawButton", "idwField", "idwCellSize",
                             "runIdwButton", "statusLabel", "thicknessCaption",
                             "thicknessTable", "thicknessHint" } )
  {
    QVERIFY2( page.findChild<QWidget *>( QString::fromLatin1( name ) ) != nullptr,
              qPrintable( QStringLiteral( "missing legacy objectName %1" ).arg( name ) ) );
  }
}

void FactorPageTests::strathickEngineRowsAndPayload()
{
  // 引擎分级：strathick → isopach；welldist/confidence → 冻结契约 id。
  bool ok = false;
  const SingleFactorDefinition strathick =
      SingleFactorRegistry::byId( QStringLiteral( "strathick" ), &ok );
  QVERIFY( ok );
  QCOMPARE( strathick.processingAlgId, QStringLiteral( "paleo:paleo_isopach" ) );
  const SingleFactorDefinition welldist =
      SingleFactorRegistry::byId( QStringLiteral( "welldist" ), &ok );
  QVERIFY( ok );
  QCOMPARE( welldist.processingAlgId, SingleFactorContracts::welldistEngineId() );
  const SingleFactorDefinition confidence =
      SingleFactorRegistry::byId( QStringLiteral( "confidence" ), &ok );
  QVERIFY( ok );
  QCOMPARE( confidence.processingAlgId, SingleFactorContracts::confidenceEngineId() );

  ConstraintPage page( nullptr );
  auto *horizons = page.findChild<QComboBox *>( QStringLiteral( "horizonCombo" ) );
  auto *factors = page.findChild<QTableWidget *>( QStringLiteral( "factorTable" ) );
  auto *surfaceRow = page.findChild<QWidget *>( QStringLiteral( "factorSurfaceRow" ) );
  horizons->addItem( QStringLiteral( "T1" ) );
  horizons->setCurrentIndex( 0 );
  QVERIFY( surfaceRow != nullptr );
  QVERIFY( !surfaceRow->isVisibleTo( &page ) ); // 无勾选 → 隐藏

  // IDW 引擎（sandthick）勾选 → 顶/底行仍隐藏。
  factors->item( 0, 0 )->setCheckState( Qt::Checked );
  QVERIFY( !surfaceRow->isVisibleTo( &page ) );

  // 地层厚度默认井分层 MD；构造面口径必须显式选择。
  factors->item( 2, 0 )->setCheckState( Qt::Checked );
  auto *source = page.findChild<QComboBox *>(QStringLiteral("factorSourceCombo"));
  QVERIFY(source);
  QCOMPARE(source->currentData().toString(), QStringLiteral("wells"));
  QVERIFY(!surfaceRow->isVisibleTo(&page));
  QVERIFY(page.findChild<QPushButton *>(QStringLiteral("extractWellFactorsButton"))->isEnabled());
  source->setCurrentIndex(source->findData(QStringLiteral("surfaces")));
  QVERIFY( surfaceRow->isVisibleTo( &page ) );
  auto *generate = page.findChild<QPushButton *>( QStringLiteral( "generateFactorButton" ) );
  QVERIFY( !generate->isEnabled() );
  QVERIFY2( generate->toolTip().contains( QStringLiteral( "顶、底构造面" ) ),
            qPrintable( generate->toolTip() ) );

  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  LayerManifest manifest( dir.filePath( QStringLiteral( "sf.sqlite" ) ) );
  QVERIFY( manifest.open() );
  QgisLayerService layers( nullptr, &manifest );
  QString err;
  LayerDeclaration topDecl;
  topDecl.layerId = QStringLiteral( "struct.T1.top" );
  topDecl.horizon = QStringLiteral( "T1" );
  topDecl.type = QStringLiteral( "raster" );
  topDecl.source = QStringLiteral( "/tmp/struct.T1.top.tif" );
  topDecl.group = QStringLiteral( "01_Structure" );
  LayerDeclaration baseDecl = topDecl;
  baseDecl.layerId = QStringLiteral( "struct.T1.base" );
  baseDecl.source = QStringLiteral( "/tmp/struct.T1.base.tif" );
  QVERIFY( layers.declare( topDecl, &err ) );
  QVERIFY( layers.declare( baseDecl, &err ) );
  page.bindLayerService( &layers );
  auto *top = page.findChild<QComboBox *>( QStringLiteral( "factorTopSurfaceCombo" ) );
  auto *base = page.findChild<QComboBox *>( QStringLiteral( "factorBaseSurfaceCombo" ) );
  QVERIFY( top && base );
  const int topIdx = top->findData( QStringLiteral( "struct.T1.top" ) );
  const int baseIdx = base->findData( QStringLiteral( "struct.T1.base" ) );
  QVERIFY( topIdx >= 0 && baseIdx >= 0 );
  top->setCurrentIndex( topIdx );
  base->setCurrentIndex( baseIdx );
  page.setRunBusy( false ); // 触发 updateFactorActionStates
  QVERIFY( generate->isEnabled() );

  QSignalSpy spy( &page, &ConstraintPage::generateFactorRequested );
  generate->click();
  QCOMPARE( spy.count(), 1 );
  const QVariantMap params = spy.at( 0 ).at( 2 ).toMap();
  QCOMPARE( params.value( QStringLiteral( "topLayerId" ) ).toString(),
            QStringLiteral( "struct.T1.top" ) );
  QCOMPARE( params.value( QStringLiteral( "baseLayerId" ) ).toString(),
            QStringLiteral( "struct.T1.base" ) );
  QVERIFY( !params.contains( QStringLiteral( "method" ) ) );
}

void FactorPageTests::factorResultsTableShowsLineageColumn()
{
  QTemporaryDir dir;
  DataCatalog catalog;
  QString err;
  QVERIFY2( catalog.open( dir.path(), &err ), qPrintable( err ) );
  CatalogAsset asset;
  asset.id = QStringLiteral( "ast-f" );
  asset.type = QStringLiteral( "single_factor" );
  asset.format = QStringLiteral( "tif" );
  asset.displayName = QStringLiteral( "f.tif" );
  QVERIFY( catalog.addAsset( asset ) );
  CatalogVersion v;
  v.id = QStringLiteral( "ver-f-1" );
  v.assetId = asset.id;
  v.stage = QStringLiteral( "DERIVED" );
  v.path = QStringLiteral( "DERIVED/ast-f/ver-f-1/f.tif" );
  v.fileName = QStringLiteral( "f.tif" );
  v.extra = { { "kind", "single_factor_raster" },
              { "horizon", "T1" },
              { "factor_id", "sandthick" },
              { "method", "kriging" },
              { "constraints_consumed", QVariantList{ QVariantMap{ { "id", "b1" }, { "role", "hard_barrier" } } } },
              { "constraints_unused", QVariantList{ QVariantMap{ { "id", "b2" }, { "reason", "outside_compute_domain" } } } } };
  QVERIFY( catalog.addVersion( v, &err ) );

  ConstraintWorkflow wf( nullptr, nullptr );
  wf.setCatalog( &catalog, dir.path() );
  ConstraintPage page( &wf );
  auto *horizons = page.findChild<QComboBox *>( QStringLiteral( "horizonCombo" ) );
  auto *factors = page.findChild<QTableWidget *>( QStringLiteral( "factorTable" ) );
  auto *results = page.findChild<QTableWidget *>( QStringLiteral( "factorResultsTable" ) );
  QVERIFY( horizons && factors && results );
  if ( horizons->findText( QStringLiteral( "T1" ) ) < 0 )
    horizons->addItem( QStringLiteral( "T1" ) );
  horizons->setCurrentText( QStringLiteral( "T1" ) );
  QCOMPARE( factors->item( 0, 1 )->text(), QStringLiteral( "砂体厚度" ) );
  factors->item( 0, 0 )->setCheckState( Qt::Checked );
  page.refreshFactorResults();

  QCOMPARE( results->columnCount(), 4 );
  QCOMPARE( results->horizontalHeaderItem( 3 )->text(), QStringLiteral( "来源与质控" ) );
  QCOMPARE( results->rowCount(), 1 );
  const QVariantMap lineage = ConstraintWorkflow::factorResultLineage( v.extra );
  QCOMPARE( results->item( 0, 3 )->text(), lineage.value( "summary" ).toString() );
  QVERIFY2( results->item( 0, 3 )->text().contains( QStringLiteral( "约束 参与 1 / 未参与 1" ) ),
            qPrintable( results->item( 0, 3 )->text() ) );
  QCOMPARE( results->item( 0, 3 )->toolTip(), lineage.value( "details" ).toString() );
  QVERIFY2( results->item( 0, 3 )->toolTip().contains( QStringLiteral( "b2（outside_compute_domain）" ) ),
            qPrintable( results->item( 0, 3 )->toolTip() ) );
}

// U7：解释性成果可见性——制图工作场（single_factor_cartographic_work）版本
// 进成果表（savedFactorResults 白名单），且 interpretiveContoursGenerated 回执
// 即时刷新表格（此前只 setText，新版本要等手动刷新才出现）。
void FactorPageTests::interpretiveResultsShowInFactorResultsTable()
{
  QTemporaryDir dir;
  DataCatalog catalog;
  QString err;
  QVERIFY2( catalog.open( dir.path(), &err ), qPrintable( err ) );
  const auto addVersion = [&]( const QString &id, const QString &kind, int number ) {
    CatalogAsset asset;
    asset.id = QStringLiteral( "ast-" ) + id;
    asset.type = QStringLiteral( "single_factor" );
    asset.format = QStringLiteral( "gpkg" );
    asset.displayName = id;
    QVERIFY( catalog.addAsset( asset ) );
    CatalogVersion v;
    v.id = QStringLiteral( "ver-" ) + id;
    v.assetId = asset.id;
    v.stage = QStringLiteral( "DERIVED" );
    v.path = QStringLiteral( "DERIVED/%1/%2/%3.gpkg" ).arg( asset.id, v.id, id );
    v.fileName = id + QStringLiteral( ".gpkg" );
    v.versionNumber = number;
    v.extra = { { "kind", kind },
                { "horizon", "T1" },
                { "factor_id", "sandthick" },
                { "interval", 20.0 } };
    QVERIFY2( catalog.addVersion( v, &err ), qPrintable( err ) );
  };
  addVersion( QStringLiteral( "work1" ), QStringLiteral( "single_factor_cartographic_work" ), 1 );

  ConstraintWorkflow wf( nullptr, nullptr );
  wf.setCatalog( &catalog, dir.path() );
  ConstraintPage page( &wf );
  auto *horizons = page.findChild<QComboBox *>( QStringLiteral( "horizonCombo" ) );
  auto *factors = page.findChild<QTableWidget *>( QStringLiteral( "factorTable" ) );
  auto *results = page.findChild<QTableWidget *>( QStringLiteral( "factorResultsTable" ) );
  QVERIFY( horizons && factors && results );
  horizons->addItem( QStringLiteral( "T1" ) );
  horizons->setCurrentIndex( 0 );
  factors->item( 0, 0 )->setCheckState( Qt::Checked ); // 砂体厚度
  page.refreshFactorResults();
  QCOMPARE( results->rowCount(), 1 );
  QCOMPARE( results->item( 0, 0 )->text(), QStringLiteral( "制图工作场" ) );

  // 生成回执到达前表格不知道新版本；回执到达即刷新（无需手动刷新）。
  addVersion( QStringLiteral( "interp1" ), QStringLiteral( "single_factor_cartographic_contour" ), 2 );
  QCOMPARE( results->rowCount(), 1 );
  emit wf.interpretiveContoursGenerated( QStringLiteral( "T1" ), QStringLiteral( "factor.T1.sandthick" ),
                                         QStringLiteral( "cartographic.T1.sandthick.contours" ) );
  QCOMPARE( results->rowCount(), 2 );
  QCOMPARE( results->item( 0, 0 )->text(), QStringLiteral( "解释性等值线" ) ); // 版本号大者在前
  QCOMPARE( results->item( 1, 0 )->text(), QStringLiteral( "制图工作场" ) );
}

// U10：井点字段清单只依赖（层位, 数据世代），因子勾选切换不得重取——
// paleo.page.fieldrefreshcount 只计真实 wf->wellFactorFields 取数次数。
void FactorPageTests::wellFactorFieldFetchesAreCached()
{
  QTemporaryDir dir;
  DataCatalog catalog;
  QString err;
  QVERIFY2( catalog.open( dir.path(), &err ), qPrintable( err ) );
  ConstraintWorkflow wf( nullptr, nullptr );
  wf.setCatalog( &catalog, dir.path() );
  ConstraintPage page( &wf );
  auto *factors = page.findChild<QTableWidget *>( QStringLiteral( "factorTable" ) );
  QVERIFY( factors );
  QCOMPARE( page.property( "paleo.page.fieldrefreshcount" ).toInt(), 1 ); // ctor 取数一次

  // 勾选切换 N 次：字段清单不变 → 缓存命中，计数仍为 1。
  for ( int i = 0; i < 3; ++i )
  {
    factors->item( 0, 0 )->setCheckState( Qt::Checked );
    factors->item( 0, 0 )->setCheckState( Qt::Unchecked );
  }
  QCOMPARE( page.property( "paleo.page.fieldrefreshcount" ).toInt(), 1 );

  // 数据世代变化（wellAttributesChanged）→ 下一次刷新重取（计数 2），
  // 其后的勾选切换再次命中缓存。
  emit wf.wellAttributesChanged();
  QCOMPARE( page.property( "paleo.page.fieldrefreshcount" ).toInt(), 2 );
  factors->item( 1, 0 )->setCheckState( Qt::Checked );
  QCOMPARE( page.property( "paleo.page.fieldrefreshcount" ).toInt(), 2 );
}

// U10/S5-Med：缓存键须含 wells 声明 source 的 size+mtime——既有 wells 图层被
// 就地编辑（属性表加数值列/填数值后保存）不换声明、不发 wellAttributesChanged，
// 靠文件态变化失效缓存；否则字段下拉陈旧且 showEvent 刷新不能自愈。
void FactorPageTests::wellFactorCacheInvalidatesOnLayerFileEdit()
{
  QTemporaryDir dir;
  DataCatalog catalog;
  QString err;
  QVERIFY2( catalog.open( dir.path(), &err ), qPrintable( err ) );
  QFile wellsFile( dir.filePath( QStringLiteral( "wells.T1.geojson" ) ) );
  QVERIFY( wellsFile.open( QIODevice::WriteOnly ) );
  wellsFile.write( "{\"type\":\"FeatureCollection\",\"features\":[]}" );
  wellsFile.close();

  LayerManifest manifest( dir.filePath( QStringLiteral( "wfcache.sqlite" ) ) );
  QVERIFY( manifest.open() );
  QgisLayerService layers( nullptr, &manifest );
  LayerDeclaration decl;
  decl.layerId = QStringLiteral( "wells.T1" );
  decl.horizon = QStringLiteral( "T1" );
  decl.type = QStringLiteral( "vector" );
  decl.source = wellsFile.fileName();
  decl.group = QStringLiteral( "01_Wells" );
  QVERIFY( layers.declare( decl, &err ) );

  ConstraintWorkflow wf( nullptr, nullptr );
  wf.setCatalog( &catalog, dir.path() );
  ConstraintPage page( &wf );
  page.bindLayerService( &layers );
  auto *factors = page.findChild<QTableWidget *>( QStringLiteral( "factorTable" ) );
  QVERIFY( factors );
  // ctor 取数发生在 bindLayerService 之前——绑定后首次刷新必然 miss（键加入
  // wells 声明与文件态），计一次后进入稳态。
  factors->item( 0, 0 )->setCheckState( Qt::Unchecked );
  factors->item( 0, 0 )->setCheckState( Qt::Checked );
  const int primed = page.property( "paleo.page.fieldrefreshcount" ).toInt();
  factors->item( 0, 0 )->setCheckState( Qt::Unchecked );
  QCOMPARE( page.property( "paleo.page.fieldrefreshcount" ).toInt(), primed ); // 声明未变，命中缓存

  // wells 图层文件被就地编辑（size/mtime 变化、声明不变、无信号）→ 缓存失效。
  QVERIFY( wellsFile.open( QIODevice::Append ) );
  wellsFile.write( " " );
  wellsFile.close();
  factors->item( 0, 0 )->setCheckState( Qt::Checked );
  QCOMPARE( page.property( "paleo.page.fieldrefreshcount" ).toInt(), primed + 1 );
}

void FactorPageTests::generateFactorPayload()
{
  ConstraintPage page( nullptr );
  auto *horizons = page.findChild<QComboBox *>(QStringLiteral("horizonCombo"));
  auto *factors = page.findChild<QTableWidget *>(QStringLiteral("factorTable"));
  auto *field = page.findChild<QLineEdit *>(QStringLiteral("factorFieldEdit"));
  auto *cell = page.findChild<QDoubleSpinBox *>(QStringLiteral("factorCellSizeSpin"));
  horizons->addItem( QStringLiteral( "T1" ) );
  horizons->setCurrentIndex( 0 );
  field->setText( QStringLiteral( "sand_thick" ) );
  cell->setValue( 2.5 );

  factors->item(0, 0)->setCheckState(Qt::Unchecked);
  // 未勾选 → 生成按钮禁用，点击无信号。
  QSignalSpy spy( &page, &ConstraintPage::generateFactorRequested );
  auto *generate = page.findChild<QPushButton *>(QStringLiteral("generateFactorButton"));
  QVERIFY( !generate->isEnabled() );
  factors->item( 0, 0 )->setCheckState( Qt::Checked );
  QVERIFY( generate->isEnabled() );

  generate->click();
  QCOMPARE( spy.count(), 1 );
  QCOMPARE( spy.at( 0 ).at( 0 ).toString(), QStringLiteral( "sandthick" ) );
  QCOMPARE( spy.at( 0 ).at( 1 ).toString(), QStringLiteral( "T1" ) );
  const QVariantMap params = spy.at( 0 ).at( 2 ).toMap();
  QCOMPARE( params.value( QStringLiteral( "field" ) ).toString(), QStringLiteral( "sand_thick" ) );
  QCOMPARE( params.value( QStringLiteral( "cellSize" ) ).toDouble(), 2.5 );
}

void FactorPageTests::factorCheckIsExclusive()
{
  ConstraintPage page( nullptr );
  auto *factors = page.findChild<QTableWidget *>(QStringLiteral("factorTable"));
  factors->item( 0, 0 )->setCheckState( Qt::Checked ); // 砂体厚度
  QCOMPARE( factors->item( 0, 0 )->checkState(), Qt::Checked );
  factors->item( 2, 0 )->setCheckState( Qt::Checked ); // 地层厚度
  QCOMPARE( factors->item( 2, 0 )->checkState(), Qt::Checked );
  QCOMPARE( factors->item( 0, 0 )->checkState(), Qt::Unchecked ); // 互斥：A 自动取消
  int checked = 0;
  for ( int r = 0; r < factors->rowCount(); ++r )
    if ( factors->item( r, 0 )->checkState() == Qt::Checked )
      ++checked;
  QCOMPARE( checked, 1 );
}

void FactorPageTests::visibilitySignalOnCheckTransitions()
{
  ConstraintPage page( nullptr );
  // #430 noteFactorLayer 要求 layerId 与当前层位一致，否则直接 return。
  page.setHorizons( { QStringLiteral( "T1" ) } );
  auto *factors = page.findChild<QTableWidget *>(QStringLiteral("factorTable"));
  QSignalSpy spy( &page, &ConstraintPage::factorVisibilityRequested );

  // 未生成因素的勾选不带上图意图（无 layerId 可指）。
  factors->item( 0, 0 )->setCheckState( Qt::Checked );
  QCOMPARE( spy.count(), 0 );

  page.noteFactorLayer( QStringLiteral( "sandthick" ), QStringLiteral( "factor.T1.sandthick" ) );
  // 勾选行刚好生成 → 直接上图。
  QCOMPARE( spy.count(), 1 );
  QCOMPARE( spy.at( 0 ).at( 0 ).toString(), QStringLiteral( "factor.T1.sandthick" ) );
  QCOMPARE( spy.at( 0 ).at( 1 ).toBool(), true );

  page.noteFactorLayer( QStringLiteral( "strathick" ), QStringLiteral( "factor.T1.strathick" ) );
  factors->item( 2, 0 )->setCheckState( Qt::Checked ); // 换看地层厚度
  QCOMPARE( spy.count(), 3 ); // 旧层 false + 新层 true
  QCOMPARE( spy.at( 1 ).at( 0 ).toString(), QStringLiteral( "factor.T1.sandthick" ) );
  QCOMPARE( spy.at( 1 ).at( 1 ).toBool(), false );
  QCOMPARE( spy.at( 2 ).at( 0 ).toString(), QStringLiteral( "factor.T1.strathick" ) );
  QCOMPARE( spy.at( 2 ).at( 1 ).toBool(), true );

  // 手动取消勾选 → 该层下图。
  factors->item( 2, 0 )->setCheckState( Qt::Unchecked );
  QCOMPARE( spy.count(), 4 );
  QCOMPARE( spy.at( 3 ).at( 0 ).toString(), QStringLiteral( "factor.T1.strathick" ) );
  QCOMPARE( spy.at( 3 ).at( 1 ).toBool(), false );
}

void FactorPageTests::contourRowGatingAndSignal()
{
  ConstraintPage page( nullptr );
  // #430 noteFactorLayer 要求 layerId 与当前层位一致，否则直接 return。
  page.setHorizons( { QStringLiteral( "T1" ) } );
  auto *factors = page.findChild<QTableWidget *>(QStringLiteral("factorTable"));
  auto *interval = page.findChild<QDoubleSpinBox *>(QStringLiteral("contourIntervalSpin"));
  auto *contour = page.findChild<QPushButton *>(QStringLiteral("contourButton"));
  QVERIFY( !contour->isEnabled() );

  // 勾选但未生成 → 仍禁用，tooltip 说明原因。
  factors->item( 0, 0 )->setCheckState( Qt::Checked );
  QVERIFY( !contour->isEnabled() );
  // #430：未生成时的原因改成步骤 3 文案（不再含「生成」字面）。
  QVERIFY2( contour->toolTip().contains( QStringLiteral( "等值面" ) ),
            qPrintable( contour->toolTip() ) );

  page.noteFactorLayer( QStringLiteral( "sandthick" ), QStringLiteral( "factor.T1.sandthick" ) );
  QVERIFY( contour->isEnabled() );
  interval->setValue( 15.0 );
  QSignalSpy spy( &page, &ConstraintPage::contourRequested );
  contour->click();
  QCOMPARE( spy.count(), 1 );
  QCOMPARE( spy.at( 0 ).at( 0 ).toString(), QStringLiteral( "factor.T1.sandthick" ) );
  QCOMPARE( spy.at( 0 ).at( 1 ).toDouble(), 15.0 );

  // #430：状态列回执改为「等值面已保存」（layerId 在 tooltip）。
  QCOMPARE( factors->item( 0, 3 )->text(), QStringLiteral( "等值面已保存" ) );
  QCOMPARE( factors->item( 0, 3 )->toolTip(), QStringLiteral( "factor.T1.sandthick" ) );
}

void FactorPageTests::thicknessSamplesInsideCollapsibleSection()
{
  ConstraintPage page( nullptr );
  // 补充诊断默认收起；展开后原有内容仍可用。
  auto *section = page.findChild<QWidget *>(QStringLiteral("thicknessSection"));
  auto *table = page.findChild<QTableWidget *>(QStringLiteral("thicknessTable"));
  auto *caption = page.findChild<QLabel *>(QStringLiteral("thicknessCaption"));
  auto *hint = page.findChild<QLabel *>(QStringLiteral("thicknessHint"));
  QVERIFY( section && table && caption && hint );
  QVERIFY( !table->isVisibleTo( section ) );
  // 厚度表确实在折叠区容器内（挪进了 CollapsibleSection）。
  QVERIFY( table->parentWidget() != &page );
  QCOMPARE( table->columnCount(), 4 );
  QCOMPARE( caption->text().contains( QStringLiteral( "厚度样本" ) ), true );

  // 折叠后仍能 findChild（objectName 不因折叠失效），且表隐藏。
  auto *toggle = section->findChild<QAbstractButton *>(QStringLiteral("collapsibleSectionToggle"));
  QVERIFY( toggle != nullptr );
  toggle->setChecked(true);
  QVERIFY(table->isVisibleTo(section));
  toggle->setChecked( false );
  QVERIFY( !table->isVisibleTo( section ) );
  QVERIFY( page.findChild<QTableWidget *>(QStringLiteral("thicknessTable")) != nullptr );
}

void FactorPageTests::typedDrawEntries()
{
  ConstraintPage page( nullptr );
  auto *horizons = page.findChild<QComboBox *>(QStringLiteral("horizonCombo"));
  auto *spin = page.findChild<QSpinBox *>(QStringLiteral("faciesCodeSpin"));
  horizons->addItem( QStringLiteral( "D61" ) );
  horizons->setCurrentIndex( 0 );
  spin->setValue( 7 );

  QSignalSpy spy( &page, &ConstraintPage::drawTypedConstraintRequested );
  auto *direction = page.findChild<QPushButton *>(QStringLiteral("directionButton"));
  auto *breakLine = page.findChild<QPushButton *>(QStringLiteral("breakLineButton"));
  QVERIFY( direction && breakLine );

  direction->click();
  breakLine->click();
  QCOMPARE( spy.count(), 2 );
  QCOMPARE( spy.at( 0 ).at( 0 ).toString(), QStringLiteral( "D61" ) );
  QCOMPARE( spy.at( 0 ).at( 1 ).toString(), QStringLiteral( "line" ) );
  QCOMPARE( spy.at( 0 ).at( 2 ).toString(), QStringLiteral( "direction_line" ) );
  QCOMPARE( spy.at( 0 ).at( 3 ).toInt(), 7 );
  QCOMPARE( spy.at( 1 ).at( 1 ).toString(), QStringLiteral( "line" ) );
  QCOMPARE( spy.at( 1 ).at( 2 ).toString(), QStringLiteral( "break_line" ) );

  // 方向23：其余三语义入口 + 已绘约束线编辑面（顶点编辑/删除）就位。
  auto *soft = page.findChild<QPushButton *>(QStringLiteral("softBoundaryButton"));
  auto *stop = page.findChild<QPushButton *>(QStringLiteral("contourStopButton"));
  auto *detour = page.findChild<QPushButton *>(QStringLiteral("cartographicDetourButton"));
  QVERIFY( soft && stop && detour );
  soft->click();
  stop->click();
  detour->click();
  QCOMPARE( spy.count(), 5 );
  QCOMPARE( spy.at( 2 ).at( 2 ).toString(), QStringLiteral( "interpretive_boundary" ) );
  QCOMPARE( spy.at( 3 ).at( 2 ).toString(), QStringLiteral( "contour_stop" ) );
  QCOMPARE( spy.at( 4 ).at( 2 ).toString(), QStringLiteral( "cartographic_detour" ) );

  auto *vertexEdit = page.findChild<QPushButton *>(QStringLiteral("constraintVertexEditButton"));
  auto *removeButton = page.findChild<QPushButton *>(QStringLiteral("constraintDeleteButton"));
  QVERIFY( vertexEdit && removeButton );
  QVERIFY( !removeButton->isEnabled() ); // 未选约束行时删除禁用
  QSignalSpy editSpy( &page, &ConstraintPage::editConstraintVerticesRequested );
  vertexEdit->click();
  QCOMPARE( editSpy.count(), 1 );
  QCOMPARE( editSpy.at( 0 ).at( 0 ).toString(), QStringLiteral( "D61" ) );

  // 旧绘制链共存（drawButton → drawConstraintRequested，tst_panels 详测，此处冒烟）。
  QSignalSpy legacy( &page, &ConstraintPage::drawConstraintRequested );
  page.findChild<QPushButton *>(QStringLiteral("drawButton"))->click();
  QCOMPARE( legacy.count(), 1 );
}

void FactorPageTests::nativeMethodContourAndBusyStates()
{
  ConstraintPage page( nullptr );
  // #430 noteFactorLayer 要求 layerId 与当前层位一致，否则直接 return。
  page.setHorizons( { QStringLiteral( "T1" ) } );
  page.resize( 280, 420 );
  page.show();
  QVERIFY( page.findChild<QScrollArea *>( QStringLiteral( "constraintPageScroll" ) ) != nullptr );
  QVERIFY( page.findChild<QWidget *>( QStringLiteral( "constraintPageBody" ) ) != nullptr );
  auto *method = page.findChild<QComboBox *>( QStringLiteral( "factorMethodCombo" ) );
  auto *coverage = page.findChild<QComboBox *>( QStringLiteral( "factorCoverageCombo" ) );
  auto *power = page.findChild<QDoubleSpinBox *>( QStringLiteral( "factorPowerSpin" ) );
  auto *advanced = page.findChild<QWidget *>( QStringLiteral( "factorAdvancedSection" ) );
  auto *cancel = page.findChild<QPushButton *>( QStringLiteral( "factorCancelButton" ) );
  auto *save = page.findChild<QPushButton *>( QStringLiteral( "constraintParamSaveButton" ) );
  auto *source = page.findChild<QLabel *>( QStringLiteral( "factorContourSourceLabel" ) );
  QVERIFY( method && coverage && power && advanced && cancel && save && source );
  QCOMPARE( method->currentData().toString(), QStringLiteral( "surfer_idw" ) );
  QCOMPARE( coverage->currentData().toString(), QStringLiteral( "well_supported" ) );
  QCOMPARE( power->value(), 2.0 );
  QVERIFY( power->toolTip().contains( QStringLiteral( "0.5" ) ) );
  QCOMPARE( power->font().family(), PaleoTheme::monoFont().family() );
  QVERIFY( power->isVisibleTo( advanced ) );
  QVERIFY( !cancel->isEnabled() );
  QVERIFY( cancel->toolTip().contains( QStringLiteral( "没有正在运行" ) ) );
  QVERIFY( !save->isEnabled() );
  QVERIFY( save->toolTip().contains( QStringLiteral( "选择一条线" ) ) );
  QCOMPARE( source->text(), QStringLiteral( "等值线取自分析场" ) );

  auto *factors = page.findChild<QTableWidget *>( QStringLiteral( "factorTable" ) );
  auto *horizons = page.findChild<QComboBox *>( QStringLiteral( "horizonCombo" ) );
  horizons->addItem( QStringLiteral( "T1" ) );
  horizons->setCurrentIndex( 0 );
  factors->item( 0, 0 )->setCheckState( Qt::Checked );
  QSignalSpy generated( &page, &ConstraintPage::generateFactorRequested );
  page.findChild<QPushButton *>( QStringLiteral( "generateFactorButton" ) )->click();
  QCOMPARE( generated.count(), 1 );
  const QVariantMap local = generated.at( 0 ).at( 2 ).toMap();
  QCOMPARE( local.value( QStringLiteral( "method" ) ).toString(), QStringLiteral( "surfer_idw" ) );
  QCOMPARE( local.value( QStringLiteral( "coverage" ) ).toString(), QStringLiteral( "well_supported" ) );
  QCOMPARE( local.value( QStringLiteral( "power" ) ).toDouble(), 2.0 );
  QCOMPARE( local.value( QStringLiteral( "wellClusterLocality" ) ).toBool(), false );
  method->setCurrentIndex( method->findData( QStringLiteral( "legacy" ) ) );
  page.findChild<QPushButton *>( QStringLiteral( "generateFactorButton" ) )->click();
  QVERIFY( !generated.at( 1 ).at( 2 ).toMap().contains( QStringLiteral( "method" ) ) );

  factors->item( 5, 0 )->setCheckState( Qt::Checked );
  QVERIFY( !method->isVisibleTo( &page ) );
  QCOMPARE(factors->rowCount(), 6); // Confidence engine is unavailable and omitted.
  auto *generate = page.findChild<QPushButton *>( QStringLiteral( "generateFactorButton" ) );
  QVERIFY(generate->isEnabled());

  factors->item( 0, 0 )->setCheckState( Qt::Checked );
  page.noteFactorLayer( QStringLiteral( "sandthick" ), QStringLiteral( "factor.T1.sandthick" ) );
  auto *mode = page.findChild<QComboBox *>( QStringLiteral( "factorContourModeCombo" ) );
  auto *levels = page.findChild<QLineEdit *>( QStringLiteral( "factorContourLevelsEdit" ) );
  auto *contour = page.findChild<QPushButton *>( QStringLiteral( "contourButton" ) );
  mode->setCurrentIndex( mode->findData( QStringLiteral( "cartographic_detour" ) ) );
  QVERIFY( source->text().contains( QStringLiteral( "制图工作场" ) ) );
  QVERIFY( !contour->isEnabled() );
  QVERIFY( contour->toolTip().contains( QStringLiteral( "等值级别" ) ) );
  levels->setText( QStringLiteral( "10, 20, 30" ) );
  QVERIFY( contour->isEnabled() );
  QSignalSpy interpretive( &page, &ConstraintPage::interpretiveContourRequested );
  contour->click();
  QCOMPARE( interpretive.count(), 1 );
  QCOMPARE( interpretive.at( 0 ).at( 0 ).toString(), QStringLiteral( "factor.T1.sandthick" ) );
  QCOMPARE( interpretive.at( 0 ).at( 1 ).value<QVector<double>>(), ( QVector<double>{ 10.0, 20.0, 30.0 } ) );

  page.setRunBusy( true );
  QVERIFY( !generate->isEnabled() );
  QVERIFY( generate->toolTip().contains( QStringLiteral( "可取消" ) ) );
  auto *idw = page.findChild<QPushButton *>( QStringLiteral( "runIdwButton" ) );
  QVERIFY( idw != nullptr );
  QVERIFY( !idw->isEnabled() );
  QVERIFY( idw->toolTip().contains( QStringLiteral( "可取消" ) ) );
  QVERIFY( cancel->isEnabled() );
  QSignalSpy cancelSpy( &page, &ConstraintPage::runCancelRequested );
  cancel->click();
  QCOMPARE( cancelSpy.count(), 1 );
  page.noteRunStage( QStringLiteral( "正在插值" ), 40 );
  QVERIFY( page.findChild<QLabel *>( QStringLiteral( "statusLabel" ) )->text().contains( QStringLiteral( "40%" ) ) );
  page.setRunBusy( false );
  QVERIFY( idw->isEnabled() );

  QSignalSpy drawSpy( &page, &ConstraintPage::drawTypedConstraintRequested );
  page.findChild<QPushButton *>( QStringLiteral( "softBoundaryButton" ) )->click();
  page.findChild<QPushButton *>( QStringLiteral( "contourStopButton" ) )->click();
  page.findChild<QPushButton *>( QStringLiteral( "cartographicDetourButton" ) )->click();
  QCOMPARE( drawSpy.count(), 3 );
  QCOMPARE( drawSpy.at( 0 ).at( 2 ).toString(), QStringLiteral( "interpretive_boundary" ) );
  QCOMPARE( drawSpy.at( 1 ).at( 2 ).toString(), QStringLiteral( "contour_stop" ) );
  QCOMPARE( drawSpy.at( 2 ).at( 2 ).toString(), QStringLiteral( "cartographic_detour" ) );

  PaleoTheme::applyDarkTheme();
  QVERIFY( page.findChild<QComboBox *>( QStringLiteral( "factorMethodCombo" ) ) != nullptr );
  QCOMPARE( PaleoTheme::kColorPrimary, QColor( QStringLiteral( "#1B73D0" ) ) );
  PaleoTheme::applyLightTheme();
  QVERIFY( page.findChild<QPushButton *>( QStringLiteral( "generateFactorButton" ) ) != nullptr );
}

// 方向67 Oracle 5：方法下拉由 singlefactorstrategy 词表驱动——标签/参数预览/
// strategy_id 载荷三面一致；SGS 不在词表内，显示其自身说明。
void FactorPageTests::strategyPackDrivesMethodCombo()
{
  ConstraintPage page( nullptr );
  page.show();
  auto *method = page.findChild<QComboBox *>( QStringLiteral( "factorMethodCombo" ) );
  auto *note = page.findChild<QLabel *>( QStringLiteral( "factorStrategyNote" ) );
  QVERIFY( method && note );
  // 词表策略 + 页面专属 SGS；标签与词表逐项一致（单一真源）。
  QCOMPARE( method->count(), paleo::singlefactor::surfaceMethodPacks().size() + 1 );
  for ( const paleo::singlefactor::SurfaceMethodPack &pack : paleo::singlefactor::surfaceMethodPacks() )
  {
    const QString methodId =
        pack.id == QLatin1String( "idw" ) ? QStringLiteral( "legacy" ) : pack.id;
    const int index = method->findData( methodId );
    QVERIFY2( index >= 0, qPrintable( QStringLiteral( "词表策略 %1 未出现在下拉" ).arg( pack.id ) ) );
    QCOMPARE( method->itemText( index ), pack.label );
  }
  QCOMPARE( method->currentData().toString(), QStringLiteral( "surfer_idw" ) );
  // 默认项参数预览 = 词表 geologicalNote。
  const paleo::singlefactor::SurfaceMethodPack *defaultPack =
      paleo::singlefactor::surfaceMethodPack( QStringLiteral( "surfer_idw" ) );
  QVERIFY( defaultPack );
  QCOMPARE( note->text(), defaultPack->geologicalNote );

  // SGS：不在曲面词表，仍须在主流程明确其模拟语义。
  method->setCurrentIndex( method->findData( QStringLiteral( "sgs" ) ) );
  QVERIFY( note->text().contains( QStringLiteral( "模拟" ) ) );

  // 载荷：legacy（词表 id="idw"）→ strategy_id 写词表 id 且不写 method。
  auto *factors = page.findChild<QTableWidget *>( QStringLiteral( "factorTable" ) );
  auto *horizons = page.findChild<QComboBox *>( QStringLiteral( "horizonCombo" ) );
  horizons->addItem( QStringLiteral( "T1" ) );
  factors->item( 0, 0 )->setCheckState( Qt::Checked );
  method->setCurrentIndex( method->findData( QStringLiteral( "legacy" ) ) );
  QSignalSpy generated( &page, &ConstraintPage::generateFactorRequested );
  page.findChild<QPushButton *>( QStringLiteral( "generateFactorButton" ) )->click();
  QCOMPARE( generated.count(), 1 );
  const QVariantMap legacyParams = generated.at( 0 ).at( 2 ).toMap();
  QVERIFY( !legacyParams.contains( QStringLiteral( "method" ) ) );
  QCOMPARE( legacyParams.value( QStringLiteral( "strategy_id" ) ).toString(), QStringLiteral( "idw" ) );

  // 载荷：local_direction_kriging → strategy_id 同名进参数。
  method->setCurrentIndex( method->findData( QStringLiteral( "local_direction_kriging" ) ) );
  page.findChild<QPushButton *>( QStringLiteral( "generateFactorButton" ) )->click();
  QCOMPARE( generated.at( 1 ).at( 2 ).toMap().value( QStringLiteral( "strategy_id" ) ).toString(),
            QStringLiteral( "local_direction_kriging" ) );
}

void FactorPageTests::cokrigingCovariateControls()
{
  ConstraintPage page( nullptr );
  page.findChild<QWidget *>(QStringLiteral("factorAdvancedOptionsSection"))->findChild<QAbstractButton *>(QStringLiteral("collapsibleSectionToggle"))->setChecked(true);
  page.show();
  auto *method = page.findChild<QComboBox *>( QStringLiteral( "factorMethodCombo" ) );
  auto *covariate = page.findChild<QComboBox *>( QStringLiteral( "factorCovariateCombo" ) );
  auto *rho = page.findChild<QDoubleSpinBox *>( QStringLiteral( "factorCrossCorrelationSpin" ) );
  QVERIFY( method && covariate && rho );
  QVERIFY( !covariate->isVisibleTo( &page ) );
  QVERIFY( !rho->isVisibleTo( &page ) );

  auto *factors = page.findChild<QTableWidget *>( QStringLiteral( "factorTable" ) );
  auto *horizons = page.findChild<QComboBox *>( QStringLiteral( "horizonCombo" ) );
  horizons->addItem( QStringLiteral( "T1" ) );
  factors->item( 0, 0 )->setCheckState( Qt::Checked );
  method->setCurrentIndex( method->findData( QStringLiteral( "cokriging" ) ) );
  QVERIFY( covariate->isVisibleTo( &page ) );
  QVERIFY( covariate->isEnabled() );
  QVERIFY( rho->isVisibleTo( &page ) );
  QCOMPARE( covariate->count(), 1 );
  QCOMPARE( covariate->itemText( 0 ), QStringLiteral( "请选择协变量栅格" ) );
  QCOMPARE( covariate->currentData().toString(), QString() );
  rho->setValue( 0.4 );

  QSignalSpy generated( &page, &ConstraintPage::generateFactorRequested );
  page.findChild<QPushButton *>( QStringLiteral( "generateFactorButton" ) )->click();
  QCOMPARE( generated.count(), 1 );
  const QVariantMap params = generated.at( 0 ).at( 2 ).toMap();
  QCOMPARE( params.value( QStringLiteral( "method" ) ).toString(), QStringLiteral( "cokriging" ) );
  QVERIFY( params.contains( QStringLiteral( "covariateLayerId" ) ) );
  QCOMPARE( params.value( QStringLiteral( "covariateLayerId" ) ).toString(), QString() );
  QCOMPARE( params.value( QStringLiteral( "crossCorrelation" ) ).toDouble(), 0.4 );

  method->setCurrentIndex( method->findData( QStringLiteral( "kriging" ) ) );
  QVERIFY( !covariate->isVisibleTo( &page ) );
  page.findChild<QPushButton *>( QStringLiteral( "generateFactorButton" ) )->click();
  QVERIFY( !generated.at( 1 ).at( 2 ).toMap().contains( QStringLiteral( "covariateLayerId" ) ) );
}

// 方向04 D9：等值距「自动(0)」是 structural 因素专用态；切回非 structural
// 因素必须恢复正间距——interval=0 会被服务层以「间距必须是正数」拒绝，
// 残留会让用户点一个必失败的按钮。
void FactorPageTests::contourIntervalResetsAcrossFactorTypes()
{
  ConstraintPage page( nullptr );
  page.setHorizons( { QStringLiteral( "T1" ) } );
  auto *horizons = page.findChild<QComboBox *>( QStringLiteral( "horizonCombo" ) );
  horizons->setCurrentIndex( 0 );
  auto *factors = page.findChild<QTableWidget *>( QStringLiteral( "factorTable" ) );
  auto *spin = page.findChild<QDoubleSpinBox *>( QStringLiteral( "contourIntervalSpin" ) );
  QVERIFY( factors && spin );

  factors->item( 1, 0 )->setCheckState( Qt::Checked ); // sandratio（非 structural）
  spin->setValue( 0.0 );                               // 模拟 structural 残留的自动态
  factors->item( 2, 0 )->setCheckState( Qt::Checked ); // 切到 strathick（仍非 structural）
  QCOMPARE( spin->value(), 20.0 );                     // 修复前保持 0.0（跨类型残留）
}

// #676：手改等值距后切换层位再切回，间距保持手改值，不被新层位建议覆盖。
void FactorPageTests::contourIntervalUserValueSurvivesHorizonSwitch()
{
  ConstraintPage page( nullptr );
  page.setHorizons( { QStringLiteral( "D61" ), QStringLiteral( "D62" ) } );
  auto *h = page.findChild<QComboBox *>( QStringLiteral( "horizonCombo" ) );
  auto *factors = page.findChild<QTableWidget *>( QStringLiteral( "factorTable" ) );
  auto *spin = page.findChild<QDoubleSpinBox *>( QStringLiteral( "contourIntervalSpin" ) );
  QVERIFY( h && factors && spin );
  h->setCurrentIndex( h->findText( QStringLiteral( "D61" ) ) );
  factors->item( 1, 0 )->setCheckState( Qt::Checked ); // sandratio：空表也有建议值域
  spin->setValue( 5.0 );
  h->setCurrentIndex( h->findText( QStringLiteral( "D62" ) ) );
  QCOMPARE( spin->value(), 5.0 );
  h->setCurrentIndex( h->findText( QStringLiteral( "D61" ) ) );
  QCOMPARE( spin->value(), 5.0 );
}

void FactorPageTests::contourIntervalDefaultsToSixthOfFactorRange()
{
  ConstraintPage page( nullptr );
  page.setHorizons( { QStringLiteral( "T1" ) } );
  page.findChild<QComboBox *>( QStringLiteral( "horizonCombo" ) )->setCurrentIndex( 0 );
  auto *factors = page.findChild<QTableWidget *>( QStringLiteral( "factorTable" ) );
  auto *table = page.findChild<QTableWidget *>( QStringLiteral( "wellFactorTable" ) );
  auto *spin = page.findChild<QDoubleSpinBox *>( QStringLiteral( "contourIntervalSpin" ) );
  QVERIFY( factors && table && spin );
  factors->item( 1, 0 )->setCheckState( Qt::Checked ); // sandratio
  table->setRowCount( 3 );
  table->setItem( 0, 1, new QTableWidgetItem( QStringLiteral( "0.20" ) ) );
  table->setItem( 1, 1, new QTableWidgetItem( QStringLiteral( "0.80" ) ) );
  table->setItem( 2, 1, new QTableWidgetItem( QStringLiteral( "—" ) ) );
  page.applySuggestedContourInterval();
  QCOMPARE( spin->value(), 0.1 ); // (0.80-0.20)/6 = 0.1

  spin->setValue( 0.05 );
  table->item( 1, 1 )->setText( QStringLiteral( "0.90" ) );
  page.applySuggestedContourInterval();
  QCOMPARE( spin->value(), 0.05 ); // 同一因素上用户改过的间距保留

  factors->item( 0, 0 )->setCheckState( Qt::Checked ); // 换因素：#682 起手改值跟层位/因素走
  QCOMPARE( spin->value(), 0.05 );                     // 用户手设间距不被新极差覆写

  // 换因素会 invalidateWellFactors（清空井点值表，待重新提取）——新因素的井点值
  // 须在切换之后填入，否则重算面对空表（旧写法先填后切，断言实际跑在 0 行上）。
  QCOMPARE( table->rowCount(), 0 );
  table->setRowCount( 2 );
  table->setItem( 0, 1, new QTableWidgetItem( QStringLiteral( "10" ) ) );
  table->setItem( 1, 1, new QTableWidgetItem( QStringLiteral( "70" ) ) );
  page.setProperty( "paleo.page.contourIntervalUser", false ); // 未手改时按新极差重算
  page.applySuggestedContourInterval();
  QCOMPARE( spin->value(), 20.0 ); // 厚度建议值域 0–100 m，整刻度间距 20 m。
}

// 方向04 D10：「10, abc, 30」不再静默解析成 [10,30]——按钮提示脏 token，
// 点击被拒绝并说明原因（DESIGN.md：动作失败必须有理由）。
void FactorPageTests::parseLevelsDroppedTokensAreRefused()
{
  ConstraintPage page( nullptr );
  page.setHorizons( { QStringLiteral( "T1" ) } );
  auto *horizons = page.findChild<QComboBox *>( QStringLiteral( "horizonCombo" ) );
  horizons->setCurrentIndex( 0 );
  QVariantMap generated;
  generated.insert( QStringLiteral( "sandthick" ), QStringLiteral( "factor.T1.sandthick" ) );
  page.setProperty( "paleo.page.factorgen", generated );
  auto *factors = page.findChild<QTableWidget *>( QStringLiteral( "factorTable" ) );
  factors->item( 0, 0 )->setCheckState( Qt::Checked ); // poro
  auto *mode = page.findChild<QComboBox *>( QStringLiteral( "factorContourModeCombo" ) );
  mode->setCurrentIndex( mode->findData( QStringLiteral( "cartographic_detour" ) ) );
  auto *levels = page.findChild<QLineEdit *>( QStringLiteral( "factorContourLevelsEdit" ) );
  levels->setText( QStringLiteral( "10, abc, 30" ) );

  auto *contour = page.findChild<QPushButton *>( QStringLiteral( "contourButton" ) );
  QVERIFY( contour->isEnabled() );
  QVERIFY2( contour->toolTip().contains( QStringLiteral( "无法识别" ) ),
            qPrintable( contour->toolTip() ) );
  QSignalSpy spy( &page, &ConstraintPage::interpretiveContourRequested );
  contour->click();
  QCOMPARE( spy.count(), 0 ); // 脏输入拒绝发请求
  auto *status = page.findChild<QLabel *>( QStringLiteral( "statusLabel" ) );
  QVERIFY2( status && status->text().contains( QStringLiteral( "无法识别" ) ),
            status ? qPrintable( status->text() ) : "statusLabel missing" );
}

// U9：固定级别数上限的 UI 层闸——与工作流层 prepare 闸对偶（#149 自动/间距
// 路径的对偶缺口）。超限的合法 token 与脏 token 同走 dropped 机制：截断保留
// 前 N 条并如实计数，页面据此拒绝发请求，不静默截断。
void FactorPageTests::parseLevelsAboveCapCountsDropped()
{
  using paleo::constraintpage_internal::kMaxContourLevelTokens;
  using paleo::constraintpage_internal::parseLevels;
  // 纯函数面：kMaxContourLevelTokens+2 个合法值 → 恰保留前 N 条、dropped=2。
  // 用单字符 token（全 "1"）压在 QLineEdit 32767 字符上限之下——控件截断
  // 不参与本断言（粘贴超大文本时控件自身截断是另一层现实边界）。
  QStringList tokens;
  tokens.reserve( kMaxContourLevelTokens + 2 );
  for ( int i = 0; i < kMaxContourLevelTokens + 2; ++i )
    tokens << QStringLiteral( "1" );
  int dropped = 0;
  const QVector<double> levels = parseLevels( tokens.join( QLatin1Char( ' ' ) ), &dropped );
  QCOMPARE( levels.size(), kMaxContourLevelTokens );
  QCOMPARE( dropped, 2 );
  QCOMPARE( levels.first(), 1.0 );
  QCOMPARE( levels.last(), 1.0 );

  // 页面面：超限输入经同一 dropped 通道拒绝发解释性等值线请求。
  ConstraintPage page( nullptr );
  page.setHorizons( { QStringLiteral( "T1" ) } );
  auto *horizons = page.findChild<QComboBox *>( QStringLiteral( "horizonCombo" ) );
  horizons->setCurrentIndex( 0 );
  QVariantMap generated;
  generated.insert( QStringLiteral( "sandthick" ), QStringLiteral( "factor.T1.sandthick" ) );
  page.setProperty( "paleo.page.factorgen", generated );
  auto *factors = page.findChild<QTableWidget *>( QStringLiteral( "factorTable" ) );
  factors->item( 0, 0 )->setCheckState( Qt::Checked ); // poro
  auto *mode = page.findChild<QComboBox *>( QStringLiteral( "factorContourModeCombo" ) );
  mode->setCurrentIndex( mode->findData( QStringLiteral( "cartographic_detour" ) ) );
  auto *levelsEdit = page.findChild<QLineEdit *>( QStringLiteral( "factorContourLevelsEdit" ) );
  levelsEdit->setText( tokens.join( QLatin1Char( ' ' ) ) );

  auto *contour = page.findChild<QPushButton *>( QStringLiteral( "contourButton" ) );
  QVERIFY( contour->isEnabled() );
  QVERIFY2( contour->toolTip().contains( QStringLiteral( "无法识别" ) ),
            qPrintable( QStringLiteral( "mode=%1 levels=%2 tip=%3" )
                            .arg( mode->currentIndex() )
                            .arg( levelsEdit->text().size() )
                            .arg( contour->toolTip() ) ) );
  QSignalSpy spy( &page, &ConstraintPage::interpretiveContourRequested );
  contour->click();
  QCOMPARE( spy.count(), 0 ); // 超限输入拒绝发请求
}

// 方向04 D11：「（旧输入）」标记记在按层位的属性里——任何 layerDeclared
// 触发的状态列重写不得抹掉它；重新生成（noteFactorLayer）后清除。
void FactorPageTests::staleSuffixSurvivesLayerRedeclare()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  LayerManifest manifest( dir.filePath( QStringLiteral( "stale.sqlite" ) ) );
  QVERIFY( manifest.open() );
  QgisLayerService layers( nullptr, &manifest );
  QString err;
  LayerDeclaration decl;
  decl.layerId = QStringLiteral( "factor.T1.sandratio" );
  decl.horizon = QStringLiteral( "T1" );
  decl.type = QStringLiteral( "raster" );
  decl.source = QStringLiteral( "/tmp/factor.T1.sandratio.tif" );
  decl.group = QStringLiteral( "02_Factor" );
  QVERIFY( layers.declare( decl, &err ) );

  ConstraintPage page( nullptr );
  page.setHorizons( { QStringLiteral( "T1" ) } );
  page.bindLayerService( &layers );
  auto *horizons = page.findChild<QComboBox *>( QStringLiteral( "horizonCombo" ) );
  horizons->setCurrentIndex( 0 );
  auto *factors = page.findChild<QTableWidget *>( QStringLiteral( "factorTable" ) );
  auto *cellSize = page.findChild<QDoubleSpinBox *>( QStringLiteral( "factorCellSizeSpin" ) );
  QVERIFY( factors && cellSize );
  factors->item( 1, 0 )->setCheckState( Qt::Checked ); // sandratio（已有声明且不依赖孔隙度字段）

  // 用户改像元大小 → 输入陈旧 → 勾选行状态列加「（旧输入）」。
  cellSize->setValue( cellSize->value() + 1.0 );
  QVERIFY2( factors->item( 1, 3 )->text().contains( QStringLiteral( "（旧输入）" ) ),
            qPrintable( factors->item( 1, 3 )->text() ) );

  // 任何 factor.* 层重声明触发 refreshFactorLayers 重写状态列——标记必须存活。
  LayerDeclaration other = decl;
  other.layerId = QStringLiteral( "factor.T1.sandthick" );
  other.source = QStringLiteral( "/tmp/factor.T1.sandthick.tif" );
  QVERIFY( layers.declare( other, &err ) );
  QVERIFY2( factors->item( 1, 3 )->text().contains( QStringLiteral( "（旧输入）" ) ),
            qPrintable( factors->item( 1, 3 )->text() ) );

  // 重新生成 → 新输入生效，标记清除。
  page.noteFactorLayer( QStringLiteral( "sandratio" ), QStringLiteral( "factor.T1.sandratio" ) );
  QVERIFY2( !factors->item( 1, 3 )->text().contains( QStringLiteral( "（旧输入）" ) ),
            qPrintable( factors->item( 1, 3 )->text() ) );
}

// 回归（真实工程 D61）：结构 IDW 成图边界下拉曾把约束线（constraints.<层位>）
// 和井点属性表（点几何）列进清单甚至默认选中——计算到算法深处才报匿名的
// 「边界图层中没有有效 polygon」。清单只列面图层；默认值依次落
// boundary.* → survey.area，都没有才留空交给生成门控引导。
void FactorPageTests::structuralBoundaryComboPrefersPolygonDefaults()
{
  QTemporaryDir dir;
  QVERIFY( dir.isValid() );
  LayerManifest manifest( dir.filePath( QStringLiteral( "boundary.sqlite" ) ) );
  QVERIFY( manifest.open() );
  QgisLayerService layers( nullptr, &manifest );
  QString err;
  const auto declareVector = [&layers, &err]( const QString &id, const QString &title ) {
    LayerDeclaration d;
    d.layerId = id;
    d.type = QStringLiteral( "vector" );
    d.source = QStringLiteral( "/tmp/%1.geojson" ).arg( id );
    d.title = title;
    return layers.declare( d, &err );
  };
  // 声明顺序把约束线/井点/属性表放在最前——旧下拉会列出甚至默认选中它们。
  QVERIFY2( declareVector( QStringLiteral( "constraints.C6" ), QString() ), qPrintable( err ) );
  QVERIFY2( declareVector( QStringLiteral( "wells" ), QStringLiteral( "井位" ) ), qPrintable( err ) );
  QVERIFY2( declareVector( QStringLiteral( "welllog.attributes" ), QStringLiteral( "测井岩性与相属性" ) ),
            qPrintable( err ) );
  QVERIFY2( declareVector( QStringLiteral( "survey.area" ), QStringLiteral( "测区范围" ) ), qPrintable( err ) );

  ConstraintPage page( nullptr );
  page.bindLayerService( &layers );
  auto *h = page.findChild<QComboBox *>( QStringLiteral( "horizonCombo" ) );
  h->addItem( QStringLiteral( "D61" ) );
  h->setCurrentIndex( 0 );
  page.findChild<QTableWidget *>( QStringLiteral( "factorTable" ) )
      ->item( 1, 0 )
      ->setCheckState( Qt::Checked ); // 砂地比（插值引擎）
  auto *method = page.findChild<QComboBox *>( QStringLiteral( "factorMethodCombo" ) );
  auto *boundary = page.findChild<QComboBox *>( QStringLiteral( "factorBoundaryCombo" ) );
  method->setCurrentIndex( method->findData( QStringLiteral( "structural_idw" ) ) );
  QCOMPARE( boundary->currentData().toString(), QStringLiteral( "survey.area" ) );
  QCOMPARE( boundary->currentText(), QStringLiteral( "测区范围" ) );
  QVERIFY( boundary->findData( QStringLiteral( "constraints.C6" ) ) < 0 );
  QVERIFY( boundary->findData( QStringLiteral( "wells" ) ) < 0 );
  QVERIFY( boundary->findData( QStringLiteral( "welllog.attributes" ) ) < 0 );

  // 专有边界后补声明：清单一目之首是 boundary.*；用户之前的 survey.area 选择回填保留。
  QVERIFY2( declareVector( QStringLiteral( "boundary.imported" ), QStringLiteral( "导入边界" ) ),
            qPrintable( err ) );
  method->setCurrentIndex( method->findData( QStringLiteral( "kriging" ) ) );
  method->setCurrentIndex( method->findData( QStringLiteral( "structural_idw" ) ) );
  QCOMPARE( boundary->currentData().toString(), QStringLiteral( "survey.area" ) );
  QCOMPARE( boundary->itemData( 0 ).toString(), QStringLiteral( "boundary.imported" ) );
  boundary->setCurrentIndex( boundary->findData( QStringLiteral( "boundary.imported" ) ) );

  // 专有边界与测区面都不在时：留空（占位提示），剩余声明的非面层不再进清单。
  QVERIFY( layers.removeDeclaration( QStringLiteral( "boundary.imported" ), &err ) );
  QVERIFY( layers.removeDeclaration( QStringLiteral( "survey.area" ), &err ) );
  method->setCurrentIndex( method->findData( QStringLiteral( "kriging" ) ) );
  method->setCurrentIndex( method->findData( QStringLiteral( "structural_idw" ) ) );
  QVERIFY( boundary->currentData().toString().isEmpty() );
  QCOMPARE( boundary->count(), 0 );
}

// ---- 任务 C（智能编图页）：融合清单栅格过滤/参考图/相属性/设计器入口。--
// 真实栈（属性回写 edit buffer、壳自动编辑态）在 tst_composeworkflow.cpp。

void FactorPageTests::geologicalAutoModeAndTaskRecommendation()
{
  ConstraintPage page(nullptr);
  page.findChild<QWidget *>("factorDataDetailsSection")->findChild<QAbstractButton *>("collapsibleSectionToggle")->setChecked(true);
  page.setHorizons({QStringLiteral("D61")});
  auto *table = page.findChild<QTableWidget *>(QStringLiteral("factorTable"));
  table->item(1, 0)->setCheckState(Qt::Checked); // sandratio
  auto *mode = page.findChild<QComboBox *>(QStringLiteral("factorModeCombo"));
  auto *extract = page.findChild<QPushButton *>(QStringLiteral("extractWellFactorsButton"));
  auto *geo = page.findChild<QLabel *>(QStringLiteral("wellFactorGeoHint"));
  auto *fields = page.findChild<QWidget *>(QStringLiteral("wellFactorFieldMappingSection"));
  auto *task = page.findChild<QComboBox *>(QStringLiteral("factorTaskCombo"));
  auto *method = page.findChild<QComboBox *>(QStringLiteral("factorMethodCombo"));
  QVERIFY(mode && extract && geo && fields && task && method);
  QCOMPARE(mode->currentData().toString(), QStringLiteral("auto"));
  QVERIFY(geo->text().contains(QStringLiteral("解释砂厚")));
  QVERIFY(geo->text().contains(QStringLiteral("[0, 1]")));
  QVERIFY(!fields->isVisibleTo(&page));
  QVERIFY(extract->isEnabled());
  mode->setCurrentIndex(mode->findData(QStringLiteral("direct")));
  QVERIFY(fields->isVisibleTo(&page));
  // 任务导向：地统计估计 → 克里金（真实算法 id）
  task->setCurrentIndex(task->findText(QStringLiteral("地统计估计并检查误差")));
  QCOMPARE(method->currentData().toString(), QStringLiteral("kriging"));
  task->setCurrentIndex(task->findText(QStringLiteral("分析多种可能的模拟结果")));
  QCOMPARE(method->currentData().toString(), QStringLiteral("sgs"));
}


// 场景 I：层位/因素切换不泄漏上一状态（逐井表、提取提示、地质口径）。
void FactorPageTests::horizonAndFactorSwitchDoNotLeakState()
{
  ConstraintPage page(nullptr);
  page.setHorizons({QStringLiteral("D61"), QStringLiteral("D62")});
  auto *table = page.findChild<QTableWidget *>(QStringLiteral("factorTable"));
  auto *wells = page.findChild<QTableWidget *>(QStringLiteral("wellFactorTable"));
  auto *hint = page.findChild<QLabel *>(QStringLiteral("wellFactorHint"));
  auto *geo = page.findChild<QLabel *>(QStringLiteral("wellFactorGeoHint"));
  auto *mode = page.findChild<QComboBox *>(QStringLiteral("factorModeCombo"));
  auto *h = page.findChild<QComboBox *>(QStringLiteral("horizonCombo"));
  QVERIFY(table && wells && hint && geo && mode && h);
  table->item(1, 0)->setCheckState(Qt::Checked); // 砂地比
  wells->setRowCount(3);
  hint->setText(QStringLiteral("旧层位结果"));
  h->setCurrentIndex(h->findText(QStringLiteral("D62")));
  QCOMPARE(wells->rowCount(), 0);
  QVERIFY(!hint->text().contains(QStringLiteral("旧层位结果")));
  QVERIFY(geo->text().contains(QStringLiteral("解释砂厚")));
  // 手动改成 direct 后换因素：回到该因素的推荐口径，地质说明跟随新因素。
  mode->setCurrentIndex(mode->findData(QStringLiteral("direct")));
  wells->setRowCount(2);
  table->item(0, 0)->setCheckState(Qt::Checked); // 砂体厚度
  QCOMPARE(wells->rowCount(), 0);
  QCOMPARE(mode->currentData().toString(), QStringLiteral("auto"));
  QVERIFY(geo->text().contains(QStringLiteral("解释砂厚（MD")));
  QVERIFY(!geo->text().contains(QStringLiteral("地层厚度")));
}

void FactorPageTests::constraintDoubleClickRequestsLocate()
{
  ConstraintPage page(nullptr);
  page.setHorizons({QStringLiteral("D61")});
  auto *list = page.findChild<QListWidget *>(QStringLiteral("constraintList"));
  QVERIFY(list);
  auto *item = new QListWidgetItem(QStringLiteral("c7"), list);
  item->setData(Qt::UserRole, QStringLiteral("c7"));
  QSignalSpy spy(&page, &ConstraintPage::constraintLocateRequested);
  emit list->itemDoubleClicked(item);
  QCOMPARE(spy.count(), 1);
  QCOMPARE(spy[0][0].toString(), QStringLiteral("D61"));
  QCOMPARE(spy[0][1].toString(), QStringLiteral("c7"));
}

void FactorPageTests::wellFactorExtractionPayload() {
  ConstraintPage page(nullptr);
  auto *h = page.findChild<QComboBox *>(QStringLiteral("horizonCombo")); h->addItem(QStringLiteral("D61"));
  page.findChild<QTableWidget *>(QStringLiteral("factorTable"))->item(1, 0)->setCheckState(Qt::Checked);
  page.setWellFactorFields({QVariantMap{{"id", "sand"}, {"label", "砂厚"}},
                            QVariantMap{{"id", "gross"}, {"label", "层厚"}},
                            QVariantMap{{"id", "ratio"}, {"label", "砂地比"}}});
  auto *mode = page.findChild<QComboBox *>(QStringLiteral("factorModeCombo"));
  auto *value = page.findChild<QComboBox *>(QStringLiteral("factorValueFieldCombo"));
  auto *num = page.findChild<QComboBox *>(QStringLiteral("factorNumeratorFieldCombo"));
  auto *den = page.findChild<QComboBox *>(QStringLiteral("factorDenominatorFieldCombo"));
  auto *extract = page.findChild<QPushButton *>(QStringLiteral("extractWellFactorsButton"));
  auto *generate = page.findChild<QPushButton *>(QStringLiteral("generateFactorButton"));
  // 默认「推荐地质口径」无需选字段即可提取（场景 F + 地质语义）。
  QCOMPARE(mode->currentData().toString(), QStringLiteral("auto"));
  QVERIFY2(extract->isEnabled(), qPrintable(extract->toolTip()));
  QVERIFY(!extract->toolTip().contains(QStringLiteral("间隔线")));
  value->setCurrentIndex(value->findData(QStringLiteral("ratio")));
  // 未提取井点前生成仍不可用，但原因是井点而非约束线。
  QVERIFY(!generate->isEnabled());
  QVERIFY2(generate->toolTip().contains(QStringLiteral("井点")), qPrintable(generate->toolTip()));

  QSignalSpy extracted(&page, &ConstraintPage::extractWellFactorsRequested);
  extract->click(); QCOMPARE(extracted.count(), 1);
  QCOMPARE(extracted[0][0].toString(), QStringLiteral("sandratio"));
  QCOMPARE(extracted[0][2].toMap().value("factorMode").toString(), QStringLiteral("auto"));
  // 切到直接字段后再提取，技术字段映射生效。
  mode->setCurrentIndex(mode->findData(QStringLiteral("direct")));
  value->setCurrentIndex(value->findData(QStringLiteral("ratio")));
  extract->click(); QCOMPARE(extracted.count(), 2);
  QCOMPARE(extracted[1][2].toMap().value("factorMode").toString(), QStringLiteral("direct"));
  QCOMPARE(extracted[1][2].toMap().value("valueField").toString(), QStringLiteral("ratio"));
  mode->setCurrentIndex(mode->findData(QStringLiteral("ratio")));
  QVERIFY(!extract->isEnabled());
  num->setCurrentIndex(num->findData(QStringLiteral("sand")));
  den->setCurrentIndex(den->findData(QStringLiteral("gross")));
  extract->click(); QCOMPARE(extracted.count(), 3);
  const auto params = extracted[2][2].toMap();
  QCOMPARE(params.value("factorMode").toString(), QStringLiteral("ratio"));
  QCOMPARE(params.value("numeratorField").toString(), QStringLiteral("sand"));
  QCOMPARE(params.value("denominatorField").toString(), QStringLiteral("gross"));
  // 无 ConstraintWorkflow 时无法登记 preparedWellFactorVersion → 停在步骤 2。
  QVERIFY(!generate->isEnabled());
  QVERIFY2(generate->toolTip().contains(QStringLiteral("井点数据")), qPrintable(generate->toolTip()));
  page.setRunBusy(true); QVERIFY(!extract->isEnabled());
  QVERIFY(!extract->toolTip().isEmpty());
  page.setRunBusy(false);
  page.setWellFactorFields({});
  QVERIFY(!generate->isEnabled());
  page.findChild<QTableWidget *>(QStringLiteral("factorTable"))->item(0, 0)->setCheckState(Qt::Checked);
  QCOMPARE(mode->currentData().toString(), QStringLiteral("auto"));
}

void FactorPageTests::methodParameterVisibilityAndFamilies() {
  ConstraintPage page(nullptr); page.show();
  page.findChild<QWidget *>("factorBasicSettingsSection")->findChild<QAbstractButton *>("collapsibleSectionToggle")->setChecked(true);
  // 高级参数可按需展开；方法切换仍只呈现实际消费的参数。
  // #19：约束族收进「可选约束」折叠段，展开该段后断言不变。
  for (const char *name : {"factorAdvancedOptionsSection", "factorConstraintsOptionalSection",
                           "factorDirectionParametersSection"})
    page.findChild<QWidget *>(QString::fromLatin1(name))->findChild<QAbstractButton *>(QStringLiteral("collapsibleSectionToggle"))->setChecked(true);
  auto *method = page.findChild<QComboBox *>(QStringLiteral("factorMethodCombo"));
  method->setCurrentIndex(method->findData(QStringLiteral("local_direction_idw")));
  const auto visible = [&page](const char *name) { return page.findChild<QWidget *>(QString::fromLatin1(name))->isVisibleTo(&page); };
  QVERIFY(visible("factorIdwParameters")); QVERIFY(!visible("factorVariogramParameters"));
  QVERIFY(visible("factorDirectionRatioSpin"));
  QVERIFY(!visible("runIdwButton")); // 原始字段旧入口收起；主流程走统一提取。
  method->setCurrentIndex(method->findData(QStringLiteral("kriging")));
  QVERIFY(!visible("factorIdwParameters")); QVERIFY(visible("factorVariogramParameters"));
  QVERIFY(!visible("factorDirectionRatioSpin")); QVERIFY(!visible("factorSgsParameters"));
  method->setCurrentIndex(method->findData(QStringLiteral("local_direction_kriging")));
  QVERIFY(visible("factorVariogramParameters")); QVERIFY(visible("factorNeighborhoodParameters"));
  method->setCurrentIndex(method->findData(QStringLiteral("sgs")));
  QVERIFY(visible("factorSgsParameters"));
  QVERIFY(!page.findChild<QLabel *>(QStringLiteral("factorStrategyNote"))->text().isEmpty());
  method->setCurrentIndex(method->findData(QStringLiteral("legacy")));
  QVERIFY(visible("factorNoParametersLabel")); QVERIFY(!visible("factorVariogramParameters"));
  auto *h = page.findChild<QComboBox *>(QStringLiteral("horizonCombo")); h->addItem(QStringLiteral("D61"));
  auto *directionType = page.findChild<QComboBox *>(QStringLiteral("directionFamilyTypeCombo"));
  auto *breakType = page.findChild<QComboBox *>(QStringLiteral("breakFamilyTypeCombo"));
  QVERIFY(visible("directionFamilySection")); QVERIFY(visible("breakFamilySection"));
  QSignalSpy draw(&page, &ConstraintPage::drawTypedConstraintRequested);
  directionType->setCurrentIndex(directionType->findData(QStringLiteral("cartographic_detour")));
  page.findChild<QPushButton *>(QStringLiteral("directionButton"))->click();
  QCOMPARE(draw.last()[2].toString(), QStringLiteral("cartographic_detour"));
  breakType->setCurrentIndex(breakType->findData(QStringLiteral("contour_stop")));
  page.findChild<QPushButton *>(QStringLiteral("breakLineButton"))->click();
  QCOMPARE(draw.last()[2].toString(), QStringLiteral("contour_stop"));
  page.findChild<QComboBox *>(QStringLiteral("constraintBlockModeCombo"))->setCurrentIndex(1);
  QCOMPARE(page.newConstraintLineParams(QStringLiteral("contour_stop")).value("blockMode").toString(), QStringLiteral("display_only"));
  page.findChild<QDoubleSpinBox *>(QStringLiteral("factorDirectionRatioSpin"))->setValue(6.5);
  QCOMPARE(page.newConstraintLineParams(QStringLiteral("direction_line")).value("ratio").toDouble(), 6.5);
  const QString qa = qEnvironmentVariable("PALEO_SINGLEFACTOR_QA_DIR");
  if (!qa.isEmpty()) {
    QDir().mkpath(qa); page.resize(420, 960);
    for (const char *section : {"factorAdvancedOptionsSection", "factorDirectionParametersSection"})
      page.findChild<QWidget *>(QString::fromLatin1(section))->findChild<QAbstractButton *>(QStringLiteral("collapsibleSectionToggle"))->setChecked(false);
    for (const auto theme : {PaleoTheme::Theme::Light, PaleoTheme::Theme::Dark}) {
      PaleoTheme::applyTheme(theme);
      method->setCurrentIndex(method->findData(QStringLiteral("local_direction_idw")));
      QCoreApplication::processEvents();
      auto *body = page.findChild<QWidget *>(QStringLiteral("constraintPageBody"));
      const QString name = theme == PaleoTheme::Theme::Light ? QStringLiteral("light") : QStringLiteral("dark");
      QVERIFY(body->grab().save(qa + QStringLiteral("/pipeline-") + name + QStringLiteral(".png")));
      QVERIFY(page.grab().save(qa + QStringLiteral("/panel-") + name + QStringLiteral(".png")));
      method->setCurrentIndex(method->findData(QStringLiteral("kriging")));
      QCoreApplication::processEvents();
      QVERIFY(body->grab().save(qa + QStringLiteral("/kriging-") + name + QStringLiteral(".png")));
    }
    PaleoTheme::applyTheme(PaleoTheme::Theme::Light);
  }
}

void FactorPageTests::oneClickExtractThenGenerateStopsWithReason()
{
  // 反馈 #19：一键「提取并生成」驱动原两按钮；失败/门控拒绝/未能启动都停下并说明原因。
  ConstraintPage page(nullptr);
  page.show();
  auto *oneClick = page.findChild<QPushButton *>(QStringLiteral("factorOneClickButton"));
  auto *generate = page.findChild<QPushButton *>(QStringLiteral("generateFactorButton"));
  auto *extract = page.findChild<QPushButton *>(QStringLiteral("extractWellFactorsButton"));
  auto *status = page.findChild<QLabel *>(QStringLiteral("statusLabel"));
  auto *progress = page.findChild<QProgressBar *>(QStringLiteral("factorProgressBar"));
  QVERIFY(oneClick && generate && extract && status && progress);
  QCOMPARE(oneClick->text(), QStringLiteral("生成等值图"));
  QCOMPARE(generate->text(), QStringLiteral("生成等值图"));
  QVERIFY(!oneClick->isEnabled()); // 无层位/因素
  page.setHorizons({QStringLiteral("D61")});
  page.findChild<QTableWidget *>(QStringLiteral("factorTable"))->item(0, 0)->setCheckState(Qt::Checked);
  page.setWellFactorFields({QVariantMap{{"id", "sand"}, {"label", "砂厚"}}});
  QVERIFY2(extract->isEnabled(), qPrintable(extract->toolTip()));
  QVERIFY(!generate->isEnabled());
  QVERIFY2(oneClick->isEnabled(), qPrintable(oneClick->toolTip()));
  QVERIFY(oneClick->toolTip().contains(QStringLiteral("自动")));

  // 1) 提取任务失败 → 停下并带出失败原因，不触发生成。
  QSignalSpy extracted(&page, &ConstraintPage::extractWellFactorsRequested);
  QSignalSpy generated(&page, &ConstraintPage::generateFactorRequested);
  auto conn = connect(&page, &ConstraintPage::extractWellFactorsRequested, &page,
                      [&page] { page.setRunBusy(true); });
  oneClick->click();
  QCOMPARE(extracted.count(), 1);
  QCOMPARE(page.property("paleo.page.oneclick").toString(), QStringLiteral("extract"));
  QVERIFY(!oneClick->isEnabled());
  page.noteRunStage(QStringLiteral("计算"), 40);
  QVERIFY2(progress->format().contains(QStringLiteral("一键 1/2")), qPrintable(progress->format()));
  page.setProperty("paleo.page.runoutcome", QStringLiteral("井 W1 缺少分层"));
  page.setRunBusy(false);
  QTRY_VERIFY(page.property("paleo.page.oneclick").toString().isEmpty());
  QVERIFY2(status->text().contains(QStringLiteral("一键成图已停止")), qPrintable(status->text()));
  QVERIFY(status->text().contains(QStringLiteral("缺少分层")));
  QCOMPARE(generated.count(), 0);

  // 2) 提取成功但生成仍被门控（此处无工作流登记井点版本）→ 停下说明，不硬发生成。
  oneClick->click();
  QCOMPARE(extracted.count(), 2);
  page.setRunBusy(false);
  QTRY_VERIFY(page.property("paleo.page.oneclick").toString().isEmpty());
  QVERIFY2(status->text().contains(QStringLiteral("井点已提取，但暂不能生成")), qPrintable(status->text()));
  QCOMPARE(generated.count(), 0);

  // 3) 提取被同步拒绝（未进入忙态）→ 立即停下。
  disconnect(conn);
  status->setText(QStringLiteral("当前层位缺少井位"));
  oneClick->click();
  QCOMPARE(extracted.count(), 3);
  QTRY_VERIFY(page.property("paleo.page.oneclick").toString().isEmpty());
  QVERIFY2(status->text().contains(QStringLiteral("一键成图已停止：当前层位缺少井位")), qPrintable(status->text()));
  QVERIFY(oneClick->isEnabled()); // 可重试；逐步按钮仍在
  QVERIFY(extract->isEnabled());
}

void FactorPageTests::fallbackToolsGroupedUnderMoreTools()
{
  ConstraintPage page(nullptr);
  page.show();
  QCoreApplication::processEvents();
  auto *more = page.findChild<QWidget *>(QStringLiteral("factorMoreToolsSection"));
  auto *body = page.findChild<QWidget *>(QStringLiteral("constraintPageBody"));
  auto *results = page.findChild<QWidget *>(QStringLiteral("factorResultsEmptyHint"));
  auto *output = page.findChild<QWidget *>(QStringLiteral("factorOutputStageCaption"));
  QVERIFY(more && body && results && output);
  for (const char *name : {"factorLegacySection", "thicknessSection", "realizationSection",
                           "runIdwButton", "thicknessTable", "realizationPanel"}) {
    auto *w = page.findChild<QWidget *>(QString::fromLatin1(name));
    QVERIFY2(w && more->isAncestorOf(w), name);
    QVERIFY2(!w->isVisible(), name); // 默认收起
  }
  const auto y = [body](QWidget *w) { return w->mapTo(body, QPoint(0, 0)).y(); };
  QVERIFY(y(output) < y(more));
  QVERIFY(y(results) < y(more));
  more->findChild<QAbstractButton *>(QStringLiteral("collapsibleSectionToggle"))->setChecked(true);
  QVERIFY(page.findChild<QWidget *>(QStringLiteral("factorLegacySection"))->isVisible());
}

void FactorPageTests::ipoStagesAndProgressiveDisclosure()
{
  // 反馈 #19：输入 → 处理 → 输出 三段式；约束与高级参数默认收起但仍可达。
  ConstraintPage page(nullptr);
  page.findChild<QWidget *>("factorDataDetailsSection")->findChild<QAbstractButton *>("collapsibleSectionToggle")->setChecked(true);
  page.findChild<QWidget *>("factorBasicSettingsSection")->findChild<QAbstractButton *>("collapsibleSectionToggle")->setChecked(true);
  page.resize(420, 760);
  page.show();
  QCoreApplication::processEvents();
  auto *body = page.findChild<QWidget *>(QStringLiteral("constraintPageBody"));
  auto *stepper = page.findChild<QLabel *>(QStringLiteral("factorIpoStepper"));
  auto *inputStage = page.findChild<QLabel *>(QStringLiteral("factorInputStageCaption"));
  auto *processStage = page.findChild<QLabel *>(QStringLiteral("factorSettingsCaption"));
  auto *outputStage = page.findChild<QLabel *>(QStringLiteral("factorOutputStageCaption"));
  auto *optional = page.findChild<QWidget *>(QStringLiteral("factorConstraintsOptionalSection"));
  auto *families = page.findChild<QWidget *>(QStringLiteral("constraintFamiliesSection"));
  auto *advanced = page.findChild<QWidget *>(QStringLiteral("factorAdvancedSection"));
  auto *next = page.findChild<QPushButton *>(QStringLiteral("factorNextStepButton"));
  auto *table = page.findChild<QTableWidget *>(QStringLiteral("factorTable"));
  QVERIFY(body && stepper && inputStage && processStage && outputStage && optional && families
          && advanced && next && table);
  // 三段标题按 I → P → O 自上而下排列；井点数据属于输入段，等值线/成果属于输出段。
  const auto y = [body](QWidget *w) { return w->mapTo(body, QPoint(0, 0)).y(); };
  auto *wells = page.findChild<QWidget *>(QStringLiteral("wellFactorSection"));
  auto *method = page.findChild<QWidget *>(QStringLiteral("factorMethodCombo"));
  auto *contour = page.findChild<QWidget *>(QStringLiteral("factorContourSection"));
  auto *results = page.findChild<QWidget *>(QStringLiteral("factorResultsEmptyHint"));
  QVERIFY(wells && method && contour && results);
  QVERIFY(y(inputStage) < y(table));
  QVERIFY(y(table) < y(wells));
  QVERIFY(y(table) < y(page.findChild<QWidget *>("factorDataDetailsSection")));
  QVERIFY(page.findChild<QWidget *>("factorBasicSettingsSection")->isAncestorOf(method));
  QVERIFY(y(optional) < y(processStage));
  QVERIFY(y(processStage) < y(method));
  QVERIFY(y(optional) < y(outputStage));
  QVERIFY(y(outputStage) < y(contour));
  QVERIFY(y(contour) < y(results));
  // 约束是处理段的可选项：默认收起，但控件仍在（旧工程/原入口兼容）。
  QVERIFY(optional->isAncestorOf(families));
  QVERIFY(!families->isVisible());
  QVERIFY(!advanced->isVisible());
  for (const char *name : {"directionButton", "breakLineButton", "constraintList", "factorPowerSpin",
                           "runIdwButton", "contourButton", "factorResultsTable"})
    QVERIFY2(page.findChild<QWidget *>(QString::fromLatin1(name)), name);
  // 步骤条：未选层位/因素时停在「输入」。
  QCOMPARE(stepper->property("stage").toInt(), 0);
  QVERIFY(stepper->text().contains(QStringLiteral("▶ 获取单因素")));
  page.setHorizons({QStringLiteral("D61")});
  QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier,
                   table->visualItemRect(table->item(0, 1)).center());
  page.setWellFactorFields({QVariantMap{{"id", "sand"}, {"label", "砂厚"}}});
  // 井点尚未提取 → 仍是输入阶段，下一步指向输入段控件而非约束。
  QCOMPARE(stepper->property("stage").toInt(), 0);
  QVERIFY(next->property("target").toString() != QStringLiteral("constraintFamiliesSection"));
  // 门控需要约束时，「下一步」展开收起的可选段并定位（渐进展开仍可达）。
  next->setProperty("target", QStringLiteral("constraintFamiliesSection"));
  next->click();
  QCoreApplication::processEvents();
  QVERIFY(families->isVisible());
  const QString qa = qEnvironmentVariable("PALEO_SINGLEFACTOR_QA_DIR");
  if (!qa.isEmpty()) {
    QDir().mkpath(qa);
    QVERIFY(page.grab().save(qa + QStringLiteral("/ipo-after.png")));
  }
}

void FactorPageTests::fourStepFlowKeepsAcquisitionVisibleAndLinesOptional()
{
  ConstraintPage page(nullptr);
  page.setHorizons({QStringLiteral("D61")});
  page.resize(420, 900); page.show(); QCoreApplication::processEvents();
  auto *acquire = page.findChild<QPushButton *>(QStringLiteral("extractWellFactorsButton"));
  auto *details = page.findChild<QWidget *>(QStringLiteral("factorDataDetailsSection"));
  auto *lines = page.findChild<QWidget *>(QStringLiteral("factorConstraintsOptionalSection"));
  auto *map = page.findChild<QLabel *>(QStringLiteral("factorSettingsCaption"));
  auto *contours = page.findChild<QLabel *>(QStringLiteral("factorOutputStageCaption"));
  auto *body = page.findChild<QWidget *>(QStringLiteral("constraintPageBody"));
  QVERIFY(acquire && details && lines && map && contours && body);
  QVERIFY(acquire->isVisibleTo(&page));
  QVERIFY(!details->isAncestorOf(acquire));
  QVERIFY(!page.findChild<QWidget *>(QStringLiteral("constraintFamiliesSection"))->isVisibleTo(&page));
  const auto y = [body](QWidget *widget) { return widget->mapTo(body, QPoint()).y(); };
  QVERIFY(y(acquire) < y(lines)); QVERIFY(y(lines) < y(map)); QVERIFY(y(map) < y(contours));
  const auto stepper = page.findChild<QLabel *>(QStringLiteral("factorIpoStepper"))->text();
  for (const auto &step : {QStringLiteral("获取单因素"), QStringLiteral("间隔/方向线"), QStringLiteral("等值图"), QStringLiteral("等值线")})
    QVERIFY(stepper.contains(step));
  QSignalSpy acquired(&page, &ConstraintPage::extractWellFactorsRequested);
  acquire->click(); QCOMPARE(acquired.size(), 1);
  QVERIFY(!acquired.last()[2].toMap().value("forceRefresh").toBool());
  page.findChild<QPushButton *>(QStringLiteral("recomputeWellFactorsButton"))->click();
  QCOMPARE(acquired.size(), 2); QVERIFY(acquired.last()[2].toMap().value("forceRefresh").toBool());
  auto *factors = page.findChild<QTableWidget *>(QStringLiteral("factorTable"));
  factors->item(1, 0)->setCheckState(Qt::Checked);
  QCOMPARE(page.findChild<QDoubleSpinBox *>(QStringLiteral("contourIntervalSpin"))->value(), 0.1);
  QVERIFY(page.findChild<QLabel *>(QStringLiteral("factorMapDefaultsHint"))->text().contains(QStringLiteral("0–1")));
  QVERIFY(!page.findChild<QPushButton *>(QStringLiteral("contourButton"))->isEnabled());
}

void FactorPageTests::guidedPreparationAndPinnedActions()
{
  ConstraintPage page(nullptr);
  page.resize(420, 760);
  page.show();
  auto *scroll = page.findChild<QScrollArea *>(QStringLiteral("constraintPageScroll"));
  auto *table = page.findChild<QTableWidget *>(QStringLiteral("factorTable"));
  auto *hint = page.findChild<QLabel *>(QStringLiteral("factorReadinessHint"));
  auto *next = page.findChild<QPushButton *>(QStringLiteral("factorNextStepButton"));
  auto *generate = page.findChild<QPushButton *>(QStringLiteral("generateFactorButton"));
  auto *bar = page.findChild<QWidget *>(QStringLiteral("factorActionBar"));
  auto *method = page.findChild<QComboBox *>(QStringLiteral("factorMethodCombo"));
  QVERIFY(scroll && table && hint && next && generate && bar && method);
  QVERIFY(hint->text().contains(QStringLiteral("选择层位")));
  page.setHorizons({QStringLiteral("D61")});
  QCOMPARE(table->item(0, 0)->checkState(), Qt::Checked);
  QCoreApplication::processEvents();
  // 点名称即可选择，取消仍通过原生复选框；不会只高亮却操作另一个因素。
  QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier,
                   table->visualItemRect(table->item(0, 1)).center());
  QCOMPARE(table->item(0, 0)->checkState(), Qt::Checked);
  QVERIFY(page.findChild<QLabel *>(QStringLiteral("factorRequestSummary"))->text().contains(QStringLiteral("砂体厚度")));
  page.setWellFactorFields({QVariantMap{{"id", "sand"}, {"label", "砂厚"}}});
  QVERIFY(!generate->isEnabled());
  // 场景 A：无约束线不再把下一步指向约束分组。
  QVERIFY(!hint->text().contains(QStringLiteral("间隔线和方向线")));
  QVERIFY(next->property("target").toString() != QStringLiteral("constraintFamiliesSection"));
  const QString qa = qEnvironmentVariable("PALEO_SINGLEFACTOR_QA_DIR");
  const auto capture = [&page, &qa](const QString &name) {
    if (qa.isEmpty())
      return true;
    QDir().mkpath(qa);
    QCoreApplication::processEvents();
    return page.grab().save(qa + QLatin1Char('/') + name + QStringLiteral(".png"));
  };
  QVERIFY(capture(QStringLiteral("01-selection-light")));
  // 分组里的标题必须真的归该分组，不能以旧父容器坐标叠到页首。
  auto *constraintCaption = page.findChild<QLabel *>(QStringLiteral("factorConstraintCaption"));
  QVERIFY(page.findChild<QWidget *>(QStringLiteral("constraintFamiliesSection"))->isAncestorOf(constraintCaption));
  const QPoint fixedPosition = bar->pos();
  scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
  QCoreApplication::processEvents();
  QCOMPARE(bar->pos(), fixedPosition);
  QVERIFY(page.rect().contains(bar->geometry()));
  QVERIFY(page.findChild<QPushButton *>("factorOneClickButton")->isVisibleTo(&page));
  next->click();
  auto *acquire = page.findChild<QPushButton *>(QStringLiteral("extractWellFactorsButton"));
  QTRY_VERIFY(scroll->viewport()->rect().contains(QRect(acquire->mapTo(scroll->viewport(), QPoint()), acquire->size())));
  QVERIFY(capture(QStringLiteral("02-constraints-light")));
  // 推荐地质口径下无需选字段；下一步直接是提取。
  page.setProperty("paleo.page.constraintsready", true);
  page.setRunBusy(false);
  QVERIFY(page.findChild<QProgressBar *>(QStringLiteral("factorProgressBar"))->isHidden());
  QCOMPARE(next->property("target").toString(), QStringLiteral("extractWellFactorsButton"));
  QVERIFY(hint->text().contains(QStringLiteral("先提取并保存")));
  auto *advanced = page.findChild<QWidget *>(QStringLiteral("factorAdvancedOptionsSection"))->findChild<QAbstractButton *>(QStringLiteral("collapsibleSectionToggle"));
  QVERIFY(!advanced->isChecked());
  page.resize(280, 420);
  QCoreApplication::processEvents();
  QCOMPARE(page.width(), 280);
  QStringList wide;
  for (const auto *widget : scroll->widget()->findChildren<QWidget *>())
    if (widget->isVisibleTo(&page) && widget->minimumSizeHint().width() > scroll->viewport()->width() - 16)
      wide << QStringLiteral("%1 (%2): %3").arg(widget->objectName(), QLatin1String(widget->metaObject()->className()))
                  .arg(widget->minimumSizeHint().width());
  QVERIFY2(scroll->horizontalScrollBar()->maximum() == 0, qPrintable(wide.join(QLatin1Char('\n'))));
  QVERIFY(page.rect().contains(bar->geometry()));
  QVERIFY(page.findChild<QPushButton *>("factorOneClickButton")->isVisibleTo(&page));
  QVERIFY(capture(QStringLiteral("03-narrow-light")));
  page.resize(420, 760);
  // 换到非插值引擎时不展示不适用的字段/高级参数，输入提示明确。
  table->item(2, 0)->setCheckState(Qt::Checked);
  auto *source = page.findChild<QComboBox *>(QStringLiteral("factorSourceCombo"));
  source->setCurrentIndex(source->findData(QStringLiteral("surfaces")));
  QVERIFY(page.findChild<QWidget *>(QStringLiteral("wellFactorSection"))->isHidden());
  QVERIFY(page.findChild<QWidget *>(QStringLiteral("factorAdvancedOptionsSection"))->isHidden());
  QVERIFY(page.findChild<QLabel *>(QStringLiteral("factorInputHint"))->text().contains(QStringLiteral("无需提取井点")));
  QCOMPARE(next->property("target").toString(), QStringLiteral("factorTopSurfaceCombo"));
  next->click();
  QCoreApplication::processEvents();
  QVERIFY(capture(QStringLiteral("04-surfaces-light")));
  QCOMPARE(table->rowCount(), 6); // Unimplemented confidence is omitted.
  table->item(0, 0)->setCheckState(Qt::Checked);
  page.noteFactorLayer(QStringLiteral("sandthick"), QStringLiteral("factor.D61.sandthick"));
  auto *mode = page.findChild<QComboBox *>(QStringLiteral("factorContourModeCombo"));
  mode->setCurrentIndex(mode->findData(QStringLiteral("cartographic_detour")));
  QVERIFY(page.findChild<QDoubleSpinBox *>(QStringLiteral("contourIntervalSpin"))->isHidden());
  QVERIFY(page.findChild<QLabel *>(QStringLiteral("factorContourHint"))->text().contains(QStringLiteral("等值级别")));
  mode->setCurrentIndex(mode->findData(QStringLiteral("analysis")));
  QVERIFY(!page.findChild<QDoubleSpinBox *>(QStringLiteral("contourIntervalSpin"))->isHidden());
  page.setRunBusy(true);
  QVERIFY(!method->isEnabled());
  QVERIFY(!page.findChild<QDoubleSpinBox *>(QStringLiteral("factorCellSizeSpin"))->isEnabled());
  QVERIFY(next->isHidden());
  QVERIFY(page.findChild<QPushButton *>(QStringLiteral("factorCancelButton"))->isVisibleTo(&page));
  page.setRunBusy(false);
  QVERIFY(method->isEnabled());
  QVERIFY(!page.findChild<QLabel *>(QStringLiteral("statusLabel"))->text().contains(QStringLiteral("正在准备")));
}

class ComposePageTests : public QObject
{
  Q_OBJECT
  private slots:
    void constructsWithNullServices(); // 基座冒烟：拆分后类仍可构造
    void fusionListListsOnlyRasterFactors(); // m2(C)：contours 矢量不入融合清单
    void factorSelectAllAndClearRow();       // m2(C)：全选/清空小工具行
    void fuseButtonGatesOnSelectionState();  // §35：合成编图随勾选态门控+原因 tooltip
    void referenceAreaListsAndSignals();     // m2(C)：06_Reference 清单 + 可见性意图
    void faciesAttrAreaSavesPayload();       // m2(C)：相属性三字段 + 保存信号载荷
    void faciesTargetAdoptsSoleDeclaredFacies(); // m2(C)：唯一 facies.* 自动成目标
    void faciesTargetAndListsRespectComposeHorizon(); // opt-05：编图上下文层位过滤
    void openDesignerButtonEmitsSignal();    // m2(C)：布局设计器入口信号

  private:
    static LayerDeclaration makeDecl(const QString &layerId, const QString &horizon,
                                     const QString &type, const QString &group);
};

void ComposePageTests::constructsWithNullServices()
{
  ComposePage page(nullptr, nullptr);
  QVERIFY(page.findChild<QScrollArea *>(QStringLiteral("composePageScroll")));
  QVERIFY(page.findChild<QWidget *>(QStringLiteral("composePageBody")));
  QVERIFY(page.findChild<QListWidget *>(QStringLiteral("factorList")));
}

LayerDeclaration ComposePageTests::makeDecl(const QString &layerId, const QString &horizon,
                                            const QString &type, const QString &group)
{
  LayerDeclaration d;
  d.layerId = layerId;
  d.horizon = horizon;
  d.type = type;
  d.source = QStringLiteral("memory://%1").arg(layerId); // 清单行不校验存在性
  d.group = group;
  return d;
}

void ComposePageTests::fusionListListsOnlyRasterFactors()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  LayerManifest manifest(dir.filePath(QStringLiteral("c.sqlite")));
  QVERIFY(manifest.open());
  QgisLayerService layers(nullptr, &manifest);

  QString err;
  QVERIFY(layers.declare(makeDecl(QStringLiteral("factor.T1.sandthick"), QStringLiteral("T1"),
                                  QStringLiteral("raster"), QStringLiteral("04_SingleFactor")), &err));
  QVERIFY(layers.declare(makeDecl(QStringLiteral("factor.T1.poro"), QStringLiteral("T1"),
                                  QStringLiteral("raster"), QStringLiteral("04_SingleFactor")), &err));
  // B 的成果：等值线是 04_SingleFactor 下的矢量（子组 + 平组两种写法），
  // 都不是融合输入——清单只认栅格因素。
  QVERIFY(layers.declare(makeDecl(QStringLiteral("contours.T1.sandthick"), QStringLiteral("T1"),
                                  QStringLiteral("vector"), QStringLiteral("04_SingleFactor/Contours")), &err));
  QVERIFY(layers.declare(makeDecl(QStringLiteral("contours.T1.poro"), QStringLiteral("T1"),
                                  QStringLiteral("vector"), QStringLiteral("04_SingleFactor")), &err));
  QVERIFY(layers.declare(makeDecl(QStringLiteral("facies.T1"), QStringLiteral("T1"),
                                  QStringLiteral("vector"), QStringLiteral("05_PaleoMap")), &err));

  ComposePage page(nullptr, &layers);
  auto *list = page.findChild<QListWidget *>(QStringLiteral("factorList"));
  QVERIFY(list);
  QCOMPARE(list->count(), 2);
  QStringList ids;
  for (int i = 0; i < list->count(); ++i)
    ids << list->item(i)->data(Qt::UserRole).toString();
  QVERIFY(ids.contains(QStringLiteral("factor.T1.sandthick")));
  QVERIFY(ids.contains(QStringLiteral("factor.T1.poro")));
  QVERIFY(!ids.contains(QStringLiteral("contours.T1.sandthick")));
  QVERIFY(!ids.contains(QStringLiteral("contours.T1.poro")));

  // 融合按钮勾选收集（既有语义）不回归（manifest 按 layer_id 排序，首行
  // 是 factor.T1.poro）。
  const QString firstId = list->item(0)->data(Qt::UserRole).toString();
  QVERIFY(firstId.startsWith(QStringLiteral("factor.")));
  list->item(0)->setCheckState(Qt::Checked);
  QSignalSpy spy(&page, &ComposePage::fuseRequested);
  page.findChild<QPushButton *>(QStringLiteral("fuseButton"))->click();
  QCOMPARE(spy.count(), 1);
  QCOMPARE(spy.first().at(0).toStringList(), QStringList{firstId});
}

void ComposePageTests::factorSelectAllAndClearRow()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  LayerManifest manifest(dir.filePath(QStringLiteral("c.sqlite")));
  QVERIFY(manifest.open());
  QgisLayerService layers(nullptr, &manifest);
  QString err;
  QVERIFY(layers.declare(makeDecl(QStringLiteral("factor.T1.a"), QStringLiteral("T1"),
                                  QStringLiteral("raster"), QStringLiteral("04_SingleFactor")), &err));
  QVERIFY(layers.declare(makeDecl(QStringLiteral("factor.T1.b"), QStringLiteral("T1"),
                                  QStringLiteral("raster"), QStringLiteral("04_SingleFactor")), &err));

  ComposePage page(nullptr, &layers);
  auto *list = page.findChild<QListWidget *>(QStringLiteral("factorList"));
  auto *selectAll = page.findChild<QPushButton *>(QStringLiteral("factorSelectAllButton"));
  auto *clear = page.findChild<QPushButton *>(QStringLiteral("factorClearButton"));
  QVERIFY(list && selectAll && clear);

  selectAll->click();
  QSignalSpy spy(&page, &ComposePage::fuseRequested);
  page.findChild<QPushButton *>(QStringLiteral("fuseButton"))->click();
  QCOMPARE(spy.count(), 1);
  QCOMPARE(spy.first().at(0).toStringList().size(), 2);

  clear->click();
  int checked = 0;
  for (int i = 0; i < list->count(); ++i)
    if (list->item(i)->checkState() == Qt::Checked)
      ++checked;
  QCOMPARE(checked, 0);
  // §35 契约（2026-10-12 深化）：清空勾选后按钮禁用并给原因 tooltip——
  // 空清单融合在 workflow 侧必然被拒，不再放行空意图（点击为无操作）。
  auto *fuse = page.findChild<QPushButton *>(QStringLiteral("fuseButton"));
  QVERIFY(!fuse->isEnabled());
  QVERIFY(!fuse->toolTip().isEmpty());
  spy.clear();
  fuse->click();
  QCOMPARE(spy.count(), 0);
}

void ComposePageTests::fuseButtonGatesOnSelectionState()
{
  // 空清单（无工程）→ 禁用 + 「还没有单因素图层」；有清单未勾选 → 禁用 +
  // 「先勾选」；勾选后 → 启用且 tooltip 清空（§35：禁用必带原因）。
  ComposePage empty(nullptr, nullptr);
  auto *fuse = empty.findChild<QPushButton *>(QStringLiteral("fuseButton"));
  QVERIFY(fuse);
  QVERIFY(!fuse->isEnabled());
  QVERIFY(fuse->toolTip().contains(QStringLiteral("单因素图层")));

  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  LayerManifest manifest(dir.filePath(QStringLiteral("g.sqlite")));
  QVERIFY(manifest.open());
  QgisLayerService layers(nullptr, &manifest);
  QString err;
  QVERIFY(layers.declare(makeDecl(QStringLiteral("factor.T1.a"), QStringLiteral("T1"),
                                  QStringLiteral("raster"), QStringLiteral("04_SingleFactor")), &err));
  ComposePage page(nullptr, &layers);
  fuse = page.findChild<QPushButton *>(QStringLiteral("fuseButton"));
  auto *list = page.findChild<QListWidget *>(QStringLiteral("factorList"));
  QVERIFY(fuse && list);
  QVERIFY(!fuse->isEnabled()); // 清单在、未勾选
  QVERIFY(fuse->toolTip().contains(QStringLiteral("勾选")));

  list->item(0)->setCheckState(Qt::Checked);
  QVERIFY(fuse->isEnabled());
  QVERIFY(fuse->toolTip().isEmpty());

  list->item(0)->setCheckState(Qt::Unchecked);
  QVERIFY(!fuse->isEnabled());
  QVERIFY(fuse->toolTip().contains(QStringLiteral("勾选")));
}

void ComposePageTests::referenceAreaListsAndSignals()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  LayerManifest manifest(dir.filePath(QStringLiteral("c.sqlite")));
  QVERIFY(manifest.open());
  QgisLayerService layers(nullptr, &manifest);
  QString err;
  QVERIFY(layers.declare(makeDecl(QStringLiteral("ref.T1.topo"), QStringLiteral("T1"),
                                  QStringLiteral("raster"), QStringLiteral("06_Reference")), &err));
  QVERIFY(layers.declare(makeDecl(QStringLiteral("facies.T1"), QStringLiteral("T1"),
                                  QStringLiteral("vector"), QStringLiteral("05_PaleoMap")), &err));

  ComposePage page(nullptr, &layers);
  auto *area = page.findChild<QWidget *>(QStringLiteral("referenceArea"));
  auto *refs = page.findChild<QListWidget *>(QStringLiteral("referenceList"));
  QVERIFY(area && refs);
  QCOMPARE(refs->count(), 1); // 05_PaleoMap 不入参考图清单
  QCOMPARE(refs->item(0)->data(Qt::UserRole).toString(), QStringLiteral("ref.T1.topo"));

  // 勾选意图：勾 → (layerId, true)；取消 → (layerId, false)。
  QSignalSpy spy(&page, &ComposePage::referenceVisibilityRequested);
  refs->item(0)->setCheckState(Qt::Checked);
  QCOMPARE(spy.count(), 1);
  QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("ref.T1.topo"));
  QCOMPARE(spy.at(0).at(1).toBool(), true);
  refs->item(0)->setCheckState(Qt::Unchecked);
  QCOMPARE(spy.count(), 2);
  QCOMPARE(spy.at(1).at(0).toString(), QStringLiteral("ref.T1.topo"));
  QCOMPARE(spy.at(1).at(1).toBool(), false);

  // 声明落地（layerDeclared）→ 清单即时刷新（06_Reference 新行出现）。
  QVERIFY(layers.declare(makeDecl(QStringLiteral("ref.T1.bathy"), QStringLiteral("T1"),
                                  QStringLiteral("vector"), QStringLiteral("06_Reference")), &err));
  QCOMPARE(refs->count(), 2);
}

void ComposePageTests::faciesAttrAreaSavesPayload()
{
  ComposePage page(nullptr, nullptr);
  auto *area = page.findChild<QWidget *>(QStringLiteral("faciesAttrArea"));
  auto *target = page.findChild<QLabel *>(QStringLiteral("faciesTargetLabel"));
  auto *code = page.findChild<QLineEdit *>(QStringLiteral("faciesCodeEdit"));
  auto *type = page.findChild<QLineEdit *>(QStringLiteral("faciesTypeEdit"));
  auto *comment = page.findChild<QLineEdit *>(QStringLiteral("faciesCommentEdit"));
  auto *save = page.findChild<QPushButton *>(QStringLiteral("faciesAttrSaveButton"));
  QVERIFY(area && target && code && type && comment && save);

  // 无目标层：禁用 + reason tooltip（DESIGN.md），点击不发信号。
  QVERIFY(!save->isEnabled());
  QVERIFY2(!save->toolTip().isEmpty(), "disabled save needs a reason tooltip");
  QSignalSpy spy(&page, &ComposePage::faciesAttributesSaveRequested);
  save->click();
  QCOMPARE(spy.count(), 0);

  // 壳指名目标层（矢量化成功后 setFaciesEditTarget）→ 开闸 + 目标可见。
  page.setFaciesEditTarget(QStringLiteral("facies.T1"));
  QVERIFY(save->isEnabled());
  QVERIFY(save->toolTip().isEmpty());
  QVERIFY(target->text().contains(QStringLiteral("facies.T1")));

  // 非整数相代码：不发信号，状态区写原因。
  code->setText(QStringLiteral("abc"));
  save->click();
  QCOMPARE(spy.count(), 0);
  QVERIFY(page.findChild<QLabel *>(QStringLiteral("statusLabel"))
              ->text()
              .contains(QStringLiteral("整数")));

  // 合法载荷：layerId + 三字段。
  code->setText(QStringLiteral("3"));
  type->setText(QStringLiteral("辫状河三角洲"));
  comment->setText(QStringLiteral("备注"));
  save->click();
  QCOMPARE(spy.count(), 1);
  QCOMPARE(spy.first().at(0).toString(), QStringLiteral("facies.T1"));
  const QVariantMap attrs = spy.first().at(1).toMap();
  QCOMPARE(attrs.value(QStringLiteral("facies_code")), QVariant(3));
  QCOMPARE(attrs.value(QStringLiteral("facies_type")).toString(), QStringLiteral("辫状河三角洲"));
  QCOMPARE(attrs.value(QStringLiteral("comment")).toString(), QStringLiteral("备注"));

  // C2（wave/deepen-perf）→ 方向 39：相界类型四类全开——「无」不携带键，
  // 词面/顺序全由词表驱动；选中断层切割 → payload 带 boundary_kind=fault_cut。
  auto *kind = page.findChild<QComboBox *>(QStringLiteral("faciesBoundaryKindCombo"));
  QVERIFY(kind != nullptr);
  QCOMPARE(kind->count(), 1 + BoundarySemantics::activeKinds().size());
  QCOMPARE(kind->itemData(1).toString(), QStringLiteral("conformable"));
  QCOMPARE(kind->itemText(1), QStringLiteral("整合接触")); // 词面 = 词表 title
  QVERIFY(!attrs.contains(QStringLiteral("boundary_kind"))); // 「无」= 不带键
  const int faultIndex = [&kind]() {
    for (int i = 0; i < kind->count(); ++i)
      if (kind->itemData(i).toString() == QLatin1String("fault_cut"))
        return i;
    return -1;
  }();
  QVERIFY(faultIndex > 0);
  kind->setCurrentIndex(faultIndex);
  save->click();
  QCOMPARE(spy.count(), 2);
  QCOMPARE(spy.last().at(1).toMap().value(QStringLiteral("boundary_kind")).toString(),
           QStringLiteral("fault_cut"));
}

void ComposePageTests::faciesTargetAdoptsSoleDeclaredFacies()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  LayerManifest manifest(dir.filePath(QStringLiteral("c.sqlite")));
  QVERIFY(manifest.open());
  QgisLayerService layers(nullptr, &manifest);
  QString err;
  QVERIFY(layers.declare(makeDecl(QStringLiteral("facies.T1"), QStringLiteral("T1"),
                                  QStringLiteral("vector"), QStringLiteral("05_PaleoMap")), &err));

  ComposePage page(nullptr, &layers);
  auto *save = page.findChild<QPushButton *>(QStringLiteral("faciesAttrSaveButton"));
  auto *target = page.findChild<QLabel *>(QStringLiteral("faciesTargetLabel"));
  QVERIFY(save && target);
  QVERIFY(save->isEnabled()); // 唯一 facies.* 声明 → 自动成目标
  QVERIFY(target->text().contains(QStringLiteral("facies.T1")));

  // 显式指名优先于自动采纳：换目标后标签跟随。
  page.setFaciesEditTarget(QStringLiteral("facies.T2"));
  QVERIFY(target->text().contains(QStringLiteral("facies.T2")));
}

// opt-05：编图上下文层位（壳喂 activeHorizon）非空时，因子表/相栅格下拉/
// 相属性目标只收本层位声明——跨层位因子勾不进本层位产物，相属性/边界核查
// 不落到别的层位图层上。
void ComposePageTests::faciesTargetAndListsRespectComposeHorizon()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  LayerManifest manifest(dir.filePath(QStringLiteral("h.sqlite")));
  QVERIFY(manifest.open());
  QgisLayerService layers(nullptr, &manifest);
  QString err;
  QVERIFY(layers.declare(makeDecl(QStringLiteral("factor.T1.sand"), QStringLiteral("T1"),
                                  QStringLiteral("raster"), QStringLiteral("04_SingleFactor")), &err));
  QVERIFY(layers.declare(makeDecl(QStringLiteral("factor.T2.sand"), QStringLiteral("T2"),
                                  QStringLiteral("raster"), QStringLiteral("04_SingleFactor")), &err));
  QVERIFY(layers.declare(makeDecl(QStringLiteral("composite.T1"), QStringLiteral("T1"),
                                  QStringLiteral("raster"), QStringLiteral("03_Composite")), &err));
  QVERIFY(layers.declare(makeDecl(QStringLiteral("composite.T2"), QStringLiteral("T2"),
                                  QStringLiteral("raster"), QStringLiteral("03_Composite")), &err));
  QVERIFY(layers.declare(makeDecl(QStringLiteral("facies.T1"), QStringLiteral("T1"),
                                  QStringLiteral("vector"), QStringLiteral("05_PaleoMap")), &err));
  QVERIFY(layers.declare(makeDecl(QStringLiteral("facies.T2"), QStringLiteral("T2"),
                                  QStringLiteral("vector"), QStringLiteral("05_PaleoMap")), &err));

  ComposePage page(nullptr, &layers);
  auto *list = page.findChild<QListWidget *>(QStringLiteral("factorList"));
  auto *combo = page.findChild<QComboBox *>(QStringLiteral("faciesRasterCombo"));
  auto *save = page.findChild<QPushButton *>(QStringLiteral("faciesAttrSaveButton"));
  auto *target = page.findChild<QLabel *>(QStringLiteral("faciesTargetLabel"));
  auto *qa = page.findChild<QPushButton *>(QStringLiteral("faciesBoundaryQaButton"));
  QVERIFY(list && combo && save && target && qa);

  // 未喂层位：旧语义不滤——两条 facies 候选无唯一兜底，目标空。
  QCOMPARE(list->count(), 2);
  QCOMPARE(combo->count(), 2);
  QVERIFY(!save->isEnabled());

  // 喂 T1：清单只收 T1 候选；facies.T1 成本层位唯一声明自动成目标。
  page.setComposeHorizon(QStringLiteral("T1"));
  QCOMPARE(list->count(), 1);
  QCOMPARE(list->item(0)->data(Qt::UserRole).toString(),
           QStringLiteral("factor.T1.sand"));
  QCOMPARE(combo->count(), 1);
  QCOMPARE(combo->itemData(0).toString(), QStringLiteral("composite.T1"));
  QVERIFY(save->isEnabled());
  QVERIFY(target->text().contains(QStringLiteral("facies.T1")));

  // 边界核查意图落在本层位图层（写错层位防护链的页内证据）。
  QSignalSpy qaSpy(&page, &ComposePage::boundaryQaRequested);
  qa->click();
  QCOMPARE(qaSpy.count(), 1);
  QCOMPARE(qaSpy.first().at(0).toString(), QStringLiteral("facies.T1"));

  // 切到 T2：目标随层位迁移，T1 图层不再可写。
  page.setComposeHorizon(QStringLiteral("T2"));
  QCOMPARE(list->count(), 1);
  QCOMPARE(list->item(0)->data(Qt::UserRole).toString(),
           QStringLiteral("factor.T2.sand"));
  QVERIFY(target->text().contains(QStringLiteral("facies.T2")));
  qa->click();
  QCOMPARE(qaSpy.count(), 2);
  QCOMPARE(qaSpy.last().at(0).toString(), QStringLiteral("facies.T2"));

  // 显式跨层位指名在下一次声明刷新被重判收回——壳侧只对本层位作业指名，
  // 但即使别处喂错，声明集仍会把它校正回本层位唯一候选。
  page.setFaciesEditTarget(QStringLiteral("facies.T1"));
  QVERIFY(target->text().contains(QStringLiteral("facies.T1")));
  page.refreshFactors();
  QVERIFY(target->text().contains(QStringLiteral("facies.T2")));
}

void ComposePageTests::openDesignerButtonEmitsSignal()
{
  ComposePage page(nullptr, nullptr);
  auto *btn = page.findChild<QPushButton *>(QStringLiteral("openDesignerButton"));
  QVERIFY(btn);
  QCOMPARE(btn->text(), QStringLiteral("在布局设计器中打开"));
  QSignalSpy spy(&page, &ComposePage::layoutDesignerRequested);
  btn->click();
  QCOMPARE(spy.count(), 1);
}

// ---- X10/X14（智能编图页证据区）：回退按钮 + 归因可定位对象 + 激活载荷 ----
// 与上面三类同一约定：面板只发意图信号；页面构造需要真 MappingWorkbench
// （ctor 取 localOperator/查询声明），配空清单图层服务即可。
class WorkbenchPageTests : public QObject
{
  Q_OBJECT
  private slots:
    void evidenceBackButtonReflectsCanGoBack();
    void attributionTargetsListAndActivationPayload();
    void faciesDraftSurvivesNavigationAndExternalRefresh();
    void propertyParametersFollowSelectedMethodWithoutLosingValues();
};

void WorkbenchPageTests::faciesDraftSurvivesNavigationAndExternalRefresh()
{
  QTemporaryDir dir;
  LayerManifest manifest(dir.filePath(QStringLiteral("draft.sqlite")));
  QVERIFY(manifest.open());
  QgisLayerService layers(nullptr, &manifest);
  MappingWorkbench workbench(&layers, nullptr, nullptr, nullptr);
  MappingWorkbenchPage page(QStringLiteral("compose"), &workbench);
  page.setHorizon(QStringLiteral("C3"));
  auto *table = page.findChild<QTableWidget *>(QStringLiteral("faciesSchema"));
  auto *tree = page.findChild<QTreeWidget *>(QStringLiteral("faciesHierarchyTree"));
  QVERIFY(table && tree && table->rowCount() > 1);
  const int originalRows = table->rowCount();
  table->item(0, 1)->setText(QStringLiteral("未保存的微相"));
  table->removeRow(originalRows - 1);
  QCoreApplication::processEvents();
  QSignalSpy resets(tree->model(), &QAbstractItemModel::modelReset);
  page.setHorizon(QStringLiteral("D61"));
  QTRY_COMPARE(resets.count(), 1);
  QCOMPARE(table->rowCount(), originalRows);
  page.setHorizon(QStringLiteral("C3"));
  QCOMPARE(table->rowCount(), originalRows - 1);
  QCOMPARE(table->item(0, 1)->text(), QStringLiteral("未保存的微相"));
  emit workbench.faciesChanged(QStringLiteral("C3"));
  QCOMPARE(table->item(0, 1)->text(), QStringLiteral("未保存的微相"));
  emit workbench.catalogBound();
  QCOMPARE(table->rowCount(), originalRows);
  QVERIFY(table->item(0, 1)->text() != QStringLiteral("未保存的微相"));
}

void WorkbenchPageTests::propertyParametersFollowSelectedMethodWithoutLosingValues()
{
  PropertyModelPanel panel;
  auto *method = panel.findChild<QComboBox *>(QStringLiteral("propMethodCombo"));
  auto *sgs = panel.findChild<QGroupBox *>(QStringLiteral("propSgsGroup"));
  auto *objects = panel.findChild<QGroupBox *>(QStringLiteral("propObjectGroup"));
  auto *power = panel.findChild<QDoubleSpinBox *>(QStringLiteral("propPowerSpin"));
  auto *width = panel.findChild<QDoubleSpinBox *>(QStringLiteral("propObjectWidthSpin"));
  QVERIFY(method && sgs && objects && power && width);
  QVERIFY(sgs->isHidden()); QVERIFY(width->isHidden());
  power->setValue(3.5);
  method->setCurrentIndex(method->findData(1));
  QVERIFY(!sgs->isHidden()); QVERIFY(power->isHidden());
  objects->setChecked(true);
  QVERIFY(!width->isHidden());
  width->setValue(315.0);
  objects->setChecked(false); objects->setChecked(true);
  QCOMPARE(width->value(), 315.0);
  method->setCurrentIndex(method->findData(0));
  QCOMPARE(power->value(), 3.5);
  QVERIFY(sgs->isHidden()); QVERIFY(!power->isHidden());
}

void WorkbenchPageTests::evidenceBackButtonReflectsCanGoBack()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  LayerManifest manifest(dir.filePath(QStringLiteral("smoke.sqlite")));
  QVERIFY(manifest.open());
  QgisLayerService layers(nullptr, &manifest);
  MappingWorkbench workbench(&layers, nullptr, nullptr, nullptr);
  MappingWorkbenchPage page(QStringLiteral("compose"), &workbench);
  auto *back = page.findChild<QPushButton *>(QStringLiteral("evidenceBack"));
  QVERIFY(back);
  QVERIFY(!back->isEnabled()); // 初始无回退栈
  QVERIFY(!back->toolTip().isEmpty()); // DESIGN.md：禁用带 reason
  page.setCanGoBack(true);
  QVERIFY(back->isEnabled());
  QSignalSpy spy(&page, &MappingWorkbenchPage::evidenceBackRequested);
  back->click();
  QCOMPARE(spy.count(), 1);
  page.setCanGoBack(false);
  QVERIFY(!back->isEnabled());
}

void WorkbenchPageTests::attributionTargetsListAndActivationPayload()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  LayerManifest manifest(dir.filePath(QStringLiteral("smoke.sqlite")));
  QVERIFY(manifest.open());
  QgisLayerService layers(nullptr, &manifest);
  MappingWorkbench workbench(&layers, nullptr, nullptr, nullptr);
  MappingWorkbenchPage page(QStringLiteral("compose"), &workbench);
  auto *targets = page.findChild<QListWidget *>(QStringLiteral("attributionTargets"));
  QVERIFY(targets);
  QVERIFY(!targets->accessibleName().isEmpty());
  const QVariantMap snapshot{
      {"feature_uid", "uid-A"}, {"horizon", "D61"},
      {"interpretation",
       QVariantMap{{"id", "i-1"}, {"status", "Proposed"}, {"revision", 1}}},
      {"wells_in_feature",
       QVariantList{QVariantMap{{"well_name", "W-1"}, {"entity_id", "well-W-1"},
                                {"x", 10.5}, {"y", 20.5}}}},
      {"conflicts",
       QVariantList{
           QVariantMap{{"severity", "Warning"},
                       {"conflict_type", "minority_opinion"},
                       {"message", "井相与图斑相不同"},
                       {"evidence",
                        QVariantMap{{"label", "井 W-2"},
                                    {"locator",
                                     QVariantMap{{"entity_id", "well-W-2"},
                                                 {"x", 1.0}, {"y", 2.0},
                                                 {"has_coordinates", true}}}}}},
           // 无定位信息的冲突不产可定位行——不可定位的事实不虚构目标。
           QVariantMap{{"severity", "Info"},
                       {"conflict_type", "minority_opinion"},
                       {"message", "无定位"},
                       {"evidence", QVariantMap{{"label", "预测"}}}}}}};
  page.setEvidenceSnapshot(snapshot);
  QCOMPARE(targets->count(), 2);
  QVERIFY(targets->item(0)->text().contains(QStringLiteral("W-1")));
  QSignalSpy spy(&page, &MappingWorkbenchPage::evidenceActivated);
  emit targets->itemActivated(targets->item(0));
  QCOMPARE(spy.count(), 1);
  const auto wellRow = spy.first().at(0).toMap();
  QCOMPARE(wellRow.value("entity_id").toString(), QStringLiteral("well-W-1"));
  QCOMPARE(wellRow.value("has_coordinates").toBool(), true);
  QCOMPARE(wellRow.value("x").toDouble(), 10.5);
  QCOMPARE(wellRow.value("y").toDouble(), 20.5);
  QCOMPARE(wellRow.value("horizon").toString(), QStringLiteral("D61"));
  // 带定位冲突行：按实体坐标导航（Navigate 走井口坐标分支）。
  emit targets->itemActivated(targets->item(1));
  QCOMPARE(spy.count(), 2);
  QCOMPARE(spy.at(1).at(0).toMap().value("entity_id").toString(),
           QStringLiteral("well-W-2"));
}

// 多测试类单可执行体：QTEST_MAIN 只支持单类，这里手动跑四个类。
// QgsApplication 引导（tst_workflows 同款）：页面图层档案段已并入 m1 实装的
// tst_layerplatform（applyPageProfile 语义不同），本文件不再跑档案用例；
// 保留 QgsApplication 引导以防页面类触达 QGIS 栈。
int main(int argc, char *argv[])
{
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")), true); // distro install
  app.initQgis();

  // add_paleo_test 传入单一 -o 路径；多 qExec 会互相覆盖。每类跑完后另存一份
  // （.<Class> 后缀），并把各类输出按序累积，全部跑完后整体回写 -o 主文件——
  // 主文件即合并结果，triage/grep 主 txt 也能看到 Predict/Factor 的 FAIL! 行
  // （review-fix-ci：旧实现主文件只剩最后一类 WorkbenchPageTests，失败被掩盖）。
  QString sharedOut;
  for (int i = 1; i + 1 < argc; ++i) {
    if (qstrcmp(argv[i], "-o") == 0) {
      sharedOut = QString::fromLocal8Bit(argv[i + 1]).section(QLatin1Char(','), 0, 0);
      break;
    }
  }
  QByteArray merged;
  const auto runClass = [&](QObject &t, const char *tag) {
    const int code = QTest::qExec(&t, argc, argv);
    if (!sharedOut.isEmpty() && QFile::exists(sharedOut)) {
      const QString tagged = sharedOut + QLatin1Char('.') + QString::fromLatin1(tag);
      QFile::remove(tagged);
      QFile::copy(sharedOut, tagged);
      QFile f(sharedOut);
      if (f.open(QIODevice::ReadOnly))
        merged += f.readAll();
    }
    return code;
  };

  int status = 0;
  {
    PredictPageTests t;
    status |= runClass(t, "PredictPageTests");
  }
  {
    FactorPageTests t;
    status |= runClass(t, "FactorPageTests");
  }
  {
    ComposePageTests t;
    status |= runClass(t, "ComposePageTests");
  }
  {
    WorkbenchPageTests t;
    status |= runClass(t, "WorkbenchPageTests");
  }
  if (!sharedOut.isEmpty()) {
    QFile f(sharedOut);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
      f.write(merged);
  }
  QgsApplication::exitQgis();
  return status;
}

#include "tst_mappingpages.moc"
