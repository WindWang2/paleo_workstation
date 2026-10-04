// 层：视图
#include "evolutionplayerpanel.h"

#include "../../domain/mappinghorizons.h"
#include "../../linkage/selectioncontext.h"
#include "../../qgis/qgislayerservice.h"
#include "../paleotheme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QToolButton>

EvolutionPlayerPanel::EvolutionPlayerPanel( SelectionContext *selection, QgisLayerService *layers,
                                            QWidget *parent )
  : QWidget( parent )
  , m_selection( selection )
  , m_layers( layers )
  , m_frames( mappingHorizons() )
{
  m_timer = new QTimer( this );
  m_timer->setInterval( 1200 ); // 功能性步进节奏（非编排动画）
  connect( m_timer, &QTimer::timeout, this, [this] {
    if ( m_index + 1 >= m_frames.size() )
    {
      setPlaying( false ); // 到末帧停——不循环（演化是有向序列）
      return;
    }
    stepBy( 1 );
  } );

  buildUi();

  if ( m_selection )
  {
    connect( m_selection, &SelectionContext::activeHorizonChanged, this,
             [this]( const QString &h ) { applyFrame( h ); } );
    applyFrame( m_selection->activeHorizon() );
  }
}

void EvolutionPlayerPanel::buildUi()
{
  auto *lay = new QHBoxLayout( this );
  lay->setContentsMargins( 8, 2, 8, 2 ); // spacing.sm 水平、紧凑垂直
  lay->setSpacing( 4 );                  // spacing.xs

  auto makeButton = [this]( const QString &text, const char *name ) {
    auto *button = new QToolButton( this );
    button->setObjectName( QString::fromLatin1( name ) );
    button->setText( text );
    button->setToolButtonStyle( Qt::ToolButtonTextOnly );
    return button;
  };

  m_prevButton = makeButton( tr( "◀上一期" ), "evolutionPrevButton" );
  m_playButton = makeButton( tr( "播放" ), "evolutionPlayButton" );
  m_playButton->setCheckable( true );
  m_nextButton = makeButton( tr( "下一期▶" ), "evolutionNextButton" );
  m_exportButton = makeButton( tr( "定格导出" ), "evolutionExportButton" );

  m_frameLabel = new QLabel( this );
  m_frameLabel->setObjectName( QStringLiteral( "evolutionFrameLabel" ) );
  m_frameLabel->setFont( PaleoTheme::monoFont() ); // 帧号/层位名走等宽数字面
  m_frameLabel->setAlignment( Qt::AlignCenter );
  m_frameLabel->setMinimumWidth( 90 );

  lay->addWidget( m_prevButton );
  lay->addWidget( m_playButton );
  lay->addWidget( m_nextButton );
  lay->addWidget( m_frameLabel );
  lay->addWidget( m_exportButton );

  connect( m_prevButton, &QToolButton::clicked, this, [this] { stepBy( -1 ); } );
  connect( m_nextButton, &QToolButton::clicked, this, [this] { stepBy( 1 ); } );
  connect( m_playButton, &QToolButton::toggled, this, [this]( bool on ) {
    setPlaying( on );
    m_playButton->setText( on ? tr( "暂停" ) : tr( "播放" ) );
  } );
  connect( m_exportButton, &QToolButton::clicked, this,
           [this] { emit frameExportRequested( currentHorizon() ); } );

  setPlaying( false );
}

QStringList EvolutionPlayerPanel::frameOrder() const
{
  return m_frames;
}

QString EvolutionPlayerPanel::currentHorizon() const
{
  return m_index >= 0 && m_index < m_frames.size() ? m_frames.at( m_index ) : QString();
}

bool EvolutionPlayerPanel::isPlaying() const
{
  return m_timer->isActive();
}

void EvolutionPlayerPanel::setPlayIntervalMs( int ms )
{
  m_timer->setInterval( ms );
}

void EvolutionPlayerPanel::applyFrame( const QString &horizon )
{
  const int index = m_frames.indexOf( horizon );
  if ( index < 0 )
    return; // 集合外层位不挪帧（与 chip「不点亮」同口径）
  m_index = index;
  m_frameLabel->setText( QStringLiteral( "%1 · %2/%3" )
                           .arg( horizon )
                           .arg( index + 1 )
                           .arg( m_frames.size() ) );
}

void EvolutionPlayerPanel::stepBy( int delta )
{
  const int next = m_index + delta;
  if ( next < 0 || next >= m_frames.size() )
    return; // 越界不动（播放方向的末帧停播由 timer 分支处理）
  if ( !switchHorizon( m_frames.at( next ) ) )
  {
    setPlaying( false ); // setPlaying 内部会同步按钮 checked/文案
    return;
  }
  applyFrame( m_frames.at( next ) );
}

bool EvolutionPlayerPanel::switchHorizon( const QString &horizon )
{
  QString editingName;
  if ( m_layers && m_layers->isEditingAnyLayer( &editingName ) )
  {
    emit horizonSwitchRefused(
      tr( "正在编辑「%1」——先保存或放弃编辑，再切换演化帧" ).arg( editingName ) );
    return false;
  }
  if ( m_selection )
    m_selection->setActiveHorizon( horizon );
  if ( m_layers )
    m_layers->setActiveHorizon( horizon );
  return true;
}

void EvolutionPlayerPanel::setPlaying( bool playing )
{
  if ( playing )
    m_timer->start();
  else
    m_timer->stop();
  if ( m_playButton->isChecked() != playing )
    m_playButton->setChecked( playing );
  m_playButton->setText( playing ? tr( "暂停" ) : tr( "播放" ) );
}
