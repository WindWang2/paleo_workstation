// 层：视图
#include "wellsitingpanel.h"
#include "../windowsizing_internal.h"
#include "pageshared.h"
#include "../paleotheme.h"
#include "../../workflow/wellsitingworkflow.h"
#include "qgis/coordinateeditor.h"

#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include "../notifications/paleonotify.h"
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QToolButton>
#include <QVBoxLayout>

using namespace paleo::pagesinternal;

namespace
{

QDoubleSpinBox *paramSpin( double value, double min, double max, double step,
                           const QString &objectName, const QString &accessibleName,
                           QWidget *parent )
{
  auto *spin = new QDoubleSpinBox( parent );
  spin->setObjectName( objectName );
  spin->setAccessibleName( accessibleName );
  spin->setRange( min, max );
  spin->setDecimals( 0 );
  spin->setSingleStep( step );
  spin->setValue( value );
  spin->setSuffix( QObject::tr( " m" ) );
  spin->setFont( PaleoTheme::monoFont() );
  return spin;
}

QTableWidgetItem *readonlyItem( const QString &text, bool mono = false )
{
  auto *item = new QTableWidgetItem( text );
  item->setFlags( item->flags() & ~Qt::ItemIsEditable );
  if ( mono )
    item->setFont( PaleoTheme::monoFont() );
  if ( mono )
    item->setTextAlignment( Qt::AlignRight | Qt::AlignVCenter );
  return item;
}

QString km2( double area )
{
  return QObject::tr( "%1 km²" ).arg( area, 0, 'f', 2 );
}

QString meters( double value )
{
  return QObject::tr( "%1 m" ).arg( value, 0, 'f', 0 );
}

} // namespace

WellSitingPanel::WellSitingPanel( WellSitingWorkflow *wf, QWidget *parent )
  : QWidget( parent ), m_wf( wf )
{
  setObjectName( QStringLiteral( "wellSitingPanel" ) );
  auto *lay = panelLayout( this );

  // ---- 诊断参数段 ----
  lay->addWidget( caption( tr( "覆盖诊断" ), this ) );
  auto *paramsRow = new QWidget( this );
  // #879：三组参数原挤在单行（最小宽≈475 撑开窄停靠）——改为「标签 | 数值」两列网格。
  auto *paramsLay = new QGridLayout( paramsRow );
  paramsLay->setContentsMargins( 0, 0, 0, 0 );
  paramsLay->setHorizontalSpacing( PaleoTheme::tokens().spacingSm );
  paramsLay->setVerticalSpacing( PaleoTheme::tokens().spacingXs );
  paramsLay->addWidget( new QLabel( tr( "井控半径" ), paramsRow ), 0, 0 );
  auto *radius = paramSpin( 2500, 100, 20000, 100, QStringLiteral( "sitingControlRadius" ),
                            tr( "井控半径" ), paramsRow );
  paramsLay->addWidget( radius, 0, 1 );
  paramsLay->addWidget( new QLabel( tr( "采样格宽" ), paramsRow ), 1, 0 );
  auto *cell = paramSpin( 100, 10, 1000, 10, QStringLiteral( "sitingCellSize" ),
                          tr( "采样格宽" ), paramsRow );
  paramsLay->addWidget( cell, 1, 1 );
  auto *spacingLabel = new QLabel( tr( "候选间距" ), paramsRow );
  paramsLay->addWidget( spacingLabel, 2, 0 );
  auto *spacing = paramSpin( 0, 0, 20000, 100, QStringLiteral( "sitingGridSpacing" ),
                             tr( "候选间距（0 = 同井控半径）" ), paramsRow );
  paramsLay->addWidget( spacing, 2, 1 );
  paramsLay->setColumnStretch( 1, 1 );
  lay->addWidget( paramsRow );

  auto *run = new QPushButton( tr( "诊断覆盖" ), this );
  run->setObjectName( QStringLiteral( "sitingRunButton" ) );
  markPrimaryButton( run );
  lay->addWidget( run );
  connect( run, &QPushButton::clicked, this, &WellSitingPanel::runDiagnosis );

  auto *summary = new QLabel( tr( "还没有运行覆盖诊断" ), this );
  summary->setObjectName( QStringLiteral( "sitingSummaryLabel" ) );
  summary->setWordWrap( true );
  lay->addWidget( summary );

  auto *holeTable = new QTableWidget( 0, 4, this );
  holeTable->setObjectName( QStringLiteral( "sitingHoleTable" ) );
  holeTable->setAccessibleName( tr( "覆盖空洞表" ) );
  holeTable->setHorizontalHeaderLabels(
      { tr( "空洞" ), tr( "面积" ), tr( "最深距" ), tr( "最深点" ) } );
  holeTable->setEditTriggers( QAbstractItemView::NoEditTriggers );
  holeTable->setSelectionBehavior( QAbstractItemView::SelectRows );
  holeTable->verticalHeader()->setVisible( false );
  holeTable->horizontalHeader()->setStretchLastSection( true );
  lay->addWidget( holeTable );

  auto *note = new QLabel( this );
  note->setObjectName( QStringLiteral( "sitingNoteLabel" ) );
  note->setWordWrap( true );
  PaleoTheme::applyThemedStyleSheet( note, [] { return PaleoTheme::mutedCaptionStyleSheet(); } );
  lay->addWidget( note );

  // ---- 候选点位段 ----
  lay->addWidget( caption( tr( "候选点位" ), this ) );
  auto *genRow = new QWidget( this );
  auto *genLay = new QHBoxLayout( genRow );
  genLay->setContentsMargins( 0, 0, 0, 0 );
  auto *gen = new QPushButton( tr( "生成候选" ), genRow );
  gen->setObjectName( QStringLiteral( "sitingGenButton" ) );
  genLay->addWidget( gen );
  auto *clearGen = new QPushButton( tr( "清除候选" ), genRow );
  clearGen->setObjectName( QStringLiteral( "sitingClearGenButton" ) );
  genLay->addWidget( clearGen );
  genLay->addStretch( 1 );
  lay->addWidget( genRow );
  connect( gen, &QPushButton::clicked, this, &WellSitingPanel::generateCandidates );
  connect( clearGen, &QPushButton::clicked, this, [this] {
    if ( m_wf )
    {
      m_wf->clearCandidates();
      refreshCandidateTable();
    }
  } );

  auto *candTable = new QTableWidget( 0, 6, this );
  candTable->setObjectName( QStringLiteral( "sitingCandidateTable" ) );
  candTable->setAccessibleName( tr( "候选点位表" ) );
  candTable->setHorizontalHeaderLabels(
      { tr( "序" ), tr( "策略" ), tr( "X" ), tr( "Y" ), tr( "空洞深度" ), tr( "空洞贡献" ) } );
  candTable->setEditTriggers( QAbstractItemView::NoEditTriggers );
  candTable->setSelectionBehavior( QAbstractItemView::SelectRows );
  candTable->verticalHeader()->setVisible( false );
  candTable->horizontalHeader()->setStretchLastSection( true );
  lay->addWidget( candTable, 1 );

  // ---- 计划井段（交互布点）----
  buildCoordinateSection( lay );

  auto *plannedTable = new QTableWidget( 0, 3, this );
  plannedTable->setObjectName( QStringLiteral( "sitingPlannedTable" ) );
  plannedTable->setAccessibleName( tr( "计划井表" ) );
  plannedTable->setHorizontalHeaderLabels( { tr( "名称" ), tr( "X" ), tr( "Y" ) } );
  plannedTable->setEditTriggers( QAbstractItemView::NoEditTriggers );
  plannedTable->setSelectionBehavior( QAbstractItemView::SelectRows );
  plannedTable->verticalHeader()->setVisible( false );
  plannedTable->horizontalHeader()->setStretchLastSection( true );
  lay->addWidget( plannedTable );

  auto *plannedOps = new QWidget( this );
  auto *opsLay = new QHBoxLayout( plannedOps );
  opsLay->setContentsMargins( 0, 0, 0, 0 );
  auto *rename = new QPushButton( tr( "重命名" ), plannedOps );
  rename->setObjectName( QStringLiteral( "sitingRenameButton" ) );
  rename->setEnabled( false ); // §35 禁用带原因
  rename->setToolTip( tr( "先在计划井表选中一行" ) );
  opsLay->addWidget( rename );
  auto *remove = new QPushButton( tr( "删除" ), plannedOps );
  remove->setObjectName( QStringLiteral( "sitingRemoveButton" ) );
  remove->setEnabled( false );
  remove->setToolTip( tr( "先在计划井表选中一行" ) );
  opsLay->addWidget( remove );
  opsLay->addStretch( 1 );
  lay->addWidget( plannedOps );
  connect( rename, &QPushButton::clicked, this, &WellSitingPanel::renameSelectedPlanned );
  connect( remove, &QPushButton::clicked, this, &WellSitingPanel::removeSelectedPlanned );
  const auto armPlannedOps = [plannedTable, rename, remove]() {
    const bool has = plannedTable->currentRow() >= 0;
    rename->setEnabled( has );
    remove->setEnabled( has );
    if ( has )
    {
      rename->setToolTip( QString() );
      remove->setToolTip( QString() );
    }
    else
    {
      rename->setToolTip( tr( "先在计划井表选中一行" ) );
      remove->setToolTip( tr( "先在计划井表选中一行" ) );
    }
  };
  connect( plannedTable, &QTableWidget::itemSelectionChanged, plannedOps, armPlannedOps );

  // ---- 方案评估段 ----
  buildEvaluationSection( lay );

  // ---- 方案对比段 ----
  lay->addWidget( caption( tr( "方案对比" ), this ) );
  auto *saveRow = new QWidget( this );
  auto *saveLay = new QHBoxLayout( saveRow );
  saveLay->setContentsMargins( 0, 0, 0, 0 );
  saveLay->setSpacing(PaleoTheme::tokens().spacingSm);
  auto *scenarioName = new QLineEdit( saveRow );
  scenarioName->setObjectName( QStringLiteral( "sitingScenarioNameEdit" ) );
  scenarioName->setPlaceholderText( tr( "方案名" ) );
  saveLay->addWidget( scenarioName, 1 );
  auto *save = new QPushButton( tr( "保存方案" ), saveRow );
  save->setObjectName( QStringLiteral( "sitingSaveScenarioButton" ) );
  markPrimaryButton( save );
  save->setEnabled( false ); // §35 禁用带原因
  save->setToolTip( tr( "还没有计划井——先在地图或表单添加布井候选" ) );
  saveLay->addWidget( save );
  connect( save, &QPushButton::clicked, this, &WellSitingPanel::saveScenarioFromInput );
  lay->addWidget( saveRow );

  auto *scenarioTable = new QTableWidget( 0, 5, this );
  scenarioTable->setObjectName( QStringLiteral( "sitingScenarioTable" ) );
  scenarioTable->setAccessibleName( tr( "方案对比表" ) );
  scenarioTable->setHorizontalHeaderLabels(
      { tr( "方案" ), tr( "井数" ), tr( "空洞面积" ), tr( "覆盖率" ), tr( "均距" ) } );
  scenarioTable->setEditTriggers( QAbstractItemView::NoEditTriggers );
  scenarioTable->setSelectionBehavior( QAbstractItemView::SelectRows );
  scenarioTable->verticalHeader()->setVisible( false );
  scenarioTable->horizontalHeader()->setStretchLastSection( true );
  lay->addWidget( scenarioTable, 1 );

  auto *exportRow = new QWidget( this );
  auto *exportLay = new QHBoxLayout( exportRow );
  exportLay->setContentsMargins( 0, 0, 0, 0 );
  exportLay->setSpacing(PaleoTheme::tokens().spacingSm);
  auto *delScenario = new QPushButton( tr( "删除方案" ), exportRow );
  delScenario->setObjectName( QStringLiteral( "sitingDeleteScenarioButton" ) );
  delScenario->setEnabled( false );
  delScenario->setToolTip( tr( "先在方案对比表选中一行" ) );
  exportLay->addWidget( delScenario );
  auto *exportCsv = new QPushButton( tr( "导出点位表 CSV" ), exportRow );
  exportCsv->setObjectName( QStringLiteral( "sitingExportCsvButton" ) );
  exportCsv->setEnabled( false );
  exportCsv->setToolTip( tr( "先在方案对比表选中一行" ) );
  exportLay->addWidget( exportCsv );
  auto *exportChart = new QPushButton( tr( "导出对比图" ), exportRow );
  exportChart->setObjectName( QStringLiteral( "sitingExportChartButton" ) );
  exportLay->addWidget( exportChart );
  exportLay->addStretch( 1 );
  lay->addWidget( exportRow );
  connect( delScenario, &QPushButton::clicked, this, &WellSitingPanel::deleteSelectedScenario );
  connect( exportCsv, &QPushButton::clicked, this, &WellSitingPanel::exportScenarioCsv );
  connect( exportChart, &QPushButton::clicked, this, &WellSitingPanel::exportComparisonChart );
  const auto armScenarioOps = [scenarioTable, delScenario, exportCsv]() {
    const bool has = scenarioTable->currentRow() >= 0;
    delScenario->setEnabled( has );
    exportCsv->setEnabled( has );
    if ( has )
    {
      delScenario->setToolTip( QString() );
      exportCsv->setToolTip( QString() );
    }
    else
    {
      delScenario->setToolTip( tr( "先在方案对比表选中一行" ) );
      exportCsv->setToolTip( tr( "先在方案对比表选中一行" ) );
    }
  };
  connect( scenarioTable, &QTableWidget::itemSelectionChanged, exportRow, armScenarioOps );

  // workflow 信号 → 面板刷新（计划井增删改名/候选/方案变化都实时刷）。
  if ( m_wf )
  {
    connect( m_wf, &WellSitingWorkflow::plannedWellsChanged, this, [this]() {
      refreshPlannedTable();
      refreshEvaluation(); // 目标形态4：计划井变化 → 指标实时刷新
    } );
    connect( m_wf, &WellSitingWorkflow::scenariosChanged, this,
             &WellSitingPanel::refreshScenarioTable );
    connect( m_wf, &WellSitingWorkflow::candidatesChanged, this,
             &WellSitingPanel::refreshCandidateTable );
    connect( m_wf, &WellSitingWorkflow::diagnosisDone, this,
             &WellSitingPanel::refreshDiagnosisLabels );
    // D.3：后台评估交付回 GUI 线程落地（陈旧/取消结果由 workflow 世代号丢弃）。
    connect( m_wf, &WellSitingWorkflow::evaluationAsyncDone, this,
             &WellSitingPanel::onEvaluationAsyncDone );
  }
  reloadFromWorkflow();
}

void WellSitingPanel::buildCoordinateSection( QVBoxLayout *lay )
{
  lay->addWidget( caption( tr( "计划井（布井候选）" ), this ) );
  m_pointEditor = new PaleoCoordinateEditor( this );
  m_pointEditor->setObjectName( QStringLiteral( "sitingCoordinateEditor" ) );
  m_pointEditor->xEdit()->setObjectName( QStringLiteral( "sitingXEdit" ) );
  m_pointEditor->yEdit()->setObjectName( QStringLiteral( "sitingYEdit" ) );
  m_pointEditor->xEdit()->setFont( PaleoTheme::monoFont() );
  m_pointEditor->yEdit()->setFont( PaleoTheme::monoFont() );
  const auto gridCrs = paleo::coordinates::engineeringGrid();
  m_pointEditor->setSourceCrs( gridCrs );
  m_pointEditor->setTargetCrs( gridCrs );
  auto *pick = m_pointEditor->mapPickButton();
  pick->setObjectName( QStringLiteral( "sitingPickButton" ) );
  pick->setText( tr( "地图布点" ) );
  pick->setToolTip( tr( "在地图上拾取一个计划井点位，按当前工程转换上下文写入；支持吸附。" ) );
  lay->addWidget( m_pointEditor->widget() );
  auto *nameRow = new QWidget( this );
  auto *nameLay = new QHBoxLayout( nameRow );
  nameLay->setContentsMargins( 0, 0, 0, 0 );
  nameLay->setSpacing( PaleoTheme::tokens().spacingSm );
  nameLay->addWidget( new QLabel( tr( "名称" ), nameRow ) );
  auto *name = new QLineEdit( nameRow );
  name->setObjectName( QStringLiteral( "sitingNameEdit" ) );
  name->setPlaceholderText( tr( "计划井名" ) );
  nameLay->addWidget( name, 1 );
  auto *add = new QPushButton( tr( "添加" ), nameRow );
  add->setObjectName( QStringLiteral( "sitingAddButton" ) );
  nameLay->addWidget( add );
  lay->addWidget( nameRow );
  connect( pick, &QToolButton::clicked, this, &WellSitingPanel::mapPlacementRequested );
  connect( add, &QPushButton::clicked, this, &WellSitingPanel::addPlannedFromInputs );
}

namespace
{

WellSitingParams paramsFromPanel( const WellSitingPanel *panel )
{
  WellSitingParams params;
  if ( auto *radius = panel->findChild<QDoubleSpinBox *>(
           QStringLiteral( "sitingControlRadius" ) ) )
    params.controlRadius = radius->value();
  if ( auto *cell = panel->findChild<QDoubleSpinBox *>( QStringLiteral( "sitingCellSize" ) ) )
    params.cellSize = cell->value();
  if ( auto *spacing =
           panel->findChild<QDoubleSpinBox *>( QStringLiteral( "sitingGridSpacing" ) ) )
    params.gridSpacing = spacing->value();
  return params;
}

} // namespace

// 方案评估段（D.3 从 ctor 拆出：评估含全场采样，改后台线程异步跑——忙面给
// 不确定进度条 + 取消钮，不冻界面；无进度回调，不伪造百分比）。
void WellSitingPanel::buildEvaluationSection( QVBoxLayout *lay )
{
  lay->addWidget( caption( tr( "方案评估" ), this ) );
  auto *evalLabel = new QLabel( tr( "还没有评估——添加计划井后自动评估" ), this );
  evalLabel->setObjectName( QStringLiteral( "sitingEvalLabel" ) );
  evalLabel->setWordWrap( true );
  lay->addWidget( evalLabel );
  auto *evalRow = new QWidget( this );
  auto *evalLay = new QHBoxLayout( evalRow );
  evalLay->setContentsMargins( 0, 0, 0, 0 );
  auto *eval = new QPushButton( tr( "重新评估" ), evalRow );
  eval->setObjectName( QStringLiteral( "sitingEvalButton" ) );
  evalLay->addWidget( eval );
  auto *evalProgress = new QProgressBar( evalRow );
  evalProgress->setObjectName( QStringLiteral( "sitingEvalProgress" ) );
  evalProgress->setRange( 0, 0 ); // 不确定进度（跑马灯）
  evalProgress->setTextVisible( false );
  evalProgress->setMaximumWidth( paleo::ui_detail::charColumns( evalProgress, 16 ) ); // R6 #845
  evalProgress->hide();
  evalLay->addWidget( evalProgress );
  auto *evalCancel = new QPushButton( tr( "取消评估" ), evalRow );
  evalCancel->setObjectName( QStringLiteral( "sitingEvalCancelButton" ) );
  evalCancel->setToolTip( tr( "停止正在后台运行的方案评估（已算出的指标不落地）" ) );
  evalCancel->hide();
  evalLay->addWidget( evalCancel );
  evalLay->addStretch( 1 );
  lay->addWidget( evalRow );
  connect( eval, &QPushButton::clicked, this, &WellSitingPanel::refreshEvaluation );
  connect( evalCancel, &QPushButton::clicked, this, [this]() {
    if ( m_wf )
      m_wf->cancelScenarioEvaluation();
    m_evalBusy = false;
    m_evalPending = false;
    setEvalBusy( false );
    if ( auto *label = findChild<QLabel *>( QStringLiteral( "sitingEvalLabel" ) ) )
      label->setText( tr( "评估已取消" ) );
  } );
}

void WellSitingPanel::reloadFromWorkflow()
{
  refreshPlannedTable();
  refreshScenarioTable();
  refreshCandidateTable();
  refreshDiagnosisLabels();
  refreshEvaluation();
}

void WellSitingPanel::runDiagnosis()
{
  if ( !m_wf )
    return;
  QString error;
  if ( !m_wf->runDiagnosis( paramsFromPanel( this ), &error ) )
    PaleoNotify::warning( this, tr( "覆盖诊断" ), error );
  refreshDiagnosisLabels();
}

void WellSitingPanel::refreshDiagnosisLabels()
{
  if ( !m_wf )
    return;
  auto *summary = findChild<QLabel *>( QStringLiteral( "sitingSummaryLabel" ) );
  auto *note = findChild<QLabel *>( QStringLiteral( "sitingNoteLabel" ) );
  auto *table = findChild<QTableWidget *>( QStringLiteral( "sitingHoleTable" ) );
  if ( !summary || !table )
    return;
  const QVariantMap last = m_wf->lastDiagnosisSummary();
  if ( last.isEmpty() )
  {
    summary->setText( tr( "还没有运行覆盖诊断" ) );
    if ( note )
      note->clear();
    table->setRowCount( 0 );
    return;
  }
  summary->setText(
      tr( "%1 口井 · %2 个空洞（%3）· 覆盖率 %4% · 域面积 %5 · 域来源：%6" )
          .arg( last.value( QStringLiteral( "well_count" ) ).toInt() )
          .arg( last.value( QStringLiteral( "hole_count" ) ).toInt() )
          .arg( km2( last.value( QStringLiteral( "hole_area_km2" ) ).toDouble() ) )
          .arg( last.value( QStringLiteral( "coverage_ratio" ) ).toDouble() * 100.0, 0, 'f', 1 )
          .arg( km2( last.value( QStringLiteral( "domain_area_km2" ) ).toDouble() ) )
          .arg( last.value( QStringLiteral( "domain_source" ) ).toString() ) );
  if ( note )
    note->setText( last.value( QStringLiteral( "note" ) ).toString() );

  table->setRowCount( 0 );
  for ( const QVariantMap &hole : m_wf->lastHoles() )
  {
    const int row = table->rowCount();
    table->insertRow( row );
    table->setItem( row, 0, readonlyItem( QStringLiteral( "#%1" ).arg(
                                    hole.value( QStringLiteral( "id" ) ).toInt() ) ) );
    table->setItem( row, 1,
                    readonlyItem( km2( hole.value( QStringLiteral( "area_km2" ) ).toDouble() ),
                                  true ) );
    const QVariant maxDistance = hole.value( QStringLiteral( "max_distance_m" ) );
    table->setItem( row, 2,
                    readonlyItem( maxDistance.isNull()
                                      ? tr( "∞（无井）" )
                                      : meters( maxDistance.toDouble() ),
                                  true ) );
    table->setItem( row, 3,
                    readonlyItem( QStringLiteral( "%1, %2" )
                                      .arg( hole.value( QStringLiteral( "deepest_x" ) ).toDouble(),
                                            0, 'f', 0 )
                                      .arg( hole.value( QStringLiteral( "deepest_y" ) ).toDouble(),
                                            0, 'f', 0 ),
                                  true ) );
  }
}

void WellSitingPanel::generateCandidates()
{
  if ( !m_wf )
    return;
  QString error;
  if ( !m_wf->generateCandidates( paramsFromPanel( this ), &error ) )
    PaleoNotify::warning( this, tr( "候选生成" ), error );
  refreshCandidateTable();
}

void WellSitingPanel::refreshCandidateTable()
{
  if ( !m_wf )
    return;
  auto *table = findChild<QTableWidget *>( QStringLiteral( "sitingCandidateTable" ) );
  if ( !table )
    return;
  table->setRowCount( 0 );
  for ( const QVariantMap &c : m_wf->lastCandidates() )
  {
    const int row = table->rowCount();
    table->insertRow( row );
    table->setItem( row, 0,
                    readonlyItem( QString::number( c.value( QStringLiteral( "index" ) ).toInt() ),
                                  true ) );
    table->setItem( row, 1, readonlyItem( c.value( QStringLiteral( "strategy" ) ).toString() ==
                                                  QLatin1String( "deepest" )
                                                  ? tr( "最大空洞圆心" )
                                                  : tr( "规则网格" ) ) );
    table->setItem( row, 2, readonlyItem( QString::number(
                                    c.value( QStringLiteral( "x" ) ).toDouble(), 'f', 0 ),
                                  true ) );
    table->setItem( row, 3, readonlyItem( QString::number(
                                    c.value( QStringLiteral( "y" ) ).toDouble(), 'f', 0 ),
                                  true ) );
    table->setItem( row, 4, readonlyItem( meters(
                                    c.value( QStringLiteral( "hole_distance_m" ) ).toDouble() ),
                                  true ) );
    table->setItem( row, 5,
                    readonlyItem( km2( c.value( QStringLiteral( "contribution_hole_area_m2" ) )
                                           .toDouble() /
                                       1e6 ),
                                  true ) );
  }
}

void WellSitingPanel::addPlannedFromInputs()
{
  if ( !m_wf || !m_pointEditor )
    return;
  auto point = paleo::coordinates::point();
  QString error;
  if ( !m_pointEditor->targetPoint( &point, &error ) )
  {
    m_pointEditor->showError( error );
    PaleoNotify::warning( this, tr( "添加计划井" ), error );
    return;
  }
  auto *name = findChild<QLineEdit *>( QStringLiteral( "sitingNameEdit" ) );
  const QString id = m_wf->addPlannedWell(
      name && !name->text().isEmpty() ? name->text() : tr( "计划井" ),
      point.x(), point.y(), &error );
  if ( id.isEmpty() )
    PaleoNotify::warning( this, tr( "添加计划井" ), error );
}

void WellSitingPanel::setCoordinateCanvas( QWidget *canvas )
{
  if ( m_pointEditor )
    m_pointEditor->setMapCanvas( canvas );
}

void WellSitingPanel::placePlannedAt( double x, double y )
{
  if ( !m_wf )
    return;
  auto *name = findChild<QLineEdit *>( QStringLiteral( "sitingNameEdit" ) );
  const QString base = name && !name->text().isEmpty() ? name->text() : tr( "计划井" );
  QString error;
  if ( m_wf->addPlannedWell( base, x, y, &error ).isEmpty() )
    PaleoNotify::warning( this, tr( "地图布点" ), error );
}

void WellSitingPanel::placeMapPoint( double x, double y )
{
  if ( !m_pointEditor )
    return;
  if ( !m_pointEditor->showMapPoint( x, y ) )
  {
    PaleoNotify::warning( this, tr( "地图布点" ), tr( "无法把地图坐标转换到计划井输入坐标系，未添加计划井。" ) );
    return;
  }
  addPlannedFromInputs();
}

void WellSitingPanel::refreshPlannedTable()
{
  if ( !m_wf )
    return;
  auto *table = findChild<QTableWidget *>( QStringLiteral( "sitingPlannedTable" ) );
  auto *save = findChild<QPushButton *>( QStringLiteral( "sitingSaveScenarioButton" ) );
  if ( save )
  {
    const bool has = !m_wf->plannedWells().isEmpty();
    save->setEnabled( has );
    save->setToolTip( has ? QString()
                          : tr( "还没有计划井——先在地图或表单添加布井候选" ) );
  }
  if ( !table )
    return;
  table->setRowCount( 0 );
  for ( const QVariantMap &w : m_wf->plannedWells() )
  {
    const int row = table->rowCount();
    table->insertRow( row );
    auto *nameItem = readonlyItem( w.value( QStringLiteral( "name" ) ).toString() );
    nameItem->setData( Qt::UserRole, w.value( QStringLiteral( "id" ) ).toString() );
    table->setItem( row, 0, nameItem );
    table->setItem( row, 1, readonlyItem( QString::number(
                                    w.value( QStringLiteral( "x" ) ).toDouble(), 'f', 0 ),
                                  true ) );
    table->setItem( row, 2, readonlyItem( QString::number(
                                    w.value( QStringLiteral( "y" ) ).toDouble(), 'f', 0 ),
                                  true ) );
  }
}

void WellSitingPanel::removeSelectedPlanned()
{
  if ( !m_wf )
    return;
  auto *table = findChild<QTableWidget *>( QStringLiteral( "sitingPlannedTable" ) );
  const int row = table ? table->currentRow() : -1;
  if ( row < 0 || !table->item( row, 0 ) )
    return;
  const QString id = table->item( row, 0 )->data( Qt::UserRole ).toString();
  // PW-050：破坏性操作先确认（无撤销）。
  if ( !PaleoNotify::ask( this, tr( "删除计划井" ),
                          tr( "删除计划井“%1”？此操作无法撤销。" ).arg( table->item( row, 0 )->text() ),
                          PaleoNotify::AskButtons::YesNo, PaleoNotify::AskDefault::Reject,
                          PaleoNotify::AskIcon::Warning ) )
    return;
  QString error;
  if ( !m_wf->removePlannedWell( id, &error ) )
    PaleoNotify::warning( this, tr( "删除计划井" ), error );
}

void WellSitingPanel::renameSelectedPlanned()
{
  if ( !m_wf )
    return;
  auto *table = findChild<QTableWidget *>( QStringLiteral( "sitingPlannedTable" ) );
  const int row = table ? table->currentRow() : -1;
  if ( row < 0 || !table->item( row, 0 ) )
    return;
  const QString id = table->item( row, 0 )->data( Qt::UserRole ).toString();
  const QString old = table->item( row, 0 )->text();
  bool ok = false;
  const QString name = QInputDialog::getText( this, tr( "重命名计划井" ),
                                              tr( "计划井新名称" ), QLineEdit::Normal, old, &ok );
  if ( !ok || name.trimmed().isEmpty() )
    return;
  QString error;
  if ( !m_wf->renamePlannedWell( id, name, &error ) )
    PaleoNotify::warning( this, tr( "重命名计划井" ), error );
}

void WellSitingPanel::refreshEvaluation()
{
  if ( !m_wf )
    return;
  auto *label = findChild<QLabel *>( QStringLiteral( "sitingEvalLabel" ) );
  if ( !label )
    return;
  QStringList plannedIds;
  for ( const QVariantMap &w : m_wf->plannedWells() )
    plannedIds.append( w.value( QStringLiteral( "id" ) ).toString() );
  if ( plannedIds.isEmpty() )
  {
    // 计划井清空：在飞评估失去意义——停掉并回到「还没有评估」态。
    if ( m_evalBusy )
    {
      m_wf->cancelScenarioEvaluation();
      m_evalBusy = false;
      m_evalPending = false;
      setEvalBusy( false );
    }
    label->setText( tr( "还没有评估——添加计划井后自动评估" ) );
    return;
  }
  if ( m_evalBusy )
  {
    // 单飞：忙期到达的新意图只登记待办，落地后补跑一次（不在池里堆任务）。
    m_evalPending = true;
    return;
  }
  m_evalBusy = true;
  setEvalBusy( true );
  label->setText( tr( "评估计算中…" ) );
  m_wf->evaluateScenarioAsync( paramsFromPanel( this ), plannedIds );
}

void WellSitingPanel::onEvaluationAsyncDone( const QVariantMap &evaluation,
                                             const QString &error )
{
  if ( !evaluation.isEmpty() )
    applyEvaluation( evaluation );
  else if ( auto *label = findChild<QLabel *>( QStringLiteral( "sitingEvalLabel" ) ) )
    label->setText( error.isEmpty() ? tr( "评估失败" ) : error );
  m_evalBusy = false;
  setEvalBusy( false );
  // 忙期积压的新意图（连改计划井）——补跑一次，只落地最后状态。
  if ( m_evalPending )
  {
    m_evalPending = false;
    refreshEvaluation();
  }
}

void WellSitingPanel::setEvalBusy( bool busy )
{
  m_evalBusy = busy;
  if ( auto *cancel = findChild<QPushButton *>( QStringLiteral( "sitingEvalCancelButton" ) ) )
    cancel->setVisible( busy );
  if ( auto *bar = findChild<QProgressBar *>( QStringLiteral( "sitingEvalProgress" ) ) )
    bar->setVisible( busy );
  // 忙期禁掉会触发同类重算的动作；计划井增删改名仍可用（新意图走待办补跑）。
  for ( const QString &name : { QStringLiteral( "sitingRunButton" ),
                                QStringLiteral( "sitingGenButton" ),
                                QStringLiteral( "sitingEvalButton" ) } )
    if ( auto *btn = findChild<QPushButton *>( name ) )
      btn->setEnabled( !busy );
  // finding-135：保存方案纳入忙期禁用清单——它内部会同步重算全场指标（与在飞
  // 评估重复采样、冻结界面）。解除忙态时按「有无计划井」恢复其启用条件（与
  // refreshPlannedTable 同口径），避免清空计划井后残留启用。
  if ( auto *save = findChild<QPushButton *>( QStringLiteral( "sitingSaveScenarioButton" ) ) )
  {
    if ( busy )
    {
      save->setEnabled( false );
      save->setToolTip( tr( "评估计算中——请等待或取消" ) );
    }
    else
    {
      const bool has = m_wf && !m_wf->plannedWells().isEmpty();
      save->setEnabled( has );
      save->setToolTip( has ? QString()
                            : tr( "还没有计划井——先在地图或表单添加布井候选" ) );
    }
  }
}

void WellSitingPanel::applyEvaluation( const QVariantMap &evaluation )
{
  auto *label = findChild<QLabel *>( QStringLiteral( "sitingEvalLabel" ) );
  if ( !label )
    return;
  const QVariantMap before = evaluation.value( QStringLiteral( "before" ) ).toMap();
  const QVariantMap after = evaluation.value( QStringLiteral( "after" ) ).toMap();
  label->setText( tr( "基线：空洞 %1 / 覆盖率 %2% / 均距 %3\n"
                      "方案（%4 口计划井）：空洞 %5 / 覆盖率 %6% / 均距 %7" )
                      .arg( km2( before.value( QStringLiteral( "hole_area_total" ) ).toDouble() /
                                 1e6 ) )
                      .arg( before.value( QStringLiteral( "coverage_ratio" ) ).toDouble() * 100.0,
                            0, 'f', 1 )
                      .arg( meters( before.value( QStringLiteral( "domain_mean_distance" ) )
                                        .toDouble() ) )
                      .arg( evaluation.value( QStringLiteral( "candidate_count" ) ).toInt() )
                      .arg( km2( after.value( QStringLiteral( "hole_area_total" ) ).toDouble() /
                                 1e6 ) )
                      .arg( after.value( QStringLiteral( "coverage_ratio" ) ).toDouble() * 100.0,
                            0, 'f', 1 )
                      .arg(
                          meters( after.value( QStringLiteral( "domain_mean_distance" ) ).toDouble() ) ) );
}

void WellSitingPanel::saveScenarioFromInput()
{
  if ( !m_wf )
    return;
  auto *name = findChild<QLineEdit *>( QStringLiteral( "sitingScenarioNameEdit" ) );
  QStringList plannedIds;
  for ( const QVariantMap &w : m_wf->plannedWells() )
    plannedIds.append( w.value( QStringLiteral( "id" ) ).toString() );
  QString error;
  if ( !m_wf->saveScenario( name ? name->text() : QString(), plannedIds,
                            paramsFromPanel( this ), &error ) )
    PaleoNotify::warning( this, tr( "保存方案" ), error );
}

void WellSitingPanel::refreshScenarioTable()
{
  if ( !m_wf )
    return;
  auto *table = findChild<QTableWidget *>( QStringLiteral( "sitingScenarioTable" ) );
  if ( !table )
    return;
  table->setRowCount( 0 );
  for ( const QVariantMap &s : m_wf->scenarios() )
  {
    const QVariantMap after = s.value( QStringLiteral( "metrics_after" ) ).toMap();
    const int row = table->rowCount();
    table->insertRow( row );
    auto *nameItem = readonlyItem( s.value( QStringLiteral( "name" ) ).toString() );
    nameItem->setData( Qt::UserRole, s.value( QStringLiteral( "id" ) ).toString() );
    table->setItem( row, 0, nameItem );
    table->setItem( row, 1,
                    readonlyItem( QString::number( s.value( QStringLiteral( "well_count" ) ).toInt() ),
                                  true ) );
    table->setItem( row, 2, readonlyItem( km2( after.value( QStringLiteral( "hole_area_total" ) )
                                                   .toDouble() /
                                               1e6 ),
                                          true ) );
    table->setItem( row, 3, readonlyItem( QStringLiteral( "%1%" ).arg(
                                             after.value( QStringLiteral( "coverage_ratio" ) )
                                                   .toDouble() * 100.0, 0, 'f', 1 ),
                                          true ) );
    table->setItem( row, 4,
                    readonlyItem( meters( after.value( QStringLiteral( "domain_mean_distance" ) )
                                              .toDouble() ),
                                  true ) );
  }
}

void WellSitingPanel::deleteSelectedScenario()
{
  if ( !m_wf )
    return;
  auto *table = findChild<QTableWidget *>( QStringLiteral( "sitingScenarioTable" ) );
  const int row = table ? table->currentRow() : -1;
  if ( row < 0 || !table->item( row, 0 ) )
    return;
  const QString id = table->item( row, 0 )->data( Qt::UserRole ).toString();
  if ( !PaleoNotify::ask( this, tr( "删除方案" ),
                          tr( "删除方案“%1”及其计划井？此操作无法撤销。" ).arg( table->item( row, 0 )->text() ),
                          PaleoNotify::AskButtons::YesNo, PaleoNotify::AskDefault::Reject,
                          PaleoNotify::AskIcon::Warning ) )
    return;
  QString error;
  if ( !m_wf->deleteScenario( id, &error ) )
    PaleoNotify::warning( this, tr( "删除方案" ), error );
}

void WellSitingPanel::exportScenarioCsv()
{
  if ( !m_wf )
    return;
  auto *table = findChild<QTableWidget *>( QStringLiteral( "sitingScenarioTable" ) );
  const int row = table ? table->currentRow() : -1;
  if ( row < 0 || !table->item( row, 0 ) )
    return;
  emit exportRequested( QStringLiteral( "csv" ),
                        table->item( row, 0 )->data( Qt::UserRole ).toString() );
}

void WellSitingPanel::exportComparisonChart()
{
  if ( !m_wf )
    return;
  emit exportRequested( QStringLiteral( "chart" ), QString() );
}
