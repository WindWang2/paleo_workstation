#include <QtTest>
#include <QApplication>
#include <QComboBox>
#include <QListWidget>
#include <QTableWidget>

#include "../src/ui/pages/pagepanels.h"

// m2/mapping-pages — 三个编图页（预测/单因素/智能编图）的贯通测试。
// 约定与 tst_panels 相同：面板只发意图信号，测试用 nullptr/null 服务栈 +
// QSignalSpy + objectName 子件查找（plain QApplication，无 QisRuntime）。
// 文件内三个测试类共用一个 main（多类 qExec）；每页一个类，各任务段
// 只改自己的类：
//   PredictPageTests   —— 预测编图页（任务 A）
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
#include "../src/qgis/qgislayerservice.h"
#include "../src/services/algoparamschema.h"

class PredictPageTests : public QObject
{
  Q_OBJECT
  private slots:
    void constructsWithNullServices(); // 基座冒烟：拆分后类仍可构造
    void predictTypeVocabulary();      // m2(A)：预测类型词表 + stable id
    void schemaRegistryMirrorsAlgorithmTruth(); // m2(A)：schema 注册表对照算法真值
    void schemaFormBuildsPerAlgorithm();        // m2(A)：表单按 schema 动态建控件
    void schemaParamsCollectedIntoRunRequest(); // m2(A)：schema 控件 → QVariantMap
    void predictTypeSwitchRebuildsForm();       // m2(A)：类型切换联动重建（值保留）
    void runBusyLifecycleAndCancel();           // m2(A)：任务化忙碌/取消/进度
    void historyListFiltersAndShows();          // m2(A)：历史清单过滤/显示/刷新

  private:
    static LayerDeclaration makeDecl(const QString &layerId, const QString &horizon,
                                     const QString &group);
};

void PredictPageTests::constructsWithNullServices()
{
  PredictPage page(nullptr, nullptr);
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
  QCOMPARE(polygonize.size(), 4);
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

class FactorPageTests : public QObject
{
  Q_OBJECT
  private slots:
    void constructsWithNullServices(); // 基座冒烟：拆分后类仍可构造
};

void FactorPageTests::constructsWithNullServices()
{
  ConstraintPage page(nullptr);
  QVERIFY(page.findChild<QTableWidget *>(QStringLiteral("thicknessTable")));
}

class ComposePageTests : public QObject
{
  Q_OBJECT
  private slots:
    void constructsWithNullServices(); // 基座冒烟：拆分后类仍可构造
};

void ComposePageTests::constructsWithNullServices()
{
  ComposePage page(nullptr, nullptr);
  QVERIFY(page.findChild<QListWidget *>(QStringLiteral("factorList")));
}

// 多测试类单可执行体：QTEST_MAIN 只支持单类，这里手动跑三个类。
int main(int argc, char *argv[])
{
  QApplication app(argc, argv);
  int status = 0;
  {
    PredictPageTests t;
    status |= QTest::qExec(&t, argc, argv);
  }
  {
    FactorPageTests t;
    status |= QTest::qExec(&t, argc, argv);
  }
  {
    ComposePageTests t;
    status |= QTest::qExec(&t, argc, argv);
  }
  return status;
}

#include "tst_mappingpages.moc"
