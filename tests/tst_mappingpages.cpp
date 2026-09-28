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
#include <QToolButton>
#include <QVariantMap>

#include "../src/services/singlefactordef.h"
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

#include <qgsapplication.h>

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
    void runBusyLifecycleAndCancel();
    void busyStateSurvivesPageHideShow();
    void schemaValidationFailureRefusesRun();           // m2(A)：任务化忙碌/取消/进度
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

void PredictPageTests::busyStateSurvivesPageHideShow()
{
  PredictPage page(nullptr, nullptr);
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
  QCOMPARE( factors->rowCount(), 7 ); // 词表行
  QCOMPARE( factors->columnCount(), 4 ); // 勾选/名称/输入/状态
  QCOMPARE( factors->item( 0, 1 )->text(), QStringLiteral( "砂体厚度" ) );
  QCOMPARE( factors->item( 0, 2 )->text(), QStringLiteral( "wells" ) );
  QCOMPARE( factors->item( 0, 3 )->text(), QStringLiteral( "未生成" ) );
  auto *field = page.findChild<QLineEdit *>(QStringLiteral("factorFieldEdit"));
  auto *cell = page.findChild<QDoubleSpinBox *>(QStringLiteral("factorCellSizeSpin"));
  auto *generate = page.findChild<QPushButton *>(QStringLiteral("generateFactorButton"));
  auto *interval = page.findChild<QDoubleSpinBox *>(QStringLiteral("contourIntervalSpin"));
  auto *contour = page.findChild<QPushButton *>(QStringLiteral("contourButton"));
  QVERIFY( field && cell && generate && interval && contour );
  QCOMPARE( field->text(), QStringLiteral( "z" ) );
  QCOMPARE( cell->value(), 1.0 );
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

  // strathick（第 3 行）勾选 → 行展开；payload 携带 topLayerId/baseLayerId。
  factors->item( 2, 0 )->setCheckState( Qt::Checked );
  QVERIFY( surfaceRow->isVisibleTo( &page ) );
  QSignalSpy spy( &page, &ConstraintPage::generateFactorRequested );
  page.findChild<QPushButton *>( QStringLiteral( "generateFactorButton" ) )->click();
  QCOMPARE( spy.count(), 1 );
  const QVariantMap params = spy.at( 0 ).at( 2 ).toMap();
  QVERIFY( params.contains( QStringLiteral( "topLayerId" ) ) );
  QVERIFY( params.contains( QStringLiteral( "baseLayerId" ) ) );
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
  auto *factors = page.findChild<QTableWidget *>(QStringLiteral("factorTable"));
  auto *interval = page.findChild<QDoubleSpinBox *>(QStringLiteral("contourIntervalSpin"));
  auto *contour = page.findChild<QPushButton *>(QStringLiteral("contourButton"));
  QVERIFY( !contour->isEnabled() );

  // 勾选但未生成 → 仍禁用，tooltip 说明原因。
  factors->item( 0, 0 )->setCheckState( Qt::Checked );
  QVERIFY( !contour->isEnabled() );
  QVERIFY2( contour->toolTip().contains( QStringLiteral( "生成" ) ),
            "reason tooltip should say why (not generated yet)" );

  page.noteFactorLayer( QStringLiteral( "sandthick" ), QStringLiteral( "factor.T1.sandthick" ) );
  QVERIFY( contour->isEnabled() );
  interval->setValue( 15.0 );
  QSignalSpy spy( &page, &ConstraintPage::contourRequested );
  contour->click();
  QCOMPARE( spy.count(), 1 );
  QCOMPARE( spy.at( 0 ).at( 0 ).toString(), QStringLiteral( "factor.T1.sandthick" ) );
  QCOMPARE( spy.at( 0 ).at( 1 ).toDouble(), 15.0 );

  // 状态列回执：noteFactorLayer 后写「已生成·<layerId>」。
  QCOMPARE( factors->item( 0, 3 )->text(), QStringLiteral( "已生成·factor.T1.sandthick" ) );
}

void FactorPageTests::thicknessSamplesInsideCollapsibleSection()
{
  ConstraintPage page( nullptr );
  // 折叠区在（默认展开），三个 objectName 仍可 findChild 命中（递归无关）。
  auto *section = page.findChild<QWidget *>(QStringLiteral("thicknessSection"));
  auto *table = page.findChild<QTableWidget *>(QStringLiteral("thicknessTable"));
  auto *caption = page.findChild<QLabel *>(QStringLiteral("thicknessCaption"));
  auto *hint = page.findChild<QLabel *>(QStringLiteral("thicknessHint"));
  QVERIFY( section && table && caption && hint );
  QVERIFY( table->isVisibleTo( section ) ); // 默认展开（不依赖整页 show）
  // 厚度表确实在折叠区容器内（挪进了 CollapsibleSection）。
  QVERIFY( table->parentWidget() != &page );
  QCOMPARE( table->columnCount(), 4 );
  QCOMPARE( caption->text().contains( QStringLiteral( "厚度样本" ) ), true );

  // 折叠后仍能 findChild（objectName 不因折叠失效），且表隐藏。
  auto *toggle = section->findChild<QToolButton *>();
  QVERIFY( toggle != nullptr );
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
  auto *provenance = page.findChild<QPushButton *>(QStringLiteral("provenanceButton"));
  auto *distribution = page.findChild<QPushButton *>(QStringLiteral("distributionButton"));
  auto *controlPoint = page.findChild<QPushButton *>(QStringLiteral("controlPointButton"));
  QVERIFY( provenance && distribution && controlPoint );

  provenance->click();
  distribution->click();
  controlPoint->click();
  QCOMPARE( spy.count(), 3 );
  QCOMPARE( spy.at( 0 ).at( 0 ).toString(), QStringLiteral( "D61" ) );
  QCOMPARE( spy.at( 0 ).at( 1 ).toString(), QStringLiteral( "line" ) );
  QCOMPARE( spy.at( 0 ).at( 2 ).toString(), QStringLiteral( "provenance_line" ) );
  QCOMPARE( spy.at( 0 ).at( 3 ).toInt(), 7 );
  QCOMPARE( spy.at( 1 ).at( 2 ).toString(), QStringLiteral( "distribution_line" ) );
  QCOMPARE( spy.at( 2 ).at( 1 ).toString(), QStringLiteral( "point" ) );
  QCOMPARE( spy.at( 2 ).at( 2 ).toString(), QStringLiteral( "control_point" ) );

  // 旧绘制链共存（drawButton → drawConstraintRequested，tst_panels 详测，此处冒烟）。
  QSignalSpy legacy( &page, &ConstraintPage::drawConstraintRequested );
  page.findChild<QPushButton *>(QStringLiteral("drawButton"))->click();
  QCOMPARE( legacy.count(), 1 );
}

// ---- 任务 C（智能编图页）：融合清单栅格过滤/参考图/相属性/设计器入口。--
// 真实栈（属性回写 edit buffer、壳自动编辑态）在 tst_composeworkflow.cpp。
class ComposePageTests : public QObject
{
  Q_OBJECT
  private slots:
    void constructsWithNullServices(); // 基座冒烟：拆分后类仍可构造
    void fusionListListsOnlyRasterFactors(); // m2(C)：contours 矢量不入融合清单
    void factorSelectAllAndClearRow();       // m2(C)：全选/清空小工具行
    void referenceAreaListsAndSignals();     // m2(C)：06_Reference 清单 + 可见性意图
    void faciesAttrAreaSavesPayload();       // m2(C)：相属性三字段 + 保存信号载荷
    void faciesTargetAdoptsSoleDeclaredFacies(); // m2(C)：唯一 facies.* 自动成目标
    void openDesignerButtonEmitsSignal();    // m2(C)：布局设计器入口信号

  private:
    static LayerDeclaration makeDecl(const QString &layerId, const QString &horizon,
                                     const QString &type, const QString &group);
};

void ComposePageTests::constructsWithNullServices()
{
  ComposePage page(nullptr, nullptr);
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
  spy.clear();
  page.findChild<QPushButton *>(QStringLiteral("fuseButton"))->click();
  QCOMPARE(spy.count(), 1);
  QVERIFY(spy.first().at(0).toStringList().isEmpty());
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

// 多测试类单可执行体：QTEST_MAIN 只支持单类，这里手动跑三个类。
// QgsApplication 引导（tst_workflows 同款）：页面图层档案段已并入 m1 实装的
// tst_layerplatform（applyPageProfile 语义不同），本文件不再跑档案用例；
// 保留 QgsApplication 引导以防页面类触达 QGIS 栈。
int main(int argc, char *argv[])
{
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(QStringLiteral("/usr"), true); // distro install
  app.initQgis();
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
  QgsApplication::exitQgis();
  return status;
}

#include "tst_mappingpages.moc"
