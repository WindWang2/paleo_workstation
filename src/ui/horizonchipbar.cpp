// 层：视图
#include "horizonchipbar.h"

#include "../domain/mappinghorizons.h"
#include "../linkage/selectioncontext.h"
#include "../qgis/qgislayerservice.h"
#include "paleotheme.h"
#include "wellsection/wellsectionstyle.h"

#include <QColor>
#include <QSet>
#include <QToolButton>
#include <QHBoxLayout>

// DESIGN.md components.chip / chip-active（rounded.full = 胶囊）。未选 chip
// 的底色 = 该层位在井道图（连井剖面）上的层段色（2026-10-10 用户要求颜色与
// 井道图一致、文字必须保留）：数据符号色不进 UI token、不随主题明暗变，字色
// 取同色相深色（pastel 底上明暗主题都可读）。checked 仍是 chip-active
//（primary 底 + on-primary 字，交互蓝三用途之一），圆角/留白走 token。
static void applyChipStyle( QToolButton *chip, const QColor &zone )
{
  PaleoTheme::applyThemedStyleSheet( chip, [chip, zone] {
    const PaleoTheme::ThemeTokens &t = PaleoTheme::tokens();
    const QString faded = QStringLiteral( "rgba(%1,%2,%3,0.45)" )
                              .arg( zone.red() ).arg( zone.green() ).arg( zone.blue() );
    return PaleoTheme::metricStyleSheet(QStringLiteral(
               "QToolButton { border-radius: %8px; padding: {spacing.xs}px {spacing.md}px;"
               " background: %1; border: 1px solid %2; color: %3; }"
               "QToolButton:checked { background: %4; border-color: %4; color: %5; }"
               "QToolButton:hover:!checked { border-color: %6; }"
               "QToolButton:disabled { background: %9; color: %7; }" ))
        .arg( zone.name(), zone.darker( 125 ).name(), zone.darker( 380 ).name(),
              t.primary.name(), t.onPrimary.name(), zone.darker( 170 ).name(),
              t.textDisabled.name() )
        .arg( PaleoTheme::chipRadius( chip->font() ) )
        .arg( faded );
  } );
}

HorizonChipBar::HorizonChipBar( SelectionContext *selection, QgisLayerService *layers,
                                QWidget *parent )
  : QWidget( parent )
  , m_selection( selection )
  , m_layers( layers )
{
  auto *lay = new QHBoxLayout( this );
  lay->setContentsMargins( 0, 0, 0, 0 );
  lay->setSpacing(PaleoTheme::tokens().spacingXs); // spacing.xs
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

// 层位 H 的 chip 底色 = 连井剖面上 H 顶→下一层位顶那段「层段」的底色
//（wellsection::zoneColor 同一色板、同一浅→深序）。
QColor HorizonChipBar::zoneColorOf( const QString &horizon )
{
  return wellsection::zoneColor( mappingHorizons().indexOf( horizon ) );
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
    // 颜色落在底色上、不另占图标位：文字始终完整可见，条宽不被色点挤到
    // 文字省略成「…」只剩颜色（2026-10-10 用户反馈）。
    chip->setToolButtonStyle( Qt::ToolButtonTextOnly );
    chip->setSizePolicy( QSizePolicy::Minimum, QSizePolicy::Fixed );
    applyChipStyle( chip, zoneColorOf( h ) );
    chip->setAccessibleName( tr( "层位 %1" ).arg( h ) );
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
    const bool ok = m_allowEmptyHorizons || available.contains( chip->text() );
    chip->setEnabled( ok );
    chip->setToolTip( ok ? QString() : tr( "这一阶段还没有这个层位的栅格" ) );
  }
}

void HorizonChipBar::setAllowEmptyHorizons(bool enabled)
{
  m_allowEmptyHorizons=enabled;applyAvailability();
}
