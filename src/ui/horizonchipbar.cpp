// 层：视图
#include "horizonchipbar.h"

#include "../domain/mappinghorizons.h"
#include "../linkage/selectioncontext.h"
#include "../qgis/qgislayerservice.h"

#include <QSet>
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
  buildChips();
  lay->addStretch( 1 );

  if ( m_selection )
  {
    connect( m_selection, &SelectionContext::activeHorizonChanged, this,
             [this]( const QString &h ) { applyActive( h ); } );
    applyActive( m_selection->activeHorizon() );
  }
  if ( m_layers )
    connect( m_layers, &QgisLayerService::layerDeclared, this,
             [this]( const QString & ) { applyAvailability(); } );
  applyAvailability();
}

void HorizonChipBar::buildChips()
{
  // insertWidget 在末尾 stretch 之前落 chip——reload 时 stretch 已在布局里。
  auto *lay = qobject_cast<QHBoxLayout *>( layout() );
  int pos = 0;
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
      QString editingName;
      if ( m_layers && m_layers->isEditingAnyLayer( &editingName ) )
      {
        applyActive( m_selection ? m_selection->activeHorizon() : QString() );
        // 主线3：不再静默拦截——文案告诉用户编辑挡住了切换及恢复路径。
        emit horizonSwitchRefused(
            tr( "正在编辑「%1」——先保存或放弃编辑，再切换层位" ).arg( editingName ) );
        return;
      }
      if ( m_selection )
        m_selection->setActiveHorizon( h );
      if ( m_layers )
        m_layers->setActiveHorizon( h );
      applyActive( h );
    } );
    lay->insertWidget( pos++, chip );
  }
}

void HorizonChipBar::reloadHorizons()
{
  // 层位名单是工程级参数（AreaRules）——工程打开后按新词表重建；旧 chip
  // 连同其连接一并销毁，激活态交由 applyActive 按当前 selection 恢复。
  const auto chips = findChildren<QToolButton *>();
  for ( QToolButton *chip : chips )
  {
    layout()->removeWidget( chip );
    chip->deleteLater();
  }
  buildChips();
  applyActive( m_selection ? m_selection->activeHorizon() : QString() );
  applyAvailability();
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

void HorizonChipBar::applyAvailability()
{
  // 有栅格声明的层位才可点（horizon.<h>.* / factor.<h>.idw 都算——按层位名 +
  // type=raster 判定）；没有的层位这一阶段还没有它的栅格。清单读不出来同样
  // 关闸（发布链路的保守方向：缺数据不放开入口）。
  QSet<QString> available;
  QVector<LayerDeclaration> declared;
  if ( m_layers && m_layers->tryDeclared( &declared ) )
    for ( const LayerDeclaration &d : declared )
      if ( d.type.compare( QStringLiteral( "raster" ), Qt::CaseInsensitive ) == 0 )
        available.insert( d.horizon );
  for ( QToolButton *chip : findChildren<QToolButton *>() )
  {
    const bool ok = available.contains( chip->text() );
    chip->setEnabled( ok );
    chip->setToolTip( ok ? QString() : tr( "这一阶段还没有这个层位的栅格" ) );
  }
}
