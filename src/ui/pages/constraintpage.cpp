#include "constraintpage.h"

#include "panelshared.h"

#include "../../domain/arearules.h" // 历史存量 include（勿增新 io include）
#include "../../domain/mappinghorizons.h"
#include "../../qgis/qgislayerservice.h"
#include "../../services/singlefactordef.h"
#include "../../workflow/workflows.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QShowEvent>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

using namespace PaleoPanel;

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
ConstraintPage::ConstraintPage( ConstraintWorkflow *wf, QWidget *parent )
  : QWidget( parent )
{
  auto *lay = panelLayout( this );

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

  auto *field = new QLineEdit( QStringLiteral( "z" ), this );
  field->setObjectName( QStringLiteral( "factorFieldEdit" ) );
  field->setPlaceholderText( tr( "井属性字段" ) );
  field->setAccessibleName( tr( "单因素插值字段" ) );
  lay->addWidget( field );

  auto *cell = new QDoubleSpinBox( this );
  cell->setObjectName( QStringLiteral( "factorCellSizeSpin" ) );
  cell->setRange( 0.0001, 1.0e9 );
  cell->setDecimals( 4 );
  cell->setValue( 1.0 );
  cell->setAccessibleName( tr( "单因素像元大小" ) );
  lay->addWidget( cell );

  auto *generate = new QPushButton( tr( "生成单因素图" ), this );
  generate->setObjectName( QStringLiteral( "generateFactorButton" ) );
  generate->setEnabled( false ); // 先勾选一个单因素（updateFactorActionStates 管 tooltip）
  lay->addWidget( generate );
  connect( generate, &QPushButton::clicked, this, [this, horizons, field, cell, factors] {
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
    emit generateFactorRequested( factorId, horizons->currentText(), params );
  } );

  auto *interval = new QDoubleSpinBox( this );
  interval->setObjectName( QStringLiteral( "contourIntervalSpin" ) );
  interval->setRange( 0.01, 1.0e9 );
  interval->setDecimals( 2 );
  interval->setValue( 20.0 );
  interval->setSuffix( tr( " m" ) );
  interval->setAccessibleName( tr( "等值线间距" ) );
  lay->addWidget( interval );

  auto *contour = new QPushButton( tr( "生成等值线" ), this );
  contour->setObjectName( QStringLiteral( "contourButton" ) );
  contour->setEnabled( false ); // 勾选且已生成的因素才有可等值线的栅格
  lay->addWidget( contour );
  connect( contour, &QPushButton::clicked, this, [this, interval] {
    const QString layerId = checkedFactorLayerId();
    if ( layerId.isEmpty() )
      return;
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

  lay->addWidget( caption( tr( "约束" ), this ) );
  auto *list = new QListWidget( this );
  list->setObjectName( QStringLiteral( "constraintList" ) );
  list->setAccessibleName( tr( "约束列表" ) );
  lay->addWidget( list, 1 );

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

  // ---- m2(B)：三入口（物源线/展布线/控制点——03_Constraints 组的类型化
  // 约束；shape 决定画布工具，constraintType 进 ConstraintStore 词表）------
  auto *provenance = new QPushButton( tr( "画物源线" ), this );
  provenance->setObjectName( QStringLiteral( "provenanceButton" ) );
  lay->addWidget( provenance );
  connect( provenance, &QPushButton::clicked, this, [this, horizons, spin] {
    emit drawTypedConstraintRequested( horizons->currentText(), QStringLiteral( "line" ),
                                       QStringLiteral( "provenance_line" ), spin->value() );
  } );
  auto *distribution = new QPushButton( tr( "画展布线" ), this );
  distribution->setObjectName( QStringLiteral( "distributionButton" ) );
  lay->addWidget( distribution );
  connect( distribution, &QPushButton::clicked, this, [this, horizons, spin] {
    emit drawTypedConstraintRequested( horizons->currentText(), QStringLiteral( "line" ),
                                       QStringLiteral( "distribution_line" ), spin->value() );
  } );
  auto *controlPoint = new QPushButton( tr( "画控制点" ), this );
  controlPoint->setObjectName( QStringLiteral( "controlPointButton" ) );
  lay->addWidget( controlPoint );
  connect( controlPoint, &QPushButton::clicked, this, [this, horizons, spin] {
    emit drawTypedConstraintRequested( horizons->currentText(), QStringLiteral( "point" ),
                                       QStringLiteral( "control_point" ), spin->value() );
  } );
  // ---- m2(B) 三入口 end ----------------------------------------------------

  // 旧 IDW 行（objectName 保留；runIdwRequested 原语义不动）。
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
  thHint->setStyleSheet( QStringLiteral( "color: #5D6E80; " ) ); // text-muted
  thLay->addWidget( thHint );

  if ( wf ) // workflow feedback lands on the status label
  {
    setProperty( kWfProp, QVariant::fromValue( static_cast<QObject *>( wf ) ) );
    connect( wf, &ConstraintWorkflow::constraintAdded, status,
             [status]( const QString &id ) { status->setText( tr( "已添加约束 %1" ).arg( id ) ); } );
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
  }

  updateFactorActionStates();
}

void ConstraintPage::showEvent( QShowEvent *event )
{
  QWidget::showEvent( event );
  refreshThicknessSamples();
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

void ConstraintPage::updateFactorActionStates()
{
  auto *factors = child<QTableWidget>( this, "factorTable" );
  auto *generate = child<QPushButton>( this, "generateFactorButton" );
  auto *contour = child<QPushButton>( this, "contourButton" );
  if ( !factors || !generate || !contour )
    return;
  const int r = checkedRow( factors );
  if ( r < 0 )
  {
    generate->setEnabled( false );
    generate->setToolTip( tr( "先在清单中勾选一个单因素" ) );
    contour->setEnabled( false );
    contour->setToolTip( tr( "先在清单中勾选一个单因素" ) );
    return;
  }
  generate->setEnabled( true );
  generate->setToolTip( QString() );
  const QString layerId = checkedFactorLayerId();
  if ( layerId.isEmpty() )
  {
    contour->setEnabled( false );
    contour->setToolTip( tr( "该因素尚未生成——先运行「生成单因素图」" ) );
  }
  else
  {
    contour->setEnabled( true );
    contour->setToolTip( QString() );
  }
}
