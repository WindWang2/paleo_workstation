// 层：视图
// token 例外：DESIGN 数据符号例外：多剖面系列色保持固定，低对比度仅添加中性轮廓。（tools/ui-token-exceptions.json 精确计数）。
#include "previewprofilepanel.h"

#include "../paleotheme.h"

#include <QColor>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include "ui/notifications/notificationmanager.h"
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QTextStream>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>
#include <limits>

namespace
{
  const QVector<QColor> &seriesPalette()
  {
    static const QVector<QColor> palette = {
        QColor( QStringLiteral( "#1B73D0" ) ), QColor( QStringLiteral( "#E65100" ) ),
        QColor( QStringLiteral( "#7B1FA2" ) ), QColor( QStringLiteral( "#00838F" ) ),
        QColor( QStringLiteral( "#C2185B" ) ), QColor( QStringLiteral( "#388E3C" ) ),
    };
    return palette;
  }

  QFont mono8()
  {
    QFont f = PaleoTheme::monoFont();
    f.setPointSize(PaleoTheme::tokens().labelPt);
    return f;
  }

  double niceStep( double raw )
  {
    if ( !( raw > 0.0 ) )
      return 1.0;
    const double mag = std::pow( 10.0, std::floor( std::log10( raw ) ) );
    const double n = raw / mag;
    double nice = 10.0;
    if ( n <= 1.0 )
      nice = 1.0;
    else if ( n <= 2.0 )
      nice = 2.0;
    else if ( n <= 5.0 )
      nice = 5.0;
    return nice * mag;
  }
} // namespace

// 剖面图本体：距离 X / 值 Y，多序列叠绘，悬停十字读数。
class PreviewProfilePanel::ProfileChart : public QWidget
{
  public:
    explicit ProfileChart( PreviewProfilePanel *host )
      : QWidget( host )
      , m_host( host )
    {
      setMouseTracking( true );
      setMinimumSize( 280, 160 );
    }

    void refresh()
    {
      computeBounds();
      update();
    }

  protected:
    void paintEvent( QPaintEvent * ) override
    {
      QPainter p( this );
      p.setRenderHint( QPainter::Antialiasing, true );
      p.fillRect( rect(), PaleoTheme::tokens().surface );

      const QRect pr = plotRect();
      if ( pr.isNull() )
        return;
      p.setPen( PaleoTheme::tokens().border );
      p.drawRect( pr );
      // goal/ui-experience-polish：空序列不静默——画面心 muted 指引
      //（有序列时不画）。#5D6E80 = DESIGN.md text-muted（chart 笔色出口）。
      if ( m_host->m_series.isEmpty() )
      {
        p.setPen( PaleoTheme::tokens().textMuted );
        p.drawText( pr, Qt::AlignCenter,
                    QObject::tr( "还没有剖面——在预览地图上用「剖面」工具画一条线" ) );
        return;
      }

      // 轴刻度（nice steps，mono 标注）
      p.setFont( mono8() );
      const double xStep = niceStep( m_xSpan / 5.0 );
      p.setPen( PaleoTheme::tokens().textMuted );
      for ( double x = 0.0; x <= m_xSpan + 1e-9; x += xStep )
      {
        const int px = pr.left() + int( ( x / m_xSpan ) * pr.width() );
        p.drawLine( px, pr.bottom(), px, pr.bottom() + 3 );
        p.setPen( PaleoTheme::tokens().surfaceAltRaised );
        p.drawLine( px, pr.top(), px, pr.bottom() );
        p.setPen( PaleoTheme::tokens().textMuted );
        p.drawText( QRect( px - 30, pr.bottom() + 4, 60, 12 ), Qt::AlignCenter,
                    QString::number( x, 'f', xStep < 1.0 ? 1 : 0 ) );
      }
      const double yStep = niceStep( m_ySpan / 4.0 );
      for ( double y = m_yMin; y <= m_yMax + 1e-9; y += yStep )
      {
        const int py = pr.bottom() - int( ( ( y - m_yMin ) / m_ySpan ) * pr.height() );
        p.drawLine( pr.left() - 3, py, pr.left(), py );
        p.drawText( QRect( 0, py - 6, pr.left() - 6, 12 ), Qt::AlignRight,
                    QString::number( y, 'f', yStep < 1.0 ? 1 : 0 ) );
      }

      // 序列折线（无效点断线）
      for ( const Series &s : m_host->m_series )
      {
        const QPen dataPen(s.color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        const QColor halo = PaleoTheme::dataHaloColor(s.color);
        bool pen = false;
        QPointF prev;
        for ( const Sample &sm : s.samples )
        {
          if ( !sm.valid )
          {
            pen = false;
            continue;
          }
          const QPointF mapped( pr.left() + ( sm.distance / m_xSpan ) * pr.width(),
                                pr.bottom() - ( ( sm.value - m_yMin ) / m_ySpan ) * pr.height() );
          if ( pen )
          {
            if (halo.isValid())
            {
              p.setPen(QPen(halo, dataPen.widthF() + 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
              p.drawLine(prev, mapped);
            }
            p.setPen(dataPen);
            p.drawLine( prev, mapped );
          }
          prev = mapped;
          pen = true;
        }
      }

      // 悬停十字 + 读数
      if ( m_hoverX >= 0.0 && m_hoverX <= m_xSpan )
      {
        const int px = pr.left() + int( ( m_hoverX / m_xSpan ) * pr.width() );
        p.setPen( QPen( PaleoTheme::tokens().primaryText, 1, Qt::DashLine ) );
        p.drawLine( px, pr.top(), px, pr.bottom() );

        // 各序列在悬停距离处的值点
        for ( const Series &s : m_host->m_series )
        {
          const double v = valueAt( s, m_hoverX, nullptr );
          if ( std::isnan( v ) )
            continue;
          const double py = pr.bottom() - ( ( v - m_yMin ) / m_ySpan ) * pr.height();
          p.setBrush( s.color );
          p.setPen( Qt::NoPen );
          p.drawEllipse( QPointF( px, py ), 3.0, 3.0 );
        }
      }

      // 轴标题
      p.setPen( PaleoTheme::tokens().textMuted );
      QFont f = font();
      f.setPointSize(PaleoTheme::tokens().labelPt);
      p.setFont( f );
      p.drawText( QRect( pr.left(), height() - 14, pr.width(), 12 ), Qt::AlignCenter,
                  QObject::tr( "距离 (m)" ) );
      p.save();
      p.translate( 10, pr.top() + pr.height() / 2.0 );
      p.rotate( -90 );
      p.drawText( QRect( -60, -6, 120, 12 ), Qt::AlignCenter, QObject::tr( "值" ) );
      p.restore();
    }

    void mouseMoveEvent( QMouseEvent *event ) override
    {
      const QRect pr = plotRect();
      if ( pr.contains( event->pos() ) && m_xSpan > 0 )
      {
        m_hoverX = ( double( event->pos().x() - pr.left() ) / pr.width() ) * m_xSpan;
        emitReadout();
      }
      else
      {
        m_hoverX = -1.0;
        emit m_host->hoverReadoutChanged( QString() );
      }
      update();
    }

    void leaveEvent( QEvent * ) override
    {
      m_hoverX = -1.0;
      emit m_host->hoverReadoutChanged( QString() );
      update();
    }

  private:
    QRect plotRect() const
    {
      const int left = 48;
      const int top = 12;
      const int right = width() - 12;
      const int bottom = height() - 28;
      if ( right - left < 40 || bottom - top < 40 )
        return QRect();
      return QRect( left, top, right - left, bottom - top );
    }

    void computeBounds()
    {
      m_xSpan = 1.0;
      m_yMin = 0.0;
      m_yMax = 1.0;
      double xMax = 0.0;
      double yMin = std::numeric_limits<double>::max();
      double yMax = std::numeric_limits<double>::lowest();
      bool any = false;
      for ( const Series &s : m_host->m_series )
        for ( const Sample &sm : s.samples )
          if ( sm.valid )
          {
            any = true;
            xMax = std::max( xMax, sm.distance );
            yMin = std::min( yMin, sm.value );
            yMax = std::max( yMax, sm.value );
          }
      if ( !any )
        return;
      m_xSpan = xMax > 0 ? xMax : 1.0;
      if ( !( yMin < yMax ) )
      {
        yMin -= 0.5;
        yMax += 0.5;
      }
      const double pad = ( yMax - yMin ) * 0.08;
      m_yMin = yMin - pad;
      m_yMax = yMax + pad;
      m_ySpan = m_yMax - m_yMin;
    }

    // 距离处插值取值；slope 出参 = 相邻样本坡度 dZ/dX（D5.2）。
    double valueAt( const Series &s, double distance, double *slope ) const
    {
      const QVector<Sample> &sm = s.samples;
      if ( sm.isEmpty() )
        return std::numeric_limits<double>::quiet_NaN();
      if ( distance <= sm.first().distance )
        return sm.first().valid ? sm.first().value : std::numeric_limits<double>::quiet_NaN();
      for ( int i = 1; i < sm.size(); ++i )
      {
        const Sample &a = sm.at( i - 1 );
        const Sample &b = sm.at( i );
        if ( a.valid && b.valid && distance <= b.distance && distance >= a.distance )
        {
          const double t = ( b.distance - a.distance ) > 1e-12
                               ? ( distance - a.distance ) / ( b.distance - a.distance )
                               : 0.0;
          if ( slope )
            *slope = ( b.distance - a.distance ) > 1e-12
                         ? ( b.value - a.value ) / ( b.distance - a.distance )
                         : 0.0;
          return a.value + t * ( b.value - a.value );
        }
      }
      const Sample &last = sm.last();
      return last.valid ? last.value : std::numeric_limits<double>::quiet_NaN();
    }

    void emitReadout()
    {
      if ( m_hoverX < 0.0 )
      {
        emit m_host->hoverReadoutChanged( QString() );
        return;
      }
      QString text = QObject::tr( "距离 %1 m" ).arg( QString::number( m_hoverX, 'f', 1 ) );
      for ( const Series &s : m_host->m_series )
      {
        double slope = 0.0;
        const double v = valueAt( s, m_hoverX, &slope );
        if ( std::isnan( v ) )
          continue;
        text += QStringLiteral( " | %1: %2" )
                    .arg( s.name, QString::number( v, 'f', 2 ) );
        text += QObject::tr( " 坡度 %1" ).arg( QString::number( slope, 'f', 4 ) );
      }
      emit m_host->hoverReadoutChanged( text );
    }

    PreviewProfilePanel *m_host = nullptr;
    double m_xSpan = 1.0;
    double m_yMin = 0.0;
    double m_yMax = 1.0;
    double m_ySpan = 1.0;
    double m_hoverX = -1.0;
};

// ------------------------------------------------------------------ panel --

PreviewProfilePanel::PreviewProfilePanel( QWidget *parent )
  : QWidget( parent )
{
  auto *lay = new QVBoxLayout( this );
  lay->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm);
  lay->setSpacing(PaleoTheme::tokens().spacingXs);

  auto *bar = new QWidget( this );
  auto *barLay = new QHBoxLayout( bar );
  barLay->setContentsMargins( 0, 0, 0, 0 );
  barLay->setSpacing(PaleoTheme::tokens().spacingSm);

  auto *title = new QLabel( QObject::tr( "层位剖面" ), bar );
  PaleoTheme::applyThemedStyleSheet(
      title, [] { return PaleoTheme::sectionTitleStyleSheet(); } );
  barLay->addWidget( title );

  auto *clearBtn = new QToolButton( bar );
  clearBtn->setText( QObject::tr( "清空" ) );
  clearBtn->setObjectName( QStringLiteral( "profileClearBtn" ) );
  connect( clearBtn, &QToolButton::clicked, this, &PreviewProfilePanel::clearProfiles );
  barLay->addWidget( clearBtn );

  auto *csvBtn = new QToolButton( bar );
  csvBtn->setText( QObject::tr( "导出 CSV" ) );
  csvBtn->setObjectName( QStringLiteral( "profileCsvBtn" ) );
  connect( csvBtn, &QToolButton::clicked, this, &PreviewProfilePanel::exportCsv );
  barLay->addWidget( csvBtn );

  auto *pngBtn = new QToolButton( bar );
  pngBtn->setText( QObject::tr( "导出 PNG" ) );
  pngBtn->setObjectName( QStringLiteral( "profilePngBtn" ) );
  connect( pngBtn, &QToolButton::clicked, this, &PreviewProfilePanel::exportPng );
  barLay->addWidget( pngBtn );

  m_legend = new QLabel( bar );
  PaleoTheme::applyThemedStyleSheet( m_legend, [] {
    return PaleoTheme::mutedCaptionStyleSheet();
  } );
  barLay->addWidget( m_legend, 1 );
  lay->addWidget( bar );

  m_chart = new ProfileChart( this );
  m_chart->setObjectName( QStringLiteral( "profileChart" ) );
  lay->addWidget( m_chart, 1 );
}

void PreviewProfilePanel::addProfile( const QString &name, const QVector<Sample> &samples,
                                      const QgsPointXY &p1, const QgsPointXY &p2 )
{
  Series s;
  s.name = name.isEmpty() ? QObject::tr( "剖面 %1" ).arg( m_series.size() + 1 ) : name;
  s.color = seriesPalette().at( m_series.size() % seriesPalette().size() );
  s.samples = samples;
  s.p1 = p1;
  s.p2 = p2;
  m_series.append( s );
  rebuildLegend();
  m_chart->refresh();
}

void PreviewProfilePanel::setProfiles( const QVector<Series> &series )
{
  m_series = series;
  rebuildLegend();
  m_chart->refresh();
}

void PreviewProfilePanel::clearProfiles()
{
  m_series.clear();
  rebuildLegend();
  m_chart->refresh();
}

void PreviewProfilePanel::rebuildLegend()
{
  QStringList parts;
  for ( const Series &s : m_series )
    parts.append( QStringLiteral( "%1(%2,%3)→(%4,%5)" )
                      .arg( s.name )
                      .arg( QString::number( s.p1.x(), 'f', 0 ), QString::number( s.p1.y(), 'f', 0 ),
                            QString::number( s.p2.x(), 'f', 0 ), QString::number( s.p2.y(), 'f', 0 ) ) );
  m_legend->setText( parts.join( QStringLiteral( "  " ) ) );
}

void PreviewProfilePanel::setExportTargetForTesting( const QString &csvPath, const QString &pngPath )
{
  m_csvTarget = csvPath;
  m_pngTarget = pngPath;
}

void PreviewProfilePanel::exportCsv()
{
  if ( m_series.isEmpty() )
    return;
  const QString path = m_csvTarget.isEmpty()
      ? QFileDialog::getSaveFileName( this, QObject::tr( "导出剖面 CSV" ), QString(),
                                      QStringLiteral( "CSV (*.csv)" ) )
      : m_csvTarget;
  if ( path.isEmpty() )
    return;
  if ( !writeCsv( path ) )
    paleo::ui::NotificationManager::showWarning( this, QObject::tr( "导出失败" ), QObject::tr( "无法写入 %1" ).arg( path ) );
}

void PreviewProfilePanel::exportPng()
{
  if ( m_series.isEmpty() )
    return;
  const QString path = m_pngTarget.isEmpty()
      ? QFileDialog::getSaveFileName( this, QObject::tr( "导出剖面 PNG" ), QString(),
                                      QStringLiteral( "PNG (*.png)" ) )
      : m_pngTarget;
  if ( path.isEmpty() )
    return;
  if ( !writePng( path ) )
    paleo::ui::NotificationManager::showWarning( this, QObject::tr( "导出失败" ), QObject::tr( "无法写入 %1" ).arg( path ) );
}

bool PreviewProfilePanel::writeCsv( const QString &path ) const
{
  QFile f( path );
  if ( !f.open( QIODevice::WriteOnly | QIODevice::Text ) )
    return false;
  QTextStream ts( &f ); // Qt6 默认 UTF-8
  ts << QStringLiteral( "series,distance,value\n" );
  for ( const Series &s : m_series )
    for ( const Sample &sm : s.samples )
      if ( sm.valid )
        ts << s.name << QStringLiteral( "," ) << QString::number( sm.distance, 'f', 2 )
           << QStringLiteral( "," ) << QString::number( sm.value, 'f', 4 ) << QLatin1Char( '\n' );
  return true;
}

bool PreviewProfilePanel::writePng( const QString &path ) const
{
  return m_chart->grab().toImage().save( path, "PNG" );
}
