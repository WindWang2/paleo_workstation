// 层：视图
#include "subintervalselectorbar.h"

#include "../linkage/selectioncontext.h"
#include "../services/stratigraphicsubintervals.h"
#include "paleoicons.h"
#include "paleotheme.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>

using paleo::strat::UnitSelection;
using paleo::strat::kSubIntervalCount;

namespace {

void styleSubButton( QToolButton *btn )
{
  PaleoTheme::applyThemedStyleSheet( btn, [btn] {
    const PaleoTheme::ThemeTokens &t = PaleoTheme::tokens();
    return PaleoTheme::metricStyleSheet( QStringLiteral(
               "QToolButton { border-radius: %6px; padding: {spacing.xs}px {spacing.sm}px;"
               " background: %1; border: 1px solid %2; color: %3; }"
               "QToolButton:checked { background: %4; border-color: %4; color: %5; }"
               "QToolButton:hover:!checked { border-color: %4; }"
               "QToolButton:disabled { color: %7; }" ) )
        .arg( t.surface.name(), t.border.name(), t.text.name(), t.primary.name(),
              t.onPrimary.name() )
        .arg( PaleoTheme::chipRadius( btn->font() ) )
        .arg( t.textDisabled.name() );
  } );
}

} // namespace

SubIntervalSelectorBar::SubIntervalSelectorBar( SelectionContext *selection, QWidget *parent )
  : QWidget( parent )
  , m_selection( selection )
{
  setObjectName( QStringLiteral( "subIntervalSelectorBar" ) );
  setAccessibleName( tr( "层段分析选择" ) );

  auto *root = new QVBoxLayout( this );
  root->setContentsMargins( 0, 0, 0, 0 );
  root->setSpacing( PaleoTheme::tokens().spacingXs );

  auto *row = new QWidget( this );
  row->setObjectName( QStringLiteral( "subIntervalSelectorRow" ) );
  auto *lay = new QHBoxLayout( row );
  lay->setContentsMargins( 0, 0, 0, 0 );
  lay->setSpacing( PaleoTheme::tokens().spacingSm );

  m_modeLabel = new QLabel( tr( "层段分析" ), row );
  m_modeLabel->setObjectName( QStringLiteral( "subIntervalModeLabel" ) );
  m_modeLabel->setToolTip( tr( "层段分析模式：选择父单元与 S1–S4 子层段；"
                               "与上方「界面编辑」层位条独立，不切换地图图层或中断编辑。" ) );
  if ( const QIcon ic = PaleoIcons::qgisTheme( QStringLiteral( "mIconPolygonLayer.svg" ) ); !ic.isNull() )
  {
    m_modeLabel->setPixmap( ic.pixmap( PaleoIcons::toolbarSize() ) );
    m_modeLabel->setText( tr( " 层段分析" ) );
  }
  lay->addWidget( m_modeLabel );

  m_parent = new QComboBox( row );
  m_parent->setObjectName( QStringLiteral( "subIntervalParentCombo" ) );
  m_parent->setAccessibleName( tr( "父层间单元" ) );
  m_parent->setToolTip( tr( "父层间单元（相邻两条界面，如 T70–T71）。"
                            "展开后可查看四个子层段的就绪状态。" ) );
  m_parent->setMinimumContentsLength( 10 );
  m_parent->setSizeAdjustPolicy( QComboBox::AdjustToContents );
  connect( m_parent, QOverload<int>::of( &QComboBox::currentIndexChanged ), this,
           &SubIntervalSelectorBar::onParentChanged );
  lay->addWidget( m_parent );

  for ( int i = 1; i <= kSubIntervalCount; ++i )
  {
    auto *btn = new QToolButton( row );
    btn->setObjectName( QStringLiteral( "subIntervalS%1" ).arg( i ) );
    btn->setText( QStringLiteral( "S%1" ).arg( i ) );
    btn->setCheckable( true );
    btn->setToolButtonStyle( Qt::ToolButtonTextBesideIcon );
    btn->setIcon( PaleoIcons::qgisTheme( QStringLiteral( "mIconRasterLayer.svg" ) ) );
    btn->setIconSize( PaleoIcons::toolbarSize() );
    btn->setToolTip( tr( "子层段 S%1（自上而下第 %1 段）。选中后单因素/编图/相分析"
                         "按稳定子层段 ID 归属，不拆分界面名。" )
                         .arg( i ) );
    btn->setStatusTip( tr( "分析层段：S%1" ).arg( i ) );
    styleSubButton( btn );
    connect( btn, &QToolButton::clicked, this, [this, i] { onSubClicked( i ); } );
    m_subs.push_back( btn );
    lay->addWidget( btn );
  }

  m_agg = new QToolButton( row );
  m_agg->setObjectName( QStringLiteral( "subIntervalAggregate" ) );
  m_agg->setText( tr( "综合" ) );
  m_agg->setCheckable( true );
  m_agg->setToolButtonStyle( Qt::ToolButtonTextBesideIcon );
  m_agg->setIcon( PaleoIcons::qgisTheme( QStringLiteral( "processingAlgorithm.svg" ) ) );
  if ( m_agg->icon().isNull() )
    m_agg->setIcon( PaleoIcons::qgisTheme( QStringLiteral( "processingAlgorithm.svg" ) ) );
  m_agg->setIconSize( PaleoIcons::toolbarSize() );
  m_agg->setToolTip( tr( "父单元显式综合（厚度加权）。不是任一子层段；"
                         "缺任一子层段成果时综合结果不可用。" ) );
  m_agg->setStatusTip( tr( "分析层段：父单元综合" ) );
  styleSubButton( m_agg );
  connect( m_agg, &QToolButton::clicked, this, [this] { onSubClicked( 0 ); } );
  lay->addWidget( m_agg );

  m_expand = new QToolButton( row );
  m_expand->setObjectName( QStringLiteral( "subIntervalExpand" ) );
  m_expand->setCheckable( true );
  m_expand->setToolButtonStyle( Qt::ToolButtonTextBesideIcon );
  m_expand->setIcon( PaleoIcons::qgisTheme( QStringLiteral( "mActionIdentify.svg" ) ) );
  if ( m_expand->icon().isNull() )
    m_expand->setIcon( PaleoIcons::qgisTheme( QStringLiteral( "mActionIdentify.svg" ) ) );
  m_expand->setIconSize( PaleoIcons::toolbarSize() );
  m_expand->setText( tr( "状态" ) );
  m_expand->setToolTip( tr( "展开查看当前父单元四个子层段的就绪与成果提示。" ) );
  m_expand->setStatusTip( tr( "展开子层段状态" ) );
  connect( m_expand, &QToolButton::toggled, this, [this]( bool on ) {
    if ( m_expandStatus )
      m_expandStatus->setVisible( on );
    rebuildStatusPanel();
  } );
  lay->addWidget( m_expand );

  m_status = new QLabel( row );
  m_status->setObjectName( QStringLiteral( "subIntervalStatus" ) );
  m_status->setTextInteractionFlags( Qt::TextSelectableByMouse );
  lay->addWidget( m_status, 1 );

  root->addWidget( row );

  m_expandStatus = new QLabel( this );
  m_expandStatus->setObjectName( QStringLiteral( "subIntervalExpandStatus" ) );
  m_expandStatus->setWordWrap( true );
  m_expandStatus->setVisible( false );
  m_expandStatus->setTextInteractionFlags( Qt::TextSelectableByMouse );
  root->addWidget( m_expandStatus );

  if ( m_selection )
  {
    connect( m_selection, &SelectionContext::activeUnitChanged, this,
             &SubIntervalSelectorBar::syncFromContext );
    syncFromContext( m_selection->activeUnit() );
  }
  m_status->setText( readinessText() );
}

QString SubIntervalSelectorBar::currentDisplayName() const
{
  return m_current.displayName;
}

QStringList SubIntervalSelectorBar::subIntervalStatusLines() const
{
  QStringList lines;
  if ( m_scheme.intervals.empty() )
  {
    lines << tr( "尚未加载界面名单——请先打开工程并确认层序界面。" );
    return lines;
  }
  const QString iid = m_parent && m_parent->currentIndex() >= 0
                          ? m_parent->currentData().toString()
                          : QString();
  if ( iid.isEmpty() )
  {
    lines << tr( "请选择父层间单元。" );
    return lines;
  }
  const auto opts = paleo::strat::selectorOptions( m_scheme );
  for ( const auto &p : opts )
  {
    if ( p.intervalId != iid )
      continue;
    for ( int i = 0; i < p.subLabels.size(); ++i )
    {
      const QString sid = i < p.subIntervalIds.size() ? p.subIntervalIds.at( i ) : QString();
      const bool selected = m_current.isValid() && !m_current.aggregate && m_current.subIntervalId == sid;
      lines << tr( "%1：稳定 ID %2%3" )
                   .arg( p.subLabels.at( i ), sid.isEmpty() ? tr( "（未派生）" ) : sid,
                         selected ? tr( " · 当前选中" ) : QString() );
    }
    if ( m_current.aggregate && m_current.intervalId == iid )
      lines << tr( "综合：当前选中（需 S1–S4 齐全才可出综合成果）" );
    else
      lines << tr( "综合：未选中" );
    return lines;
  }
  lines << tr( "当前父单元在方案中不存在。" );
  return lines;
}

void SubIntervalSelectorBar::reloadFromBoundaries( const QStringList &orderedBoundaries,
                                                   const QString &schemeId, const QString &schemeName )
{
  m_scheme = paleo::strat::draftScheme( schemeId, schemeName, orderedBoundaries, {} );
  rebuildParentCombo();
  rebuildSubButtons();
  rebuildStatusPanel();
  m_status->setText( readinessText() );
  if ( orderedBoundaries.size() < 2 )
    emit readinessMessage( tr( "界面不足两条，无法构成父层间单元。请在工程参数中配置层序界面。" ) );
}

void SubIntervalSelectorBar::rebuildParentCombo()
{
  m_block = true;
  const QString keep = m_parent->currentData().toString();
  m_parent->clear();
  const auto opts = paleo::strat::selectorOptions( m_scheme );
  for ( const auto &p : opts )
    m_parent->addItem( p.label, p.intervalId );
  int idx = m_parent->findData( keep );
  if ( idx < 0 && m_current.isValid() )
    idx = m_parent->findData( m_current.intervalId );
  if ( idx < 0 && m_parent->count() > 0 )
    idx = 0;
  if ( idx >= 0 )
    m_parent->setCurrentIndex( idx );
  m_block = false;
  const bool ok = m_parent->count() > 0;
  for ( auto *b : m_subs )
    b->setEnabled( ok );
  if ( m_agg )
    m_agg->setEnabled( ok );
}

void SubIntervalSelectorBar::rebuildSubButtons()
{
  const bool ok = m_parent && m_parent->count() > 0;
  for ( auto *b : m_subs )
  {
    b->setEnabled( ok );
    b->setChecked( false );
  }
  if ( m_agg )
  {
    m_agg->setEnabled( ok );
    m_agg->setChecked( false );
  }
  if ( !m_current.isValid() )
    return;
  if ( m_current.aggregate )
  {
    if ( m_agg )
      m_agg->setChecked( true );
    return;
  }
  const QString iid = m_current.intervalId;
  for ( int i = 1; i <= kSubIntervalCount; ++i )
  {
    const UnitSelection probe = paleo::strat::makeSelection( m_scheme, iid, i );
    if ( probe.isValid() && probe.subIntervalId == m_current.subIntervalId && i - 1 < m_subs.size() )
      m_subs.at( i - 1 )->setChecked( true );
  }
}

void SubIntervalSelectorBar::rebuildStatusPanel()
{
  if ( !m_expandStatus )
    return;
  if ( !m_expand || !m_expand->isChecked() )
  {
    m_expandStatus->clear();
    return;
  }
  m_expandStatus->setText( subIntervalStatusLines().join( QChar( '\n' ) ) );
}

QString SubIntervalSelectorBar::readinessText() const
{
  if ( m_scheme.boundaries.empty() )
    return tr( "未加载界面——打开工程后自动按层序界面生成父单元。" );
  if ( m_scheme.intervals.empty() )
    return tr( "界面不足——至少需要两条相邻界面才能分析层段。" );
  if ( !m_current.isValid() )
    return tr( "请选择父单元与子层段（S1–S4）；界面编辑请用上方层位条。" );
  if ( m_current.aggregate )
    return tr( "当前：%1（综合需各子层段成果齐全）" ).arg( m_current.displayName );
  return tr( "当前：%1" ).arg( m_current.displayName );
}

void SubIntervalSelectorBar::onParentChanged( int )
{
  if ( m_block )
    return;
  // 换父单元时保留子序号偏好：若已有 S_n 选中则尝试同序号，否则默认 S1。
  int prefer = 1;
  if ( m_current.isValid() && !m_current.aggregate )
  {
    for ( int i = 1; i <= kSubIntervalCount; ++i )
    {
      if ( i - 1 < m_subs.size() && m_subs.at( i - 1 )->isChecked() )
        prefer = i;
    }
  }
  onSubClicked( prefer );
  rebuildStatusPanel();
}

void SubIntervalSelectorBar::onSubClicked( int subIndex )
{
  if ( !m_parent || m_parent->currentIndex() < 0 )
  {
    emit readinessMessage( tr( "请先选择父层间单元。" ) );
    return;
  }
  const QString iid = m_parent->currentData().toString();
  const UnitSelection sel = paleo::strat::makeSelection( m_scheme, iid, subIndex );
  if ( !sel.isValid() )
  {
    emit readinessMessage( tr( "该父单元尚未派生完整四子层段，请检查解释方案。" ) );
    rebuildSubButtons();
    return;
  }
  applySelection( sel, true );
}

void SubIntervalSelectorBar::applySelection( const UnitSelection &sel, bool broadcast )
{
  m_current = sel;
  rebuildSubButtons();
  m_status->setText( readinessText() );
  rebuildStatusPanel();
  if ( broadcast && m_selection )
    m_selection->setActiveUnit( sel );
  emit unitSelectionCommitted( sel );
}

void SubIntervalSelectorBar::syncFromContext( const UnitSelection &unit )
{
  if ( unit == m_current )
    return;
  m_current = unit;
  if ( unit.isValid() && m_parent )
  {
    m_block = true;
    const int idx = m_parent->findData( unit.intervalId );
    if ( idx >= 0 )
      m_parent->setCurrentIndex( idx );
    m_block = false;
  }
  rebuildSubButtons();
  m_status->setText( readinessText() );
  rebuildStatusPanel();
}
