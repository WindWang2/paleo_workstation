#include "horizonchipbar.h"

#include "../domain/mappinghorizons.h"
#include "../linkage/selectioncontext.h"
#include "../qgis/qgislayerservice.h"

#include <QToolButton>
#include <QHBoxLayout>

// DESIGN.md components.chip / chip-active（rounded.full = 胶囊）。
static const char kChipStyle[] =
    "QToolButton { border-radius: 9999px; padding: 4px 16px;"
    " background: #FFFFFF; border: 1px solid #DFE5EC; color: #5D6E80; }"
    "QToolButton:checked { background: #1B73D0; border-color: #1B73D0; color: #FFFFFF; }"
    "QToolButton:hover:!checked { color: #24303E; }";

HorizonChipBar::HorizonChipBar( SelectionContext *selection, QgisLayerService *layers,
                                QWidget *parent )
  : QWidget( parent )
  , m_selection( selection )
  , m_layers( layers )
{
  auto *lay = new QHBoxLayout( this );
  lay->setContentsMargins( 0, 0, 0, 0 );
  lay->setSpacing( 4 ); // spacing.xs

  for ( const QString &h : mappingHorizons() )
  {
    auto *chip = new QToolButton( this );
    chip->setObjectName( QStringLiteral( "chip_%1" ).arg( h ) );
    chip->setText( h );
    chip->setCheckable( true );
    chip->setToolButtonStyle( Qt::ToolButtonTextOnly );
    chip->setStyleSheet( QLatin1String( kChipStyle ) );
    chip->setAccessibleName( QStringLiteral( "层位 %1" ).arg( h ) );
    connect( chip, &QToolButton::clicked, this, [this, h] {
      // 切层位 = 广播 + 懒加载（物化目标、释放其他层位实例）。
      if ( m_selection )
        m_selection->setActiveHorizon( h );
      if ( m_layers )
        m_layers->setActiveHorizon( h );
      applyActive( h );
    } );
    lay->addWidget( chip );
  }
  lay->addStretch( 1 );

  if ( m_selection )
  {
    connect( m_selection, &SelectionContext::activeHorizonChanged, this,
             [this]( const QString &h ) { applyActive( h ); } );
    applyActive( m_selection->activeHorizon() );
  }
}

QStringList HorizonChipBar::chipNames() const
{
  QStringList names;
  const auto chips = findChildren<QToolButton *>();
  for ( const QString &h : mappingHorizons() )
    for ( QToolButton *chip : chips )
      if ( chip->objectName() == QLatin1String( "chip_" ) + h )
        names.append( h );
  return names;
}

bool HorizonChipBar::isChipActive( const QString &horizon ) const
{
  auto *chip = findChild<QToolButton *>( QStringLiteral( "chip_%1" ).arg( horizon ) );
  return chip ? chip->isChecked() : false;
}

void HorizonChipBar::applyActive( const QString &horizon )
{
  for ( QToolButton *chip : findChildren<QToolButton *>() )
    chip->setChecked( chip->text() == horizon ); // 集合外层位不点亮任何 chip
}
