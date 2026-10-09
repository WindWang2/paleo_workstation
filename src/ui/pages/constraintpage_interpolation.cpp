// 层：视图
// constraintpage_interpolation — 插值方法域 TU（方向 96 自 constraintpage.cpp
// 拆出，原文行序保持）：字段/像元行、成图方法+策略说明+覆盖方式、结构 IDW
// 边界/格网行、高级参数面（幂次/方向/软边界/变差函数/协克里金协变量/SGS +
// parameterGroup 五组重排）、等厚引擎顶/底构造面行，及方法→参数组可见性
// 联动 updateEngineRows。
#include "constraintpage.h"
#include "constraintpage_internal.h"

#include "pageshared.h"

#include "../../domain/singlefactorstrategy.h" // 方向67：策略包词表（方法下拉单一真源）
#include "../../qgis/qgislayerservice.h"
#include "../../services/singlefactordef.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

using namespace paleo::pagesinternal;
using namespace paleo::constraintpage_internal;

QLabel *ConstraintPage::buildFieldAndCellRows( QVBoxLayout *lay )
{
  auto *fieldCaption = caption(tr("井属性字段"), this);
  lay->addWidget(fieldCaption);
  auto *field = new QLineEdit( QStringLiteral( "z" ), this );
  field->setObjectName( QStringLiteral( "factorFieldEdit" ) );
  field->setPlaceholderText( tr( "井属性字段" ) );
  field->setAccessibleName( tr( "单因素插值字段" ) );
  lay->addWidget( field );
  fieldCaption->hide(); field->hide(); // 保留旧自动化契约；可见入口由真实字段下拉承担。

  auto *cellCaption = caption(tr("像元大小"), this);
  lay->addWidget(cellCaption);
  auto *cell = new QDoubleSpinBox( this );
  cell->setObjectName( QStringLiteral( "factorCellSizeSpin" ) );
  cell->setRange( 0.0001, 1.0e9 );
  cell->setDecimals( 4 );
  cell->setValue( 1.0 );
  cell->setAccessibleName( tr( "单因素像元大小" ) );
  useMono( cell );
  lay->addWidget( cell );
  return cellCaption;
}

void ConstraintPage::buildMethodSection( QWidget *content, QVBoxLayout *lay )
{
  auto *methodCaption = caption( tr( "成图方法" ), content );
  methodCaption->setObjectName( QStringLiteral( "factorMethodCaption" ) );
  lay->addWidget( methodCaption );
  auto *method = new QComboBox( content );
  method->setObjectName( QStringLiteral( "factorMethodCombo" ) );
  // 方向67：成图方法下拉由 singlefactorstrategy 词表驱动（标签与真实算法一致，
  // 参数预览 = 词表 geologicalNote）。词表 id → 本页 method 参数映射唯一例外：
  // 词表 "idw"（上游口径）在本仓请求里是 "legacy"（不写 method，走旧约束 IDW）。
  // SGS 是实现族不是曲面策略包，作为页面专属项追加（不带策略 id）。
  for ( const paleo::singlefactor::SurfaceMethodPack &pack : paleo::singlefactor::surfaceMethodPacks() )
  {
    const QString methodId = pack.id == QLatin1String( "idw" )
                                 ? QStringLiteral( "legacy" )
                                 : pack.id;
    method->addItem( pack.label, methodId );
    method->setItemData( method->count() - 1, pack.geologicalNote, Qt::ToolTipRole );
  }
  method->addItem( tr( "序贯高斯模拟（SGS）" ), QStringLiteral( "sgs" ) );
  method->setItemData(method->count() - 1, tr("序贯高斯模拟按变差模型生成实现集合，输出均值与标准差；逐线方向/屏障参数不参与地统求解。"), Qt::ToolTipRole);
  const int defaultMethod = method->findData( QStringLiteral( "local_direction_idw" ) );
  if ( defaultMethod >= 0 )
    method->setCurrentIndex( defaultMethod );
  method->setAccessibleName( tr( "成图方法" ) );
  lay->addWidget( method );
  // 策略参数预览（随选择联动；SGS 无词表项时隐藏）。
  auto *strategyNote = new QLabel( content );
  PaleoTheme::applyThemedStyleSheet(strategyNote, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  strategyNote->setObjectName( QStringLiteral( "factorStrategyNote" ) );
  strategyNote->setWordWrap( true );
  const auto updateStrategyNote = [method, strategyNote]() {
    const QVariant note = method->currentIndex() >= 0
                              ? method->itemData( method->currentIndex(), Qt::ToolTipRole )
                              : QVariant();
    strategyNote->setText( note.toString() );
    strategyNote->setVisible( !note.toString().isEmpty() );
  };
  updateStrategyNote();
  connect( method, &QComboBox::currentIndexChanged, method, updateStrategyNote );
  lay->addWidget( strategyNote );

  auto *coverageCaption = caption( tr( "覆盖方式" ), content );
  coverageCaption->setObjectName( QStringLiteral( "factorCoverageCaption" ) );
  lay->addWidget( coverageCaption );
  auto *coverage = new QComboBox( content );
  coverage->setObjectName( QStringLiteral( "factorCoverageCombo" ) );
  coverage->addItem( tr( "井点支撑" ), QStringLiteral( "well_supported" ) );
  coverage->addItem( tr( "域内外推" ), QStringLiteral( "domain_extrapolation" ) );
  coverage->setAccessibleName( tr( "覆盖方式" ) );
  lay->addWidget( coverage );
}

void ConstraintPage::buildBoundaryRows( QWidget *content, QVBoxLayout *lay )
{
  // WS-C5：结构 IDW 专属行——成图边界（面图层，上游 data_mode=current_layers
  // 必选）+ 格网分辨率 + 导入入口。method=structural_idw 时展开
  //（updateEngineRows 管可见性，默认隐藏）。
  auto *boundaryCaption = caption( tr( "成图边界" ), content );
  boundaryCaption->setObjectName( QStringLiteral( "factorBoundaryCaption" ) );
  lay->addWidget( boundaryCaption );
  auto *boundaryRow = new QWidget( this );
  boundaryRow->setObjectName( QStringLiteral( "factorBoundaryRow" ) );
  auto *boundaryLay = new QHBoxLayout( boundaryRow );
  boundaryLay->setContentsMargins( 0, 0, 0, 0 );
  boundaryLay->setSpacing(PaleoTheme::tokens().spacingXs);
  auto *boundary = new QComboBox( boundaryRow );
  boundary->setObjectName( QStringLiteral( "factorBoundaryCombo" ) );
  boundary->setAccessibleName( tr( "成图边界面图层" ) );
  auto *boundaryImport = new QPushButton( tr( "导入…" ), boundaryRow );
  boundaryImport->setObjectName( QStringLiteral( "factorBoundaryImportButton" ) );
  boundaryImport->setAccessibleName( tr( "导入测区边界面图层" ) );
  boundaryImport->setToolTip( tr( "把面图层（SHP/GPKG）导入为测区边界" ) );
  boundaryLay->addWidget( boundary, 1 );
  boundaryLay->addWidget( boundaryImport, 0 );
  lay->addWidget( boundaryRow );
  connect( boundaryImport, &QPushButton::clicked, this,
           &ConstraintPage::boundaryImportRequested );
  auto *gridCaption = caption( tr( "格网分辨率" ), content );
  gridCaption->setObjectName( QStringLiteral( "factorGridCaption" ) );
  lay->addWidget( gridCaption );
  auto *gridRes = new QSpinBox( this );
  gridRes->setObjectName( QStringLiteral( "factorGridResolutionSpin" ) );
  gridRes->setRange( 16, 8192 );
  gridRes->setValue( 339 );
  gridRes->setToolTip( tr( "沿成图边界最长边的结点数（上游默认 339）" ) );
  gridRes->setAccessibleName( tr( "结构 IDW 格网分辨率" ) );
  useMono( gridRes );
  lay->addWidget( gridRes );
  boundaryCaption->setVisible( false );
  boundaryRow->setVisible( false );
  gridCaption->setVisible( false );
  gridRes->setVisible( false );
}

void ConstraintPage::buildAdvancedSection( QWidget *content, QVBoxLayout *lay )
{
  auto *advanced = new QWidget(content);
  advanced->setObjectName( QStringLiteral( "factorAdvancedSection" ) );

  lay->addWidget( advanced );
  auto *adv = new QVBoxLayout(advanced);
  adv->setContentsMargins(0, 0, 0, 0);
  adv->setSpacing(PaleoTheme::tokens().spacingSm);
  auto *power = new QDoubleSpinBox( advanced );
  power->setObjectName( QStringLiteral( "factorPowerSpin" ) );
  power->setRange( 0.01, 100.0 );
  power->setDecimals( 2 );
  power->setValue( 2.0 );
  power->setToolTip( tr( "幂次建议 0.5–8。算法接受任意有限正数。" ) );
  useMono( power );
  adv->addWidget( caption( tr( "幂次" ), advanced ) );
  adv->addWidget( power );
  auto *cluster = new QCheckBox( tr( "井群局部权重" ), advanced );
  cluster->setObjectName( QStringLiteral( "factorClusterCheck" ) );
  cluster->setChecked( false );
  cluster->setToolTip( tr( "默认关闭。打开后按井群距离降低边缘井的权重，不是无数据掩膜。" ) );
  adv->addWidget( cluster );
  auto *ratio = new QDoubleSpinBox( advanced );
  ratio->setObjectName( QStringLiteral( "factorDirectionRatioSpin" ) );
  ratio->setRange( 1.0, 100.0 );
  ratio->setDecimals( 2 );
  ratio->setValue( 8.0 );
  ratio->setToolTip( tr( "方向线的新任务默认比值。保存到选中的约束线。" ) );
  useMono( ratio );
  adv->addWidget( caption( tr( "方向比值" ), advanced ) );
  adv->addWidget( ratio );
  auto *influence = new QDoubleSpinBox( advanced );
  influence->setObjectName( QStringLiteral( "factorInfluenceSpin" ) );
  influence->setRange( 0.0, 1.0e12 );
  influence->setDecimals( 2 );
  influence->setSpecialValueText( tr( "自动" ) );
  influence->setToolTip( tr( "0 表示按井距和线长自动取影响半径。" ) );
  useMono( influence );
  adv->addWidget( caption( tr( "方向影响半径" ), advanced ) );
  adv->addWidget( influence );
  auto *core = new QDoubleSpinBox( advanced );
  core->setObjectName( QStringLiteral( "factorCoreSpin" ) );
  core->setRange( 0.0, 1.0e12 );
  core->setDecimals( 2 );
  core->setSpecialValueText( tr( "自动" ) );
  core->setToolTip( tr( "0 表示核心半径取影响半径的 0.3。" ) );
  useMono( core );
  adv->addWidget( caption( tr( "方向核心半径" ), advanced ) );
  adv->addWidget( core );
  auto *softStrength = new QDoubleSpinBox( advanced );
  softStrength->setObjectName( QStringLiteral( "factorSoftStrengthSpin" ) );
  softStrength->setRange( 0.0, 0.8 );
  softStrength->setSingleStep( 0.05 );
  softStrength->setDecimals( 2 );
  softStrength->setValue( 0.35 );
  softStrength->setToolTip( tr( "软边界强度。0 表示这条线不改变权重。" ) );
  useMono( softStrength );
  adv->addWidget( caption( tr( "软边界强度" ), advanced ) );
  adv->addWidget( softStrength );
  auto *softRadius = new QDoubleSpinBox( advanced );
  softRadius->setObjectName( QStringLiteral( "factorSoftRadiusSpin" ) );
  softRadius->setRange( 0.0, 1.0e12 );
  softRadius->setDecimals( 2 );
  softRadius->setSpecialValueText( tr( "自动" ) );
  softRadius->setToolTip( tr( "0 表示自动软边界半径，与显示缓冲无关。" ) );
  useMono( softRadius );
  adv->addWidget( caption( tr( "软边界半径" ), advanced ) );
  adv->addWidget( softRadius );

  // 方向18：克里金/SGS 参数——变差函数模型 + 变程/块金/拱高数值输入（0=自动
  // 拟合）+ 走向方位（-1=自动全向）。仅 method=kriging/sgs 时消费这些值。
  auto *variogramModel = new QComboBox( advanced );
  variogramModel->setObjectName( QStringLiteral( "factorVariogramModelCombo" ) );
  variogramModel->addItem( tr( "球状模型" ), QStringLiteral( "spherical" ) );
  variogramModel->addItem( tr( "指数模型" ), QStringLiteral( "exponential" ) );
  variogramModel->addItem( tr( "高斯模型" ), QStringLiteral( "gaussian" ) );
  variogramModel->setAccessibleName( tr( "变差函数模型" ) );
  adv->addWidget( caption( tr( "变差函数模型" ), advanced ) );
  adv->addWidget( variogramModel );
  auto *nugget = new QDoubleSpinBox( advanced );
  nugget->setObjectName( QStringLiteral( "factorNuggetSpin" ) );
  nugget->setRange( 0.0, 1.0e12 );
  nugget->setDecimals( 4 );
  nugget->setSpecialValueText( tr( "自动" ) );
  nugget->setToolTip( tr( "块金。0 表示随变程/拱高一起自动拟合。" ) );
  useMono( nugget );
  adv->addWidget( caption( tr( "块金" ), advanced ) );
  adv->addWidget( nugget );
  auto *sill = new QDoubleSpinBox( advanced );
  sill->setObjectName( QStringLiteral( "factorSillSpin" ) );
  sill->setRange( 0.0, 1.0e12 );
  sill->setDecimals( 4 );
  sill->setSpecialValueText( tr( "自动" ) );
  sill->setToolTip( tr( "拱高（不含块金）。0 表示自动拟合。" ) );
  useMono( sill );
  adv->addWidget( caption( tr( "拱高" ), advanced ) );
  adv->addWidget( sill );
  auto *rangeSpin = new QDoubleSpinBox( advanced );
  rangeSpin->setObjectName( QStringLiteral( "factorRangeSpin" ) );
  rangeSpin->setRange( 0.0, 1.0e12 );
  rangeSpin->setDecimals( 4 );
  rangeSpin->setSpecialValueText( tr( "自动" ) );
  rangeSpin->setToolTip( tr( "变程（实用变程口径）。0 表示自动拟合。" ) );
  useMono( rangeSpin );
  adv->addWidget( caption( tr( "变程" ), advanced ) );
  adv->addWidget( rangeSpin );
  auto *azimuth = new QDoubleSpinBox( advanced );
  azimuth->setObjectName( QStringLiteral( "factorAzimuthSpin" ) );
  azimuth->setRange( -1.0, 360.0 );
  azimuth->setDecimals( 1 );
  azimuth->setValue( -1.0 );
  azimuth->setSpecialValueText( tr( "自动（各向同性）" ) );
  azimuth->setToolTip( tr( "走向方位（度，从北顺时针）。长变程方向；自动则全向拟合。" ) );
  useMono( azimuth );
  adv->addWidget( caption( tr( "走向方位" ), advanced ) );
  adv->addWidget( azimuth );
  auto *maxPoints = new QSpinBox( advanced );
  maxPoints->setObjectName( QStringLiteral( "factorKrigingMaxPointsSpin" ) );
  maxPoints->setRange( 4, 64 );
  maxPoints->setValue( 16 );
  maxPoints->setToolTip( tr( "克里金/SGS 局部邻域的最近点数上限。" ) );
  useMono( maxPoints );
  adv->addWidget( caption( tr( "邻域点数" ), advanced ) );
  adv->addWidget( maxPoints );
  // 协克里金协变量。只在 method==cokriging 时可见；没有栅格时下拉只有空态，
  // 生成仍把空 covariateLayerId 传出，由工作流如实拒绝。
  auto *covariate = new QComboBox( advanced );
  covariate->setObjectName( QStringLiteral( "factorCovariateCombo" ) );
  covariate->setAccessibleName( tr( "协变量栅格" ) );
  covariate->setToolTip( tr( "协克里金软数据源：栅格按井位采样。没有可选栅格时不能冒充已选择。" ) );
  adv->addWidget( caption( tr( "协变量栅格" ), advanced ) );
  adv->addWidget( covariate );
  auto *crossCorrelation = new QDoubleSpinBox( advanced );
  crossCorrelation->setObjectName( QStringLiteral( "factorCrossCorrelationSpin" ) );
  crossCorrelation->setRange( -1.0, 1.0 );
  crossCorrelation->setDecimals( 2 );
  crossCorrelation->setSingleStep( 0.05 );
  crossCorrelation->setToolTip( tr( "交叉相关系数 ρ（MM1 模型 γ12 = ρ·γ1）。0 时协变量不参与权重。" ) );
  useMono( crossCorrelation );
  adv->addWidget( caption( tr( "交叉相关 ρ" ), advanced ) );
  adv->addWidget( crossCorrelation );
  auto *realizations = new QSpinBox( advanced );
  realizations->setObjectName( QStringLiteral( "factorSgsRealizationsSpin" ) );
  realizations->setRange( 1, 16 );
  realizations->setValue( 4 );
  realizations->setToolTip( tr( "SGS 实现数。产物栅格为实现均值，离散度见旁路标准差场。" ) );
  useMono( realizations );
  adv->addWidget( caption( tr( "SGS 实现数" ), advanced ) );
  adv->addWidget( realizations );
  auto *seed = new QSpinBox( advanced );
  seed->setObjectName( QStringLiteral( "factorSgsSeedSpin" ) );
  seed->setRange( 0, 2147483647 );
  seed->setValue( 42 );
  seed->setToolTip( tr( "SGS 随机种子。同种子逐位可复现。" ) );
  useMono( seed );
  adv->addWidget( caption( tr( "SGS 种子" ), advanced ) );
  adv->addWidget( seed );
  // 方向 47：SGS 成员持久化——勾选时各实现收编为 realization_set 集合版本
  //（成员数 = 实现数），供统计面/成员切换/集合对比；取消则只产均值+标准差旁路。
  auto *persistRealizations = new QCheckBox( tr( "保留实现集合（成员可切换/派生不确定性面）" ),
                                           advanced );
  persistRealizations->setObjectName( QStringLiteral( "factorSgsPersistRealizationsCheck" ) );
  persistRealizations->setChecked( true );
  persistRealizations->setToolTip(
      tr( "成员栅格逐实现落库为集合版本（惰性寻址）。取消时 SGS 仍产均值与标准差旁路，"
          "但集合成员不入库，后续不可成员切换/派生统计面。" ) );
  adv->addWidget( persistRealizations );

  const auto parameterGroup = [advanced, adv](const char *name) {
    auto *group = new QWidget(advanced); group->setObjectName(QString::fromLatin1(name));
    auto *layout = new QVBoxLayout(group); layout->setContentsMargins(0,0,0,0);
    layout->setSpacing(PaleoTheme::tokens().spacingSm); adv->addWidget(group); return layout;
  };
  auto *idwParams = parameterGroup("factorIdwParameters");
  moveParamWidget( adv, power, idwParams );
  adv->removeWidget(cluster); idwParams->addWidget(cluster);
  auto *variogramParams = parameterGroup("factorVariogramParameters");
  for (QWidget *w : QVector<QWidget *>{variogramModel, nugget, sill, rangeSpin, azimuth, maxPoints,
                                       covariate, crossCorrelation})
    moveParamWidget( adv, w, variogramParams );
  auto *sgsParams = parameterGroup("factorSgsParameters");
  moveParamWidget( adv, realizations, sgsParams ); moveParamWidget( adv, seed, sgsParams );
  adv->removeWidget(persistRealizations); sgsParams->addWidget(persistRealizations);
  auto *anisotropyParams = parameterGroup("factorAnisotropyParameters");
  auto *globalRatio = new QDoubleSpinBox(advanced);
  globalRatio->setObjectName(QStringLiteral("factorAnisotropyRatioSpin"));
  globalRatio->setRange(1, 100); globalRatio->setValue(1); useMono(globalRatio);
  anisotropyParams->addWidget(caption(tr("全局各向异性比值"), advanced)); anisotropyParams->addWidget(globalRatio);
  auto *globalAngle = new QDoubleSpinBox(advanced);
  globalAngle->setObjectName(QStringLiteral("factorAnisotropyAngleSpin"));
  globalAngle->setRange(0, 360); useMono(globalAngle);
  auto *globalAngleCaption = caption(tr("全局长轴角度（Surfer，从 X 轴逆时针）"), advanced);
  globalAngleCaption->setObjectName(QStringLiteral("factorAnisotropyAngleCaption"));
  anisotropyParams->addWidget(globalAngleCaption); anisotropyParams->addWidget(globalAngle);
  auto *neighborhood = parameterGroup("factorNeighborhoodParameters");
  auto *minimumPoints = new QSpinBox(advanced); minimumPoints->setRange(1, 64); minimumPoints->setValue(3);
  minimumPoints->setObjectName(QStringLiteral("factorMinPointsSpin")); useMono(minimumPoints);
  neighborhood->addWidget(caption(tr("最少支撑井点"), advanced)); neighborhood->addWidget(minimumPoints);
  auto *maximumPoints = new QSpinBox(advanced); maximumPoints->setRange(0, 256); maximumPoints->setValue(12);
  maximumPoints->setSpecialValueText(tr("全部井点")); maximumPoints->setObjectName(QStringLiteral("factorMaxPointsSpin")); useMono(maximumPoints);
  neighborhood->addWidget(caption(tr("最多支撑井点"), advanced)); neighborhood->addWidget(maximumPoints);
  auto *searchRadius = new QDoubleSpinBox(advanced); searchRadius->setRange(0, 1e12);
  searchRadius->setSpecialValueText(tr("自动")); searchRadius->setObjectName(QStringLiteral("factorSearchRadiusSpin")); useMono(searchRadius);
  neighborhood->addWidget(caption(tr("搜索半径"), advanced)); neighborhood->addWidget(searchRadius);
  auto *noParameters = new QLabel(tr("此方法无可调插值参数。"), advanced);
  noParameters->setObjectName(QStringLiteral("factorNoParametersLabel")); adv->addWidget(noParameters);
}

void ConstraintPage::buildSurfaceRow( QVBoxLayout *lay )
{
  // 主线6：等厚引擎（strathick）专属行——顶/底构造面栅格选择。默认隐藏，
  // 勾选等厚引擎因素时展开（updateEngineRows 管可见性）。
  auto *surfaceRow = new QWidget( this );
  surfaceRow->setObjectName( QStringLiteral( "factorSurfaceRow" ) );
  auto *surfaceLay = new QHBoxLayout( surfaceRow );
  surfaceLay->setContentsMargins( 0, 0, 0, 0 );
  surfaceLay->setSpacing(PaleoTheme::tokens().spacingXs);
  auto *topCombo = new QComboBox( surfaceRow );
  topCombo->setObjectName( QStringLiteral( "factorTopSurfaceCombo" ) );
  topCombo->setAccessibleName( tr( "顶构造面图层" ) );
  auto *baseCombo = new QComboBox( surfaceRow );
  baseCombo->setObjectName( QStringLiteral( "factorBaseSurfaceCombo" ) );
  baseCombo->setAccessibleName( tr( "底构造面图层" ) );
  surfaceLay->addWidget( new QLabel( tr( "顶面" ), surfaceRow ), 0 );
  surfaceLay->addWidget( topCombo, 1 );
  surfaceLay->addWidget( new QLabel( tr( "底面" ), surfaceRow ), 0 );
  surfaceLay->addWidget( baseCombo, 1 );
  surfaceRow->setVisible( false );
  lay->addWidget( surfaceRow );
}

void ConstraintPage::updateEngineRows()
{
  // 等厚引擎行：勾选因素的 processingAlgId 是 isopach 时展开并按当前层位
  // 列出已声明栅格（构造面随层位声明；层位无关的也列）。
  auto *factors = child<QTableWidget>( this, "factorTable" );
  auto *row = child<QWidget>( this, "factorSurfaceRow" );
  auto *horizons = child<QComboBox>( this, "horizonCombo" );
  auto *topCombo = child<QComboBox>( this, "factorTopSurfaceCombo" );
  auto *baseCombo = child<QComboBox>( this, "factorBaseSurfaceCombo" );
  if ( !factors || !row || !topCombo || !baseCombo )
    return;
  const int r = checkedRow( factors );
  bool isopach = false;
  bool interpolant = true;
  if ( r >= 0 )
  {
    bool known = false;
    const SingleFactorDefinition def =
        SingleFactorRegistry::byId( factorIdOfRow( factors, r ), &known );
    isopach = known && def.processingAlgId == QLatin1String( "paleo:paleo_isopach" );
    interpolant = !known || interpolantEngine( def.processingAlgId );
  }
  const QStringList interpolationNames = {
      QStringLiteral( "factorMethodCaption" ), QStringLiteral( "factorMethodCombo" ),
      QStringLiteral( "factorCoverageCaption" ), QStringLiteral( "factorCoverageCombo" ),
      QStringLiteral( "factorAdvancedSection" ), QStringLiteral( "factorLegacyIdwCaption" ),
      QStringLiteral( "idwField" ), QStringLiteral( "idwCellSize" ), QStringLiteral( "runIdwButton" ) };
  for ( const QString &name : interpolationNames )
  {
    if ( auto *widget = findChild<QWidget *>( name ) )
      widget->setVisible( interpolant );
  }

  const QString selectedMethod = child<QComboBox>(this, "factorMethodCombo")->currentData().toString();
  const bool idw = selectedMethod == QLatin1String("structural_idw") || selectedMethod == QLatin1String("local_direction_idw") || selectedMethod == QLatin1String("surfer_idw");
  const bool kriging = selectedMethod == QLatin1String("kriging") || selectedMethod == QLatin1String("local_direction_kriging") || selectedMethod == QLatin1String("cokriging") || selectedMethod == QLatin1String("sgs");
  for (const auto &entry : QVector<QPair<QString, bool>>{
       {QStringLiteral("factorIdwParameters"), idw}, {QStringLiteral("factorVariogramParameters"), kriging},
       {QStringLiteral("factorSgsParameters"), selectedMethod == QLatin1String("sgs")},
       {QStringLiteral("factorAnisotropyParameters"), selectedMethod == QLatin1String("surfer_idw") || selectedMethod == QLatin1String("kriging") || selectedMethod == QLatin1String("sgs")},
       {QStringLiteral("factorNeighborhoodParameters"), selectedMethod == QLatin1String("structural_idw") || selectedMethod == QLatin1String("local_direction_idw") || selectedMethod == QLatin1String("local_direction_kriging")},
       {QStringLiteral("factorNoParametersLabel"), selectedMethod == QLatin1String("legacy")}})
    findChild<QWidget *>(entry.first)->setVisible(interpolant && entry.second);
  if ( auto *covariateCombo = findChild<QComboBox *>( QStringLiteral( "factorCovariateCombo" ) ) )
  {
    const QString keep = covariateCombo->currentData().toString();
    covariateCombo->blockSignals( true );
    covariateCombo->clear();
    covariateCombo->addItem( tr( "（无——将拒绝协克里金请求）" ), QString() );
    if ( auto *svc = qobject_cast<QgisLayerService *>( property( kLayersProp ).value<QObject *>() ) )
    {
      QVector<LayerDeclaration> declared;
      if ( svc->tryDeclared( &declared ) )
      {
        for ( const LayerDeclaration &d : declared )
        {
          if ( d.type.compare( QStringLiteral( "raster" ), Qt::CaseInsensitive ) != 0 )
            continue;
          covariateCombo->addItem( d.title.isEmpty() ? d.layerId : d.title, d.layerId );
        }
      }
    }
    const int idx = covariateCombo->findData( keep );
    covariateCombo->setCurrentIndex( idx >= 0 ? idx : 0 );
    covariateCombo->blockSignals( false );
  }
  const bool covariateVisible = interpolant && selectedMethod == QLatin1String( "cokriging" );
  for ( const char *name : { "factorCovariateCombo", "factorCrossCorrelationSpin" } )
  {
    auto *w = findChild<QWidget *>( QString::fromLatin1( name ) );
    if ( !w )
      continue;
    w->setVisible( covariateVisible );
    w->setEnabled( covariateVisible );
    if ( auto *layout = w->parentWidget() ? w->parentWidget()->layout() : nullptr )
    {
      const int index = layout->indexOf( w );
      if ( index > 0 )
        if ( auto *caption = layout->itemAt( index - 1 )->widget() )
          caption->setVisible( covariateVisible );
    }
  }
  child<QCheckBox>(this, "factorClusterCheck")->setVisible(selectedMethod == QLatin1String("local_direction_idw") || selectedMethod == QLatin1String("structural_idw"));
  const bool directionWeights = selectedMethod == QLatin1String("local_direction_idw") || selectedMethod == QLatin1String("structural_idw");
  for (const char *name : {"factorDirectionRatioSpin", "factorInfluenceSpin", "factorCoreSpin"}) {
    auto *w = findChild<QWidget *>(QString::fromLatin1(name));
    w->setVisible(directionWeights);
    auto *layout = w->parentWidget()->layout();
    const int index = layout->indexOf(w);
    if (index > 0) layout->itemAt(index - 1)->widget()->setVisible(directionWeights);
  }
  child<QDoubleSpinBox>(this, "factorAnisotropyAngleSpin")->setVisible(selectedMethod == QLatin1String("surfer_idw"));
  child<QLabel>(this, "factorAnisotropyAngleCaption")->setVisible(selectedMethod == QLatin1String("surfer_idw"));
  const bool coverageParameter = selectedMethod == QLatin1String("local_direction_idw") || selectedMethod == QLatin1String("local_direction_kriging") || selectedMethod == QLatin1String("structural_idw") || selectedMethod == QLatin1String("surfer_idw");
  child<QWidget>(this, "factorCoverageCaption")->setVisible(interpolant && coverageParameter);
  child<QWidget>(this, "factorCoverageCombo")->setVisible(interpolant && coverageParameter);
  child<QLabel>(this, "factorStrategyNote")->setVisible(interpolant);
  findChild<QWidget *>(QStringLiteral("wellFactorSection"))->setVisible(interpolant || isopach);
  // WS-C5：结构 IDW 专属行——成图边界（面图层）+ 格网分辨率。
  auto *method = child<QComboBox>( this, "factorMethodCombo" );
  auto *boundaryCombo = child<QComboBox>( this, "factorBoundaryCombo" );
  const QString methodId = method ? method->currentData().toString() : QString();
  const bool structural =
      interpolant && methodId == QLatin1String( "structural_idw" );
  for ( const QString &name :
        { QStringLiteral( "factorBoundaryCaption" ), QStringLiteral( "factorBoundaryRow" ),
          QStringLiteral( "factorGridCaption" ), QStringLiteral( "factorGridResolutionSpin" ) } )
  {
    if ( auto *widget = findChild<QWidget *>( name ) )
      widget->setVisible( structural );
  }
  // 结构 IDW 按格网分辨率成图，没有 cellSize 契约——禁用并说明（DESIGN
  // 禁用即告原因）。
  if ( auto *cell = child<QDoubleSpinBox>( this, "factorCellSizeSpin" ) )
  {
    cell->setEnabled( !structural );
    cell->setToolTip( structural
                          ? tr( "结构 IDW 按格网分辨率成图，不使用像元大小" )
                          : QString() );
  }
  if ( structural && boundaryCombo )
  {
    // 边界清单：boundary.* 声明前置，其后其它矢量图层——任一已声明矢量
    // 层均可作边界，面几何由算法侧校验。
    const QString keep = boundaryCombo->currentData().toString();
    boundaryCombo->blockSignals( true );
    boundaryCombo->clear();
    if ( auto *svc = qobject_cast<QgisLayerService *>(
             property( kLayersProp ).value<QObject *>() ) )
    {
      const QVector<LayerDeclaration> declared = svc->declared();
      for ( int pass = 0; pass < 2; ++pass )
        for ( const LayerDeclaration &d : declared )
        {
          if ( d.type.compare( QStringLiteral( "vector" ), Qt::CaseInsensitive ) != 0 )
            continue;
          const bool dedicated = d.layerId.startsWith( QStringLiteral( "boundary." ) );
          if ( ( pass == 0 ) != dedicated )
            continue;
          boundaryCombo->addItem( d.title.isEmpty() ? d.layerId : d.title, d.layerId );
        }
    }
    const int idx = boundaryCombo->findData( keep );
    boundaryCombo->setCurrentIndex( idx >= 0 ? idx : 0 );
    boundaryCombo->blockSignals( false );
  }

  row->setVisible( isopach );
  if ( !isopach )
    return;

  const QString horizon = horizons ? horizons->currentText() : QString();
  const QString keepTop = topCombo->currentData().toString();
  const QString keepBase = baseCombo->currentData().toString();
  auto fill = [&]( QComboBox *combo, const QString &keep ) {
    combo->blockSignals( true );
    combo->clear();
    if ( auto *svc = qobject_cast<QgisLayerService *>(
             property( kLayersProp ).value<QObject *>() ) )
    {
      const QVector<LayerDeclaration> declared = svc->declared();
      for ( const LayerDeclaration &d : declared )
      {
        if ( d.type.compare( QStringLiteral( "raster" ), Qt::CaseInsensitive ) != 0 )
          continue;
        if ( !d.horizon.isEmpty() && !horizon.isEmpty() && d.horizon != horizon )
          continue;
        combo->addItem( d.title.isEmpty() ? d.layerId : d.title, d.layerId );
      }
    }
    const int idx = combo->findData( keep );
    if ( idx >= 0 )
      combo->setCurrentIndex( idx );
    combo->blockSignals( false );
  };
  fill( topCombo, keepTop );
  fill( baseCombo, keepBase );
}
