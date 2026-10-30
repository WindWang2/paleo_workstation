// 层：视图
// constraintpage_generate — 生成动作域 TU（方向 96 自 constraintpage.cpp 拆出，
// 原文行序保持）：生成/取消按钮 + 生成 handler（参数装配）、等值线间距/
// 来源/级别段，及动作可用性状态机 updateFactorActionStates 与忙态
// setRunBusy/noteRunStage。
#include "constraintpage.h"
#include "constraintpage_internal.h"

#include "pageshared.h"
#include "../../linkage/selectioncontext.h"

#include "../../domain/singlefactorstrategy.h" // 方向67：strategy_id 血缘（生成 handler 查策略包）
#include "../../services/singlefactordef.h"
#include "../../workflow/workflows.h"
#include <QHeaderView>
#include <QProgressBar>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QFileInfo>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QScrollArea>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>

using namespace paleo::pagesinternal;
using namespace paleo::constraintpage_internal;

void ConstraintPage::buildGenerateSection( QWidget *content, QVBoxLayout *lay )
{
  PaleoTheme::applyThemedStyleSheet(content, [] {
    const auto &t = PaleoTheme::tokens();
    return QStringLiteral("QWidget#factorActionBar { background: %1; border-top: 1px solid %2; }")
        .arg(t.surface.name(), t.border.name());
  });
  auto *summary = caption(QString(), content);
  summary->setObjectName(QStringLiteral("factorRequestSummary"));
  summary->setWordWrap(true);
  lay->addWidget(summary);
  auto *guidanceRow = new QHBoxLayout;
  auto *guidance = new QLabel(content);
  guidance->setObjectName(QStringLiteral("factorReadinessHint"));
  guidance->setWordWrap(true);
  guidance->setTextFormat(Qt::PlainText);
  guidance->setAccessibleName(tr("单因素生成下一步"));
  guidanceRow->addWidget(guidance, 1);
  auto *locate = new QPushButton(tr("去准备"), content);
  locate->setObjectName(QStringLiteral("factorNextStepButton"));
  guidanceRow->addWidget(locate);
  lay->addLayout(guidanceRow);
  connect(locate, &QPushButton::clicked, this, [this, locate] {
    auto *target = findChild<QWidget *>(locate->property("target").toString());
    auto *scroll = child<QScrollArea>(this, "constraintPageScroll");
    if (!target || !scroll)
      return;
    for (QWidget *ancestor = target->parentWidget(); ancestor && ancestor != this;
         ancestor = ancestor->parentWidget())
      if (auto *section = dynamic_cast<CollapsibleSection *>(ancestor))
        section->setExpanded(true);
    QTimer::singleShot(0, target, [scroll, target] {
      scroll->ensureWidgetVisible(target, 0, PaleoTheme::tokens().spacingMd);
      if (target->focusPolicy() != Qt::NoFocus)
        target->setFocus(Qt::OtherFocusReason);
    });
  });
  auto *actions = new QHBoxLayout;
  actions->setSpacing(PaleoTheme::tokens().spacingXs);
  // 本页主操作（DESIGN.md primary 三用途之一）：生成单因素图突出；
  // 其余动作保持常规面并按组分隔。
  // 反馈 #19：一键默认路径「提取并生成」= 提取井点 → 生成单因素图，逐步按钮保留。
  auto *oneClick = new QPushButton(tr("生成等值图"), content);
  oneClick->setObjectName(QStringLiteral("factorOneClickButton"));
  oneClick->setEnabled(false);
  oneClick->setAccessibleName(tr("按推荐或当前参数生成等值图"));
  markPrimaryButton(oneClick); actions->addWidget(oneClick, 1);
  connect(oneClick, &QPushButton::clicked, this, &ConstraintPage::startOneClick);
  auto *generate = new QPushButton(tr("生成等值图"), this);
  generate->setObjectName( QStringLiteral( "generateFactorButton" ) );
  generate->setEnabled( false ); // 先勾选一个单因素（updateFactorActionStates 管 tooltip）
  markPrimaryButton( generate );
  actions->addWidget( generate, 1 ); generate->hide();
  auto *cancel = new QPushButton( tr( "取消" ), content );
  cancel->setObjectName( QStringLiteral( "factorCancelButton" ) );
  cancel->setEnabled( false );
  cancel->setToolTip( tr( "当前没有正在运行的单因素任务" ) );
  actions->addWidget( cancel );
  cancel->hide();
  connect( cancel, &QPushButton::clicked, this, &ConstraintPage::runCancelRequested );
  auto *tasks = new QPushButton(tr("任务中心"), content);
  tasks->setObjectName(QStringLiteral("factorTaskCenterButton"));
  tasks->setToolTip(tr("查看计算进度、任务日志与失败原因"));
  actions->addWidget(tasks);
  lay->addLayout(actions);
  connect(tasks, &QPushButton::clicked, this, &ConstraintPage::taskCenterRequested);
  auto *progress = new QProgressBar(content);
  progress->setObjectName(QStringLiteral("factorProgressBar"));
  progress->setRange(0, 100);
  progress->setValue(0);
  progress->setVisible(false);
  progress->setAccessibleName(tr("单因素计算进度"));
  lay->addWidget(progress);

  connect( generate, &QPushButton::clicked, this, [this] {
    // 方向 96：原 ctor 期捕获的 22 个控件改事件期 findChild（objectName 全部
    // 命中、控件终身存活，解析时机等价；null 容忍分支与原文一致）。
    auto *horizons = findChild<QComboBox *>( QStringLiteral( "horizonCombo" ) );
    auto *field = findChild<QLineEdit *>( QStringLiteral( "factorFieldEdit" ) );
    auto *cell = findChild<QDoubleSpinBox *>( QStringLiteral( "factorCellSizeSpin" ) );
    auto *factors = findChild<QTableWidget *>( QStringLiteral( "factorTable" ) );
    auto *topCombo = findChild<QComboBox *>( QStringLiteral( "factorTopSurfaceCombo" ) );
    auto *baseCombo = findChild<QComboBox *>( QStringLiteral( "factorBaseSurfaceCombo" ) );
    auto *method = findChild<QComboBox *>( QStringLiteral( "factorMethodCombo" ) );
    auto *coverage = findChild<QComboBox *>( QStringLiteral( "factorCoverageCombo" ) );
    auto *power = findChild<QDoubleSpinBox *>( QStringLiteral( "factorPowerSpin" ) );
    auto *cluster = findChild<QCheckBox *>( QStringLiteral( "factorClusterCheck" ) );
    auto *variogramModel = findChild<QComboBox *>( QStringLiteral( "factorVariogramModelCombo" ) );
    auto *nugget = findChild<QDoubleSpinBox *>( QStringLiteral( "factorNuggetSpin" ) );
    auto *sill = findChild<QDoubleSpinBox *>( QStringLiteral( "factorSillSpin" ) );
    auto *rangeSpin = findChild<QDoubleSpinBox *>( QStringLiteral( "factorRangeSpin" ) );
    auto *azimuth = findChild<QDoubleSpinBox *>( QStringLiteral( "factorAzimuthSpin" ) );
    auto *maxPoints = findChild<QSpinBox *>( QStringLiteral( "factorKrigingMaxPointsSpin" ) );
    auto *realizations = findChild<QSpinBox *>( QStringLiteral( "factorSgsRealizationsSpin" ) );
    auto *seed = findChild<QSpinBox *>( QStringLiteral( "factorSgsSeedSpin" ) );
    auto *persistRealizations = findChild<QCheckBox *>( QStringLiteral( "factorSgsPersistRealizationsCheck" ) );
    auto *boundary = findChild<QComboBox *>( QStringLiteral( "factorBoundaryCombo" ) );
    auto *gridRes = findChild<QSpinBox *>( QStringLiteral( "factorGridResolutionSpin" ) );
    auto *covariate = findChild<QComboBox *>( QStringLiteral( "factorCovariateCombo" ) );
    auto *crossCorrelation = findChild<QDoubleSpinBox *>( QStringLiteral( "factorCrossCorrelationSpin" ) );
    const int r = checkedRow( factors );
    if ( r < 0 )
      return;
    const QString factorId = factorIdOfRow( factors, r );
    QVariantMap params;
    bool known = false;
    const SingleFactorDefinition def = factorDefinition(factorId, &known);
    params.insert( QStringLiteral( "field" ),
                   field->text().trimmed().isEmpty() && known
                       ? def.defaultParams.value( QStringLiteral( "field" ) )
                       : field->text().trimmed() );
    params.insert( QStringLiteral( "cellSize" ), cell->value() );
    if (known) params.insert(QStringLiteral("valueRange"), def.defaultParams.value(QStringLiteral("valueRange")));
    if (factorId == QLatin1String("strathick"))
      params.insert(QStringLiteral("factorSource"), wellFactorParams().value(QStringLiteral("factorSource")));
    if (known && interpolantEngine(def.processingAlgId)) {
      const QVariantMap extraction = wellFactorParams();
      // auto：用已提取的井点因子（factor_value），不要退回隐藏的字段 z。
      const QString mode = extraction.value(QStringLiteral("factorMode")).toString();
      if (!extraction.value(QStringLiteral("valueField")).toString().isEmpty() ||
          mode == QLatin1String("ratio") || mode == QLatin1String("auto")) {
        for (auto i = extraction.cbegin(); i != extraction.cend(); ++i) params.insert(i.key(), i.value());
      }
    }
    if ( known && interpolantEngine( def.processingAlgId ) )
    {
      const QString methodId = method->currentData().toString();
      if ( methodId != QLatin1String( "legacy" ) )
        params.insert( QStringLiteral( "method" ), methodId );
      // 方向67：所选策略包 id（词表口径）进参数 → 血缘 strategy_id。页面把词表
      // "idw" 显示为 legacy；SGS 不在曲面词表内，不写 strategy_id（不冒充）。
      const QString strategyLookup =
          methodId == QLatin1String( "legacy" ) ? QStringLiteral( "idw" ) : methodId;
      if ( const paleo::singlefactor::SurfaceMethodPack *pack =
               paleo::singlefactor::surfaceMethodPack( strategyLookup ) )
      {
        params.insert( QStringLiteral( "strategy_id" ), pack->id );
      }
      params.insert( QStringLiteral( "coverage" ), coverage->currentData().toString() );
      params.insert( QStringLiteral( "power" ), power->value() );
      params.insert( QStringLiteral( "wellClusterLocality" ), cluster->isChecked() );
      for (const auto &entry : QVector<QPair<QString, QString>>{
           {QStringLiteral("minPoints"), QStringLiteral("factorMinPointsSpin")},
           {QStringLiteral("maxPoints"), QStringLiteral("factorMaxPointsSpin")}})
        params.insert(entry.first, findChild<QSpinBox *>(entry.second)->value());
      params.insert(QStringLiteral("searchRadius"), findChild<QDoubleSpinBox *>(QStringLiteral("factorSearchRadiusSpin"))->value());
      params.insert(QStringLiteral("anisotropyRatio"), findChild<QDoubleSpinBox *>(QStringLiteral("factorAnisotropyRatioSpin"))->value());
      params.insert(QStringLiteral("anisotropyAngle"), findChild<QDoubleSpinBox *>(QStringLiteral("factorAnisotropyAngleSpin"))->value());
      if ( methodId == QLatin1String( "kriging" ) || methodId == QLatin1String( "sgs" ) ||
           methodId == QLatin1String( "local_direction_kriging" ) ||
           methodId == QLatin1String( "cokriging" ) )
      {
        params.insert( QStringLiteral( "variogramModel" ), variogramModel->currentData().toString() );
        params.insert( QStringLiteral( "nugget" ), nugget->value() );
        params.insert( QStringLiteral( "sill" ), sill->value() );
        params.insert( QStringLiteral( "range" ), rangeSpin->value() );
        params.insert( QStringLiteral( "azimuth" ), azimuth->value() );
        if (methodId != QLatin1String("local_direction_kriging"))
          params.insert( QStringLiteral( "maxPoints" ), maxPoints->value() );
        // 方向41：局部方向克里金另有自己的邻域 K（0 = 全部样本）。
        params.insert( QStringLiteral( "krigingMaxPoints" ), maxPoints->value() );
        if ( methodId == QLatin1String( "cokriging" ) )
        {
          params.insert( QStringLiteral( "covariateLayerId" ), covariate->currentData().toString() );
          params.insert( QStringLiteral( "crossCorrelation" ), crossCorrelation->value() );
        }
        if ( methodId == QLatin1String( "sgs" ) )
        {
          params.insert( QStringLiteral( "realizations" ), realizations->value() );
          params.insert( QStringLiteral( "seed" ), seed->value() );
          params.insert( QStringLiteral( "persistRealizations" ),
                         persistRealizations->isChecked() );
        }
      }
      // 结构 IDW：边界图层（必选）+ 格网分辨率；覆盖方式→extendToBoundary。
      if ( methodId == QLatin1String( "structural_idw" ) )
      {
        params.insert( QStringLiteral( "boundaryLayerId" ),
                       boundary ? boundary->currentData().toString() : QString() );
        params.insert( QStringLiteral( "gridResolution" ),
                       gridRes ? gridRes->value() : 339 );
        params.insert( QStringLiteral( "extendToBoundary" ),
                       coverage->currentData().toString()
                           == QLatin1String( "domain_extrapolation" ) );
      }
    }
    // 主线6：等厚引擎参数——顶/底构造面图层随 payload（空即工作流侧拒绝）。
    if ( def.processingAlgId == QLatin1String( "paleo:paleo_isopach" ) )
    {
      params.insert( QStringLiteral( "topLayerId" ), topCombo->currentData().toString() );
      params.insert( QStringLiteral( "baseLayerId" ), baseCombo->currentData().toString() );
    }
    emit generateFactorRequested( factorId, horizons->currentText(), params );
  } );
}

void ConstraintPage::buildContourSection( QWidget *content, QVBoxLayout *lay )
{
  auto *intervalCaption = caption( tr( "等值线间距" ), this );
  intervalCaption->setObjectName(QStringLiteral("factorContourIntervalCaption"));
  lay->addWidget( intervalCaption );
  auto *interval = new QDoubleSpinBox( this );
  interval->setObjectName( QStringLiteral( "contourIntervalSpin" ) );
  // 0 = 自动等值距（structural_idw 因素的上游自适应步长规则）。
  interval->setRange( 0.0, 1.0e9 );
  interval->setSpecialValueText( tr( "自动（按数据自适应步长）" ) );
  interval->setDecimals( 2 );
  interval->setValue( 20.0 );
  interval->setSuffix( tr( " m" ) );
  interval->setAccessibleName( tr( "等值线间距" ) );
  interval->setToolTip( tr( "默认约为当前单因素有效值极差的 1/6，可再改。" ) );
  connect( interval, qOverload<double>( &QDoubleSpinBox::valueChanged ), this, [this]( double ) {
    setProperty( "paleo.page.contourIntervalUser", true );
  } );
  useMono( interval );
  lay->addWidget( interval );

  auto *sourceCaption = caption( tr( "等值线来源" ), content );
  sourceCaption->setObjectName( QStringLiteral( "factorContourSourceCaption" ) );
  lay->addWidget( sourceCaption );
  auto *contourMode = new QComboBox( content );
  contourMode->setObjectName( QStringLiteral( "factorContourModeCombo" ) );
  contourMode->addItem( tr( "真实数值等值线" ), QStringLiteral( "analysis" ) );
  contourMode->addItem( tr( "解释性绕行" ), QStringLiteral( "cartographic_detour" ) );
  contourMode->setAccessibleName( tr( "等值线来源" ) );
  lay->addWidget( contourMode );
  auto *levelsEdit = new QLineEdit( content );
  levelsEdit->setObjectName( QStringLiteral( "factorContourLevelsEdit" ) );
  levelsEdit->setPlaceholderText( tr( "等值级别，例如 10, 20, 30" ) );
  levelsEdit->setAccessibleName( tr( "等值级别" ) );
  useMono( levelsEdit );
  levelsEdit->setVisible( false );
  lay->addWidget( levelsEdit );
  auto *sourceNote = new QLabel( tr( "等值线取自分析场" ), content );
  sourceNote->setObjectName( QStringLiteral( "factorContourSourceLabel" ) );
  sourceNote->setWordWrap( true );
  lay->addWidget( sourceNote );

  auto *algorithmCaption = caption( tr( "等值线算法" ), content );
  algorithmCaption->setObjectName( QStringLiteral( "factorContourAlgorithmCaption" ) );
  lay->addWidget( algorithmCaption );
  auto *algorithm = new QComboBox( content );
  algorithm->setObjectName( QStringLiteral( "factorContourAlgorithmCombo" ) );
  algorithm->setAccessibleName( tr( "等值线算法" ) );
  for ( const paleo::singlefactor::ContourExtractPack &pack :
        paleo::singlefactor::contourExtractPacks() )
  {
    algorithm->addItem( pack.label, pack.id );
    algorithm->setItemData( algorithm->count() - 1, pack.geologicalNote, Qt::ToolTipRole );
  }
  const int partitioned =
      algorithm->findData( QStringLiteral( "partitioned_marching_squares" ) );
  if ( partitioned >= 0 )
    algorithm->setCurrentIndex( partitioned );
  lay->addWidget( algorithm );


  PaleoTheme::applyThemedStyleSheet(sourceNote, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  connect( contourMode, &QComboBox::currentIndexChanged, this, [this, contourMode, levelsEdit, sourceNote, interval, intervalCaption] {
    const bool interpretive = contourMode->currentData().toString() == QLatin1String( "cartographic_detour" );
    levelsEdit->setVisible( interpretive );
    interval->setVisible(!interpretive);
    intervalCaption->setVisible(!interpretive);
    sourceNote->setText( interpretive ? tr( "解释性等值线的值来自制图工作场，不能参与融合、分相或厚度统计" )
                                      : tr( "等值线取自分析场" ) );
    updateFactorActionStates();
  } );
  connect( levelsEdit, &QLineEdit::textChanged, this, [this] { updateFactorActionStates(); } );

  auto *contour = new QPushButton(tr("计算并保存等值线"), content);
  contour->setObjectName( QStringLiteral( "contourButton" ) );
  contour->setEnabled( false ); // 勾选且已生成的因素才有可等值线的栅格
  lay->addWidget( contour );
  auto *hint = caption(QString(), content);
  hint->setObjectName(QStringLiteral("factorContourHint"));
  hint->setWordWrap(true);
  lay->addWidget(hint);
  connect( contour, &QPushButton::clicked, this, [this, interval, contourMode, levelsEdit, algorithm] {
    const QString layerId = checkedFactorLayerId();
    if ( layerId.isEmpty() )
      return;
    if ( contourMode->currentData().toString() == QLatin1String( "cartographic_detour" ) )
    {
      int dropped = 0;
      const QVector<double> levels = parseLevels( levelsEdit->text(), &dropped );
      if ( levels.isEmpty() )
        return;
      if ( dropped > 0 )
      {
        // 静默丢 token 会让用户以为全部级别都进了提线——如实拒绝并提示。
        if ( auto *status = child<QLabel>( this, "statusLabel" ) )
          status->setText( tr( "等值级别含 %1 个无法识别或超出上限的值，请修正后再计算。" ).arg( dropped ) );
        return;
      }
      emit interpretiveContourRequested( layerId, levels );
      return;
    }
    emit contourRequested( layerId, interval->value(), algorithm->currentData().toString() );
  } );
}

void ConstraintPage::updateFactorActionStates()
{
  const bool busy = property( "paleo.page.runbusy" ).toBool();
  updateWellFactorActionState();
  if ( auto *idw = child<QPushButton>( this, "runIdwButton" ) )
  {
    idw->setEnabled( !busy );
    idw->setToolTip( busy ? tr( "正在计算，可取消" ) : QString() );
  }
  auto *factors = child<QTableWidget>( this, "factorTable" );
  auto *generate = child<QPushButton>( this, "generateFactorButton" );
  auto *contour = child<QPushButton>( this, "contourButton" );
  if ( !factors || !generate || !contour )
    return;
  auto *saveLine = child<QPushButton>( this, "constraintParamSaveButton" );
  auto *rows = child<QListWidget>( this, "constraintList" );
  if ( saveLine )
  {
    const bool hasLine = rows && rows->currentItem();
    saveLine->setEnabled( hasLine && !busy );
    if ( busy )
      saveLine->setToolTip( tr( "正在计算，可取消" ) );
    else
      saveLine->setToolTip( hasLine ? QString() : tr( "先在约束列表中选择一条线" ) );
  }
  if (auto *batch = child<QPushButton>(this, "constraintBatchTypeButton"))
  {
    const bool selected = rows && !rows->selectedItems().isEmpty();
    batch->setEnabled(selected && !busy);
    batch->setToolTip(busy ? tr("正在计算，可取消") : selected ? QString() : tr("在列表中按 Ctrl 或 Shift 多选约束"));
  }
  const int r = checkedRow( factors );
  generate->setProperty("preparationTarget", QStringLiteral("factorTable"));
  if ( r < 0 )
  {
    generate->setEnabled( false );
    generate->setToolTip( tr( "先在清单中勾选一个单因素" ) );
    contour->setEnabled( false );
    contour->setToolTip( tr( "先在清单中勾选一个单因素" ) );
    updateEngineRows();
    updateOperationGuidance();
    return;
  }
  updateEngineRows();
  if ( busy )
  {
    generate->setEnabled( false );
    generate->setToolTip( tr( "正在计算，可取消" ) );
    contour->setEnabled( false );
    contour->setToolTip( tr( "正在计算，可取消" ) );
    updateOperationGuidance();
    return;
  }
  const auto *horizon = child<QComboBox>(this, "horizonCombo");
  if (auto *sel = qobject_cast<SelectionContext *>(
          property("paleo.selection").value<QObject *>());
      sel && !sel->activeUnit().isValid())
  {
    generate->setEnabled(false);
    generate->setToolTip(tr("输入：请先在顶部层段选择器选定子层段；层位下拉不可驱动分析"));
    generate->setProperty("preparationTarget", QStringLiteral("subIntervalSelectorBar"));
    contour->setEnabled(false);
    contour->setToolTip(generate->toolTip());
    updateOperationGuidance();
    return;
  }
  if (!property("paleo.selection").isValid()
      && (!horizon || horizon->currentText().trimmed().isEmpty()))
  {
    generate->setEnabled(false);
    generate->setToolTip(tr("输入：请先打开工程并选择层位"));
    generate->setProperty("preparationTarget", QStringLiteral("horizonCombo"));
    contour->setEnabled(false);
    contour->setToolTip(generate->toolTip());
    updateOperationGuidance();
    return;
  }
  bool known = false;
  const SingleFactorDefinition def = factorDefinition(factorIdOfRow(factors, r), &known);
  if ( known && def.processingAlgId == SingleFactorContracts::confidenceEngineId() )
  {
    generate->setEnabled( false );
    generate->setToolTip( tr( "预测置信度引擎尚未接入" ) );
  }
  else
  {
    generate->setEnabled( true );
    generate->setToolTip( QString() );
    // 结构 IDW：成图边界必选（上游 data_mode=current_layers 契约）——
    // 未选即禁用并说明，不静默退化成井点包络域。
    auto *method = child<QComboBox>( this, "factorMethodCombo" );
    auto *boundaryCombo = child<QComboBox>( this, "factorBoundaryCombo" );
    if ( known && interpolantEngine(def.processingAlgId) && method && boundaryCombo
         && method->currentData().toString() == QLatin1String( "structural_idw" )
         && boundaryCombo->currentData().toString().isEmpty() )
    {
      generate->setEnabled( false );
      generate->setToolTip( tr( "请先选择或导入成图边界面图层" ) );
      generate->setProperty("preparationTarget", QStringLiteral("factorBoundaryCombo"));
    }
  }
  if (known && def.processingAlgId == QLatin1String("paleo:paleo_isopach")) {
    const auto *top = child<QComboBox>(this, "factorTopSurfaceCombo");
    const auto *base = child<QComboBox>(this, "factorBaseSurfaceCombo");
    if (!top || !base || top->currentData().toString().isEmpty() ||
        base->currentData().toString().isEmpty() || top->currentData() == base->currentData()) {
      generate->setEnabled(false);
      generate->setToolTip(tr("输入：选择不同的顶、底构造面，按现有等厚算法计算地层厚度"));
      generate->setProperty("preparationTarget", QStringLiteral("factorTopSurfaceCombo"));
    }
  }
  if (known && interpolantEngine(def.processingAlgId) && property("paleo.page.fieldsbound").toBool()) {
    auto *wf = qobject_cast<ConstraintWorkflow *>(property(kWfProp).value<QObject *>());
    auto *h = child<QComboBox>(this, "horizonCombo");
    // 未刷新约束计划前视为「无约束可运行」；只有计划判定会被拒绝时才阻止。
    const QVariant constraintsReady = property("paleo.page.constraintsready");
    if (constraintsReady.isValid() && !constraintsReady.toBool()) {
      generate->setEnabled(false);
      generate->setToolTip(property("paleo.page.constraintreadinessmessage").toString());
      generate->setProperty("preparationTarget", QStringLiteral("constraintFamiliesSection"));
    } else {
      // 井点成果按父单元上界面归档；已接线 selection 时不回落 combo。
      const QString editH = pageEditHorizon(this, h);
      if (!wf || editH.isEmpty() ||
          wf->preparedWellFactorVersion(editH, factorIdOfRow(factors, r), wellFactorParams())
              .isEmpty())
      {
        generate->setEnabled(false);
        generate->setToolTip(tr("输入：先提取并保存当前地层的井点数据"));
        generate->setProperty("preparationTarget", QStringLiteral("extractWellFactorsButton"));
      }
    }
    // 必需的协变量不能因高级参数折叠而被忽略；定位入口会展开其所在分组。
    const auto *method = child<QComboBox>(this, "factorMethodCombo");
    const auto *covariate = child<QComboBox>(this, "factorCovariateCombo");
    if (generate->isEnabled() && method->currentData() == QStringLiteral("cokriging") &&
        covariate->currentData().toString().isEmpty())
    {
      generate->setEnabled(false);
      generate->setToolTip(tr("处理：协克里金需选择协变量栅格，请在高级插值参数中补齐"));
      generate->setProperty("preparationTarget", QStringLiteral("factorCovariateCombo"));
    }
  }
  if (auto *interval = child<QDoubleSpinBox>(this, "contourIntervalSpin")) {
    const QString id = factorIdOfRow(factors, r);
    interval->setSuffix(id == QLatin1String("sandthick") || id == QLatin1String("strathick") || id == QLatin1String("welldist") ? tr(" m") : QString());
  }
  const QString layerId = checkedFactorLayerId();
  auto *mode = child<QComboBox>( this, "factorContourModeCombo" );
  auto *levels = child<QLineEdit>( this, "factorContourLevelsEdit" );
  const bool interpretive = mode && mode->currentData().toString() == QLatin1String( "cartographic_detour" );
  int droppedTokens = 0;
  const bool levelsReady =
      !interpretive || ( levels && !parseLevels( levels->text(), &droppedTokens ).isEmpty() );
  if ( layerId.isEmpty() )
  {
    contour->setEnabled( false );
    contour->setToolTip(tr("输出：先计算并保存本层位的等值面"));
  }
  else if ( !levelsReady )
  {
    contour->setEnabled( false );
    contour->setToolTip( tr( "解释性绕行需要填写等值级别" ) );
  }
  else if ( interpretive && droppedTokens > 0 )
  {
    // 部分级别可解析：允许点击（点击时拒绝并发提示），tooltip 说明脏输入。
    contour->setEnabled( true );
    contour->setToolTip( tr( "等值级别含 %1 个无法识别的值，计算时会要求先修正" ).arg( droppedTokens ) );
  }
  else
  {
    contour->setEnabled( true );
    contour->setToolTip( QString() );
  }
  updateOperationGuidance();
}

void ConstraintPage::updateOperationGuidance()
{
  auto *hint = child<QLabel>(this, "factorReadinessHint");
  auto *next = child<QPushButton>(this, "factorNextStepButton");
  auto *generate = child<QPushButton>(this, "generateFactorButton");
  if (!hint || !next || !generate)
    return;
  const auto *horizon = child<QComboBox>(this, "horizonCombo");
  const auto *table = child<QTableWidget>(this, "factorTable");
  const int row = checkedRow(table);
  const QString factorId = factorIdOfRow(table, row);
  const auto def = factorDefinition(factorId);
  const QString analysisKey = pageAnalysisKey(this, horizon);
  const QString editH = pageEditHorizon(this, horizon);
  const QString labelH = !analysisKey.isEmpty() ? analysisKey : editH;
  child<QLabel>(this, "factorRequestSummary")->setText(
      row < 0 ? tr("当前图件：尚未选择因素") : tr("当前图件：%1 · %2").arg(labelH.isEmpty() ? tr("未选层位") : labelH, def.title));
  QString inputNote;
  if (row >= 0)
    inputNote = tr("输入：%1").arg(table->item(row, 2)->text());
  if (def.processingAlgId == QLatin1String("paleo:paleo_isopach"))
    inputNote = tr("输入：选择不同的顶、底构造面计算厚度，与井分层 MD 厚度分开使用，无需提取井点。");
  else if (def.processingAlgId == SingleFactorContracts::welldistEngineId())
    inputNote += tr("。使用井位与硬屏障计算绕障距离，无需提取指标字段。");
  child<QLabel>(this, "factorInputHint")->setText(inputNote);
  refreshFactorDefaultsHint();

  QString target = QStringLiteral("factorTable");
  QString action = tr("去选择");
  QString text = generate->toolTip();
  const bool busy = property("paleo.page.runbusy").toBool();
  if (busy)
    text = tr("任务运行中，可取消；详细进度与日志见任务中心。");
  else if (auto *sel = qobject_cast<SelectionContext *>(
               property("paleo.selection").value<QObject *>());
           sel && !sel->activeUnit().isValid())
  {
    text = tr("请先在顶部层段选择器选定子层段；层位下拉不可驱动分析。");
    target = QStringLiteral("subIntervalSelectorBar");
  }
  else if (!property("paleo.selection").isValid() && editH.trimmed().isEmpty())
  {
    text = tr("请先打开工程并选择层位。");
    target = QStringLiteral("horizonCombo");
  }
  else if (row < 0)
    text = tr("请选择一个因素，再准备对应的输入数据。");
  else if (generate->isEnabled())
  {
    text = checkedFactorLayerId().isEmpty()
        ? tr("输入已就绪，可生成单因素图。")
        : tr("等值面已保存；可继续提取等值线，或按当前参数重新计算。");
    target = checkedFactorLayerId().isEmpty() ? QStringLiteral("factorSettingsCaption")
                                            : QStringLiteral("factorContourModeCombo");
    action = checkedFactorLayerId().isEmpty() ? tr("检查参数") : tr("去提等值线");
    const QString staleKey = QStringLiteral("paleo.page.factorstale.%1").arg(analysisKey);
    if (property(staleKey.toUtf8().constData()).toStringList().contains(factorId))
    {
      text = tr("输入已变更；已保存的等值面对应旧输入，请重新计算更新成果。");
      target = QStringLiteral("factorSettingsCaption");
      action = tr("检查参数");
    }
  }
  else if (generate->property("preparationTarget") == QStringLiteral("constraintFamiliesSection"))
  {
    target = QStringLiteral("constraintFamiliesSection");
    action = tr("添加约束");
  }
  else if (generate->property("preparationTarget") == QStringLiteral("extractWellFactorsButton"))
  {
    target = QStringLiteral("extractWellFactorsButton");
    action = tr("去提取");
    const auto *extract = child<QPushButton>(this, "extractWellFactorsButton");
    if (extract && !extract->isEnabled())
    {
      const auto *fields = child<QComboBox>(this, "factorValueFieldCombo");
      text = fields->count() == 0
          ? tr("当前层位没有可用数值字段，请先维护井点属性或导入井点资料。")
          : tr("请选择要提取的字段；计算比值时需同时选择分子与分母。");
      target = fields->count() == 0 ? QStringLiteral("maintainWellFactorsButton")
                                   : QStringLiteral("factorModeCombo");
      action = fields->count() == 0 ? tr("去维护") : tr("选字段");
    }
  }
  else
    target = generate->property("preparationTarget").toString();
  const bool supported = def.processingAlgId != SingleFactorContracts::confidenceEngineId();
  next->setVisible(!busy && supported);
  next->setText(action);
  next->setProperty("target", target);
  next->setToolTip(tr("定位到下一步操作"));
  hint->setText(text);
  updateIpoStepper(analysisKey.isEmpty() ? editH : analysisKey, row, generate, target);
  updateOneClickState();
  if (auto *contourHint = child<QLabel>(this, "factorContourHint"))
  {
    const auto *contour = child<QPushButton>(this, "contourButton");
    contourHint->setText(contour->toolTip().isEmpty() ? tr("从当前因素已保存的等值面提取，并保存为新的成果版本。")
                                                   : contour->toolTip());
  }
}

void ConstraintPage::setRunBusy( bool busy )
{
  const bool wasBusy = property("paleo.page.runbusy").toBool();
  setProperty( "paleo.page.runbusy", busy );
  if (busy)
    setProperty("paleo.page.runoutcome", QString());
  if (auto *progress = child<QProgressBar>(this, "factorProgressBar"); progress && (busy || wasBusy)) {
    progress->setVisible(true);
    if (busy) {
      progress->setRange(0, 0);
      progress->setFormat(tr("正在准备"));
    } else {
      progress->setRange(0, 100);
      const bool complete = property("paleo.page.runoutcome").toString().isEmpty() && progress->value() >= 100;
      progress->setFormat(complete ? tr("任务完成 · %p%") : tr("任务已结束 · %p%"));
    }
  }
  // Freeze input controls while a captured request is being computed/published.
  for (const char *name :
       {"horizonCombo", "factorTable", "wellFactorSection", "factorSourceCombo", "constraintFamiliesSection",
        "factorMethodCombo", "factorCoverageCombo", "factorAdvancedOptionsSection",
        "factorCellSizeSpin", "factorSurfaceRow", "factorBoundaryRow",
        "factorGridResolutionSpin", "factorContourSection", "factorLegacySection"})
    if (auto *widget = findChild<QWidget *>(QString::fromLatin1(name)))
      widget->setEnabled(!busy);
  if ( auto *cancel = child<QPushButton>( this, "factorCancelButton" ) )
  {
    cancel->setVisible(busy);
    cancel->setEnabled( busy );
    cancel->setToolTip( busy ? tr( "取消当前单因素任务" ) : tr( "当前没有正在运行的单因素任务" ) );
  }
  if ( busy )
  {
    if ( auto *status = child<QLabel>( this, "statusLabel" ) )
      status->setText( tr( "正在准备" ) );
  }
  else if (wasBusy)
  {
    if (auto *status = child<QLabel>(this, "statusLabel");
        status && (status->text() == tr("正在准备") || status->text().startsWith(tr("正在计算："))))
      status->setText(tr("任务已结束，详细结果请查看任务中心。"));
  }
  updateFactorActionStates();
  updateWellFactorActionState();
  if (wasBusy && !busy && !property(kOneClickProp).toString().isEmpty())
    QTimer::singleShot(0, this, [this] { continueOneClick(); }); // 等失败/登记回执落定
}

void ConstraintPage::noteRunStage( const QString &stage, int percent )
{
  if (auto *progress = child<QProgressBar>(this, "factorProgressBar")) {
    progress->setVisible(true);
    progress->setRange(0, percent < 0 ? 0 : 100);
    if (percent >= 0)
      progress->setValue(percent);
    const QString pending = property(kOneClickProp).toString();
    const QString prefix = pending == QLatin1String("extract") ? tr("一键 1/2 提取井点 · ")
        : pending == QLatin1String("generate") ? tr("一键 2/2 生成 · ") : QString();
    progress->setFormat(prefix + (percent < 0 ? stage : stage + QStringLiteral(" · %p%")));
  }
  if ( auto *status = child<QLabel>( this, "statusLabel" ) )
    status->setText(percent >= 100 ? tr("计算及保存已完成")
        : percent < 0 ? tr("正在计算：%1").arg(stage) : tr("正在计算：%1 %2%").arg(stage).arg(percent));
}

void ConstraintPage::buildFactorResultsSection(QWidget *content, QVBoxLayout *lay) {
  lay->addWidget(caption(tr("成果版本 · 显示预览、追溯来源、发布到编图"), content));
  auto *hint = new QLabel(tr("井点数据、等值面和等值线自动保存为工程版本。后续编图引用具体版本，保"
                             "留输入数据、约束及算法参数来源。"),
                          content);
  hint->setWordWrap(true);
  lay->addWidget(hint);
  auto *empty = caption(tr("尚无已保存成果。生成等值图后自动显示在这里；等值线按需生成。"), content);
  empty->setObjectName(QStringLiteral("factorResultsEmptyHint"));
  empty->setWordWrap(true);
  lay->addWidget(empty);
  auto *results = new QTableWidget(0, 4, content);
  results->setObjectName(QStringLiteral("factorResultsTable"));
  results->setAccessibleName(tr("当前层位与因素的已保存成果"));
  // P3 任务G：第 4 列来源与质控（方法/约束实际参与/交叉验证/被忽略输入），悬停看逐条。
  results->setHorizontalHeaderLabels({tr("成果"), tr("版本"), tr("保存状态"), tr("来源与质控")});
  results->verticalHeader()->hide();
  results->horizontalHeader()->setStretchLastSection(true);
  results->setSelectionBehavior(QAbstractItemView::SelectRows);
  results->setSelectionMode(QAbstractItemView::SingleSelection);
  results->setEditTriggers(QAbstractItemView::NoEditTriggers);
  results->setVisible(false);
  lay->addWidget(results);
  auto *actions = new QVBoxLayout;
  for (const auto &entry :
       QVector<QPair<QString, QString>>{{QStringLiteral("show"), tr("显示成果")},
                                        {QStringLiteral("graph"), tr("追溯数据来源")},
                                        {QStringLiteral("compose"), tr("到编图中引用")}}) {
    auto *button = new QPushButton(entry.second, content);
    button->setObjectName(QStringLiteral("factorResult_") + entry.first);
    button->setEnabled(false);
    button->setToolTip(tr("先选择一个已保存且文件可用的成果版本"));
    actions->addWidget(button);
    connect(button, &QPushButton::clicked, this, [this, results, action = entry.first] {
      const auto *item = results->item(results->currentRow(), 0);
      if (item)
        emit factorResultRequested(item->data(Qt::UserRole).toMap().value("version_id").toString(),
                                   action);
    });
    connect(results, &QTableWidget::itemSelectionChanged, button,
            [results, button, action = entry.first] {
              const auto *item = results->item(results->currentRow(), 0);
              const bool available =
                  item && item->data(Qt::UserRole).toMap().value("available").toBool();
              const bool enabled = available || (item && action == QLatin1String("graph"));
              button->setEnabled(enabled);
              button->setToolTip(!enabled ? tr("先选择一个文件可用的成果版本")
                  : action == QLatin1String("graph") ? tr("追溯所选版本的数据与约束来源")
                  : action == QLatin1String("compose") ? tr("将所选版本设为编图解释证据来源")
                  : tr("显示所选成果版本"));
            });
  }
  lay->addLayout(actions);
  connect(results, &QTableWidget::cellDoubleClicked, this, [this, results](int row, int) {
    const auto data = results->item(row, 0)->data(Qt::UserRole).toMap();
    if (data.value("available").toBool())
      emit factorResultRequested(data.value("version_id").toString(), QStringLiteral("show"));
  });
}

void ConstraintPage::refreshFactorResults() {
  auto *table = child<QTableWidget>(this, "factorResultsTable");
  auto *factors = child<QTableWidget>(this, "factorTable");
  auto *h = child<QComboBox>(this, "horizonCombo");
  auto *wf = qobject_cast<ConstraintWorkflow *>(property(kWfProp).value<QObject *>());
  if (!table || !h || !factors)
    return;
  const auto *selected = table->item(table->currentRow(), 0);
  const QString version =
      selected ? selected->data(Qt::UserRole).toMap().value("version_id").toString() : QString();
  table->setRowCount(0);
  auto *empty = child<QLabel>(this, "factorResultsEmptyHint");
  empty->show();
  table->hide();
  if (!wf)
    return;
  const QString editH = pageEditHorizon(this, h);
  for (const auto &entry :
       wf->savedFactorResults(editH, factorIdOfRow(factors, checkedRow(factors)))) {
    auto row = entry.toMap();
    const bool available = QFileInfo::exists(row.value("path").toString());
    row.insert("available", available);
    const int r = table->rowCount();
    table->insertRow(r);
    auto *item = new QTableWidgetItem(row.value("type").toString());
    item->setData(Qt::UserRole, row);
    item->setToolTip(row.value("path").toString());
    table->setItem(r, 0, item);
    table->setItem(r, 1, new QTableWidgetItem(tr("v%1").arg(row.value("version").toInt())));
    table->setItem(r, 2, new QTableWidgetItem(available ? tr("已保存") : tr("文件缺失")));
    const auto lineage = row.value("lineage").toMap();
    auto *lineageItem = new QTableWidgetItem(lineage.value("summary").toString());
    lineageItem->setToolTip(lineage.value("details").toString());
    table->setItem(r, 3, lineageItem);
    if (row.value("version_id").toString() == version)
      table->selectRow(r);
  }
  empty->setVisible(table->rowCount() == 0);
  table->setVisible(table->rowCount() > 0);
  updateOperationGuidance(); // 成果数变化 → IPO 步骤条「输出」态
}

void ConstraintPage::updateIpoStepper(const QString &horizon, int row, const QPushButton *generate,
                                      const QString &nextTarget)
{
  auto *stepper = child<QLabel>(this, "factorIpoStepper");
  if (!stepper || property("paleo.page.runbusy").toBool())
    return; // 任务运行中保持原阶段，避免默认 target 误判回「输入」
  // 输入就绪：层位 + 因素已选，且门控不再指向输入阶段（井点提取/字段维护/选因素）。
  const bool inputTarget = nextTarget == QLatin1String("horizonCombo")
      || nextTarget == QLatin1String("subIntervalSelectorBar")
      || nextTarget == QLatin1String("factorTable")
      || nextTarget == QLatin1String("extractWellFactorsButton")
      || nextTarget == QLatin1String("maintainWellFactorsButton")
      || nextTarget == QLatin1String("factorModeCombo");
  const auto *horizons = child<QComboBox>(this, "horizonCombo");
  const QString analysisKey = pageAnalysisKey(this, horizons);
  const QString editH = pageEditHorizon(this, horizons);
  const QString displayKey = !analysisKey.isEmpty() ? analysisKey : horizon;
  bool inputDone = !displayKey.trimmed().isEmpty() && row >= 0 && !inputTarget;
  const QString staleKey = QStringLiteral("paleo.page.factorstale.%1").arg(analysisKey);
  const QString factorId = factorIdOfRow(child<QTableWidget>(this, "factorTable"), row);
  if (auto *wf = qobject_cast<ConstraintWorkflow *>(property(kWfProp).value<QObject *>());
      wf && interpolantEngine(factorDefinition(factorId).processingAlgId))
    inputDone = !editH.trimmed().isEmpty() && row >= 0 &&
        !wf->preparedWellFactorVersion(editH, factorId, wellFactorParams()).isEmpty();
  const bool stale = property(staleKey.toUtf8().constData()).toStringList().contains(factorId);
  const bool processDone = inputDone && !checkedFactorLayerId().isEmpty() && !stale;
  const auto *results = child<QTableWidget>(this, "factorResultsTable");
  bool outputReady = false;
  if (processDone && results)
    for (int result = 0; result < results->rowCount(); ++result) {
      const auto resultRow = results->item(result, 0)->data(Qt::UserRole).toMap();
      const auto kind = resultRow.value("kind").toString();
      outputReady |= resultRow.value("available").toBool() &&
          (kind == QLatin1String("contour_lines") || kind == QLatin1String("single_factor_cartographic_contour"));
    }
  const auto mark = [](bool done, bool current) {
    return done ? QStringLiteral("✓") : current ? QStringLiteral("▶") : QStringLiteral("○");
  };
  const int stage = !inputDone ? 0 : !processDone ? 2 : 3;
  stepper->setProperty("stage", stage);
  const auto *lines = child<QListWidget>(this, "constraintList");
  stepper->setText(tr("%1 获取单因素  →  %2 间隔/方向线（可选）  →  %3 等值图  →  %4 等值线（可选）")
                       .arg(mark(inputDone, stage == 0), mark(lines && lines->count() > 0, false),
                            mark(processDone, stage == 2), mark(outputReady, stage == 3)));
  stepper->setToolTip(stage == 0 ? tr("当前：读取已保存单因素，或从井资料计算并保存")
                      : stage == 2 ? (generate && generate->isEnabled()
                                          ? tr("当前：输入已就绪，按默认参数即可生成")
                                          : tr("当前：处理阶段尚有必需项"))
                                   : tr("等值图已保存。等值线可选，不生成也可用于后续编图。"));
  // 已有约束（旧工程）时自动展开可选约束段一次，保证已绘内容可见。
  auto *optional = dynamic_cast<CollapsibleSection *>(findChild<QWidget *>(QStringLiteral("factorConstraintsOptionalSection")));
  auto *list = child<QListWidget>(this, "constraintList");
  const QString autoKey = QStringLiteral("paleo.page.constraintsautoexpanded.%1").arg(horizon);
  if (optional && list && list->count() > 0 && !property(autoKey.toUtf8().constData()).toBool())
  {
    setProperty(autoKey.toUtf8().constData(), true);
    optional->setExpanded(true);
  }
}

// ---- 反馈 #19：一键「提取并生成」 -------------------------------------------
// 状态机挂在页属性 kOneClickProp（"" / "extract" / "generate"）：只驱动页面
// 已有的两个按钮，因此 #606 约束门控、忙态锁定、任务中心进度全部原样生效。
void ConstraintPage::updateOneClickState()
{
  auto *oneClick = child<QPushButton>(this, "factorOneClickButton");
  auto *generate = child<QPushButton>(this, "generateFactorButton");
  auto *extract = child<QPushButton>(this, "extractWellFactorsButton");
  if (!oneClick || !generate)
    return;
  const bool busy = property("paleo.page.runbusy").toBool();
  const QString prep = generate->property("preparationTarget").toString();
  const bool canExtract = extract && extract->isEnabled()
      && prep == QLatin1String("extractWellFactorsButton");
  const bool enabled = !busy && (generate->isEnabled() || canExtract);
  oneClick->setEnabled(enabled);
  oneClick->setToolTip(busy ? tr("任务运行中")
      : generate->isEnabled() ? tr("输入已就绪：按当前（默认）参数直接生成单因素图")
      : canExtract ? tr("先提取并保存井点数据，成功后自动按当前参数生成单因素图")
      : generate->toolTip());
}

void ConstraintPage::failOneClick(const QString &reason)
{
  setProperty(kOneClickProp, QString());
  if (auto *status = child<QLabel>(this, "statusLabel"))
    status->setText(tr("一键成图已停止：%1").arg(reason));
  updateOneClickState();
}

void ConstraintPage::startOneClick()
{
  auto *generate = child<QPushButton>(this, "generateFactorButton");
  auto *extract = child<QPushButton>(this, "extractWellFactorsButton");
  if (!generate || property("paleo.page.runbusy").toBool())
    return;
  const QString step = generate->isEnabled() ? QStringLiteral("generate") : QStringLiteral("extract");
  if (step == QLatin1String("extract") && (!extract || !extract->isEnabled())) {
    failOneClick(generate->toolTip());
    return;
  }
  setProperty(kOneClickProp, step);
  setProperty("paleo.page.runoutcome", QString());
  (step == QLatin1String("generate") ? generate : extract)->click();
  // 任务启动是同步的（factorJobStateChanged(true) 在请求处理内发出）：
  // 点击返回后仍未进入忙态 = 同步拒绝 → 立即停下并说明原因。
  if (!property("paleo.page.runbusy").toBool() && step == QLatin1String("extract") && generate->isEnabled())
    continueOneClick(); // 已保存井值的读取是同步完成，无需制造后台任务。
  else if (!property("paleo.page.runbusy").toBool())
    failOneClickWithStatus(tr("任务未能启动"));
}

void ConstraintPage::failOneClickWithStatus(const QString &fallback)
{
  const auto *status = child<QLabel>(this, "statusLabel");
  failOneClick(status && !status->text().isEmpty() ? status->text() : fallback);
}

void ConstraintPage::continueOneClick()
{
  const QString step = property(kOneClickProp).toString();
  if (step.isEmpty() || property("paleo.page.runbusy").toBool())
    return;
  const QString outcome = property("paleo.page.runoutcome").toString();
  if (!outcome.isEmpty()) {
    failOneClick(outcome);
    return;
  }
  if (step == QLatin1String("generate")) {
    setProperty(kOneClickProp, QString());
    if (auto *status = child<QLabel>(this, "statusLabel"))
      status->setText(tr("等值图已生成并保存；可直接使用，也可选生成等值线。"));
    updateOneClickState();
    return;
  }
  updateFactorActionStates();
  auto *generate = child<QPushButton>(this, "generateFactorButton");
  if (!generate || !generate->isEnabled()) {
    // 典型：#606 约束门控判定当前方法会拒绝（约束参数越界等）。
    failOneClick(tr("井点已提取，但暂不能生成——%1")
                     .arg(generate ? generate->toolTip() : QString()));
    return;
  }
  setProperty(kOneClickProp, QStringLiteral("generate"));
  generate->click();
  if (!property("paleo.page.runbusy").toBool())
    failOneClickWithStatus(tr("生成任务未能启动"));
}
