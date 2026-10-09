// 层：视图
#include "ui/paleotheme.h"
#include "constraintpage.h"
#include "constraintpage_internal.h"

#include "pageshared.h"

#include "../../qgis/qgislayerservice.h"
#include "../../services/singlefactordef.h"
#include "../../workflow/workflows.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QHeaderView>
#include <QScrollArea>
#include <QShowEvent>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

using namespace paleo::pagesinternal;
using namespace paleo::constraintpage_internal;

// ---------------------------------------------------------------------------
// ConstraintPage — ②约束与单因素（m2(B) 单因素图页）
// 方向 96：本体只留 ctor 编排 + 因子清单面 + 图层服务绑定/生成态/厚度刷新；
// 其余构建段与方法分布 constraintpage_wellfactors/_interpolation/_generate/
// _constraints/_sections.cpp（见 constraintpage.h private 区段图）。
// ---------------------------------------------------------------------------

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

  // 方向 96：自此处起 ctor 按原文行序分解为构建段（段图见 constraintpage.h；
  // 调用序 = 原文行序，创建序/连接序逐行保持）。
  buildWellFactorSection( content, lay, horizons, factors );
  QLabel *cellCaption = buildFieldAndCellRows( lay );
  buildMethodSection( content, lay );
  buildBoundaryRows( content, lay );
  buildAdvancedSection( content, lay );
  buildSurfaceRow( lay );
  buildGenerateSection( content, lay );
  buildContourSection( content, lay );
  wireFactorTableSelection( factors );
  buildConstraintArea( content, lay, horizons, cellCaption );
  buildTailSections( content, lay );
  wireWorkflowFeedback( wf );
  wireInputStaleness();
  wireWellFactorTriggers( wf, horizons, factors );
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

void ConstraintPage::wireFactorTableSelection( QTableWidget *factors )
{
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
