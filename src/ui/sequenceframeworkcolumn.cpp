// 层：视图
#include "sequenceframeworkcolumn.h"

#include <QHash>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QFontMetrics>

#include <cmath>

#include "paleotheme.h"

namespace
{

  // 数据符号色（DESIGN.md：地图域配色不属 UI token）——按层序序号转色相，
  // 体系域在所属层序色相上提亮。浅/暗两主题同值，保证柱状图是「地质文档
  // 隐喻」的纸面一致性（DESIGN.md 2026-09-29 决策）。
  QColor unitFillColor( int sequenceOrdinal, int depth )
  {
    const double hue = std::fmod( 0.58 + 0.11 * double( sequenceOrdinal ), 1.0 );
    const double sat = depth >= 1 ? 0.42 : 0.30;
    const double lig = depth >= 1 ? 0.72 : 0.55;
    return QColor::fromHslF( hue, sat, lig );
  }
} // namespace

SequenceFrameworkColumnView::SequenceFrameworkColumnView( QWidget *parent )
  : QWidget( parent )
{
  setFocusPolicy( Qt::StrongFocus );
  setMouseTracking( true );
}

void SequenceFrameworkColumnView::setFramework( const SequenceFramework::Framework &fw,
                                                const QStringList &horizons )
{
  m_fw = fw;
  m_horizons = horizons;
  if ( !m_current.isEmpty() &&
       SequenceFramework::unitById( m_fw, m_current ) == nullptr )
    m_current.clear();
  relayout();
  update();
}

void SequenceFrameworkColumnView::setCurrentUnit( const QString &unitId )
{
  if ( m_current == unitId )
    return;
  m_current = unitId;
  update();
  emit currentUnitChanged( m_current );
}

QSize SequenceFrameworkColumnView::minimumSizeHint() const
{
  return QSize( 160, 120 );
}

QSize SequenceFrameworkColumnView::sizeHint() const
{
  return QSize( 220, 320 );
}

QString SequenceFrameworkColumnView::unitAt( const QPoint &pos ) const
{
  for ( const Band &b : m_bands )
    if ( b.rect.contains( pos ) )
      return b.unitId;
  return QString();
}

void SequenceFrameworkColumnView::relayout()
{
  m_bands.clear();
  const int margin = 8;
  const QRect area = rect().adjusted( margin, margin, -margin, -margin );
  if ( area.height() <= 0 || area.width() <= 0 )
    return;

  // 可视序：层序 → 其体系域（domain::childrenOf 已按 ordinal/name 稳定排）。
  QVector<SequenceFramework::FrameworkUnit> ordered;
  for ( const SequenceFramework::FrameworkUnit &seq : SequenceFramework::rootsOf( m_fw ) )
  {
    ordered.append( seq );
    for ( const SequenceFramework::FrameworkUnit &tract : SequenceFramework::childrenOf( m_fw, seq.id ) )
      ordered.append( tract );
  }
  if ( ordered.isEmpty() )
    return;

  // 带高：填了 thickness 的按厚度比例，没填的等分剩余（厚度是可选字段，
  // 缺了不能把带压成 0——保底 12px）。
  double total = 0.0;
  int missing = 0;
  for ( const SequenceFramework::FrameworkUnit &u : ordered )
  {
    if ( u.thickness > 0.0 )
      total += u.thickness;
    else
      ++missing;
  }

  const int gap = 2;
  const int usable = area.height() - gap * ( ordered.size() - 1 );
  int allocated = 0;
  QVector<int> heights( ordered.size(), 0 );
  for ( int i = 0; i < ordered.size(); ++i )
  {
    const double t = ordered.at( i ).thickness;
    if ( t > 0.0 && total > 0.0 )
    {
      heights[ i ] = int( double( usable ) * t / total );
      allocated += heights[ i ];
    }
  }
  const int rest = missing > 0 ? ( usable - allocated ) / missing : 0;

  int y = area.top();
  for ( int i = 0; i < ordered.size(); ++i )
  {
    const SequenceFramework::FrameworkUnit &u = ordered.at( i );
    int h = heights.at( i );
    if ( h <= 0 )
      h = rest;
    if ( h < 12 )
      h = 12;
    const int indent = u.parentId.isEmpty() ? 0 : 14;
    Band b;
    b.unitId = u.id;
    b.rect = QRect( area.left() + indent, y, area.width() - indent, h );
    m_bands.append( b );
    y += h + gap;
  }
}

QColor SequenceFrameworkColumnView::unitColor( const QString &unitId ) const
{
  const SequenceFramework::FrameworkUnit *u = SequenceFramework::unitById( m_fw, unitId );
  if ( u == nullptr )
    return PaleoTheme::tokens().surfaceAlt;
  int ordinal = 0;
  if ( !u->parentId.isEmpty() )
  {
    const SequenceFramework::FrameworkUnit *parent =
        SequenceFramework::unitById( m_fw, u->parentId );
    ordinal = parent != nullptr ? parent->ordinal : u->ordinal;
  }
  else
  {
    ordinal = u->ordinal;
  }
  const int depth = SequenceFramework::depthOf( m_fw, unitId );
  return unitFillColor( ordinal, depth < 0 ? 0 : depth );
}

void SequenceFrameworkColumnView::paintEvent( QPaintEvent * )
{
  const PaleoTheme::ThemeTokens &tk = PaleoTheme::tokens();
  QPainter p( this );
  p.setRenderHint( QPainter::Antialiasing, true );
  p.fillRect( rect(), tk.surface );

  if ( m_bands.isEmpty() )
  {
    p.setPen( tk.textDisabled );
    p.setFont( PaleoTheme::bodyFont() );
    p.drawText( rect(), Qt::AlignCenter, tr( "暂无格架单元" ) );
    return;
  }

  for ( const Band &b : m_bands )
  {
    const SequenceFramework::FrameworkUnit *u = SequenceFramework::unitById( m_fw, b.unitId );
    if ( u == nullptr )
      continue;
    const bool current = ( b.unitId == m_current );
    p.fillRect( b.rect, unitColor( b.unitId ) );
    p.setPen( QPen( current ? tk.primary : tk.border, current ? 2 : 1 ) );
    p.drawRect( b.rect );

    // 标签：单元路径 + 层位区间（有覆盖区间才显示区间，避免误导）。
    const QString path = SequenceFramework::unitPath( m_fw, b.unitId );
    const QStringList covered =
        SequenceFramework::horizonsCoveredBy( m_fw, b.unitId, m_horizons );
    const QString label = covered.isEmpty()
                              ? path
                              : QStringLiteral( "%1  %2–%3" ).arg( path, covered.first(),
                                                                   covered.last() );
    QFont f = PaleoTheme::bodyFont();
    f.setPointSize( PaleoTheme::kLabelPt );
    p.setFont( f );
    p.setPen( current ? tk.primaryText : tk.text );
    const QRect textRect = b.rect.adjusted( 6, 2, -6, -2 );
    p.drawText( textRect, Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics( f ).elidedText( label, Qt::ElideRight, textRect.width() ) );
  }
}

void SequenceFrameworkColumnView::resizeEvent( QResizeEvent *event )
{
  QWidget::resizeEvent( event );
  relayout();
}

void SequenceFrameworkColumnView::mousePressEvent( QMouseEvent *event )
{
  const QString hit = unitAt( event->pos() );
  if ( hit.isEmpty() )
    return;
  setCurrentUnit( hit );
  QWidget::mousePressEvent( event );
}
