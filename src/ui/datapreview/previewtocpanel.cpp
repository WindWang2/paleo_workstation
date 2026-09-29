// 层：视图
#include "previewtocpanel.h"

#include "previewmapstates.h"

#include "../paleotheme.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSlider>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <qgsfillsymbol.h>
#include <qgslinesymbol.h>
#include <qgsmaplayer.h>
#include <qgsmarkersymbol.h>
#include <qgsrasterlayer.h>
#include <qgssinglesymbolrenderer.h>
#include <qgsvectorlayer.h>

namespace
{
QString formatExtent( const QgsRectangle &ext )
{
  if ( ext.isEmpty() )
    return QObject::tr( "—" );
  return QObject::tr( "%1 × %2" )
      .arg( QString::number( ext.width(), 'f', 0 ), QString::number( ext.height(), 'f', 0 ) );
}
} // namespace

PreviewTocPanel::PreviewTocPanel( QWidget *parent )
  : QWidget( parent )
{
  auto *lay = new QVBoxLayout( this );
  lay->setContentsMargins( 6, 6, 6, 6 );
  lay->setSpacing( 4 );

  auto *title = new QLabel( QObject::tr( "图层" ), this );
  title->setStyleSheet( QStringLiteral( "font-weight: 600; color: #24303E;" ) );
  lay->addWidget( title );

  m_list = new QListWidget( this );
  m_list->setObjectName( QStringLiteral( "previewTocList" ) );
  m_list->setDragDropMode( QAbstractItemView::InternalMove ); // D4.1 拖拽排序
  m_list->setSelectionMode( QAbstractItemView::SingleSelection );
  connect( m_list, &QListWidget::itemChanged, this, [this]( QListWidgetItem *item ) {
    if ( m_suppressSignals || !item )
      return;
    const int row = m_list->row( item );
    if ( row < 0 || row >= m_rows.size() )
      return;
    emit layerVisibilityChanged( m_rows.at( row ).layer,
                                 item->checkState() == Qt::Checked );
    saveMemory();
  } );
  connect( m_list, &QListWidget::currentRowChanged, this, [this]( int ) { rebuildQuickPanel(); } );
  connect( m_list->model(), &QAbstractItemModel::rowsMoved, this, [this] {
    if ( m_suppressSignals )
      return;
    QList<RowInfo> reordered;
    for ( int i = 0; i < m_list->count(); ++i )
    {
      const int idx = m_list->item( i )->data( Qt::UserRole ).toInt();
      if ( idx >= 0 && idx < m_rows.size() )
        reordered.append( m_rows.at( idx ) );
    }
    if ( reordered.size() == m_rows.size() )
    {
      m_rows = reordered;
      emit layerOrderChanged( layersTopToBottom() );
      saveMemory();
    }
  } );
  lay->addWidget( m_list, 2 );

  // ---- 符号快调堆叠（D4.3/D4.4/D4.5）----
  m_quickPanel = new QStackedWidget( this );
  m_quickPanel->setObjectName( QStringLiteral( "previewTocQuickStack" ) );

  auto *emptyPage = new QLabel( QObject::tr( "选择一个图层查看快调" ), m_quickPanel );
  emptyPage->setAlignment( Qt::AlignCenter );
  emptyPage->setStyleSheet( QStringLiteral( "color: #5D6E80;" ) );
  m_quickPanel->addWidget( emptyPage );

  // 页 1：栅格快调（D4.3 色带/拉伸/反转 + 手动值域）
  {
    auto *page = new QWidget( m_quickPanel );
    auto *form = new QFormLayout( page );
    form->setContentsMargins( 0, 4, 0, 4 );

    m_rampCombo = new QComboBox( page );
    m_rampCombo->setObjectName( QStringLiteral( "tocRampCombo" ) );
    for ( const auto &p : PreviewRasterAnalysis::rampPresets() )
      m_rampCombo->addItem( p.name, p.id );
    form->addRow( QObject::tr( "色带" ), m_rampCombo );

    m_stretchCombo = new QComboBox( page );
    m_stretchCombo->setObjectName( QStringLiteral( "tocStretchCombo" ) );
    m_stretchCombo->addItem( QObject::tr( "全值域" ),
                             int( PreviewRasterAnalysis::Stretch::MinMax ) );
    m_stretchCombo->addItem( QObject::tr( "2%–98%" ),
                             int( PreviewRasterAnalysis::Stretch::Percent2To98 ) );
    m_stretchCombo->addItem( QObject::tr( "直方图均衡" ),
                             int( PreviewRasterAnalysis::Stretch::HistEq ) );
    m_stretchCombo->addItem( QObject::tr( "手动" ),
                             int( PreviewRasterAnalysis::Stretch::Manual ) );
    form->addRow( QObject::tr( "拉伸" ), m_stretchCombo );

    m_invertCheck = new QCheckBox( QObject::tr( "反转色带" ), page );
    m_invertCheck->setObjectName( QStringLiteral( "tocInvertCheck" ) );
    form->addRow( QString(), m_invertCheck );

    auto *rangeRow = new QWidget( page );
    auto *rangeLay = new QHBoxLayout( rangeRow );
    rangeLay->setContentsMargins( 0, 0, 0, 0 );
    m_minSpin = new QDoubleSpinBox( rangeRow );
    m_minSpin->setObjectName( QStringLiteral( "tocMinSpin" ) );
    m_maxSpin = new QDoubleSpinBox( rangeRow );
    m_maxSpin->setObjectName( QStringLiteral( "tocMaxSpin" ) );
    for ( auto *s : { m_minSpin, m_maxSpin } )
    {
      s->setRange( -1e12, 1e12 );
      s->setDecimals( 3 );
      s->setEnabled( false );
      s->setFont( PaleoTheme::monoFont() );
    }
    rangeLay->addWidget( m_minSpin );
    rangeLay->addWidget( new QLabel( QStringLiteral( "–" ), rangeRow ) );
    rangeLay->addWidget( m_maxSpin );
    form->addRow( QObject::tr( "手动值域" ), rangeRow );
    m_quickPanel->addWidget( page );
  }

  // 页 2：矢量快调（D4.4 分类字段/单色切换）
  {
    auto *page = new QWidget( m_quickPanel );
    auto *form = new QFormLayout( page );
    form->setContentsMargins( 0, 4, 0, 4 );

    m_fieldCombo = new QComboBox( page );
    m_fieldCombo->setObjectName( QStringLiteral( "tocFieldCombo" ) );
    form->addRow( QObject::tr( "字段" ), m_fieldCombo );

    m_categorizedCheck = new QCheckBox( QObject::tr( "按字段分类渲染" ), page );
    m_categorizedCheck->setObjectName( QStringLiteral( "tocCategorizedCheck" ) );
    form->addRow( QString(), m_categorizedCheck );

    m_colorBtn = new QPushButton( QObject::tr( "单色符号…" ), page );
    m_colorBtn->setObjectName( QStringLiteral( "tocColorBtn" ) );
    m_colorBtn->setProperty( "color", QColor( QStringLiteral( "#1B73D0" ) ) );
    form->addRow( QString(), m_colorBtn );
    m_quickPanel->addWidget( page );
  }

  // 页 3：属性速览（D4.5）
  m_propsLabel = new QLabel( m_quickPanel );
  m_propsLabel->setObjectName( QStringLiteral( "tocPropsLabel" ) );
  m_propsLabel->setWordWrap( true );
  m_propsLabel->setTextFormat( Qt::RichText );
  m_propsLabel->setAlignment( Qt::AlignTop | Qt::AlignLeft );
  m_quickPanel->addWidget( m_propsLabel );
  lay->addWidget( m_quickPanel, 3 );

  // ---- 通用层调节（透明度 D4.2 / 混合 D4.8，选中层任意类型可用）----
  {
    auto *box = new QWidget( this );
    auto *form = new QFormLayout( box );
    form->setContentsMargins( 0, 4, 0, 4 );

    m_opacitySlider = new QSlider( Qt::Horizontal, box );
    m_opacitySlider->setObjectName( QStringLiteral( "tocOpacitySlider" ) );
    m_opacitySlider->setRange( 0, 100 );
    m_opacitySlider->setValue( 100 );
    form->addRow( QObject::tr( "透明度" ), m_opacitySlider );

    m_blendCombo = new QComboBox( box );
    m_blendCombo->setObjectName( QStringLiteral( "tocBlendCombo" ) );
    m_blendCombo->addItem( QObject::tr( "正常" ), int( QPainter::CompositionMode_SourceOver ) );
    m_blendCombo->addItem( QObject::tr( "正片叠底" ), int( QPainter::CompositionMode_Multiply ) );
    m_blendCombo->addItem( QObject::tr( "叠加" ), int( QPainter::CompositionMode_Overlay ) );
    m_blendCombo->addItem( QObject::tr( "滤色" ), int( QPainter::CompositionMode_Screen ) );
    m_blendCombo->addItem( QObject::tr( "变暗" ), int( QPainter::CompositionMode_Darken ) );
    m_blendCombo->addItem( QObject::tr( "变亮" ), int( QPainter::CompositionMode_Lighten ) );
    form->addRow( QObject::tr( "混合" ), m_blendCombo );
    lay->addWidget( box );
  }

  // ---- 底部动作行 ----
  {
    auto *row = new QWidget( this );
    auto *rowLay = new QHBoxLayout( row );
    rowLay->setContentsMargins( 0, 0, 0, 0 );
    rowLay->setSpacing( 6 );

    m_attrTableBtn = new QPushButton( QObject::tr( "属性表" ), row );
    m_attrTableBtn->setObjectName( QStringLiteral( "tocAttrTableBtn" ) );
    m_attrTableBtn->setEnabled( false );
    m_attrTableBtn->setToolTip( QObject::tr( "打开全表浏览（分页/排序/列过滤）" ) );
    connect( m_attrTableBtn, &QPushButton::clicked, this, [this] {
      if ( auto *vl = qobject_cast<QgsVectorLayer *>( currentLayer() ) )
        emit attributeTableRequested( vl ); // D7.3
    } );
    rowLay->addWidget( m_attrTableBtn );

    m_removeBtn = new QPushButton( QObject::tr( "移除图层" ), row );
    m_removeBtn->setObjectName( QStringLiteral( "tocRemoveBtn" ) );
    m_removeBtn->setEnabled( false );
    m_removeBtn->setToolTip( QObject::tr( "仅从预览移除，不动数据目录" ) );
    connect( m_removeBtn, &QPushButton::clicked, this, [this] {
      if ( QgsMapLayer *l = currentLayer() )
        emit layerRemoveRequested( l ); // D4.6
    } );
    rowLay->addWidget( m_removeBtn );
    lay->addWidget( row );
  }

  // ---- 图例（D2.4）----
  m_legendBox = new QWidget( this );
  m_legendBox->setObjectName( QStringLiteral( "previewLegendBox" ) );
  auto *legendLay = new QVBoxLayout( m_legendBox );
  legendLay->setContentsMargins( 0, 6, 0, 0 );
  auto *legendTitle = new QLabel( QObject::tr( "图例" ), m_legendBox );
  legendTitle->setStyleSheet( QStringLiteral( "font-weight: 600; color: #24303E;" ) );
  legendLay->addWidget( legendTitle );
  m_legendContent = new QLabel( m_legendBox );
  m_legendContent->setObjectName( QStringLiteral( "previewLegendContent" ) );
  m_legendContent->setTextFormat( Qt::RichText );
  m_legendContent->setWordWrap( true );
  legendLay->addWidget( m_legendContent );
  m_legendBox->setVisible( false );
  lay->addWidget( m_legendBox );

  // ---- 即时应用接线 ----
  const auto rasterChanged = [this] {
    if ( m_suppressSignals )
      return;
    if ( auto *rl = qobject_cast<QgsRasterLayer *>( currentLayer() ) )
      applyRasterStyle( rl );
    saveMemory();
  };
  connect( m_rampCombo, &QComboBox::currentIndexChanged, this, rasterChanged );
  connect( m_stretchCombo, &QComboBox::currentIndexChanged, this, [this, rasterChanged]( int ) {
    const bool manual = m_stretchCombo->currentData().toInt() ==
                        int( PreviewRasterAnalysis::Stretch::Manual );
    m_minSpin->setEnabled( manual );
    m_maxSpin->setEnabled( manual );
    rasterChanged();
  } );
  connect( m_invertCheck, &QCheckBox::toggled, this, rasterChanged );
  connect( m_minSpin, &QDoubleSpinBox::valueChanged, this, rasterChanged );
  connect( m_maxSpin, &QDoubleSpinBox::valueChanged, this, rasterChanged );

  connect( m_categorizedCheck, &QCheckBox::toggled, this, [this] {
    if ( m_suppressSignals )
      return;
    if ( auto *vl = qobject_cast<QgsVectorLayer *>( currentLayer() ) )
      applyVectorStyle( vl );
    saveMemory();
  } );
  connect( m_fieldCombo, &QComboBox::currentTextChanged, this, [this]( const QString & ) {
    if ( m_suppressSignals || !m_categorizedCheck->isChecked() )
      return;
    if ( auto *vl = qobject_cast<QgsVectorLayer *>( currentLayer() ) )
      applyVectorStyle( vl );
    saveMemory();
  } );
  connect( m_colorBtn, &QPushButton::clicked, this, [this] {
    if ( m_suppressSignals )
      return;
    const QColor c = QColorDialog::getColor( m_colorBtn->property( "color" ).value<QColor>(),
                                             this, QObject::tr( "单色符号" ) );
    if ( !c.isValid() )
      return;
    m_colorBtn->setProperty( "color", c );
    if ( auto *vl = qobject_cast<QgsVectorLayer *>( currentLayer() ) )
      applyVectorStyle( vl );
    saveMemory();
  } );

  connect( m_opacitySlider, &QSlider::valueChanged, this, [this]( int v ) {
    if ( m_suppressSignals )
      return;
    if ( QgsMapLayer *l = currentLayer() )
      emit layerOpacityChanged( l, v / 100.0 ); // D4.2
    saveMemory();
  } );
  connect( m_blendCombo, &QComboBox::currentIndexChanged, this, [this]( int ) {
    if ( m_suppressSignals )
      return;
    if ( QgsMapLayer *l = currentLayer() )
      emit layerBlendChanged( l, m_blendCombo->currentData().toInt() ); // D4.8
    saveMemory();
  } );

  rebuildQuickPanel();
}

void PreviewTocPanel::setAssetKey( const QString &assetKey )
{
  m_assetKey = assetKey;
}

void PreviewTocPanel::addLayer( QgsMapLayer *layer, const QString &name, const QString &sourcePath,
                                bool defaultVisible )
{
  if ( !layer )
    return;
  for ( const RowInfo &r : m_rows )
    if ( r.layer == layer )
      return; // 幂等
  RowInfo row;
  row.layer = layer;
  row.name = name.isEmpty() ? layer->name() : name;
  row.sourcePath = sourcePath;
  // D4.7 记忆恢复：同资产重开时沿用上次的可见位。
  bool visible = defaultVisible;
  if ( !m_assetKey.isEmpty() )
  {
    const auto states = PreviewStateMemory::tocStates( m_assetKey );
    if ( states.contains( row.name ) )
      visible = states.value( row.name ).visible;
  }
  row.visible = visible;
  m_rows.prepend( row );
  rebuildList();
  if ( !visible )
    emit layerVisibilityChanged( layer, false );
  saveMemory();
}

void PreviewTocPanel::removeLayer( QgsMapLayer *layer )
{
  for ( int i = 0; i < m_rows.size(); ++i )
    if ( m_rows.at( i ).layer == layer )
    {
      m_rows.removeAt( i );
      break;
    }
  rebuildList();
  saveMemory();
}

void PreviewTocPanel::clear()
{
  m_rows.clear();
  rebuildList();
  saveMemory();
}

int PreviewTocPanel::layerCount() const
{
  return m_rows.size();
}

QList<QgsMapLayer *> PreviewTocPanel::layersTopToBottom() const
{
  QList<QgsMapLayer *> out;
  for ( const RowInfo &r : m_rows )
    out.append( r.layer );
  return out;
}

QgsMapLayer *PreviewTocPanel::currentLayer() const
{
  const int row = m_list->currentRow();
  return ( row >= 0 && row < m_rows.size() ) ? m_rows.at( row ).layer : nullptr;
}

void PreviewTocPanel::setLayerVisibleInUi( QgsMapLayer *layer, bool visible )
{
  for ( int i = 0; i < m_rows.size(); ++i )
    if ( m_rows.at( i ).layer == layer )
    {
      m_rows[i].visible = visible;
      if ( QListWidgetItem *item = m_list->item( i ) )
      {
        m_suppressSignals = true;
        item->setCheckState( visible ? Qt::Checked : Qt::Unchecked );
        m_suppressSignals = false;
      }
      saveMemory();
      return;
    }
}

void PreviewTocPanel::setLegendEntries( const QVector<LegendEntry> &entries )
{
  m_legendEntries = entries;
  QString html;
  for ( const LegendEntry &e : entries )
    html += QStringLiteral( "<p style=\"margin:1px 0;\"><span style=\""
                            "background-color:%1;color:%1;\">&nbsp;&nbsp;&nbsp;</span> %2</p>" )
                .arg( e.color.name(), e.name.toHtmlEscaped() );
  m_legendContent->setText( html );
  m_legendBox->setVisible( !entries.isEmpty() );
}

void PreviewTocPanel::rebuildList()
{
  m_suppressSignals = true;
  m_list->clear();
  for ( int i = 0; i < m_rows.size(); ++i )
  {
    const RowInfo &r = m_rows.at( i );
    auto *item = new QListWidgetItem( r.name, m_list );
    item->setFlags( Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsUserCheckable |
                    Qt::ItemIsDragEnabled );
    item->setCheckState( r.visible ? Qt::Checked : Qt::Unchecked );
    item->setData( Qt::UserRole, i );
    item->setToolTip( r.sourcePath );
  }
  m_suppressSignals = false;
  if ( m_list->count() > 0 )
    m_list->setCurrentRow( 0 );
  else
    rebuildQuickPanel();
}

void PreviewTocPanel::rebuildQuickPanel()
{
  QgsMapLayer *l = currentLayer();
  m_removeBtn->setEnabled( l != nullptr );
  m_attrTableBtn->setEnabled( qobject_cast<QgsVectorLayer *>( l ) != nullptr );
  if ( !l )
  {
    m_quickPanel->setCurrentIndex( 0 );
    return;
  }
  m_suppressSignals = true;
  if ( auto *rl = qobject_cast<QgsRasterLayer *>( l ) )
  {
    m_quickPanel->setCurrentIndex( 1 );
    const auto sum = PreviewRasterAnalysis::summarize( rl );
    if ( sum.valid )
    {
      m_minSpin->setValue( sum.min );
      m_maxSpin->setValue( sum.max );
    }
  }
  else if ( auto *vl = qobject_cast<QgsVectorLayer *>( l ) )
  {
    m_quickPanel->setCurrentIndex( 2 );
    m_fieldCombo->clear();
    const QgsFields fields = vl->fields();
    for ( const QgsField &f : fields )
      m_fieldCombo->addItem( f.name() );
    m_categorizedCheck->setChecked( false );
  }
  else
  {
    m_quickPanel->setCurrentIndex( 3 );
  }

  // 属性速览（D4.5：源路径/类型/范围/单元大小）。
  QString typeText;
  QString extraText;
  if ( auto *rl = qobject_cast<QgsRasterLayer *>( l ) )
  {
    typeText = QObject::tr( "栅格" );
    if ( rl->width() > 0 && !rl->extent().isEmpty() )
      extraText = QObject::tr( "单元 %1 m" )
                      .arg( QString::number( rl->extent().width() / rl->width(), 'f', 2 ) );
  }
  else if ( auto *vl = qobject_cast<QgsVectorLayer *>( l ) )
  {
    typeText = QObject::tr( "矢量（%1 要素）" ).arg( vl->featureCount() );
  }
  m_propsLabel->setText( QStringLiteral( "<b>%1</b><br>%2: %3<br>%4: %5<br>%6: %7" )
                             .arg( l->name().toHtmlEscaped(), QObject::tr( "类型" ), typeText,
                                   QObject::tr( "范围" ), formatExtent( l->extent() ),
                                   QObject::tr( "源" ), l->source().toHtmlEscaped() ) +
                             ( extraText.isEmpty() ? QString() : QStringLiteral( "<br>%1" ).arg( extraText ) ) );
  m_propsLabel->setVisible( m_quickPanel->currentIndex() == 3 || true );
  m_suppressSignals = false;
}

void PreviewTocPanel::applyRasterStyle( QgsRasterLayer *rl )
{
  const QString rampId = m_rampCombo->currentData().toString();
  const auto *preset = PreviewRasterAnalysis::rampPreset( rampId );
  if ( !preset )
    return;
  const auto stretch = PreviewRasterAnalysis::Stretch( m_stretchCombo->currentData().toInt() );
  const auto hist = PreviewRasterAnalysis::histogram( rl, 64 );
  const bool quantile = stretch == PreviewRasterAnalysis::Stretch::HistEq;
  double lo = 0.0;
  double hi = 1.0;
  if ( quantile )
  {
    const auto b = PreviewRasterAnalysis::stretchBounds( hist,
                                                          PreviewRasterAnalysis::Stretch::MinMax );
    lo = b.first;
    hi = b.second;
  }
  else
  {
    const auto b = PreviewRasterAnalysis::stretchBounds( hist, stretch, m_minSpin->value(),
                                                          m_maxSpin->value() );
    lo = b.first;
    hi = b.second;
  }
  PreviewRasterAnalysis::applyPseudoColorRenderer(
      rl, 1, lo, hi, *preset, m_invertCheck->isChecked(),
      quantile ? PreviewRasterAnalysis::Classification::Quantile
               : PreviewRasterAnalysis::Classification::Continuous );
  emit rasterStyleChanged( rl );
}

void PreviewTocPanel::applyVectorStyle( QgsVectorLayer *vl )
{
  if ( !vl )
    return;
  if ( m_categorizedCheck->isChecked() )
  {
    if ( vectorStyleApplier )
      vectorStyleApplier( vl, m_fieldCombo->currentText(), true, QColor() );
    return;
  }
  // 单色：按几何类型建简单符号（D4.4 单色/分类切换）。
  const QColor c = m_colorBtn->property( "color" ).value<QColor>();
  if ( !c.isValid() )
    return;
  QVariantMap props;
  std::unique_ptr<QgsSymbol> sym;
  if ( vl->geometryType() == Qgis::GeometryType::Point )
  {
    props[QStringLiteral( "name" )] = QStringLiteral( "circle" );
    props[QStringLiteral( "color" )] = c.name();
    props[QStringLiteral( "size" )] = QStringLiteral( "4" );
    sym = QgsMarkerSymbol::createSimple( props );
  }
  else if ( vl->geometryType() == Qgis::GeometryType::Line )
  {
    props[QStringLiteral( "line_color" )] = c.name();
    props[QStringLiteral( "line_width" )] = QStringLiteral( "1.2" );
    sym = QgsLineSymbol::createSimple( props );
  }
  else
  {
    props[QStringLiteral( "color" )] = c.name();
    props[QStringLiteral( "outline_color" )] = c.darker( 150 ).name();
    sym = QgsFillSymbol::createSimple( props );
  }
  vl->setRenderer( new QgsSingleSymbolRenderer( sym.release() ) );
  vl->triggerRepaint();
}

void PreviewTocPanel::saveMemory() const
{
  if ( m_assetKey.isEmpty() )
    return;
  QStringList order;
  QHash<QString, PreviewStateMemory::TocLayerState> states;
  for ( const RowInfo &r : m_rows )
  {
    PreviewStateMemory::TocLayerState st;
    st.visible = r.visible;
    st.opacity = 1.0;
    order.append( r.name );
    states.insert( r.name, st );
  }
  PreviewStateMemory::saveToc( m_assetKey, order, states );
}
