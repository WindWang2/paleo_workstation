// 层：视图
#include "sequenceframeworkpanel.h"
#include "paleotheme.h"

#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

#include "../domain/mappinghorizons.h"
#include "../domain/wellrecords.h"
#include "sequenceframeworkcolumn.h"

namespace
{
  constexpr int kRoleUnitId = Qt::UserRole + 1;
  constexpr int kRoleMarkerId = Qt::UserRole + 2;
  constexpr int kRoleCandidateRow = Qt::UserRole + 3;
  constexpr int kRoleDiagnosticRow = Qt::UserRole + 4;
}

SequenceFrameworkPanel::SequenceFrameworkPanel( QWidget *parent )
  : QWidget( parent )
{
  auto *root = new QVBoxLayout( this );
  root->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm);
  root->setSpacing(PaleoTheme::tokens().spacingSm);

  auto *title = new QLabel( tr( "层序地层格架" ), this );
  QFont titleFont = title->font();
  titleFont.setPointSize(PaleoTheme::tokens().titlePt);
  title->setFont( titleFont );
  root->addWidget( title );

  m_splitter = new QSplitter( Qt::Horizontal, this );

  // ---- 左：格架树 + 操作按钮 ----
  auto *leftWidget = new QWidget( m_splitter );
  auto *leftLayout = new QVBoxLayout( leftWidget );
  leftLayout->setContentsMargins( 0, 0, 0, 0 );
  m_tree = new QTreeWidget( leftWidget );
  m_tree->setObjectName( QStringLiteral( "sfFrameworkTree" ) ); // 无头测试定位用
  m_tree->setHeaderLabels( QStringList() << tr( "格架单元" ) << tr( "层位区间" )
                                         << tr( "厚度(m)" ) );
  m_tree->setRootIsDecorated( true );
  m_tree->setAlternatingRowColors( false );
  m_tree->header()->setSectionResizeMode( 0, QHeaderView::Stretch );
  leftLayout->addWidget( m_tree, 1 );

  auto *buttonRow = new QHBoxLayout();
  auto *addSeqButton = new QPushButton( tr( "新增层序" ), leftWidget );
  auto *addTractButton = new QPushButton( tr( "新增体系域" ), leftWidget );
  auto *renameButton = new QPushButton( tr( "重命名" ), leftWidget );
  auto *removeButton = new QPushButton( tr( "删除" ), leftWidget );
  buttonRow->addWidget( addSeqButton );
  buttonRow->addWidget( addTractButton );
  buttonRow->addWidget( renameButton );
  buttonRow->addWidget( removeButton );
  leftLayout->addLayout( buttonRow );

  auto *orderRow = new QHBoxLayout();
  auto *upButton = new QPushButton( tr( "上移" ), leftWidget );
  auto *downButton = new QPushButton( tr( "下移" ), leftWidget );
  m_saveButton = new QPushButton( tr( "保存格架" ), leftWidget );
  m_saveButton->setEnabled( false ); // 无改动不落盘——避免空涨 catalog revision
  auto *reloadButton = new QPushButton( tr( "重新载入" ), leftWidget );
  orderRow->addWidget( upButton );
  orderRow->addWidget( downButton );
  orderRow->addWidget( m_saveButton );
  orderRow->addWidget( reloadButton );
  leftLayout->addLayout( orderRow );

  connect( addSeqButton, &QPushButton::clicked, this, &SequenceFrameworkPanel::onAddSequence );
  connect( addTractButton, &QPushButton::clicked, this, &SequenceFrameworkPanel::onAddTract );
  connect( renameButton, &QPushButton::clicked, this, &SequenceFrameworkPanel::onRename );
  connect( removeButton, &QPushButton::clicked, this, &SequenceFrameworkPanel::onRemove );
  connect( upButton, &QPushButton::clicked, this, &SequenceFrameworkPanel::onMoveUp );
  connect( downButton, &QPushButton::clicked, this, &SequenceFrameworkPanel::onMoveDown );
  connect( m_saveButton, &QPushButton::clicked, this, &SequenceFrameworkPanel::onSave );
  connect( reloadButton, &QPushButton::clicked, this, &SequenceFrameworkPanel::onReload );
  connect( m_tree, &QTreeWidget::itemSelectionChanged, this,
           &SequenceFrameworkPanel::onTreeSelectionChanged );
  m_splitter->addWidget( leftWidget );

  // ---- 右：柱状视图 + 页签 ----
  auto *rightWidget = new QWidget( m_splitter );
  auto *rightLayout = new QVBoxLayout( rightWidget );
  rightLayout->setContentsMargins( 0, 0, 0, 0 );
  m_column = new SequenceFrameworkColumnView( rightWidget );
  m_column->setMinimumHeight( 160 );
  rightLayout->addWidget( m_column, 1 );
  connect( m_column, &SequenceFrameworkColumnView::currentUnitChanged,
           this, &SequenceFrameworkPanel::onColumnUnitChanged );

  m_tabs = new QTabWidget( rightWidget );
  m_markerTable = new QTableWidget( m_tabs );
  m_markerTable->setObjectName( QStringLiteral( "sfMarkerTable" ) );
  m_markerTable->setColumnCount( 3 );
  m_markerTable->setHorizontalHeaderLabels( QStringList()
                                            << tr( "标志层" ) << tr( "所属单元" )
                                            << tr( "引用分层" ) );
  m_markerTable->horizontalHeader()->setSectionResizeMode( 2, QHeaderView::Stretch );
  m_tabs->addTab( m_markerTable, tr( "标志层" ) );

  m_suggestionTable = new QTableWidget( m_tabs );
  m_suggestionTable->setObjectName( QStringLiteral( "sfSuggestionTable" ) );
  m_suggestionTable->setColumnCount( 4 );
  m_suggestionTable->setHorizontalHeaderLabels( QStringList()
                                                << tr( "确认" ) << tr( "井" ) << tr( "分层" )
                                                << tr( "建议单元" ) );
  m_suggestionTable->horizontalHeader()->setSectionResizeMode( 3, QHeaderView::Stretch );
  auto *suggestWidget = new QWidget( m_tabs );
  auto *suggestLayout = new QVBoxLayout( suggestWidget );
  suggestLayout->setContentsMargins( 0, 0, 0, 0 );
  suggestLayout->addWidget( m_suggestionTable, 1 );
  auto *suggestButtonRow = new QHBoxLayout();
  auto *refreshSuggestButton = new QPushButton( tr( "生成建议" ), suggestWidget );
  auto *applyButton = new QPushButton( tr( "应用已确认" ), suggestWidget );
  suggestButtonRow->addWidget( refreshSuggestButton );
  suggestButtonRow->addWidget( applyButton );
  suggestButtonRow->addStretch( 1 );
  suggestLayout->addLayout( suggestButtonRow );
  connect( refreshSuggestButton, &QPushButton::clicked, this,
           &SequenceFrameworkPanel::onRefreshSuggestions );
  connect( applyButton, &QPushButton::clicked, this,
           &SequenceFrameworkPanel::onApplySuggestions );
  m_tabs->addTab( suggestWidget, tr( "井间建议" ) );

  m_diagTree = new QTreeWidget( m_tabs );
  m_diagTree->setObjectName( QStringLiteral( "sfDiagnosticTree" ) );
  m_diagTree->setHeaderLabels( QStringList() << tr( "严重度" ) << tr( "诊断项" )
                                             << tr( "定位" ) );
  m_diagTree->header()->setSectionResizeMode( 2, QHeaderView::Stretch );
  auto *diagWidget = new QWidget( m_tabs );
  auto *diagLayout = new QVBoxLayout( diagWidget );
  diagLayout->setContentsMargins( 0, 0, 0, 0 );
  diagLayout->addWidget( m_diagTree, 1 );
  auto *diagButtonRow = new QHBoxLayout();
  auto *refreshDiagButton = new QPushButton( tr( "重新诊断" ), diagWidget );
  auto *exportButton = new QPushButton( tr( "导出报告" ), diagWidget );
  diagButtonRow->addWidget( refreshDiagButton );
  diagButtonRow->addWidget( exportButton );
  diagButtonRow->addStretch( 1 );
  diagLayout->addLayout( diagButtonRow );
  connect( refreshDiagButton, &QPushButton::clicked, this,
           &SequenceFrameworkPanel::onRefreshDiagnostics );
  connect( exportButton, &QPushButton::clicked, this, [this]() {
    QString err;
    const QString path = QStringLiteral( "%1/sequence-framework-report.md" ).arg( QDir::currentPath() );
    if ( exportReport( path, &err ) )
      emit statusMessage( tr( "诊断报告已导出：%1" ).arg( path ) );
    else
      emit statusMessage( tr( "导出失败：%1" ).arg( err ) );
  } );
  m_tabs->addTab( diagWidget, tr( "一致性诊断" ) );

  rightLayout->addWidget( m_tabs, 1 );
  m_splitter->addWidget( rightWidget );
  m_splitter->setStretchFactor( 0, 2 );
  m_splitter->setStretchFactor( 1, 3 );
  root->addWidget( m_splitter, 1 );

  setEnabled( false );
}

void SequenceFrameworkPanel::setService( SequenceFramework::FrameworkService *service )
{
  m_service = service;
  setEnabled( m_service != nullptr );
  if ( m_service == nullptr )
  {
    m_draft = SequenceFramework::Framework();
    m_report = SequenceFramework::DiagnosticReport();
    m_suggestions.clear();
    rebuildTree();
    rebuildMarkers();
    rebuildSuggestionTable();
    rebuildDiagnosticTree();
    refreshColumn();
    return;
  }
  m_draft = m_service->framework();
  rebuildTree();
  rebuildMarkers();
  refreshColumn();
  markDirty( false );
}

// ---- 程序化入口 ----

bool SequenceFrameworkPanel::addSequence( const QString &name, const QString &topBoundary,
                                          const QString &baseBoundary, double thickness )
{
  if ( name.trimmed().isEmpty() )
    return false;
  SequenceFramework::FrameworkUnit u;
  u.id = SequenceFramework::nextUnitId( m_draft );
  u.name = name.trimmed();
  u.level = SequenceFramework::UnitLevel::Sequence;
  u.ordinal = SequenceFramework::rootsOf( m_draft ).size();
  u.topBoundary = topBoundary.trimmed();
  u.baseBoundary = baseBoundary.trimmed();
  u.thickness = thickness;
  m_draft.units.append( u );
  rebuildTree();
  refreshColumn();
  markDirty( true );
  return true;
}

bool SequenceFrameworkPanel::addSystemsTract( const QString &parentId, const QString &name,
                                              const QString &topBoundary,
                                              const QString &baseBoundary, double thickness )
{
  if ( name.trimmed().isEmpty() )
    return false;
  if ( SequenceFramework::unitById( m_draft, parentId ) == nullptr )
    return false;
  SequenceFramework::FrameworkUnit u;
  u.id = SequenceFramework::nextUnitId( m_draft );
  u.parentId = parentId;
  u.name = name.trimmed();
  u.level = SequenceFramework::UnitLevel::SystemsTract;
  u.ordinal = SequenceFramework::childrenOf( m_draft, parentId ).size();
  u.topBoundary = topBoundary.trimmed();
  u.baseBoundary = baseBoundary.trimmed();
  u.thickness = thickness;
  m_draft.units.append( u );
  rebuildTree();
  refreshColumn();
  markDirty( true );
  return true;
}

bool SequenceFrameworkPanel::renameUnit( const QString &unitId, const QString &name )
{
  for ( SequenceFramework::FrameworkUnit &u : m_draft.units )
  {
    if ( u.id != unitId )
      continue;
    u.name = name.trimmed();
    rebuildTree();
    refreshColumn();
    markDirty( true );
    return true;
  }
  return false;
}

bool SequenceFrameworkPanel::removeUnit( const QString &unitId )
{
  int index = -1;
  for ( int i = 0; i < m_draft.units.size(); ++i )
    if ( m_draft.units.at( i ).id == unitId )
    {
      index = i;
      break;
    }
  if ( index < 0 )
    return false;
  // 子单元一并删除（二级格架不悬空）。
  m_draft.units.removeAt( index );
  QVector<SequenceFramework::FrameworkUnit> kept;
  for ( const SequenceFramework::FrameworkUnit &u : m_draft.units )
    if ( u.parentId != unitId )
      kept.append( u );
  m_draft.units = kept;
  // 标志层若挂在被删单元上，如实清空归属（诊断面会报悬空）。
  for ( SequenceFramework::MarkerBed &m : m_draft.markers )
    if ( m.unitId == unitId )
      m.unitId.clear();
  rebuildTree();
  rebuildMarkers();
  refreshColumn();
  markDirty( true );
  return true;
}

bool SequenceFrameworkPanel::moveUnit( const QString &unitId, int delta )
{
  SequenceFramework::FrameworkUnit *self = nullptr;
  for ( SequenceFramework::FrameworkUnit &u : m_draft.units )
    if ( u.id == unitId )
      self = &u;
  if ( self == nullptr || delta == 0 )
    return false;
  const int target = self->ordinal - delta;
  // 同级内换位（ordinal 互换），越界不动。
  for ( SequenceFramework::FrameworkUnit &u : m_draft.units )
  {
    if ( u.id == unitId || u.parentId != self->parentId )
      continue;
    if ( u.ordinal != target )
      continue;
    u.ordinal = self->ordinal;
    self->ordinal = target;
    rebuildTree();
    refreshColumn();
    markDirty( true );
    return true;
  }
  return false;
}

bool SequenceFrameworkPanel::addMarker( const QString &name, const QString &unitId,
                                         const QStringList &layerNames )
{
  if ( name.trimmed().isEmpty() )
    return false;
  if ( SequenceFramework::unitById( m_draft, unitId ) == nullptr )
    return false;
  SequenceFramework::MarkerBed m;
  m.id = SequenceFramework::nextMarkerId( m_draft );
  m.name = name.trimmed();
  m.unitId = unitId;
  for ( const QString &layer : layerNames )
    if ( !layer.trimmed().isEmpty() )
      m.layerNames.append( layer.trimmed() );
  m_draft.markers.append( m );
  rebuildMarkers();
  markDirty( true );
  return true;
}

bool SequenceFrameworkPanel::removeMarker( const QString &markerId )
{
  for ( int i = 0; i < m_draft.markers.size(); ++i )
  {
    if ( m_draft.markers.at( i ).id != markerId )
      continue;
    m_draft.markers.removeAt( i );
    rebuildMarkers();
    markDirty( true );
    return true;
  }
  return false;
}

bool SequenceFrameworkPanel::saveFramework()
{
  if ( m_service == nullptr )
  {
    emit statusMessage( tr( "格架服务未接线，保存未完成" ) );
    return false;
  }
  QString err;
  if ( !m_service->save( m_draft, &err ) )
  {
    emit statusMessage( tr( "格架保存失败：%1" ).arg( err ) );
    return false;
  }
  markDirty( false );
  emit statusMessage( tr( "格架已保存" ) );
  return true;
}

bool SequenceFrameworkPanel::reloadFramework()
{
  if ( m_service == nullptr )
    return false;
  QString err;
  if ( !m_service->reload( &err ) )
  {
    emit statusMessage( tr( "格架载入失败：%1" ).arg( err ) );
    return false;
  }
  m_draft = m_service->framework();
  rebuildTree();
  rebuildMarkers();
  refreshColumn();
  markDirty( false );
  return true;
}

void SequenceFrameworkPanel::refreshSuggestions()
{
  m_suggestions.clear();
  if ( m_service != nullptr )
  {
    QString err;
    const QVector<WellTopRecord> tops = m_service->loadWellTops( &err );
    // 只出候选：这里不写库（commitAccepted 才是唯一写口）。
    m_suggestions = m_service->suggest( tops );
  }
  rebuildSuggestionTable();
}

bool SequenceFrameworkPanel::applySuggestions()
{
  if ( m_service == nullptr )
    return false;
  // 勾选状态回读到候选数组（表格第 0 列是复选框）。
  for ( int row = 0; row < m_suggestionTable->rowCount() && row < m_suggestions.size(); ++row )
  {
    auto *item = m_suggestionTable->item( row, 0 );
    const bool accepted = item != nullptr && item->checkState() == Qt::Checked;
    m_suggestions[ row ].accepted = accepted;
  }
  QString err;
  if ( !m_service->commitAccepted( m_suggestions, &err ) )
  {
    emit statusMessage( tr( "建议应用失败：%1" ).arg( err ) );
    return false;
  }
  m_draft = m_service->framework();
  m_suggestions.clear();
  rebuildSuggestionTable();
  rebuildMarkers();
  rebuildTree();
  refreshColumn();
  markDirty( false );
  emit statusMessage( tr( "已确认的建议已写入格架" ) );
  return true;
}

void SequenceFrameworkPanel::refreshDiagnostics()
{
  m_report = SequenceFramework::DiagnosticReport();
  if ( m_service != nullptr )
  {
    QString err;
    const QVector<WellTopRecord> tops = m_service->loadWellTops( &err );
    m_report = m_service->diagnose( tops );
  }
  rebuildDiagnosticTree();
}

bool SequenceFrameworkPanel::exportReport( const QString &path, QString *error ) const
{
  QFile f( path );
  if ( !f.open( QIODevice::WriteOnly ) )
  {
    if ( error )
      *error = tr( "文件无法写入：%1" ).arg( path );
    return false;
  }
  const QByteArray bytes = m_report.toMarkdown().toUtf8();
  if ( f.write( bytes ) != bytes.size() )
  {
    if ( error )
      *error = tr( "报告写入不完整：%1" ).arg( path );
    return false;
  }
  if ( error )
    error->clear();
  return true;
}

QString SequenceFrameworkPanel::currentUnitId() const
{
  return m_column != nullptr ? m_column->currentUnit() : QString();
}

void SequenceFrameworkPanel::setCurrentUnit( const QString &unitId )
{
  if ( m_column != nullptr )
    m_column->setCurrentUnit( unitId );
  QTreeWidgetItem *item = itemForUnit( unitId );
  if ( item != nullptr )
    m_tree->setCurrentItem( item );
}

// ---- 槽 ----

void SequenceFrameworkPanel::onTreeSelectionChanged()
{
  const QString id = selectedUnitId();
  if ( m_column != nullptr && m_column->currentUnit() != id )
  {
    m_column->setCurrentUnit( id );
    emit currentUnitChanged( id );
  }
}

void SequenceFrameworkPanel::onColumnUnitChanged( const QString &unitId )
{
  QTreeWidgetItem *item = itemForUnit( unitId );
  if ( item != nullptr && m_tree->currentItem() != item )
  {
    const bool blocked = m_tree->blockSignals( true );
    m_tree->setCurrentItem( item );
    m_tree->blockSignals( blocked );
  }
  emit currentUnitChanged( unitId );
}

void SequenceFrameworkPanel::onAddSequence()
{
  const QStringList horizons = mappingHorizons();
  bool ok = false;
  const QString name = QInputDialog::getText( this, tr( "新增层序" ), tr( "层序名称" ),
                                              QLineEdit::Normal, QString(), &ok );
  if ( !ok || name.trimmed().isEmpty() )
    return;
  QString top;
  if ( !horizons.isEmpty() )
  {
    top = QInputDialog::getItem( this, tr( "新增层序" ), tr( "顶界层序界面" ), horizons, 0,
                                 false, &ok );
    if ( !ok )
      return;
  }
  QString base;
  if ( !horizons.isEmpty() )
  {
    QStringList withEmpty = horizons;
    withEmpty.prepend( QString() );
    base = QInputDialog::getItem( this, tr( "新增层序" ), tr( "底界层序界面（空=最深层）" ),
                                  withEmpty, 0, false, &ok );
    if ( !ok )
      return;
  }
  if ( !addSequence( name, top, base, 0.0 ) )
    emit statusMessage( tr( "新增层序失败" ) );
}

void SequenceFrameworkPanel::onAddTract()
{
  const QString parent = selectedUnitId();
  if ( parent.isEmpty() )
  {
    emit statusMessage( tr( "请先选中一个层序" ) );
    return;
  }
  const QStringList horizons = mappingHorizons();
  bool ok = false;
  const QString name = QInputDialog::getText( this, tr( "新增体系域" ), tr( "体系域名称" ),
                                              QLineEdit::Normal, QStringLiteral( "TST" ), &ok );
  if ( !ok || name.trimmed().isEmpty() )
    return;
  QString top;
  if ( !horizons.isEmpty() )
  {
    top = QInputDialog::getItem( this, tr( "新增体系域" ), tr( "顶界层序界面" ), horizons, 0,
                                 false, &ok );
    if ( !ok )
      return;
  }
  QString base;
  if ( !horizons.isEmpty() )
  {
    QStringList withEmpty = horizons;
    withEmpty.prepend( QString() );
    base = QInputDialog::getItem( this, tr( "新增体系域" ), tr( "底界层序界面（空=最深层）" ),
                                  withEmpty, 0, false, &ok );
    if ( !ok )
      return;
  }
  if ( !addSystemsTract( parent, name, top, base, 0.0 ) )
    emit statusMessage( tr( "新增体系域失败" ) );
}

void SequenceFrameworkPanel::onRename()
{
  const QString id = selectedUnitId();
  if ( id.isEmpty() )
    return;
  const SequenceFramework::FrameworkUnit *u = SequenceFramework::unitById( m_draft, id );
  bool ok = false;
  const QString name = QInputDialog::getText( this, tr( "重命名" ), tr( "单元名称" ),
                                              QLineEdit::Normal,
                                              u != nullptr ? u->name : QString(), &ok );
  if ( !ok || name.trimmed().isEmpty() )
    return;
  renameUnit( id, name );
}

void SequenceFrameworkPanel::onRemove()
{
  const QString id = selectedUnitId();
  if ( id.isEmpty() )
    return;
  removeUnit( id );
}

void SequenceFrameworkPanel::onMoveUp()
{
  moveUnit( selectedUnitId(), 1 );
}

void SequenceFrameworkPanel::onMoveDown()
{
  moveUnit( selectedUnitId(), -1 );
}

void SequenceFrameworkPanel::onSave()
{
  saveFramework();
}

void SequenceFrameworkPanel::onReload()
{
  reloadFramework();
}

void SequenceFrameworkPanel::onRefreshSuggestions()
{
  refreshSuggestions();
  emit statusMessage( tr( "已生成 %1 条候选（未确认，不写库）" )
                          .arg( QString::number( m_suggestions.size() ) ) );
}

void SequenceFrameworkPanel::onApplySuggestions()
{
  applySuggestions();
}

void SequenceFrameworkPanel::onRefreshDiagnostics()
{
  refreshDiagnostics();
  emit statusMessage( tr( "诊断完成：%1 项（高 %2 / 中 %3 / 低 %4）" )
                          .arg( QString::number( m_report.items.size() ),
                                QString::number( m_report.countOf( SequenceFramework::Severity::High ) ),
                                QString::number( m_report.countOf( SequenceFramework::Severity::Medium ) ),
                                QString::number( m_report.countOf( SequenceFramework::Severity::Low ) ) ) );
}

// ---- 内部 ----

QString SequenceFrameworkPanel::selectedUnitId() const
{
  QTreeWidgetItem *item = m_tree->currentItem();
  if ( item == nullptr )
    return QString();
  return item->data( 0, kRoleUnitId ).toString();
}

QTreeWidgetItem *SequenceFrameworkPanel::itemForUnit( const QString &unitId ) const
{
  if ( unitId.isEmpty() )
    return nullptr;
  for ( int i = 0; i < m_tree->topLevelItemCount(); ++i )
  {
    QTreeWidgetItem *top = m_tree->topLevelItem( i );
    if ( top->data( 0, kRoleUnitId ).toString() == unitId )
      return top;
    for ( int j = 0; j < top->childCount(); ++j )
      if ( top->child( j )->data( 0, kRoleUnitId ).toString() == unitId )
        return top->child( j );
  }
  return nullptr;
}

void SequenceFrameworkPanel::rebuildTree()
{
  const QString keep = selectedUnitId();
  m_tree->clear();
  for ( const SequenceFramework::FrameworkUnit &seq : SequenceFramework::rootsOf( m_draft ) )
  {
    auto *item = new QTreeWidgetItem( m_tree );
    item->setText( 0, seq.name );
    item->setText( 1, seq.topBoundary + ( seq.baseBoundary.isEmpty()
                                              ? QString()
                                              : QStringLiteral( " → " ) + seq.baseBoundary ) );
    item->setText( 2, seq.thickness > 0.0 ? QString::number( seq.thickness, 'f', 1 ) : QString() );
    item->setData( 0, kRoleUnitId, seq.id );
    for ( const SequenceFramework::FrameworkUnit &tract :
          SequenceFramework::childrenOf( m_draft, seq.id ) )
    {
      auto *child = new QTreeWidgetItem( item );
      child->setText( 0, tract.name );
      child->setText( 1, tract.topBoundary + ( tract.baseBoundary.isEmpty()
                                                   ? QString()
                                                   : QStringLiteral( " → " ) + tract.baseBoundary ) );
      child->setText( 2, tract.thickness > 0.0 ? QString::number( tract.thickness, 'f', 1 )
                                               : QString() );
      child->setData( 0, kRoleUnitId, tract.id );
    }
    item->setExpanded( true );
  }
  QTreeWidgetItem *restore = itemForUnit( keep );
  if ( restore != nullptr )
  {
    const bool blocked = m_tree->blockSignals( true );
    m_tree->setCurrentItem( restore );
    m_tree->blockSignals( blocked );
  }
}

void SequenceFrameworkPanel::rebuildMarkers()
{
  m_markerTable->setRowCount( m_draft.markers.size() );
  for ( int i = 0; i < m_draft.markers.size(); ++i )
  {
    const SequenceFramework::MarkerBed &m = m_draft.markers.at( i );
    auto *nameItem = new QTableWidgetItem( m.name );
    nameItem->setData( kRoleMarkerId, m.id );
    m_markerTable->setItem( i, 0, nameItem );
    m_markerTable->setItem( i, 1,
                            new QTableWidgetItem( SequenceFramework::unitPath( m_draft, m.unitId ) ) );
    m_markerTable->setItem( i, 2, new QTableWidgetItem( m.layerNames.join( QStringLiteral( "、" ) ) ) );
  }
}

void SequenceFrameworkPanel::rebuildSuggestionTable()
{
  m_suggestionTable->setRowCount( m_suggestions.size() );
  for ( int i = 0; i < m_suggestions.size(); ++i )
  {
    const SequenceFramework::SuggestionCandidate &c = m_suggestions.at( i );
    auto *check = new QTableWidgetItem();
    check->setFlags( Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable );
    // 默认不勾——未经人工确认的候选不得进库。
    check->setCheckState( Qt::Unchecked );
    check->setData( kRoleCandidateRow, i );
    m_suggestionTable->setItem( i, 0, check );
    m_suggestionTable->setItem( i, 1, new QTableWidgetItem( c.wellName ) );
    m_suggestionTable->setItem( i, 2, new QTableWidgetItem( c.layerName ) );
    m_suggestionTable->setItem( i, 3,
                                new QTableWidgetItem( SequenceFramework::unitPath( m_draft, c.unitId ) ) );
  }
}

void SequenceFrameworkPanel::rebuildDiagnosticTree()
{
  m_diagTree->clear();
  auto severityLabel = []( SequenceFramework::Severity s ) {
    switch ( s )
    {
      case SequenceFramework::Severity::High:
        return QObject::tr( "高" );
      case SequenceFramework::Severity::Medium:
        return QObject::tr( "中" );
      case SequenceFramework::Severity::Low:
        return QObject::tr( "低" );
    }
    return QObject::tr( "低" );
  };
  for ( int i = 0; i < m_report.items.size(); ++i )
  {
    const SequenceFramework::Diagnostic &d = m_report.items.at( i );
    auto *item = new QTreeWidgetItem( m_diagTree );
    item->setText( 0, severityLabel( d.severity ) );
    item->setText( 1, QStringLiteral( "[%1] %2" ).arg( d.code, d.title ) );
    item->setText( 2, d.locations.join( QStringLiteral( "、" ) ) );
    item->setToolTip( 1, d.detail );
    item->setData( 0, kRoleDiagnosticRow, i );
  }
}

void SequenceFrameworkPanel::refreshColumn()
{
  if ( m_column == nullptr )
    return;
  m_column->setFramework( m_draft, mappingHorizons() );
}

void SequenceFrameworkPanel::markDirty( bool dirty )
{
  if ( m_dirty == dirty )
    return;
  m_dirty = dirty;
  if ( m_saveButton != nullptr )
    m_saveButton->setEnabled( dirty );
  emit frameworkDirtyChanged( dirty );
}
