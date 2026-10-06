// 层：视图
#include "realizationpanel.h"

#include "../../catalog/datacatalog.h"
#include "../../catalog/realizationset.h"
#include "../pages/pageshared.h"
#include "../paleotheme.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QStandardItemModel>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

using namespace paleo::pagesinternal;

namespace
{
// 集合条目文案：标题 + 在场/声明成员数；不全附缺号（诚实面——缺号从契约的
// missingIndices 直读，不靠文件存在猜测）。
QString setItemText( const paleo::realization::RealizationSet &set )
{
  QString text = QStringLiteral( "%1（%2/%3 成员）" )
                     .arg( set.title )
                     .arg( set.members.size() )
                     .arg( set.declaredCount );
  if ( !set.missingIndices.isEmpty() )
  {
    QStringList miss;
    for ( const int idx : set.missingIndices )
      miss << QStringLiteral( "#%1" ).arg( idx );
    text += RealizationPanel::tr( " · 缺 %1" ).arg( miss.join( QStringLiteral( "、" ) ) );
  }
  return text;
}
} // namespace

RealizationPanel::RealizationPanel( QWidget *parent )
    : QWidget( parent )
{
  setObjectName( QStringLiteral( "realizationPanel" ) );
  setAccessibleName( tr( "不确定性集合" ) );
  auto *lay = new QVBoxLayout( this );
  lay->setContentsMargins( 0, 0, 0, 0 );
  lay->setSpacing( PaleoTheme::tokens().spacingSm );

  lay->addWidget( caption( tr( "预测集合" ), this ) );
  m_setCombo = new QComboBox( this );
  m_setCombo->setObjectName( QStringLiteral( "realizationSetCombo" ) );
  m_setCombo->setAccessibleName( tr( "选择 realization 集合" ) );
  lay->addWidget( m_setCombo );

  // ---- 成员切换（同画布）：下拉选成员 + 滑块步进——两控件同一份当前态 ----
  lay->addWidget( caption( tr( "集合成员" ), this ) );
  m_memberCombo = new QComboBox( this );
  m_memberCombo->setObjectName( QStringLiteral( "realizationMemberCombo" ) );
  m_memberCombo->setAccessibleName( tr( "切换集合成员" ) );
  lay->addWidget( m_memberCombo );
  m_memberSlider = new QSlider( Qt::Horizontal, this );
  m_memberSlider->setObjectName( QStringLiteral( "realizationMemberSlider" ) );
  m_memberSlider->setAccessibleName( tr( "成员序滑块" ) );
  m_memberSlider->setSingleStep( 1 );
  lay->addWidget( m_memberSlider );
  m_memberState = caption( QString(), this );
  m_memberState->setObjectName( QStringLiteral( "realizationMemberState" ) );
  m_memberState->setWordWrap( true );
  lay->addWidget( m_memberState );

  auto *playRow = new QHBoxLayout();
  playRow->setSpacing( PaleoTheme::tokens().spacingXs );
  m_playButton = new QPushButton( tr( "▶ 成员动画" ), this );
  m_playButton->setObjectName( QStringLiteral( "realizationPlayButton" ) );
  m_playButton->setAccessibleName( tr( "按成员序播放集合动画" ) );
  m_frameLabel = new QLabel( this );
  m_frameLabel->setObjectName( QStringLiteral( "realizationFrameLabel" ) );
  playRow->addWidget( m_playButton );
  playRow->addWidget( m_frameLabel, 1 );
  lay->addLayout( playRow );

  // ---- 统计面（独立 DERIVED 版本；口径词与图签同源）----
  lay->addWidget( caption( tr( "不确定性统计面" ), this ) );
  m_statList = new QListWidget( this );
  m_statList->setObjectName( QStringLiteral( "realizationStatList" ) );
  m_statList->setAccessibleName( tr( "集合统计面列表" ) );
  m_statList->setMaximumHeight( 120 );
  lay->addWidget( m_statList );
  m_deriveButton = new QPushButton( tr( "派生统计面（均值/标准差/P10/P90）" ), this );
  m_deriveButton->setObjectName( QStringLiteral( "realizationDeriveButton" ) );
  lay->addWidget( m_deriveButton );

  // ---- 集合对比（两集合均值差）----
  lay->addWidget( caption( tr( "集合对比" ), this ) );
  auto *diffRow = new QHBoxLayout();
  diffRow->setSpacing( PaleoTheme::tokens().spacingXs );
  m_diffA = new QComboBox( this );
  m_diffA->setObjectName( QStringLiteral( "realizationDiffA" ) );
  m_diffB = new QComboBox( this );
  m_diffB->setObjectName( QStringLiteral( "realizationDiffB" ) );
  diffRow->addWidget( m_diffA, 1 );
  diffRow->addWidget( new QLabel( tr( "−" ), this ) );
  diffRow->addWidget( m_diffB, 1 );
  lay->addLayout( diffRow );
  m_diffButton = new QPushButton( tr( "对比两集合均值" ), this );
  m_diffButton->setObjectName( QStringLiteral( "realizationDiffButton" ) );
  lay->addWidget( m_diffButton );

  m_timer = new QTimer( this );
  m_timer->setInterval( 600 );
  connect( m_timer, &QTimer::timeout, this, &RealizationPanel::onTick );

  connect( m_setCombo, &QComboBox::currentIndexChanged, this, [this] {
    m_currentSetId = m_setCombo->currentData().toString();
    m_currentMember = -1;
    setPlaying( false );
    rebuildMembers();
    rebuildStats();
  } );
  // activated（非 currentIndexChanged）：程序化刷新不抢发成员切换。
  connect( m_memberCombo, &QComboBox::activated, this, [this]( int row ) {
    const int idx = m_memberCombo->itemData( row ).toInt();
    // 缺号位 userData 为 -1——不可选（Qt 已禁），防御分支如实报。
    if ( idx < 0 )
    {
      emit statusMessage( tr( "成员 #%1 缺席——集合不完整" ).arg( row ) );
      return;
    }
    m_currentMember = idx;
    emit memberShowRequested( m_currentSetId, idx );
  } );
  connect( m_memberSlider, &QSlider::valueChanged, this, [this]( int pos ) {
    const QList<int> order = frameOrder();
    if ( pos < 0 || pos >= order.size() )
      return;
    const int idx = order.at( pos );
    m_currentMember = idx;
    const int comboRow = m_memberCombo->findData( idx );
    if ( comboRow >= 0 )
      m_memberCombo->setCurrentIndex( comboRow );
    emit memberShowRequested( m_currentSetId, idx );
  } );
  connect( m_statList, &QListWidget::itemActivated, this, [this]( QListWidgetItem *item ) {
    const QString token = item ? item->data( Qt::UserRole ).toString() : QString();
    if ( !token.isEmpty() )
      emit statShowRequested( m_currentSetId, token );
  } );
  connect( m_deriveButton, &QPushButton::clicked, this, [this] {
    if ( !m_currentSetId.isEmpty() )
      emit deriveStatsRequested( m_currentSetId );
  } );
  connect( m_diffButton, &QPushButton::clicked, this, [this] {
    const QString a = m_diffA->currentData().toString();
    const QString b = m_diffB->currentData().toString();
    if ( a.isEmpty() || b.isEmpty() )
    {
      emit statusMessage( tr( "集合对比需要两个集合" ) );
      return;
    }
    if ( a == b )
    {
      emit statusMessage( tr( "集合对比需要两个不同的集合" ) );
      return;
    }
    emit diffRequested( a, b );
  } );
  connect( m_playButton, &QPushButton::clicked, this, [this] {
    setPlaying( !m_playing );
  } );

  rebuildSets();
}

void RealizationPanel::bindCatalog( DataCatalog *catalog )
{
  // #236-3：每次（重）绑定都是工程边界——"ast-N" 序号 id 跨工程必撞，旧选择
  // 不得沿用到新工程同号的不同集合上。catalog 指针可能跨工程复用，故即便
  // 同指针也清选择并重建。
  m_currentSetId.clear();
  if ( m_catalog == catalog )
  {
    rebuildSets();
    return;
  }
  if ( m_catalog )
    disconnect( m_catalog, nullptr, this, nullptr );
  m_catalog = catalog;
  if ( m_catalog )
    connect( m_catalog, &DataCatalog::changed, this, [this] { rebuildSets(); } );
  rebuildSets();
}

QList<int> RealizationPanel::frameOrder() const
{
  QList<int> order;
  if ( !m_catalog || m_currentSetId.isEmpty() )
    return order;
  const paleo::realization::RealizationSet set =
      paleo::realization::setById( *m_catalog, m_currentSetId );
  for ( const paleo::realization::RealizationMember &m : set.members )
    order << m.index;
  return order; // set.members 已按 index 升序——帧序即成员序。
}

void RealizationPanel::setPlayIntervalMs( int ms )
{
  m_timer->setInterval( ms );
}

QString RealizationPanel::missingSummary() const
{
  if ( m_missing.isEmpty() )
    return QString();
  QStringList miss;
  for ( const int idx : m_missing )
    miss << QStringLiteral( "#%1" ).arg( idx );
  return tr( "缺成员 %1" ).arg( miss.join( QStringLiteral( "、" ) ) );
}

void RealizationPanel::rebuildSets()
{
  const QString keep = m_currentSetId;
  const QSignalBlocker block( m_setCombo );
  m_setCombo->clear();
  m_diffA->clear();
  m_diffB->clear();
  if ( !m_catalog || !m_catalog->isOpen() )
  {
    m_currentSetId.clear();
    m_currentMember = -1;
    rebuildMembers();
    rebuildStats();
    return;
  }
  for ( const paleo::realization::RealizationSet &set :
        paleo::realization::enumerateSets( *m_catalog ) )
  {
    m_setCombo->addItem( setItemText( set ), set.setId );
    // 对比候选 = 成员 ≥2 的集合（均值面可派生；单成员集合对比无意义）。
    if ( set.members.size() >= 2 )
    {
      m_diffA->addItem( set.title, set.setId );
      m_diffB->addItem( set.title, set.setId );
    }
  }
  const int keepRow = m_setCombo->findData( keep );
  m_setCombo->setCurrentIndex( keepRow >= 0 ? keepRow : ( m_setCombo->count() ? 0 : -1 ) );
  m_currentSetId = m_setCombo->currentData().toString();
  m_currentMember = -1;
  rebuildMembers();
  rebuildStats();
}

void RealizationPanel::rebuildMembers()
{
  const QSignalBlocker blockCombo( m_memberCombo );
  const QSignalBlocker blockSlider( m_memberSlider );
  m_memberCombo->clear();
  m_missing.clear();
  const paleo::realization::RealizationSet set =
      ( m_catalog && !m_currentSetId.isEmpty() )
          ? paleo::realization::setById( *m_catalog, m_currentSetId )
          : paleo::realization::RealizationSet();
  m_missing = QList<int>( set.missingIndices.begin(), set.missingIndices.end() );

  const int declared = set.declaredCount;
  for ( int i = 0; i < declared; ++i )
  {
    const bool present =
        !paleo::realization::memberVersionId( set, i ).isEmpty();
    if ( present )
    {
      m_memberCombo->addItem( tr( "成员 #%1" ).arg( i ), i );
    }
    else
    {
      // 缺号位如实列出（禁用——选不中，但序号在列表里可见）。
      const int row = m_memberCombo->count();
      m_memberCombo->addItem( tr( "成员 #%1（缺席）" ).arg( i ), -1 );
      if ( auto *model = qobject_cast<QStandardItemModel *>( m_memberCombo->model() ) )
        if ( auto *si = model->item( row ) )
          si->setEnabled( false );
    }
  }
  const QList<int> order = frameOrder();
  m_memberSlider->setRange( 0, qMax( 0, order.size() - 1 ) );
  m_memberSlider->setEnabled( order.size() > 1 );

  // 诚实态文案：空集合 / 单成员 / 缺号。
  if ( set.isEmpty() )
    m_memberState->setText( m_currentSetId.isEmpty()
                                ? tr( "尚无集合——SGS 跑「生成单因素图」自动建集合" )
                                : tr( "集合无成员" ) );
  else if ( set.members.size() == 1 )
    m_memberState->setText( tr( "单成员集合——无不确定性；统计派生禁用" ) );
  else
  {
    const QString miss = missingSummary();
    m_memberState->setText(
        miss.isEmpty() ? tr( "%1 成员完整" ).arg( set.members.size() )
                       : tr( "%1 成员在场；%2" ).arg( set.members.size() ).arg( miss ) );
  }
  m_playButton->setEnabled( order.size() > 1 );
  m_frameLabel->setText( order.size() > 1
                             ? tr( "%1 帧" ).arg( order.size() )
                             : QString() );
  if ( m_playing && order.size() <= 1 )
    setPlaying( false );
}

void RealizationPanel::rebuildStats()
{
  m_statList->clear();
  if ( !m_catalog || m_currentSetId.isEmpty() )
  {
    m_deriveButton->setEnabled( false );
    m_deriveButton->setToolTip( tr( "先选一个集合" ) );
    return;
  }
  const paleo::realization::RealizationSet set =
      paleo::realization::setById( *m_catalog, m_currentSetId );
  for ( const paleo::realization::StatisticSurface &s :
        paleo::realization::statSurfaces( *m_catalog, m_currentSetId ) )
  {
    const QString label = paleo::realization::statisticDisplayLabel( s.token );
    auto *item = new QListWidgetItem(
        tr( "%1 · %2 成员" ).arg( label.isEmpty() ? s.token : label ).arg( s.memberCount ),
        m_statList );
    item->setData( Qt::UserRole, s.token );
    item->setToolTip( tr( "口径 token：%1（双击上图）" ).arg( s.token ) );
  }
  // 派生只在成员 ≥2 时有意义（单成员/零成员如实禁用，不产「零离散」假面）。
  const bool canDerive = set.members.size() >= 2;
  m_deriveButton->setEnabled( canDerive );
  m_deriveButton->setToolTip(
      canDerive ? tr( "为当前集合派生均值/总体标准差/P10/P90 统计面" )
                : tr( "成员不足两个——单成员集合无不确定性可派生" ) );
}

void RealizationPanel::onTick()
{
  const QList<int> order = frameOrder();
  if ( order.size() <= 1 )
  {
    setPlaying( false );
    return;
  }
  m_framePos = ( m_framePos + 1 ) % order.size();
  const int idx = order.at( m_framePos );
  m_currentMember = idx;
  const int comboRow = m_memberCombo->findData( idx );
  if ( comboRow >= 0 )
    m_memberCombo->setCurrentIndex( comboRow );
  {
    const QSignalBlocker block( m_memberSlider );
    m_memberSlider->setValue( m_framePos ); // 滑块回显不再发切换（本条已发）
  }
  m_frameLabel->setText( tr( "帧 %1/%2 · 成员 #%3" )
                             .arg( m_framePos + 1 )
                             .arg( order.size() )
                             .arg( idx ) );
  emit memberShowRequested( m_currentSetId, idx );
}

void RealizationPanel::setPlaying( bool playing )
{
  if ( m_playing == playing )
    return;
  m_playing = playing;
  m_playButton->setText( playing ? tr( "■ 停止" ) : tr( "▶ 成员动画" ) );
  if ( playing )
  {
    m_framePos = -1;
    m_timer->start();
  }
  else
    m_timer->stop();
}
