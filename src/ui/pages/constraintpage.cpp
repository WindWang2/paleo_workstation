// 层：视图
#include "constraintpage.h"

#include "pageshared.h"

#include "../../domain/arearules.h" // 历史存量 include（勿增新 io include）
#include "../../domain/mappinghorizons.h"
#include "../../qgis/qgislayerservice.h"
#include "../../services/singlefactordef.h"
#include "../../workflow/workflows.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
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

  QString factorIdOfRow( QTableWidget *table, int row )
  {
    if ( !table || row < 0 || row >= table->rowCount() )
      return QString();
    return table->item( row, 0 ) ? table->item( row, 0 )->data( Qt::UserRole ).toString()
                                 : QString();
  }

  int checkedRow( QTableWidget *table )
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

  lay->addWidget( caption( tr( "井属性字段" ), this ) );
  auto *field = new QLineEdit( QStringLiteral( "z" ), this );
  field->setObjectName( QStringLiteral( "factorFieldEdit" ) );
  field->setPlaceholderText( tr( "井属性字段" ) );
  field->setAccessibleName( tr( "单因素插值字段" ) );
  lay->addWidget( field );

  lay->addWidget( caption( tr( "像元大小" ), this ) );
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
  method->addItem( tr( "本地方向插值" ), QStringLiteral( "local_direction_idw" ) );
  method->addItem( tr( "原约束 IDW" ), QStringLiteral( "legacy" ) );
  method->setAccessibleName( tr( "成图方法" ) );
  lay->addWidget( method );

  auto *coverageCaption = caption( tr( "覆盖方式" ), content );
  coverageCaption->setObjectName( QStringLiteral( "factorCoverageCaption" ) );
  lay->addWidget( coverageCaption );
  auto *coverage = new QComboBox( content );
  coverage->setObjectName( QStringLiteral( "factorCoverageCombo" ) );
  coverage->addItem( tr( "井点支撑" ), QStringLiteral( "well_supported" ) );
  coverage->addItem( tr( "域内外推" ), QStringLiteral( "domain_extrapolation" ) );
  coverage->setAccessibleName( tr( "覆盖方式" ) );
  lay->addWidget( coverage );

  auto *advanced = new CollapsibleSection( tr( "高级参数" ), content );
  advanced->setObjectName( QStringLiteral( "factorAdvancedSection" ) );
  advanced->setExpanded( false );
  lay->addWidget( advanced );
  auto *adv = advanced->containerLayout();
  auto *power = new QDoubleSpinBox( advanced->container() );
  power->setObjectName( QStringLiteral( "factorPowerSpin" ) );
  power->setRange( 0.01, 100.0 );
  power->setDecimals( 2 );
  power->setValue( 2.0 );
  power->setToolTip( tr( "幂次建议 0.5–8。算法接受任意有限正数。" ) );
  useMono( power );
  adv->addWidget( caption( tr( "幂次" ), advanced->container() ) );
  adv->addWidget( power );
  auto *cluster = new QCheckBox( tr( "井群局部权重" ), advanced->container() );
  cluster->setObjectName( QStringLiteral( "factorClusterCheck" ) );
  cluster->setChecked( false );
  cluster->setToolTip( tr( "默认关闭。打开后按井群距离降低边缘井的权重，不是无数据掩膜。" ) );
  adv->addWidget( cluster );
  auto *ratio = new QDoubleSpinBox( advanced->container() );
  ratio->setObjectName( QStringLiteral( "factorDirectionRatioSpin" ) );
  ratio->setRange( 1.0, 100.0 );
  ratio->setDecimals( 2 );
  ratio->setValue( 8.0 );
  ratio->setToolTip( tr( "方向线的新任务默认比值。保存到选中的约束线。" ) );
  useMono( ratio );
  adv->addWidget( caption( tr( "方向比值" ), advanced->container() ) );
  adv->addWidget( ratio );
  auto *influence = new QDoubleSpinBox( advanced->container() );
  influence->setObjectName( QStringLiteral( "factorInfluenceSpin" ) );
  influence->setRange( 0.0, 1.0e12 );
  influence->setDecimals( 2 );
  influence->setSpecialValueText( tr( "自动" ) );
  influence->setToolTip( tr( "0 表示按井距和线长自动取影响半径。" ) );
  useMono( influence );
  adv->addWidget( caption( tr( "方向影响半径" ), advanced->container() ) );
  adv->addWidget( influence );
  auto *core = new QDoubleSpinBox( advanced->container() );
  core->setObjectName( QStringLiteral( "factorCoreSpin" ) );
  core->setRange( 0.0, 1.0e12 );
  core->setDecimals( 2 );
  core->setSpecialValueText( tr( "自动" ) );
  core->setToolTip( tr( "0 表示核心半径取影响半径的 0.3。" ) );
  useMono( core );
  adv->addWidget( caption( tr( "方向核心半径" ), advanced->container() ) );
  adv->addWidget( core );
  auto *softStrength = new QDoubleSpinBox( advanced->container() );
  softStrength->setObjectName( QStringLiteral( "factorSoftStrengthSpin" ) );
  softStrength->setRange( 0.0, 0.8 );
  softStrength->setSingleStep( 0.05 );
  softStrength->setDecimals( 2 );
  softStrength->setValue( 0.35 );
  softStrength->setToolTip( tr( "软边界强度。0 表示这条线不改变权重。" ) );
  useMono( softStrength );
  adv->addWidget( caption( tr( "软边界强度" ), advanced->container() ) );
  adv->addWidget( softStrength );
  auto *softRadius = new QDoubleSpinBox( advanced->container() );
  softRadius->setObjectName( QStringLiteral( "factorSoftRadiusSpin" ) );
  softRadius->setRange( 0.0, 1.0e12 );
  softRadius->setDecimals( 2 );
  softRadius->setSpecialValueText( tr( "自动" ) );
  softRadius->setToolTip( tr( "0 表示自动软边界半径，与显示缓冲无关。" ) );
  useMono( softRadius );
  adv->addWidget( caption( tr( "软边界半径" ), advanced->container() ) );
  adv->addWidget( softRadius );

  // 主线6：等厚引擎（strathick）专属行——顶/底构造面栅格选择。默认隐藏，
  // 勾选等厚引擎因素时展开（updateEngineRows 管可见性）。
  auto *surfaceRow = new QWidget( this );
  surfaceRow->setObjectName( QStringLiteral( "factorSurfaceRow" ) );
  auto *surfaceLay = new QHBoxLayout( surfaceRow );
  surfaceLay->setContentsMargins( 0, 0, 0, 0 );
  surfaceLay->setSpacing( 4 );
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
           [this, horizons, field, cell, factors, topCombo, baseCombo, method, coverage, power, cluster] {
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
    if ( known && interpolantEngine( def.processingAlgId ) )
    {
      const QString methodId = method->currentData().toString();
      if ( methodId != QLatin1String( "legacy" ) )
        params.insert( QStringLiteral( "method" ), methodId );
      params.insert( QStringLiteral( "coverage" ), coverage->currentData().toString() );
      params.insert( QStringLiteral( "power" ), power->value() );
      params.insert( QStringLiteral( "wellClusterLocality" ), cluster->isChecked() );
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
  interval->setRange( 0.01, 1.0e9 );
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
             updateFactorActionStates();
           } );
  // ---- m2(B) 双区 end ------------------------------------------------------

  lay->addSpacing( 16 ); // spacing.md：单因素区与约束区分组
  lay->addWidget( caption( tr( "约束" ), this ) );
  auto *list = new QListWidget( this );
  list->setObjectName( QStringLiteral( "constraintList" ) );
  list->setAccessibleName( tr( "约束列表" ) );
  lay->addWidget( list, 1 );
  connect( list, &QListWidget::currentItemChanged, this, [this]( QListWidgetItem *, QListWidgetItem * ) {
    loadSelectedConstraintLine();
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
  auto *saveLine = new QPushButton( tr( "保存约束参数" ), content );
  saveLine->setObjectName( QStringLiteral( "constraintParamSaveButton" ) );
  saveLine->setEnabled( false );
  saveLine->setToolTip( tr( "先在约束列表中选择一条线" ) );
  lay->addWidget( saveLine );
  connect( saveLine, &QPushButton::clicked, this, [this, semantic, ratio, influence, core, softStrength, softRadius] {
    auto *rows = child<QListWidget>( this, "constraintList" );
    QListWidgetItem *item = rows ? rows->currentItem() : nullptr;
    if ( !item )
      return;
    auto *bound = qobject_cast<ConstraintWorkflow *>( property( kWfProp ).value<QObject *>() );
    if ( !bound )
      return;
    QVariantMap lineParams = selectedLineParams();
    lineParams.insert( QStringLiteral( "semantic" ), semantic->currentData().toString() );
    lineParams.insert( QStringLiteral( "ratio" ), ratio->value() );
    lineParams.insert( QStringLiteral( "influenceRadius" ), influence->value() );
    lineParams.insert( QStringLiteral( "coreRadius" ), core->value() );
    lineParams.insert( QStringLiteral( "softStrength" ), softStrength->value() );
    lineParams.insert( QStringLiteral( "softRadius" ), softRadius->value() );
    lineParams.insert( QStringLiteral( "enabled" ), true );
    QString err;
    if ( !bound->updateConstraintLine( item->data( Qt::UserRole ).toString(), lineParams, &err ) )
    {
      auto *status = child<QLabel>( this, "statusLabel" );
      if ( status )
        status->setText( err.isEmpty() ? tr( "约束参数保存失败" ) : err );
      return;
    }
    refreshConstraintList();
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
  typedRow->setSpacing( 4 ); // xs
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
  softRow->setSpacing( 4 );
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
  // ---- 类型化约束线 end ----------------------------------------------------

  // 旧 IDW 行（objectName 保留；runIdwRequested 原语义不动）。
  lay->addSpacing( 16 ); // spacing.md：约束区与 IDW 区分组
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

  auto *status = new QLabel( this );
  status->setObjectName( QStringLiteral( "statusLabel" ) );
  status->setWordWrap( true );
  lay->addWidget( status );

  // ---- 阶段C 厚度样本表（autoplan §5C）-------------------------------------
  // 逐井：井名 / D61 TVD / D62 TVD / 层间速度或原因。行表由 MappingWorkflow
  // 镜像到 ConstraintWorkflow 的 paleo.thickness.* 动态属性；不足样本的两句
  // （「厚度样本不足以成面」/「没有厚度样本」）渲染在 thicknessHint，不弹框。
  // m2(B)：整段挪进 CollapsibleSection（objectName 全保留，默认展开）。
  lay->addSpacing( 16 ); // spacing.md
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
    connect( combo, qOverload<int>( &QComboBox::currentIndexChanged ), this, [mark]( int ) { mark(); } );
  if ( auto *combo = child<QComboBox>( this, "factorCoverageCombo" ) )
    connect( combo, qOverload<int>( &QComboBox::currentIndexChanged ), this, [mark]( int ) { mark(); } );
  if ( auto *horizons = child<QComboBox>( this, "horizonCombo" ) )
    connect( horizons, qOverload<int>( &QComboBox::currentIndexChanged ), this, [this]( int ) { refreshConstraintList(); } );

  refreshConstraintList();
  updateFactorActionStates();
}

void ConstraintPage::showEvent( QShowEvent *event )
{
  QWidget::showEvent( event );
  refreshThicknessSamples();
  refreshConstraintList();
}

void ConstraintPage::refreshThicknessSamples()
{
  auto *table = child<QTableWidget>( this, "thicknessTable" );
  auto *hint = child<QLabel>( this, "thicknessHint" );
  if ( !table )
    return;
  auto *wf = qobject_cast<ConstraintWorkflow *>( property( kWfProp ).value<QObject *>() );
  const QVariantList rows =
      wf ? wf->property( "paleo.thickness.samples" ).toList() : QVariantList();
  const QString message =
      wf ? wf->property( "paleo.thickness.message" ).toString() : QString();

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
  // layerDeclared（任何来源——重开工程/重算）里 factor.* 声明 → 状态列刷新。
  connect( svc, &QgisLayerService::layerDeclared, this,
           [this]( const QString &layerId ) {
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
        list->setCurrentItem( item );
    }
  }
  list->blockSignals( false );
  updateFactorActionStates();
}
