// 层：视图
// constraintpage_generate — 生成动作域 TU（方向 96 自 constraintpage.cpp 拆出，
// 原文行序保持）：生成/取消按钮 + 生成 handler（参数装配）、等值线间距/
// 来源/级别段，及动作可用性状态机 updateFactorActionStates 与忙态
// setRunBusy/noteRunStage。
#include "constraintpage.h"
#include "constraintpage_internal.h"

#include "pageshared.h"

#include "../../domain/singlefactorstrategy.h" // 方向67：strategy_id 血缘（生成 handler 查策略包）
#include "../../services/singlefactordef.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>

using namespace paleo::pagesinternal;
using namespace paleo::constraintpage_internal;

void ConstraintPage::buildGenerateSection( QWidget *content, QVBoxLayout *lay )
{
  // 本页主操作（DESIGN.md primary 三用途之一）：生成单因素图突出；
  // 其余动作保持常规面并按组分隔。
  auto *generate = new QPushButton( tr( "生成单因素图" ), this );
  generate->setObjectName( QStringLiteral( "generateFactorButton" ) );
  generate->setEnabled( false ); // 先勾选一个单因素（updateFactorActionStates 管 tooltip）
  markPrimaryButton( generate );
  lay->addWidget( generate );
  auto *cancel = new QPushButton( tr( "取消" ), content );
  cancel->setObjectName( QStringLiteral( "factorCancelButton" ) );
  cancel->setEnabled( false );
  cancel->setToolTip( tr( "当前没有正在运行的成图" ) );
  lay->addWidget( cancel );
  connect( cancel, &QPushButton::clicked, this, &ConstraintPage::runCancelRequested );

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
    const SingleFactorDefinition def = SingleFactorRegistry::byId( factorId, &known );
    params.insert( QStringLiteral( "field" ),
                   field->text().trimmed().isEmpty() && known
                       ? def.defaultParams.value( QStringLiteral( "field" ) )
                       : field->text().trimmed() );
    params.insert( QStringLiteral( "cellSize" ), cell->value() );
    if (known && interpolantEngine(def.processingAlgId)) {
      const QVariantMap extraction = wellFactorParams();
      // 无服务的历史调用仍可设置 factorFieldEdit；真实字段面已绑定时写明口径。
      if (!extraction.value(QStringLiteral("valueField")).toString().isEmpty() ||
          extraction.value(QStringLiteral("factorMode")) == QStringLiteral("ratio")) {
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
  lay->addWidget( caption( tr( "等值线间距" ), this ) );
  auto *interval = new QDoubleSpinBox( this );
  interval->setObjectName( QStringLiteral( "contourIntervalSpin" ) );
  // 0 = 自动等值距（structural_idw 因素的上游自适应步长规则）。
  interval->setRange( 0.0, 1.0e9 );
  interval->setSpecialValueText( tr( "自动（按数据自适应步长）" ) );
  interval->setDecimals( 2 );
  interval->setValue( 20.0 );
  interval->setSuffix( tr( " m" ) );
  interval->setAccessibleName( tr( "等值线间距" ) );
  useMono( interval );
  lay->addWidget( interval );

  lay->addWidget( caption( tr( "等值线来源" ), content ) );
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
  connect( contourMode, &QComboBox::currentIndexChanged, this, [this, contourMode, levelsEdit, sourceNote] {
    const bool interpretive = contourMode->currentData().toString() == QLatin1String( "cartographic_detour" );
    levelsEdit->setVisible( interpretive );
    sourceNote->setText( interpretive ? tr( "解释性等值线的值来自制图工作场，不能参与融合、分相或厚度统计" )
                                      : tr( "等值线取自分析场" ) );
    updateFactorActionStates();
  } );
  connect( levelsEdit, &QLineEdit::textChanged, this, [this] { updateFactorActionStates(); } );

  auto *contour = new QPushButton( tr( "生成等值线" ), content );
  contour->setObjectName( QStringLiteral( "contourButton" ) );
  contour->setEnabled( false ); // 勾选且已生成的因素才有可等值线的栅格
  lay->addWidget( contour );
  connect( contour, &QPushButton::clicked, this, [this, interval, contourMode, levelsEdit] {
    const QString layerId = checkedFactorLayerId();
    if ( layerId.isEmpty() )
      return;
    if ( contourMode->currentData().toString() == QLatin1String( "cartographic_detour" ) )
    {
      const QVector<double> levels = parseLevels( levelsEdit->text() );
      if ( levels.isEmpty() )
        return;
      emit interpretiveContourRequested( layerId, levels );
      return;
    }
    emit contourRequested( layerId, interval->value() );
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
  if ( r < 0 )
  {
    generate->setEnabled( false );
    generate->setToolTip( tr( "先在清单中勾选一个单因素" ) );
    contour->setEnabled( false );
    contour->setToolTip( tr( "先在清单中勾选一个单因素" ) );
    updateEngineRows();
    return;
  }
  updateEngineRows();
  if ( busy )
  {
    generate->setEnabled( false );
    generate->setToolTip( tr( "正在计算，可取消" ) );
    contour->setEnabled( false );
    contour->setToolTip( tr( "正在计算，可取消" ) );
    return;
  }
  bool known = false;
  const SingleFactorDefinition def = SingleFactorRegistry::byId( factorIdOfRow( factors, r ), &known );
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
    if ( method && boundaryCombo
         && method->currentData().toString() == QLatin1String( "structural_idw" )
         && boundaryCombo->currentData().toString().isEmpty() )
    {
      generate->setEnabled( false );
      generate->setToolTip( tr( "请先选择或导入成图边界面图层" ) );
    }
  }
  if (known && interpolantEngine(def.processingAlgId) && property("paleo.page.fieldsbound").toBool()) {
    auto *extract = child<QPushButton>(this, "extractWellFactorsButton");
    if (extract && !extract->isEnabled()) {
      generate->setEnabled(false); generate->setToolTip(extract->toolTip());
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
  const bool levelsReady = !interpretive || ( levels && !parseLevels( levels->text() ).isEmpty() );
  if ( layerId.isEmpty() )
  {
    contour->setEnabled( false );
    contour->setToolTip( tr( "该因素尚未生成——先运行「生成单因素图」" ) );
  }
  else if ( !levelsReady )
  {
    contour->setEnabled( false );
    contour->setToolTip( tr( "解释性绕行需要填写等值级别" ) );
  }
  else
  {
    contour->setEnabled( true );
    contour->setToolTip( QString() );
  }
}

void ConstraintPage::setRunBusy( bool busy )
{
  setProperty( "paleo.page.runbusy", busy );
  if ( auto *cancel = child<QPushButton>( this, "factorCancelButton" ) )
  {
    cancel->setEnabled( busy );
    cancel->setToolTip( busy ? tr( "取消当前成图" ) : tr( "当前没有正在运行的成图" ) );
  }
  if ( busy )
  {
    if ( auto *status = child<QLabel>( this, "statusLabel" ) )
      status->setText( tr( "正在准备" ) );
  }
  updateFactorActionStates();
  updateWellFactorActionState();
}

void ConstraintPage::noteRunStage( const QString &stage, int percent )
{
  if ( auto *status = child<QLabel>( this, "statusLabel" ) )
    status->setText( tr( "正在计算：%1 %2%" ).arg( stage ).arg( percent ) );
}
