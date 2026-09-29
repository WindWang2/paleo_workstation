// 层：视图
#include "previewhistogramwidget.h"

#include "../paleotheme.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QSpinBox>
#include <QToolTip>
#include <QVBoxLayout>

#include <cmath>

namespace
{
  QFont mono8()
  {
    QFont f = PaleoTheme::monoFont();
    f.setPointSize( 8 );
    return f;
  }
}

PreviewHistogramWidget::PreviewHistogramWidget( bool compact, QWidget *parent )
  : QWidget( parent )
  , m_compact( compact )
{
  setMouseTracking( true );
  setMinimumSize( compact ? 200 : 320, compact ? 110 : 220 );

  auto *lay = new QVBoxLayout( this );
  lay->setContentsMargins( 0, 0, 0, 0 );
  lay->setSpacing( 4 );

  if ( !compact )
  {
    auto *bar = new QWidget( this );
    auto *barLay = new QHBoxLayout( bar );
    barLay->setContentsMargins( 0, 0, 0, 0 );
    barLay->setSpacing( 8 );

    auto *binsLbl = new QLabel( QObject::tr( "分箱" ), bar );
    binsLbl->setStyleSheet( QStringLiteral( "color: #5D6E80; font-size: 8pt;" ) );
    barLay->addWidget( binsLbl );
    m_binsSpin = new QSpinBox( bar );
    m_binsSpin->setObjectName( QStringLiteral( "histBinsSpin" ) );
    m_binsSpin->setRange( 8, 512 );
    m_binsSpin->setValue( 64 );
    m_binsSpin->setFixedWidth( 64 );
    barLay->addWidget( m_binsSpin );
    connect( m_binsSpin, &QSpinBox::valueChanged, this,
             &PreviewHistogramWidget::binsChanged );

    m_logCheck = new QCheckBox( QObject::tr( "对数纵轴" ), bar );
    m_logCheck->setObjectName( QStringLiteral( "histLogCheck" ) );
    connect( m_logCheck, &QCheckBox::toggled, this, [this]( bool on ) {
      m_log = on;
      emit logScaleChanged( on );
      update();
    } );
    barLay->addWidget( m_logCheck );
    barLay->addStretch( 1 );
    lay->addWidget( bar );
  }

  // 绘制面：本 widget 自身（工具条在上方，paintEvent 画图表区域）。
  lay->addStretch( 1 );
}

void PreviewHistogramWidget::setHistogram( const PreviewRasterAnalysis::Histogram &histogram )
{
  m_histogram = histogram;
  update();
}

void PreviewHistogramWidget::setStretchMarks( double lo, double hi )
{
  m_stretchLo = lo;
  m_stretchHi = hi;
  m_hasStretch = lo < hi;
  update();
}

void PreviewHistogramWidget::setTitle( const QString &title )
{
  m_title = title;
  update();
}

int PreviewHistogramWidget::bins() const
{
  return m_binsSpin ? m_binsSpin->value() : m_histogram.bins;
}

bool PreviewHistogramWidget::logScale() const
{
  return m_log;
}

QRect PreviewHistogramWidget::plotRect() const
{
  const int top = m_compact ? 14 : 24;
  const int left = 8;
  const int right = width() - 8;
  const int bottom = height() - 18;
  if ( right - left < 20 || bottom - top < 20 )
    return QRect();
  return QRect( left, top, right - left, bottom - top );
}

double PreviewHistogramWidget::barValue( int binIndex ) const
{
  if ( binIndex < 0 || binIndex >= m_histogram.counts.size() )
    return 0.0;
  const double c = double( m_histogram.counts.at( binIndex ) );
  return m_log && c > 0.0 ? std::log10( c ) + 1.0 : c;
}

double PreviewHistogramWidget::barMax() const
{
  double m = 0.0;
  for ( int i = 0; i < m_histogram.counts.size(); ++i )
    m = std::max( m, barValue( i ) );
  return m > 0.0 ? m : 1.0;
}

QString PreviewHistogramWidget::hoverText( int binIndex ) const
{
  if ( !m_histogram.valid || binIndex < 0 || binIndex >= m_histogram.bins )
    return QString();
  const double binWidth = ( m_histogram.hi - m_histogram.lo ) / m_histogram.bins;
  const double v0 = m_histogram.lo + binWidth * binIndex;
  const double v1 = v0 + binWidth;
  return QObject::tr( "[%1, %2) · %3" )
      .arg( QString::number( v0, 'f', 2 ), QString::number( v1, 'f', 2 ) )
      .arg( m_histogram.counts.at( binIndex ) );
}

void PreviewHistogramWidget::paintEvent( QPaintEvent * )
{
  QPainter p( this );
  p.setRenderHint( QPainter::Antialiasing, true );
  p.fillRect( rect(), Qt::white );

  const QRect pr = plotRect();
  if ( !m_title.isEmpty() )
  {
    p.setPen( QColor( QStringLiteral( "#5D6E80" ) ) );
    QFont f = font();
    f.setPointSize( 8 );
    p.setFont( f );
    p.drawText( QRect( 8, 2, width() - 16, m_compact ? 12 : 20 ), Qt::AlignLeft,
                m_title );
  }
  if ( !m_histogram.valid || pr.isNull() )
  {
    p.setPen( QColor( QStringLiteral( "#5D6E80" ) ) );
    p.drawText( rect(), Qt::AlignCenter, QObject::tr( "无直方图数据" ) );
    return;
  }

  // 轴
  p.setPen( QColor( QStringLiteral( "#DFE5EC" ) ) );
  p.drawRect( pr );

  const int bins = m_histogram.bins;
  const double barW = double( pr.width() ) / bins;
  const double maxV = barMax();
  const double binValWidth = ( m_histogram.hi - m_histogram.lo ) / bins;

  for ( int i = 0; i < bins; ++i )
  {
    const double v = barValue( i );
    if ( v <= 0.0 )
      continue;
    const double h = ( v / maxV ) * pr.height();
    const QRectF bar( pr.left() + i * barW + 0.5, pr.bottom() - h,
                      qMax( 1.0, barW - 1.0 ), h );
    // 拉伸界内 #1B73D0 着色；界外灰（截掉的尾巴一目了然）。
    const double binCenter = m_histogram.lo + binValWidth * ( i + 0.5 );
    const bool inStretch = !m_hasStretch ||
                           ( binCenter >= m_stretchLo && binCenter <= m_stretchHi );
    QColor col = inStretch ? QColor( QStringLiteral( "#1B73D0" ) )
                           : QColor( QStringLiteral( "#9AA7B4" ) );
    if ( i == m_hoverBin )
      col = col.darker( 130 );
    p.fillRect( bar, col );
  }

  // 拉伸界竖线（红虚线）
  if ( m_hasStretch && m_histogram.hi > m_histogram.lo )
  {
    p.setPen( QPen( QColor( QStringLiteral( "#E53935" ) ), 1, Qt::DashLine ) );
    const auto xOf = [&]( double value ) {
      return pr.left() + ( ( value - m_histogram.lo ) / ( m_histogram.hi - m_histogram.lo ) ) * pr.width();
    };
    const double xLo = xOf( m_stretchLo );
    const double xHi = xOf( m_stretchHi );
    if ( xLo >= pr.left() && xLo <= pr.right() )
      p.drawLine( QPointF( xLo, pr.top() ), QPointF( xLo, pr.bottom() ) );
    if ( xHi >= pr.left() && xHi <= pr.right() )
      p.drawLine( QPointF( xHi, pr.top() ), QPointF( xHi, pr.bottom() ) );
  }

  // 轴标注（mono，DESIGN.md tnum）
  p.setFont( mono8() );
  p.setPen( QColor( QStringLiteral( "#5D6E80" ) ) );
  p.drawText( QRect( pr.left(), pr.bottom() + 2, pr.width(), 14 ), Qt::AlignLeft,
              QString::number( m_histogram.lo, 'f', 1 ) );
  p.drawText( QRect( pr.left(), pr.bottom() + 2, pr.width(), 14 ), Qt::AlignRight,
              QString::number( m_histogram.hi, 'f', 1 ) );
  // 纵轴峰值计数
  p.drawText( QRect( pr.left(), pr.top() - 14, pr.width(), 12 ), Qt::AlignLeft,
              QString::number( qint64( maxV ), 'f', m_log ? 1 : 0 ) );
}

void PreviewHistogramWidget::mouseMoveEvent( QMouseEvent *event )
{
  const QRect pr = plotRect();
  if ( pr.contains( event->pos() ) && m_histogram.valid && m_histogram.bins > 0 )
  {
    const double frac = double( event->pos().x() - pr.left() ) / pr.width();
    m_hoverBin = qBound( 0, int( frac * m_histogram.bins ), m_histogram.bins - 1 );
    QToolTip::showText( event->globalPosition().toPoint(), hoverText( m_hoverBin ), this );
  }
  else
  {
    m_hoverBin = -1;
  }
  update();
}

void PreviewHistogramWidget::leaveEvent( QEvent * )
{
  m_hoverBin = -1;
  update();
}
