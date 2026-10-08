// 层：视图
#include "ui/paleotheme.h"
#include "constraintpage.h"

#include "pageshared.h"

#include "../../domain/arearules.h" // 历史存量 include（勿增新 io include）
#include "../../domain/mappinghorizons.h"
#include "../../domain/singlefactorstrategy.h" // 方向67：策略包词表（方法下拉单一真源）
#include "../../qgis/qgislayerservice.h"
#include "../../services/singlefactordef.h"
#include "../../workflow/workflows.h"
#include "../realization/realizationpanel.h" // 方向 47：集合查看面（视图同层）

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QShowEvent>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

using namespace paleo::pagesinternal;

namespace
{
  // 状态（无成员设计，走动态属性——与 pagepanels.cpp 同一惯例）：
  // factorId -> 已生成 layerId。
  constexpr const char kFactorGenProp[] = "paleo.page.factorgen";

  QString factorIdOfRow( const QTableWidget *table, int row )
  {
    if ( !table || row < 0 || row >= table->rowCount() )
      return QString();
    return table->item( row, 0 ) ? table->item( row, 0 )->data( Qt::UserRole ).toString()
                                 : QString();
  }

  int checkedRow( const QTableWidget *table )
  {
    if ( !table )
      return -1;
    for ( int r = 0; r < table->rowCount(); ++r )
    {
      if ( table->item( r, 0 ) &&
           table->item( r, 0 )->checkState() == Qt::Checked )
        return r;
    }
    return -1;
  }
} // namespace

// 层：视图
// ---------------------------------------------------------------------------
// ConstraintPage — ②约束与单因素（m2(B) 单因素图页）
// ---------------------------------------------------------------------------
namespace
{
bool interpolantEngine( const QString &algorithmId )
{
  return algorithmId == QLatin1String( "paleo:paleo_constraint_idw" );
}

void useMono( QWidget *widget )
{
  if ( widget )
    widget->setFont( PaleoTheme::monoFont() );
}

QVector<double> parseLevels( const QString &text )
{
  QVector<double> levels;
  const QStringList parts = text.split( QRegularExpression( QStringLiteral( "[,，\\s]+" ) ), Qt::SkipEmptyParts );
  for ( const QString &part : parts )
  {
    bool ok = false;
    const double value = part.toDouble( &ok );
    if ( ok )
      levels << value;
  }
  return levels;
}
} // namespace

ConstraintPage::ConstraintPage( ConstraintWorkflow *wf, QWidget *parent )
  : QWidget( parent )
{
  auto *scroll = new QScrollArea( this );
  scroll->setObjectName( QStringLiteral( "constraintPageScroll" ) );
  scroll->setWidgetResizable( true );
  scroll->setFrameShape( QFrame::NoFrame );
  auto *content = new QWidget( scroll );
  content->setObjectName( QStringLiteral( "constraintPageBody" ) );
  auto *lay = panelLayout( content );
  scroll->setWidget( content );
  auto *outer = new QVBoxLayout( this );
  outer->setContentsMargins( 0, 0, 0, 0 );
  outer->addWidget( scroll );

  lay->addWidget( caption( tr( "层位" ), this ) );
  auto *horizons = new QComboBox( this );
  horizons->setObjectName( QStringLiteral( "horizonCombo" ) );
  lay->addWidget( horizons );

  // ---- m2(B)：单因素清单 + 生成 双区（§10 词表驱动）------------------------
  lay->addWidget( caption( tr( "单因素图" ), this ) );
  auto *factors = new QTableWidget( 0, 4, this );
  factors->setObjectName( QStringLiteral( "factorTable" ) );
  factors->setAccessibleName( tr( "单因素清单" ) );
  factors->setHorizontalHeaderLabels( { tr( "勾选" ), tr( "名称" ), tr( "输入" ), tr( "状态" ) } );
  factors->verticalHeader()->setVisible( false );
  factors->horizontalHeader()->setStretchLastSection( true );
  factors->setEditTriggers( QAbstractItemView::NoEditTriggers );
  factors->setSelectionBehavior( QAbstractItemView::SelectRows );
  int row = 0;
  for ( const SingleFactorDefinition &def : SingleFactorRegistry::builtins() )
  {
    factors->insertRow( row );
    auto *check = new QTableWidgetItem();
    check->setFlags( Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable );
    check->setCheckState( Qt::Unchecked );
    check->setData( Qt::UserRole, def.factorId );
    auto *name = new QTableWidgetItem( def.title );
    auto *input = new QTableWidgetItem( def.inputAssetType );
    auto *status = new QTableWidgetItem( tr( "未生成" ) );
    for ( auto *it : { check, name, input, status } )
      it->setFlags( it->flags() & ~Qt::ItemIsEditable );
    // 名称/输入列挂 UserRole，测试与刷新按 factorId 对行。
    name->setData( Qt::UserRole, def.factorId );
    input->setData( Qt::UserRole, def.factorId );
    status->setData( Qt::UserRole, def.factorId );
    factors->setItem( row, 0, check );
    factors->setItem( row, 1, name );
    factors->setItem( row, 2, input );
    factors->setItem( row, 3, status );
    ++row;
  }
  lay->addWidget( factors, 1 );

  auto *wellFactors = new QWidget(content);
  wellFactors->setObjectName(QStringLiteral("wellFactorSection"));
  auto *wellLay = panelLayout(wellFactors);
  wellLay->addWidget(caption(tr("井点因子"), wellFactors));
  auto *factorMode = new QComboBox(wellFactors);
  factorMode->setObjectName(QStringLiteral("factorModeCombo"));
  factorMode->addItem(tr("直读字段"), QStringLiteral("direct"));
  factorMode->addItem(tr("比值（分子 ÷ 分母）"), QStringLiteral("ratio"));
  factorMode->setAccessibleName(tr("因子提取口径"));
  wellLay->addWidget(factorMode);
  for (const auto &entry : QVector<QPair<QString, QString>>{
       {QStringLiteral("factorValueFieldCombo"), tr("指标字段")},
       {QStringLiteral("factorNumeratorFieldCombo"), tr("分子字段")},
       {QStringLiteral("factorDenominatorFieldCombo"), tr("分母字段")}}) {
    auto *label = caption(entry.second, wellFactors);
    label->setObjectName(entry.first + QStringLiteral("Caption"));
    wellLay->addWidget(label);
    auto *combo = new QComboBox(wellFactors); combo->setObjectName(entry.first);
    combo->setAccessibleName(entry.second); combo->setPlaceholderText(tr("请选择字段")); wellLay->addWidget(combo);
    connect(combo, &QComboBox::currentIndexChanged, this, [this] { invalidateWellFactors(); });
  }
  auto *extract = new QPushButton(tr("提取井点因子"), wellFactors);
  extract->setObjectName(QStringLiteral("extractWellFactorsButton"));
  auto *maintain = new QPushButton(tr("维护井点属性"), wellFactors);
  maintain->setObjectName(QStringLiteral("maintainWellFactorsButton"));
  maintain->setToolTip(tr("按当前层位维护砂厚、层厚、砂地比与其他单因素；保存后重新提取"));
  wellLay->addWidget(maintain);
  connect(maintain, &QPushButton::clicked, this, [this, horizons] { emit maintainWellFactorsRequested(horizons->currentText()); });
  wellLay->addWidget(extract);
  auto *wellTable = new QTableWidget(0, 3, wellFactors);
  wellTable->setObjectName(QStringLiteral("wellFactorTable"));
  wellTable->setAccessibleName(tr("井点因子提取结果"));
  wellTable->setHorizontalHeaderLabels({tr("井名"), tr("因子值"), tr("来源或缺失原因")});
  wellTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
  wellTable->verticalHeader()->hide(); wellTable->horizontalHeader()->setStretchLastSection(true);
  wellLay->addWidget(wellTable);
  auto *wellHint = new QLabel(tr("选择字段后提取；解释砂厚和分层层厚采用 MD，同层段相除得到砂地比。"), wellFactors);
  wellHint->setObjectName(QStringLiteral("wellFactorHint")); wellHint->setWordWrap(true);
  PaleoTheme::applyThemedStyleSheet(wellHint, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  wellLay->addWidget(wellHint); lay->addWidget(wellFactors);
  const auto modeChanged = [this, factorMode] {
    const bool ratio = factorMode->currentData() == QStringLiteral("ratio");
    for (const QString &name : {QStringLiteral("factorValueFieldCombo"), QStringLiteral("factorNumeratorFieldCombo"),
                               QStringLiteral("factorDenominatorFieldCombo")}) {
      const bool visible = (name == QLatin1String("factorValueFieldCombo")) != ratio;
      findChild<QWidget *>(name)->setVisible(visible);
      findChild<QWidget *>(name + QStringLiteral("Caption"))->setVisible(visible);
    }
    invalidateWellFactors();
  };
  connect(factorMode, &QComboBox::currentIndexChanged, this, modeChanged); modeChanged();
  connect(extract, &QPushButton::clicked, this, [this, factors, horizons] {
    const int r = checkedRow(factors); if (r < 0) return;
    emit extractWellFactorsRequested(factorIdOfRow(factors, r), horizons->currentText(), wellFactorParams());
  });

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
  const auto moveParam = [adv](QWidget *control, QVBoxLayout *target) {
    const int index = adv->indexOf(control);
    if (index > 0) {
      auto *previous = adv->itemAt(index - 1)->widget();
      if (qobject_cast<QLabel *>(previous)) { adv->removeWidget(previous); target->addWidget(previous); }
    }
    adv->removeWidget(control); target->addWidget(control);
  };
  auto *idwParams = parameterGroup("factorIdwParameters");
  moveParam(power, idwParams);
  adv->removeWidget(cluster); idwParams->addWidget(cluster);
  auto *variogramParams = parameterGroup("factorVariogramParameters");
  for (QWidget *w : QVector<QWidget *>{variogramModel, nugget, sill, rangeSpin, azimuth, maxPoints})
    moveParam(w, variogramParams);
  auto *sgsParams = parameterGroup("factorSgsParameters");
  moveParam(realizations, sgsParams); moveParam(seed, sgsParams);
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

  connect( generate, &QPushButton::clicked, this,
           [this, horizons, field, cell, factors, topCombo, baseCombo, method, coverage, power, cluster,
             variogramModel, nugget, sill, rangeSpin, azimuth, maxPoints, realizations, seed,
             persistRealizations, boundary, gridRes] {
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
           methodId == QLatin1String( "local_direction_kriging" ) )
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

  // 勾选互斥单选（业务上同时只看一张单因素图）：勾一行自动取消其它行；
  // 生成过的行随勾选态发 factorVisibilityRequested（互斥上图意图）。
  connect( factors, &QTableWidget::itemChanged, this,
           [this, factors]( QTableWidgetItem *changed ) {
             if ( !changed || changed->column() != 0 )
               return;
             if ( changed->checkState() != Qt::Checked )
             {
               // 用户手动取消勾选：已生成的层发下图意图。
               const QString layerId = property( kFactorGenProp )
                                           .toMap()
                                           .value( factorIdOfRow( factors, changed->row() ) )
                                           .toString();
               if ( !layerId.isEmpty() )
                 emit factorVisibilityRequested( layerId, false );
               updateFactorActionStates();
               return;
             }
             // 新勾选：屏蔽信号地取消其它行（单选语义）。
             const int newRow = changed->row();
             factors->blockSignals( true );
             for ( int r = 0; r < factors->rowCount(); ++r )
             {
               if ( r != newRow && factors->item( r, 0 ) &&
                    factors->item( r, 0 )->checkState() == Qt::Checked )
               {
                 const QString prevId = factorIdOfRow( factors, r );
                 factors->item( r, 0 )->setCheckState( Qt::Unchecked );
                 const QString prevLayer =
                     property( kFactorGenProp ).toMap().value( prevId ).toString();
                 if ( !prevLayer.isEmpty() )
                   emit factorVisibilityRequested( prevLayer, false );
               }
             }
             factors->blockSignals( false );
             const QString newLayer =
                 property( kFactorGenProp ).toMap().value( factorIdOfRow( factors, newRow ) ).toString();
             if ( !newLayer.isEmpty() )
               emit factorVisibilityRequested( newLayer, true );
             // structural_idw 因素的等值线默认自动等值距（0）。
             if ( auto *spin = child<QDoubleSpinBox>( this, "contourIntervalSpin" ) )
               if ( checkedFactorIsStructural() )
                 spin->setValue( 0.0 );
             updateFactorActionStates();
           } );
  // ---- m2(B) 双区 end ------------------------------------------------------

  lay->addSpacing(PaleoTheme::tokens().spacingMd); // spacing.md：单因素区与约束区分组
  auto *constraintCaption = caption(tr("约束线"), this);
  lay->addWidget(constraintCaption);
  auto *list = new QListWidget( this );
  list->setObjectName( QStringLiteral( "constraintList" ) );
  list->setAccessibleName( tr( "约束列表" ) );
  list->setSelectionMode(QAbstractItemView::ExtendedSelection);
  connect(list, &QListWidget::itemSelectionChanged, this, [this, list] {
    QStringList ids;
    for (const auto *item : list->selectedItems())
      ids.append(item->data(Qt::UserRole).toString());
    auto *h = child<QComboBox>(this, "horizonCombo");
    emit constraintSelectionChanged(h ? h->currentText() : QString(), ids);
    updateFactorActionStates();
  });
  lay->addWidget( list, 1 );
  connect( list, &QListWidget::currentItemChanged, this, [this]( QListWidgetItem *, QListWidgetItem * ) {
    loadSelectedConstraintLine();
    if ( auto *removeButton = findChild<QPushButton *>( QStringLiteral( "constraintDeleteButton" ) ) )
    {
      auto *rows = findChild<QListWidget *>( QStringLiteral( "constraintList" ) );
      removeButton->setEnabled( rows && rows->currentItem() );
    }
  } );
  // 方向23：列表右键——语义切换（五种 Semantic 词表）与删除。
  list->setContextMenuPolicy( Qt::CustomContextMenu );
  connect( list, &QListWidget::customContextMenuRequested, this,
           [this, list]( const QPoint &pos ) {
             QListWidgetItem *item = list->itemAt( pos );
             if ( !item )
               return;
             const QString id = item->data( Qt::UserRole ).toString();
             auto *horizons = child<QComboBox>( this, "horizonCombo" );
             const QString horizon = horizons ? horizons->currentText() : QString();
             QMenu menu( this );
             QMenu *semanticMenu = menu.addMenu( tr( "切换语义" ) );
             const QVector<QPair<QString, QString>> semantics{
               { tr( "硬屏障" ), QStringLiteral( "hard_barrier" ) },
               { tr( "方向引导" ), QStringLiteral( "direction_guide" ) },
               { tr( "解释软边界" ), QStringLiteral( "interpretive_boundary" ) },
               { tr( "等值停止" ), QStringLiteral( "contour_stop" ) },
               { tr( "制图绕行" ), QStringLiteral( "cartographic_detour" ) },
             };
             for ( const auto &[label, token] : semantics )
             {
               connect( semanticMenu->addAction( label ), &QAction::triggered, this,
                        [this, horizon, id, token] {
                          emit constraintSemanticChangeRequested( horizon, id, token );
                        } );
             }
             connect( menu.addAction( tr( "删除约束" ) ), &QAction::triggered, this,
                      [this, horizon, id] { emit constraintDeleteRequested( horizon, id ); } );
             menu.exec( list->viewport()->mapToGlobal( pos ) );
           } );

  lay->addWidget( caption( tr( "约束语义" ), content ) );
  auto *semantic = new QComboBox( content );
  semantic->setObjectName( QStringLiteral( "constraintSemanticCombo" ) );
  semantic->addItem( tr( "硬屏障" ), QStringLiteral( "hard_barrier" ) );
  semantic->addItem( tr( "方向引导" ), QStringLiteral( "direction_guide" ) );
  semantic->addItem( tr( "解释软边界" ), QStringLiteral( "interpretive_boundary" ) );
  semantic->addItem( tr( "等值停止" ), QStringLiteral( "contour_stop" ) );
  semantic->addItem( tr( "制图绕行" ), QStringLiteral( "cartographic_detour" ) );
  semantic->setAccessibleName( tr( "约束语义" ) );
  lay->addWidget( semantic );
  auto *blockMode = new QComboBox(content);
  blockMode->setObjectName(QStringLiteral("constraintBlockModeCombo"));
  blockMode->addItem(tr("完全阻断"), QStringLiteral("full_block"));
  blockMode->addItem(tr("仅显示停线"), QStringLiteral("display_only"));
  blockMode->addItem(tr("不阻断"), QStringLiteral("none"));
  blockMode->setAccessibleName(tr("阻断方式"));
  lay->addWidget(blockMode);
  auto *lineAngle = new QDoubleSpinBox(content);
  lineAngle->setObjectName(QStringLiteral("constraintAngleSpin"));
  lineAngle->setRange(-1, 360);
  lineAngle->setSpecialValueText(tr("保留线条方向"));
  lineAngle->setValue(-1);
  lineAngle->setSuffix(tr(" °"));
  lineAngle->setToolTip(tr("从北顺时针；保存时绕线条中心旋转几何，单因素使用同一条线的方向"));
  lineAngle->setAccessibleName(tr("约束方向角"));
  useMono(lineAngle);
  lay->addWidget(lineAngle);
  auto *batchType = new QPushButton(tr("所选约束批量改型"), content);
  batchType->setObjectName(QStringLiteral("constraintBatchTypeButton"));
  batchType->setEnabled(false);
  batchType->setToolTip(tr("在列表中按 Ctrl 或 Shift 多选约束"));
  lay->addWidget(batchType);
  connect(batchType, &QPushButton::clicked, this, [this, list, semantic] {
    QStringList ids;
    for (const auto *item : list->selectedItems())
      ids.append(item->data(Qt::UserRole).toString());
    auto *horizons = child<QComboBox>(this, "horizonCombo");
    emit constraintParametersRequested(horizons ? horizons->currentText() : QString(), ids,
                                       {{QStringLiteral("semantic"), semantic->currentData()}});
  });
  auto *saveLine = new QPushButton( tr( "保存约束参数" ), content );
  saveLine->setObjectName( QStringLiteral( "constraintParamSaveButton" ) );
  saveLine->setEnabled( false );
  saveLine->setToolTip( tr( "先在约束列表中选择一条线" ) );
  lay->addWidget( saveLine );
  connect( saveLine, &QPushButton::clicked, this, [this, semantic, blockMode, lineAngle, ratio, influence, core, softStrength, softRadius] {
    auto *rows = child<QListWidget>( this, "constraintList" );
    QListWidgetItem *item = rows ? rows->currentItem() : nullptr;
    if ( !item )
      return;
    QVariantMap lineParams = selectedLineParams();
    lineParams.insert( QStringLiteral( "semantic" ), semantic->currentData().toString() );
    lineParams.insert( QStringLiteral( "ratio" ), ratio->value() );
    lineParams.insert( QStringLiteral( "influenceRadius" ), influence->value() );
    lineParams.insert( QStringLiteral( "coreRadius" ), core->value() );
    lineParams.insert( QStringLiteral( "softStrength" ), softStrength->value() );
    lineParams.insert( QStringLiteral( "softRadius" ), softRadius->value() );
    if (blockMode->currentIndex() >= 0)
      lineParams.insert(QStringLiteral("blockMode"), blockMode->currentData());
    if (lineAngle->value() >= 0)
      lineParams.insert(QStringLiteral("geometryAzimuth"), lineAngle->value());
    auto *horizons = child<QComboBox>(this, "horizonCombo");
    emit constraintParametersRequested(horizons ? horizons->currentText() : QString(),
                                       {item->data(Qt::UserRole).toString()}, lineParams);
  } );

  lay->addWidget( caption( tr( "相代码" ), content ) );
  auto *spin = new QSpinBox( this );
  spin->setObjectName( QStringLiteral( "faciesCodeSpin" ) );
  spin->setRange( 0, 9999 );
  spin->setAccessibleName( tr( "相代码" ) );
  lay->addWidget( spin );

  // Shape picker feeds ConstraintDrawController::startCapture's tool choice.
  auto *shape = new QComboBox( this );
  shape->setObjectName( QStringLiteral( "shapeCombo" ) );
  shape->addItem( tr( "约束线" ), QStringLiteral( "line" ) );
  shape->addItem( tr( "约束多边形" ), QStringLiteral( "polygon" ) );
  shape->addItem( tr( "约束矩形" ), QStringLiteral( "rect" ) );
  shape->addItem( tr( "约束点" ), QStringLiteral( "point" ) );
  shape->addItem( tr( "约束圆" ), QStringLiteral( "circle" ) );
  shape->addItem( tr( "约束椭圆" ), QStringLiteral( "ellipse" ) );
  lay->addWidget( shape );

  auto *draw = new QPushButton( tr( "绘制约束" ), this );
  draw->setObjectName( QStringLiteral( "drawButton" ) );
  lay->addWidget( draw );
  connect( draw, &QPushButton::clicked, this, [this, horizons, shape, spin] {
    emit drawConstraintRequested( horizons->currentText(),
                                  shape->currentData().toString(), spin->value() );
  } );

  // ---- 类型化约束线两入口（方向线/打断线——03_Constraints 组的类型化
  // 约束；shape 决定画布工具，constraintType 进 ConstraintStore 词表）------
  // 两个类型化入口是同组次要动作——归并为一行工具排，不再各占全宽。
  auto *typedRow = new QHBoxLayout();
  typedRow->setSpacing(PaleoTheme::tokens().spacingXs); // xs
  auto *direction = new QPushButton( tr( "画方向线" ), this );
  direction->setObjectName( QStringLiteral( "directionButton" ) );
  typedRow->addWidget( direction );
  connect( direction, &QPushButton::clicked, this, [this, horizons, spin] {
    emit drawTypedConstraintRequested( horizons->currentText(), QStringLiteral( "line" ),
                                       QStringLiteral( "direction_line" ), spin->value() );
  } );
  auto *breakLine = new QPushButton( tr( "画打断线" ), this );
  breakLine->setObjectName( QStringLiteral( "breakLineButton" ) );
  typedRow->addWidget( breakLine );
  connect( breakLine, &QPushButton::clicked, this, [this, horizons, spin] {
    emit drawTypedConstraintRequested( horizons->currentText(), QStringLiteral( "line" ),
                                       QStringLiteral( "break_line" ), spin->value() );
  } );
  lay->addLayout( typedRow );
  auto *softRow = new QHBoxLayout();
  softRow->setSpacing(PaleoTheme::tokens().spacingXs);
  auto *softButton = new QPushButton( tr( "画软边界" ), content );
  softButton->setObjectName( QStringLiteral( "softBoundaryButton" ) );
  softRow->addWidget( softButton );
  connect( softButton, &QPushButton::clicked, this, [this, horizons, spin] {
    emit drawTypedConstraintRequested( horizons->currentText(), QStringLiteral( "line" ),
                                       QStringLiteral( "interpretive_boundary" ), spin->value() );
  } );
  auto *stopButton = new QPushButton( tr( "画等值停止" ), content );
  stopButton->setObjectName( QStringLiteral( "contourStopButton" ) );
  softRow->addWidget( stopButton );
  connect( stopButton, &QPushButton::clicked, this, [this, horizons, spin] {
    emit drawTypedConstraintRequested( horizons->currentText(), QStringLiteral( "line" ),
                                       QStringLiteral( "contour_stop" ), spin->value() );
  } );
  auto *detourButton = new QPushButton( tr( "画制图绕行" ), content );
  detourButton->setObjectName( QStringLiteral( "cartographicDetourButton" ) );
  softRow->addWidget( detourButton );
  connect( detourButton, &QPushButton::clicked, this, [this, horizons, spin] {
    emit drawTypedConstraintRequested( horizons->currentText(), QStringLiteral( "line" ),
                                       QStringLiteral( "cartographic_detour" ), spin->value() );
  } );
  lay->addLayout( softRow );
  // ---- 方向23：已绘约束线编辑面（顶点编辑 / 删除；语义切换走列表右键）------
  auto *editRow = new QHBoxLayout();
  editRow->setSpacing(PaleoTheme::tokens().spacingXs); // xs
  auto *vertexEdit = new QPushButton( tr( "编辑约束线" ), content );
  vertexEdit->setObjectName( QStringLiteral( "constraintVertexEditButton" ) );
  vertexEdit->setAccessibleName( tr( "编辑约束线" ) );
  editRow->addWidget( vertexEdit );
  connect( vertexEdit, &QPushButton::clicked, this, [this, horizons] {
    emit editConstraintVerticesRequested( horizons->currentText() );
  } );
  auto *removeButton = new QPushButton( tr( "删除选中约束" ), content );
  removeButton->setObjectName( QStringLiteral( "constraintDeleteButton" ) );
  removeButton->setAccessibleName( tr( "删除选中约束" ) );
  removeButton->setEnabled( false );
  editRow->addWidget( removeButton );
  const auto selectedConstraintId = [this]() -> QString {
    auto *list = findChild<QListWidget *>( QStringLiteral( "constraintList" ) );
    if ( !list )
      return QString();
    QListWidgetItem *item = list->currentItem();
    return item ? item->data( Qt::UserRole ).toString() : QString();
  };
  connect( removeButton, &QPushButton::clicked, this, [this, horizons, selectedConstraintId] {
    const QString id = selectedConstraintId();
    if ( !id.isEmpty() )
      emit constraintDeleteRequested( horizons->currentText(), id );
  } );
  lay->addLayout( editRow );
  // ---- 类型化约束线 end ----------------------------------------------------

  auto *constraints = new QWidget(content);
  constraints->setObjectName(QStringLiteral("constraintFamiliesSection"));
  auto *constraintLay = panelLayout(constraints);
  const int start = lay->indexOf(constraintCaption);
  while (lay->count() > start) {
    QLayoutItem *item = lay->takeAt(start);
    constraintLay->addItem(item);
  }
  // 两族入口排在列表/编辑之前；通用形状与逐线编辑留在次级展开面。
  auto *details = new CollapsibleSection(tr("编辑已绘约束 / 其他形状"), constraints);
  details->setObjectName(QStringLiteral("constraintDetailsSection")); details->setExpanded(false);
  while (constraintLay->count() > 1) details->containerLayout()->addItem(constraintLay->takeAt(1));
  const auto family = [constraintLay, constraints](const char *name, const QString &title, const QString &note) {
    auto *group = new QWidget(constraints); group->setObjectName(QString::fromLatin1(name));
    auto *layout = panelLayout(group); layout->addWidget(caption(title, group));
    auto *label = new QLabel(note, group); label->setWordWrap(true);
    PaleoTheme::applyThemedStyleSheet(label, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
    layout->addWidget(label); constraintLay->addWidget(group); return layout;
  };
  auto *directionFamily = family("directionFamilySection", tr("方向线族"),
      tr("方向引导改变沿线权重；软边界减弱跨线影响；制图绕行只调整解释性等值线。"));
  auto *directionType = new QComboBox(constraints); directionType->setObjectName(QStringLiteral("directionFamilyTypeCombo"));
  directionType->setAccessibleName(tr("方向线族语义"));
  directionType->addItem(tr("方向引导"), QStringLiteral("direction_line"));
  directionType->addItem(tr("解释软边界"), QStringLiteral("interpretive_boundary"));
  directionType->addItem(tr("制图绕行"), QStringLiteral("cartographic_detour"));
  directionFamily->addWidget(directionType); typedRow->removeWidget(direction); directionFamily->addWidget(direction);
  moveParam(ratio, directionFamily); moveParam(influence, directionFamily); moveParam(core, directionFamily);
  moveParam(softStrength, details->containerLayout()); moveParam(softRadius, details->containerLayout());
  auto *breakFamily = family("breakFamilySection", tr("打断线族"),
      tr("硬屏障阻断跨线传播；等值停止终止提线。仅显示停线不改变插值数值。"));
  auto *breakType = new QComboBox(constraints); breakType->setObjectName(QStringLiteral("breakFamilyTypeCombo"));
  breakType->setAccessibleName(tr("打断线族语义"));
  breakType->addItem(tr("硬屏障"), QStringLiteral("break_line"));
  breakType->addItem(tr("等值停止"), QStringLiteral("contour_stop"));
  breakFamily->addWidget(breakType); typedRow->removeWidget(breakLine); breakFamily->addWidget(breakLine);
  details->containerLayout()->removeWidget(blockMode); breakFamily->addWidget(caption(tr("阻断方式"), constraints)); breakFamily->addWidget(blockMode);
  constraintLay->addWidget(details);
  lay->insertWidget(lay->indexOf(methodCaption), constraints);
  lay->removeWidget(cellCaption); lay->removeWidget(cell);
  lay->insertWidget(lay->indexOf(methodCaption) + 2, cellCaption);
  lay->insertWidget(lay->indexOf(cellCaption) + 1, cell);
  // 参数不再藏在高级区，逐线比值仍归约束族（其是否适用由方法联动说明）。
  direction->disconnect(this); breakLine->disconnect(this);
  connect(direction, &QPushButton::clicked, this, [this, horizons, spin, directionType] {
    emit drawTypedConstraintRequested(horizons->currentText(), QStringLiteral("line"), directionType->currentData().toString(), spin->value());
  });
  connect(breakLine, &QPushButton::clicked, this, [this, horizons, spin, breakType] {
    emit drawTypedConstraintRequested(horizons->currentText(), QStringLiteral("line"), breakType->currentData().toString(), spin->value());
  });
  // 将旧三个入口归入次级面，objectName 和信号兼容。
  for (QPushButton *button : {softButton, stopButton, detourButton}) {
    softRow->removeWidget(button); details->containerLayout()->addWidget(button);
  }

  // 旧 IDW 行（objectName 保留；runIdwRequested 原语义不动）。
  lay->addSpacing(PaleoTheme::tokens().spacingMd); // spacing.md：约束区与 IDW 区分组
  auto *legacyCaption = caption( tr( "插值（IDW）" ), content );
  legacyCaption->setObjectName( QStringLiteral( "factorLegacyIdwCaption" ) );
  lay->addWidget( legacyCaption );
  auto *idwField = new QLineEdit( QStringLiteral( "z" ), this );
  idwField->setObjectName( QStringLiteral( "idwField" ) );
  idwField->setPlaceholderText( tr( "井属性字段" ) );
  idwField->setAccessibleName( tr( "插值字段" ) );
  lay->addWidget( idwField );

  auto *idwCell = new QDoubleSpinBox( this );
  idwCell->setObjectName( QStringLiteral( "idwCellSize" ) );
  idwCell->setRange( 0.0001, 1.0e9 );
  idwCell->setDecimals( 4 );
  idwCell->setValue( 1.0 );
  idwCell->setAccessibleName( tr( "像元大小" ) );
  lay->addWidget( idwCell );

  auto *idw = new QPushButton( tr( "插值" ), this );
  idw->setObjectName( QStringLiteral( "runIdwButton" ) );
  lay->addWidget( idw );
  connect( idw, &QPushButton::clicked, this, [this, horizons] {
    emit runIdwRequested( horizons->currentText() );
  } );
  auto *legacySection = new CollapsibleSection(tr("兼容入口：原始井字段 IDW"), content);
  legacySection->setObjectName(QStringLiteral("factorLegacySection"));
  legacySection->setExpanded(false);
  for (QWidget *widget : QVector<QWidget *>{legacyCaption, idwField, idwCell, idw}) {
    lay->removeWidget(widget); legacySection->containerLayout()->addWidget(widget);
  }
  lay->addWidget(legacySection);

  auto *status = new QLabel( this );
  status->setObjectName( QStringLiteral( "statusLabel" ) );
  status->setWordWrap( true );
  lay->addWidget( status );

  // ---- 阶段C 厚度样本表（autoplan §5C）-------------------------------------
  // 逐井：井名 / D61 TVD / D62 TVD / 层间速度或原因。行表由 MappingWorkflow
  // typed 镜像到 ConstraintWorkflow::setThicknessSamples；不足样本的两句
  // （「厚度样本不足以成面」/「没有厚度样本」）渲染在 thicknessHint，不弹框。
  // m2(B)：整段挪进 CollapsibleSection（objectName 全保留，默认展开）。
  lay->addSpacing(PaleoTheme::tokens().spacingMd); // spacing.md
  // 厚度样本的层位/基面名随工程参数（AreaRules targetHorizon + 有序集合的
  // 下一界面）——文案在工程打开时由 refreshAreaParamLabels 重写。
  auto *section = new CollapsibleSection( tr( "厚度样本" ), this );
  section->setObjectName( QStringLiteral( "thicknessSection" ) );
  section->setExpanded( true );
  lay->addWidget( section, 1 );
  auto *thLay = section->containerLayout();
  auto *thCap = caption(
      tr( "%1→%2 厚度样本" )
          .arg( AreaRules::active().targetHorizon,
                baseHorizonFor( AreaRules::active().targetHorizon ) ),
      section->container() );
  thCap->setObjectName( QStringLiteral( "thicknessCaption" ) );
  thLay->addWidget( thCap );
  auto *thTable = new QTableWidget( 0, 4, section->container() );
  thTable->setObjectName( QStringLiteral( "thicknessTable" ) );
  thTable->setAccessibleName( tr( "厚度样本表" ) );
  thTable->setHorizontalHeaderLabels(
      { tr( "井名" ), tr( "%1 TVD" ).arg( AreaRules::active().targetHorizon ),
        tr( "%1 TVD" ).arg( baseHorizonFor( AreaRules::active().targetHorizon ) ),
        tr( "层间速度或原因" ) } );
  thTable->verticalHeader()->setVisible( false );
  thTable->horizontalHeader()->setStretchLastSection( true );
  thLay->addWidget( thTable, 1 );
  auto *thHint = new QLabel( section->container() );
  thHint->setObjectName( QStringLiteral( "thicknessHint" ) );
  thHint->setWordWrap( true );
  // text-muted 活体（随主题重算）。
  PaleoTheme::applyThemedStyleSheet(
      thHint, [] { return PaleoTheme::mutedCaptionStyleSheet(); } );
  thLay->addWidget( thHint );

  // ---- 方向 47：realization 集合查看面 -------------------------------------
  // SGS「保留实现集合」勾选后的产物面：成员切换/统计面/集合差值/动画帧全是
  // intent 信号，catalog 换绑与上图编排由壳侧 attachConstraintPage 接
  // RealizationWorkflow。默认收起——副产物面不抢单因素主流程的视觉权重。
  lay->addSpacing( PaleoTheme::tokens().spacingMd );
  auto *rsSection = new CollapsibleSection( tr( "不确定性集合" ), this );
  rsSection->setObjectName( QStringLiteral( "realizationSection" ) );
  rsSection->setExpanded( false );
  lay->addWidget( rsSection );
  auto *rsPanel = new RealizationPanel( rsSection->container() );
  rsPanel->setObjectName( QStringLiteral( "realizationPanel" ) );
  rsSection->containerLayout()->addWidget( rsPanel );

  if ( wf ) // workflow feedback lands on the status label
  {
    setProperty( kWfProp, QVariant::fromValue( static_cast<QObject *>( wf ) ) );
    connect( wf, &ConstraintWorkflow::constraintAdded, status, [this, status]( const QString &id ) {
      status->setText( tr( "已添加约束 %1" ).arg( id ) );
      refreshConstraintList();
    } );
    connect( wf, &ConstraintWorkflow::constraintLineUpdated, this, [this]( const QString & ) {
      refreshConstraintList();
    } );
    connect( wf, &ConstraintWorkflow::constraintRemoved, this, [this, status]( const QString &id ) {
      status->setText( tr( "已删除约束 %1" ).arg( id ) );
      refreshConstraintList();
    } );
    connect( wf, &ConstraintWorkflow::factorDone, status,
             [status]( const QString &h, const QString &layerId ) {
               status->setText( tr( "单因素完成：%1 → %2" ).arg( h, layerId ) );
             } );
    // m2(B)：新生成链回执 → 状态列/状态行刷新。
    connect( wf, &ConstraintWorkflow::factorGenerated, status,
             [this, status]( const QString &h, const QString &fid, const QString &layerId ) {
               noteFactorLayer( fid, layerId );
               status->setText( tr( "单因素完成：%1 %2 → %3" ).arg( h, fid, layerId ) );
             } );
    connect( wf, &ConstraintWorkflow::contoursGenerated, status,
             [status]( const QString &h, const QString &, const QString &layerId ) {
               status->setText( tr( "等值线完成：%1 → %2" ).arg( h, layerId ) );
             } );
    connect( wf, &ConstraintWorkflow::cartographicWorkGenerated, status,
             [status]( const QString &h, const QString &, const QString &layerId ) {
               status->setText( tr( "制图工作场完成：%1 → %2" ).arg( h, layerId ) );
             } );
    connect( wf, &ConstraintWorkflow::interpretiveContoursGenerated, status,
             [status]( const QString &h, const QString &, const QString &layerId ) {
               status->setText( tr( "解释性等值线完成：%1 → %2" ).arg( h, layerId ) );
             } );
  }

  const auto mark = [this] { markInputsStale(); };
  if ( auto *edit = child<QLineEdit>( this, "factorFieldEdit" ) )
    connect( edit, &QLineEdit::textEdited, this, mark );
  if ( auto *spinBox = child<QDoubleSpinBox>( this, "factorCellSizeSpin" ) )
    connect( spinBox, qOverload<double>( &QDoubleSpinBox::valueChanged ), this, [mark]( double ) { mark(); } );
  if ( auto *combo = child<QComboBox>( this, "factorMethodCombo" ) )
    connect( combo, qOverload<int>( &QComboBox::currentIndexChanged ), this,
             [this, mark]( int ) {
               mark();
               // 结构 IDW 默认铺满测区边界（上游默认）——联动覆盖选项。
               auto *method = child<QComboBox>( this, "factorMethodCombo" );
               auto *coverage = child<QComboBox>( this, "factorCoverageCombo" );
               if ( method && coverage
                    && method->currentData().toString()
                           == QLatin1String( "structural_idw" ) )
               {
                 const int idx = coverage->findData(
                     QStringLiteral( "domain_extrapolation" ) );
                 if ( idx >= 0 )
                   coverage->setCurrentIndex( idx );
               }
               updateEngineRows();
               updateFactorActionStates();
             } );
  if ( auto *combo = child<QComboBox>( this, "factorCoverageCombo" ) )
    connect( combo, qOverload<int>( &QComboBox::currentIndexChanged ), this, [mark]( int ) { mark(); } );
  if ( auto *combo = child<QComboBox>( this, "factorBoundaryCombo" ) )
    connect( combo, qOverload<int>( &QComboBox::currentIndexChanged ), this,
             [this, mark]( int ) {
               mark();
               updateFactorActionStates(); // 边界必选门随选择刷新
             } );
  if ( auto *spin = child<QSpinBox>( this, "factorGridResolutionSpin" ) )
    connect( spin, qOverload<int>( &QSpinBox::valueChanged ), this,
             [mark]( int ) { mark(); } );
  if ( auto *horizons = child<QComboBox>( this, "horizonCombo" ) )
    connect( horizons, qOverload<int>( &QComboBox::currentIndexChanged ), this, [this]( int ) { refreshConstraintList(); } );

  if (wf) {
    connect(wf, &ConstraintWorkflow::wellAttributesChanged, this, [this] { invalidateWellFactors(); refreshWellFactorFields(); });
    connect(wf, &ConstraintWorkflow::wellFactorsExtracted, this,
            [this](const QString &, const QString &) { refreshWellFactorResults(); });
  }
  connect(horizons, &QComboBox::currentIndexChanged, this, [this] { invalidateWellFactors(); refreshWellFactorFields(); });
  connect(factors, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
    if (item && item->column() == 0) { invalidateWellFactors(); refreshWellFactorFields(); }
  });
  refreshWellFactorFields();
  refreshConstraintList();
  updateFactorActionStates();
}

void ConstraintPage::showEvent( QShowEvent *event )
{
  QWidget::showEvent( event );
  refreshThicknessSamples();
  refreshWellFactorFields();
  refreshConstraintList();
}

void ConstraintPage::refreshThicknessSamples()
{
  auto *table = child<QTableWidget>( this, "thicknessTable" );
  auto *hint = child<QLabel>( this, "thicknessHint" );
  if ( !table )
    return;
  auto *wf = qobject_cast<ConstraintWorkflow *>( property( kWfProp ).value<QObject *>() );
  // ARCH-06：typed 面直读（原 paleo.thickness.* 动态属性暗道已撤）。
  const QVariantList rows = wf ? wf->thicknessSampleRows() : QVariantList();
  const QString message = wf ? wf->thicknessSampleMessage() : QString();

  table->setRowCount( 0 );
  for ( const QVariant &v : rows )
  {
    const QVariantMap m = v.toMap();
    const int r = table->rowCount();
    table->insertRow( r );
    auto *name = new QTableWidgetItem( m.value( QStringLiteral( "well_name" ) ).toString() );
    const QString tvdTop = m.contains( QStringLiteral( "tvd_top" ) )
                               ? QString::number( m.value( QStringLiteral( "tvd_top" ) ).toDouble(), 'f', 1 )
                               : QStringLiteral( "—" );
    const QString tvdBase = m.contains( QStringLiteral( "tvd_base" ) )
                                ? QString::number( m.value( QStringLiteral( "tvd_base" ) ).toDouble(), 'f', 1 )
                                : QStringLiteral( "—" );
    // 贡献井 → 层间速度（m/s）；否则 → 原因文案。
    const QString last = m.value( QStringLiteral( "contributing" ) ).toBool()
                             ? tr( "%1 m/s" ).arg( m.value( QStringLiteral( "vint" ) ).toDouble(), 0, 'f', 0 )
                             : m.value( QStringLiteral( "reason" ) ).toString();
    auto *itTop = new QTableWidgetItem( tvdTop );
    auto *itBase = new QTableWidgetItem( tvdBase );
    auto *itV = new QTableWidgetItem( last );
    for ( auto *it : { name, itTop, itBase, itV } )
      it->setFlags( it->flags() & ~Qt::ItemIsEditable );
    table->setItem( r, 0, name );
    table->setItem( r, 1, itTop );
    table->setItem( r, 2, itBase );
    table->setItem( r, 3, itV );
  }
  if ( hint )
    hint->setText( message );
}

// ---- m2(B) 单因素状态面 ------------------------------------------------------

void ConstraintPage::bindLayerService( QObject *layers )
{
  auto *svc = qobject_cast<QgisLayerService *>( layers );
  if ( !svc )
  {
    setProperty( kLayersProp, QVariant() );
    return;
  }
  setProperty( kLayersProp, QVariant::fromValue( static_cast<QObject *>( svc ) ) );
  // layerDeclared（任何来源——重开工程/重算/边界导入）里 factor.* 声明 →
  // 状态列刷新；任何声明都刷引擎行（边界/构造面清单随声明补齐）。
  connect( svc, &QgisLayerService::layerDeclared, this,
           [this]( const QString &layerId ) {
             updateEngineRows();
             if (layerId.startsWith(QStringLiteral("wells"))) refreshWellFactorFields();
             if ( !layerId.startsWith( QStringLiteral( "factor." ) ) )
               return;
             // "factor.<horizon>.<factorId>" → 尾段 factorId。
             const QString factorId = layerId.mid( layerId.lastIndexOf( QLatin1Char( '.' ) ) + 1 );
             if ( !factorId.isEmpty() )
               noteFactorLayer( factorId, layerId );
           } );
  // 初绑重读清单：已声明因素直接进「已生成」态。
  QVector<LayerDeclaration> declared;
  if ( svc->tryDeclared( &declared ) )
  {
    for ( const LayerDeclaration &d : declared )
    {
      if ( !d.layerId.startsWith( QStringLiteral( "factor." ) ) )
        continue;
      const QString factorId = d.layerId.mid( d.layerId.lastIndexOf( QLatin1Char( '.' ) ) + 1 );
      if ( !factorId.isEmpty() )
        noteFactorLayer( factorId, d.layerId );
    }
  }
}

void ConstraintPage::noteFactorLayer( const QString &factorId, const QString &layerId )
{
  QVariantMap generated = property( kFactorGenProp ).toMap();
  generated.insert( factorId, layerId );
  setProperty( kFactorGenProp, generated );

  auto *table = child<QTableWidget>( this, "factorTable" );
  if ( table )
  {
    for ( int r = 0; r < table->rowCount(); ++r )
    {
      if ( factorIdOfRow( table, r ) != factorId )
        continue;
      QTableWidgetItem *status = table->item( r, 3 );
      if ( status )
      {
        table->blockSignals( true ); // 状态文本不是勾选态，不触发单选逻辑
        status->setText( tr( "已生成·%1" ).arg( layerId ) );
        table->blockSignals( false );
      }
      break;
    }
  }

  // 该行正被勾选而图层刚生成 → 上图意图（否则新生成层不进互斥显示）。
  auto *factors = child<QTableWidget>( this, "factorTable" );
  if ( factors && checkedRow( factors ) >= 0 &&
       factorIdOfRow( factors, checkedRow( factors ) ) == factorId )
  {
    emit factorVisibilityRequested( layerId, true );
  }
  updateFactorActionStates();
}

QString ConstraintPage::checkedFactorLayerId() const
{
  auto *factors = child<QTableWidget>( const_cast<ConstraintPage *>( this ), "factorTable" );
  if ( !factors )
    return QString();
  const int r = checkedRow( factors );
  if ( r < 0 )
    return QString();
  return property( kFactorGenProp ).toMap().value( factorIdOfRow( factors, r ) ).toString();
}

// structural 判定 = 已声明因素栅格的 .structural.json 侧卡存在（该引擎
// 必落侧卡，其它引擎不会产出同名文件）。
bool ConstraintPage::checkedFactorIsStructural() const
{
  const QString layerId = checkedFactorLayerId();
  if ( layerId.isEmpty() )
    return false;
  auto *svc = qobject_cast<QgisLayerService *>(
      property( kLayersProp ).value<QObject *>() );
  if ( !svc )
    return false;
  const QVector<LayerDeclaration> declared = svc->declared();
  for ( const LayerDeclaration &d : declared )
  {
    if ( d.layerId != layerId )
      continue;
    const QString path = d.source.section( QLatin1Char( '|' ), 0, 0 );
    const int dot = path.lastIndexOf( QLatin1Char( '.' ) );
    if ( dot < 0 )
      return false;
    return QFileInfo::exists( path.left( dot ) + QStringLiteral( ".structural.json" ) );
  }
  return false;
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
  const bool kriging = selectedMethod == QLatin1String("kriging") || selectedMethod == QLatin1String("local_direction_kriging") || selectedMethod == QLatin1String("sgs");
  for (const auto &entry : QVector<QPair<QString, bool>>{
       {QStringLiteral("factorIdwParameters"), idw}, {QStringLiteral("factorVariogramParameters"), kriging},
       {QStringLiteral("factorSgsParameters"), selectedMethod == QLatin1String("sgs")},
       {QStringLiteral("factorAnisotropyParameters"), selectedMethod == QLatin1String("surfer_idw") || selectedMethod == QLatin1String("kriging") || selectedMethod == QLatin1String("sgs")},
       {QStringLiteral("factorNeighborhoodParameters"), selectedMethod == QLatin1String("structural_idw") || selectedMethod == QLatin1String("local_direction_idw") || selectedMethod == QLatin1String("local_direction_kriging")},
       {QStringLiteral("factorNoParametersLabel"), selectedMethod == QLatin1String("legacy")}})
    findChild<QWidget *>(entry.first)->setVisible(interpolant && entry.second);
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

void ConstraintPage::markInputsStale()
{
  auto *factors = child<QTableWidget>( this, "factorTable" );
  if ( !factors )
    return;
  const int r = checkedRow( factors );
  if ( r < 0 )
    return;
  const QString factorId = factorIdOfRow( factors, r );
  if ( property( kFactorGenProp ).toMap().value( factorId ).toString().isEmpty() )
    return;
  QTableWidgetItem *status = factors->item( r, 3 );
  if ( !status )
    return;
  const QString suffix = tr( "（旧输入）" );
  if ( !status->text().contains( suffix ) )
  {
    factors->blockSignals( true );
    status->setText( status->text() + suffix );
    factors->blockSignals( false );
  }
}

QVariantMap ConstraintPage::selectedLineParams() const
{
  QVariantMap params;
  auto *rows = child<QListWidget>( const_cast<ConstraintPage *>( this ), "constraintList" );
  const QListWidgetItem *item = rows ? rows->currentItem() : nullptr;
  if ( !item )
    return params;
  const QString json = item->data( Qt::UserRole + 1 ).toString();
  if ( json.isEmpty() )
    return params;
  const QJsonDocument doc = QJsonDocument::fromJson( json.toUtf8() );
  if ( !doc.isObject() )
    return params;
  return doc.object().toVariantMap();
}

void ConstraintPage::loadSelectedConstraintLine()
{
  const QVariantMap params = selectedLineParams();
  if (auto *mode = child<QComboBox>(this, "constraintBlockModeCombo"))
  {
    const QString token = params.value(QStringLiteral("blockMode"), QStringLiteral("full_block")).toString();
    mode->setCurrentIndex(mode->findData(token));
  }
  if (auto *angle = child<QDoubleSpinBox>(this, "constraintAngleSpin"))
    angle->setValue(-1);
  auto *semantic = child<QComboBox>( this, "constraintSemanticCombo" );
  if ( semantic && params.contains( QStringLiteral( "semantic" ) ) )
  {
    const int idx = semantic->findData( params.value( QStringLiteral( "semantic" ) ) );
    if ( idx >= 0 )
      semantic->setCurrentIndex( idx );
  }
  const auto setSpin = [this]( const char *name, const QString &key, const QVariantMap &source ) {
    auto *spin = child<QDoubleSpinBox>( this, name );
    if ( spin && source.contains( key ) )
      spin->setValue( source.value( key ).toDouble() );
  };
  setSpin( "factorDirectionRatioSpin", QStringLiteral( "ratio" ), params );
  setSpin( "factorInfluenceSpin", QStringLiteral( "influenceRadius" ), params );
  setSpin( "factorCoreSpin", QStringLiteral( "coreRadius" ), params );
  setSpin( "factorSoftStrengthSpin", QStringLiteral( "softStrength" ), params );
  setSpin( "factorSoftRadiusSpin", QStringLiteral( "softRadius" ), params );
  updateFactorActionStates();
}

void ConstraintPage::refreshConstraintList()
{
  auto *list = child<QListWidget>( this, "constraintList" );
  if ( !list )
    return;
  const QString keep = list->currentItem() ? list->currentItem()->data( Qt::UserRole ).toString() : QString();
  auto *wf = qobject_cast<ConstraintWorkflow *>( property( kWfProp ).value<QObject *>() );
  auto *horizons = child<QComboBox>( this, "horizonCombo" );
  const QString horizon = horizons ? horizons->currentText() : QString();
  QSet<QString> selected;
  for (const auto *item : list->selectedItems())
    selected.insert(item->data(Qt::UserRole).toString());
  list->blockSignals( true );
  list->clear();
  if ( wf )
  {
    const QVector<QVariantMap> rows = wf->loadConstraints( horizon );
    for ( const QVariantMap &row : rows )
    {
      const QString id = row.value( QStringLiteral( "id" ) ).toString();
      const QString type = row.value( QStringLiteral( "type" ) ).toString();
      auto *item = new QListWidgetItem( tr( "%1 · %2" ).arg( id, type ), list );
      item->setData( Qt::UserRole, id );
      item->setData( Qt::UserRole + 1, row.value( QStringLiteral( "params_json" ) ).toString() );
      if ( id == keep )
        list->setCurrentItem(item, QItemSelectionModel::NoUpdate);
      item->setSelected(selected.contains(id));
    }
  }
  list->blockSignals( false );
  loadSelectedConstraintLine();
}

QVariantMap ConstraintPage::wellFactorParams() const {
  const auto value = [this](const char *name) { auto *c = findChild<QComboBox *>(QString::fromLatin1(name)); return c ? c->currentData().toString() : QString(); };
  return {{QStringLiteral("factorMode"), value("factorModeCombo")},
          {QStringLiteral("valueField"), value("factorValueFieldCombo")},
          {QStringLiteral("numeratorField"), value("factorNumeratorFieldCombo")},
          {QStringLiteral("denominatorField"), value("factorDenominatorFieldCombo")}};
}
void ConstraintPage::refreshWellFactorFields() {
  auto *wf = qobject_cast<ConstraintWorkflow *>(property(kWfProp).value<QObject *>());
  auto *h = child<QComboBox>(this, "horizonCombo");
  QString error;
  const QVariantList fields = wf ? wf->wellFactorFields(h ? h->currentText() : QString(), &error) : QVariantList();
  if (!error.isEmpty()) child<QLabel>(this, "wellFactorHint")->setText(error);
  setWellFactorFields(fields);
  setProperty("paleo.page.fieldsbound", wf != nullptr);
  updateFactorActionStates();
}
void ConstraintPage::setWellFactorFields(const QVariantList &fields) {
  setProperty("paleo.page.fieldsbound", true);
  QVariantMap old = wellFactorParams();
  auto *factors = child<QTableWidget>(this, "factorTable");
  const QString factor = factorIdOfRow(factors, checkedRow(factors));
  const bool changedFactor = property("paleo.page.extractionFactor").toString() != factor;
  if (changedFactor) old.remove(QStringLiteral("valueField"));
  setProperty("paleo.page.extractionFactor", factor);
  for (const auto &entry : QVector<QPair<QString, QString>>{
       {QStringLiteral("factorValueFieldCombo"), QStringLiteral("valueField")},
       {QStringLiteral("factorNumeratorFieldCombo"), QStringLiteral("numeratorField")},
       {QStringLiteral("factorDenominatorFieldCombo"), QStringLiteral("denominatorField")}}) {
    auto *c = findChild<QComboBox *>(entry.first); if (!c) continue;
    c->blockSignals(true); c->clear();
    for (const QVariant &v : fields) { const auto m = v.toMap(); c->addItem(m.value(QStringLiteral("label")).toString(), m.value(QStringLiteral("id"))); }
    QString selected = old.value(entry.second).toString();
    if (selected.isEmpty() && entry.second == QLatin1String("valueField")) {
      const auto *table = child<QTableWidget>(this, "factorTable"); const int r = checkedRow(table);
      if (r >= 0) selected = SingleFactorRegistry::byId(factorIdOfRow(table, r)).defaultParams.value(QStringLiteral("field")).toString();
    }
    if (selected.isEmpty() && entry.second == QLatin1String("numeratorField")) selected = QStringLiteral("log_sand_thickness_md");
    if (selected.isEmpty() && entry.second == QLatin1String("denominatorField")) selected = QStringLiteral("log_layer_thickness_md");
    int index = c->findData(selected);
    if (index < 0 && entry.second == QLatin1String("valueField")) {
      if (factor == QLatin1String("sandthick")) index = c->findData(QStringLiteral("log_sand_thickness_md"));
      if (factor == QLatin1String("strathick")) index = c->findData(QStringLiteral("log_layer_thickness_md"));
    }
    c->setCurrentIndex(index); c->blockSignals(false);
  }
  if (changedFactor) {
    const bool direct = child<QComboBox>(this, "factorValueFieldCombo")->currentIndex() >= 0;
    auto *mode = child<QComboBox>(this, "factorModeCombo");
    const bool ratioAvailable = child<QComboBox>(this, "factorNumeratorFieldCombo")->currentIndex() >= 0 && child<QComboBox>(this, "factorDenominatorFieldCombo")->currentIndex() >= 0;
    mode->setCurrentIndex(mode->findData(factor == QLatin1String("sandratio") && !direct && ratioAvailable ? QStringLiteral("ratio") : QStringLiteral("direct")));
  }
  updateFactorActionStates();
}
void ConstraintPage::invalidateWellFactors() {
  if (auto *table = findChild<QTableWidget *>(QStringLiteral("wellFactorTable"))) table->setRowCount(0);
  if (auto *hint = findChild<QLabel *>(QStringLiteral("wellFactorHint")))
    hint->setText(tr("当前口径尚未提取井点因子。选择字段后提取；解释厚度采用同一 MD 层段。"));
  markInputsStale(); updateWellFactorActionState();
  updateFactorActionStates();
}
void ConstraintPage::updateWellFactorActionState() {
  auto *button = child<QPushButton>(this, "extractWellFactorsButton"); if (!button) return;
  const auto params = wellFactorParams(); const bool ratio = params.value(QStringLiteral("factorMode")) == QStringLiteral("ratio");
  const bool ready = checkedRow(child<QTableWidget>(this, "factorTable")) >= 0 &&
      (ratio ? !params.value(QStringLiteral("numeratorField")).toString().isEmpty() && !params.value(QStringLiteral("denominatorField")).toString().isEmpty()
             : !params.value(QStringLiteral("valueField")).toString().isEmpty());
  const bool busy = property("paleo.page.runbusy").toBool();
  button->setEnabled(ready && !busy);
  button->setToolTip(busy ? tr("正在计算，可取消") : ready ? QString() : tr("请勾选因子并选择真实提取字段"));
}
void ConstraintPage::refreshWellFactorResults() {
  auto *wf = qobject_cast<ConstraintWorkflow *>(property(kWfProp).value<QObject *>());
  auto *table = child<QTableWidget>(this, "wellFactorTable"); if (!wf || !table) return;
  table->setRowCount(0);
  const auto params = wellFactorParams();
  table->setHorizontalHeaderItem(1, new QTableWidgetItem(params.value(QStringLiteral("factorMode")) == QStringLiteral("ratio") ? tr("比值（分子 ÷ 分母）") : tr("因子值")));
  for (const QVariant &v : wf->wellFactorRows()) {
    const auto m = v.toMap(); const int row = table->rowCount(); table->insertRow(row);
    table->setItem(row, 0, new QTableWidgetItem(m.value(QStringLiteral("well_name")).toString()));
    auto *value = new QTableWidgetItem(m.contains(QStringLiteral("value")) ? QString::number(m.value(QStringLiteral("value")).toDouble(), 'g', 8) : QStringLiteral("—"));
    value->setFont(PaleoTheme::monoFont()); table->setItem(row, 1, value);
    auto *detail = new QTableWidgetItem(m.value(QStringLiteral("contributing")).toBool() ? m.value(QStringLiteral("source")).toString() : m.value(QStringLiteral("reason")).toString());
    detail->setToolTip(detail->text()); table->setItem(row, 2, detail);
  }
  child<QLabel>(this, "wellFactorHint")->setText(wf->wellFactorMessage());
}

QVariantMap ConstraintPage::newConstraintLineParams(const QString &type) const {
  const auto spin = [this](const char *name) { return findChild<QDoubleSpinBox *>(QString::fromLatin1(name))->value(); };
  QVariantMap params;
  if (type == QLatin1String("direction_line")) {
    params.insert(QStringLiteral("ratio"), spin("factorDirectionRatioSpin"));
    params.insert(QStringLiteral("influenceRadius"), spin("factorInfluenceSpin"));
    params.insert(QStringLiteral("coreRadius"), spin("factorCoreSpin"));
  }
  if (type == QLatin1String("interpretive_boundary")) {
    params.insert(QStringLiteral("softStrength"), spin("factorSoftStrengthSpin"));
    params.insert(QStringLiteral("softRadius"), spin("factorSoftRadiusSpin"));
  }
  if (type == QLatin1String("break_line") || type == QLatin1String("contour_stop"))
    params.insert(QStringLiteral("blockMode"), findChild<QComboBox *>(QStringLiteral("constraintBlockModeCombo"))->currentData());
  return params;
}
