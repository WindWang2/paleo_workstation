// 层：视图
#include "previewidentifypanel.h"

#include "../paleoemptystate.h"
#include "../paleotheme.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTextStream>
#include <QCloseEvent>
#include <QVBoxLayout>

#include <qgsvectorlayer.h>

namespace
{
  QFont mono9()
  {
    return PaleoTheme::monoFont();
  }

  QString resultTitle( const PreviewIdentifyResult &r )
  {
    if ( r.isRaster )
      return QObject::tr( "%1 · 栅格取值" ).arg( r.layerName );
    return QStringLiteral( "%1 · #%2" ).arg( r.layerName ).arg( r.featureId );
  }
} // namespace

// ------------------------------------------------------------- identify ----

PreviewIdentifyPanel::PreviewIdentifyPanel( QWidget *parent )
  : QWidget( parent )
{
  auto *lay = new QVBoxLayout( this );
  lay->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm);
  lay->setSpacing(PaleoTheme::tokens().spacingXs);

  auto *bar = new QWidget( this );
  auto *barLay = new QHBoxLayout( bar );
  barLay->setContentsMargins( 0, 0, 0, 0 );
  barLay->setSpacing(PaleoTheme::tokens().spacingSm);
  auto *title = new QLabel( QObject::tr( "识别结果" ), bar );
  PaleoTheme::applyThemedStyleSheet(
      title, [] { return PaleoTheme::sectionTitleStyleSheet(); } );
  barLay->addWidget( title );

  auto *flashBtn = new QPushButton( QObject::tr( "定位闪烁" ), bar );
  flashBtn->setObjectName( QStringLiteral( "identifyFlashBtn" ) );
  connect( flashBtn, &QPushButton::clicked, this, &PreviewIdentifyPanel::flashCurrent );
  barLay->addWidget( flashBtn );

  auto *csvBtn = new QPushButton( QObject::tr( "导出 CSV" ), bar );
  csvBtn->setObjectName( QStringLiteral( "identifyCsvBtn" ) );
  connect( csvBtn, &QPushButton::clicked, this, &PreviewIdentifyPanel::exportCsv );
  barLay->addWidget( csvBtn );
  barLay->addStretch( 1 );
  lay->addWidget( bar );

  m_resultList = new QListWidget( this );
  m_resultList->setObjectName( QStringLiteral( "identifyResultList" ) );
  connect( m_resultList, &QListWidget::currentRowChanged, this,
           &PreviewIdentifyPanel::showResultDetails );
  lay->addWidget( m_resultList, 1 );

  m_attrTable = new QTableWidget( this );
  m_attrTable->setObjectName( QStringLiteral( "identifyAttrTable" ) );
  m_attrTable->setColumnCount( 2 );
  m_attrTable->setHorizontalHeaderLabels( { QObject::tr( "字段" ), QObject::tr( "值" ) } );
  m_attrTable->verticalHeader()->setVisible( false );
  m_attrTable->setEditTriggers( QAbstractItemView::NoEditTriggers );
  m_attrTable->setSelectionBehavior( QAbstractItemView::SelectRows );
  // D7.1 复制行：右键菜单
  m_attrTable->setContextMenuPolicy( Qt::CustomContextMenu );
  connect( m_attrTable, &QTableWidget::customContextMenuRequested, this,
           [this]( const QPoint &pos ) {
             const int row = m_attrTable->rowAt( pos.y() );
             if ( row < 0 )
               return;
             QMenu menu( this );
             QAction *copy = menu.addAction( QObject::tr( "复制行" ) );
             if ( menu.exec( m_attrTable->viewport()->mapToGlobal( pos ) ) == copy )
             {
               const QString text = QStringLiteral( "%1: %2" )
                                        .arg( m_attrTable->item( row, 0 )->text(),
                                              m_attrTable->item( row, 1 )->text() );
               QApplication::clipboard()->setText( text );
             }
           } );
  lay->addWidget( m_attrTable, 1 );

  m_emptyLabel = new QLabel( QObject::tr( "未命中任何要素" ), this );
  m_emptyLabel->setObjectName( QStringLiteral( "identifyEmptyLabel" ) );
  m_emptyLabel->setAlignment( Qt::AlignCenter );
  PaleoTheme::applyThemedStyleSheet(
      m_emptyLabel, [] { return PaleoTheme::mutedCaptionStyleSheet(); } );
  m_emptyLabel->setVisible( false );
  lay->addWidget( m_emptyLabel, 1 );
}

void PreviewIdentifyPanel::setResults( const QVector<PreviewIdentifyResult> &results )
{
  m_results = results;
  m_resultList->clear();
  m_attrTable->setRowCount( 0 );
  if ( results.isEmpty() )
  {
    m_emptyLabel->setVisible( true ); // D7.6 空命中反馈
    m_resultList->setVisible( false );
    m_attrTable->setVisible( false );
  }
  else
  {
    m_emptyLabel->setVisible( false );
    m_resultList->setVisible( true );
    m_attrTable->setVisible( true );
    for ( const PreviewIdentifyResult &r : results )
      m_resultList->addItem( resultTitle( r ) );
    m_resultList->setCurrentRow( 0 );
  }
  emit resultsChanged();
}

void PreviewIdentifyPanel::showResultDetails( int index )
{
  m_attrTable->setRowCount( 0 );
  if ( index < 0 || index >= m_results.size() )
    return;
  const PreviewIdentifyResult &r = m_results.at( index );
  const auto addRow = [this]( const QString &k, const QString &v, bool mono ) {
    const int row = m_attrTable->rowCount();
    m_attrTable->insertRow( row );
    auto *keyItem = new QTableWidgetItem( k );
    auto *valItem = new QTableWidgetItem( v );
    if ( mono )
    {
      valItem->setFont( mono9() );
      valItem->setTextAlignment( Qt::AlignRight | Qt::AlignVCenter );
    }
    m_attrTable->setItem( row, 0, keyItem );
    m_attrTable->setItem( row, 1, valItem );
  };
  if ( r.isRaster )
  {
    addRow( QObject::tr( "层" ), r.layerName, false );
    addRow( QObject::tr( "波段" ), QString::number( r.rasterBand ), true );
    addRow( QObject::tr( "最近邻值" ), QString::number( r.rasterValueNearest, 'f', 4 ), true );
    addRow( QObject::tr( "双线性插值" ), QString::number( r.rasterValueBilinear, 'f', 4 ), true );
    return;
  }
  for ( auto it = r.attributes.constBegin(); it != r.attributes.constEnd(); ++it )
  {
    const QString value = it.value().type() == QVariant::Double
                              ? QString::number( it.value().toDouble(), 'f', 4 )
                              : it.value().toString();
    addRow( it.key(), value, it.value().type() == QVariant::Double );
  }
}

void PreviewIdentifyPanel::exportCsv()
{
  if ( m_results.isEmpty() )
    return;
  const QString path = QFileDialog::getSaveFileName( this, QObject::tr( "导出识别结果 CSV" ),
                                                     QString(), QStringLiteral( "CSV (*.csv)" ) );
  if ( path.isEmpty() )
    return;
  QFile f( path );
  if ( !f.open( QIODevice::WriteOnly | QIODevice::Text ) )
  {
    QMessageBox::warning( this, QObject::tr( "导出失败" ), QObject::tr( "无法写入 %1" ).arg( path ) );
    return;
  }
  QTextStream ts( &f );
  ts << QStringLiteral( "layer,feature,field,value\n" );
  for ( const PreviewIdentifyResult &r : m_results )
  {
    if ( r.isRaster )
    {
      ts << r.layerName << QStringLiteral( ",raster," )
         << QStringLiteral( "nearest," ) << QString::number( r.rasterValueNearest, 'f', 4 )
         << QLatin1Char( '\n' );
      continue;
    }
    for ( auto it = r.attributes.constBegin(); it != r.attributes.constEnd(); ++it )
      ts << r.layerName << QStringLiteral( ",#%1," ).arg( r.featureId ) << it.key()
         << QStringLiteral( "," ) << it.value().toString() << QLatin1Char( '\n' );
  }
  emit exportCsvRequested( path );
}

void PreviewIdentifyPanel::flashCurrent()
{
  const int row = m_resultList->currentRow();
  if ( row < 0 || row >= m_results.size() )
    return;
  const PreviewIdentifyResult &r = m_results.at( row );
  if ( r.isRaster || !r.layer )
    return;
  emit flashRequested( r.layer, r.featureId );
  if ( flashHandler )
    flashHandler( r.layer, QgsFeatureIds{ r.featureId } ); // D7.2 定位闪烁
}

// ----------------------------------------------------------- full table ----

PreviewAttributeTableDialog::PreviewAttributeTableDialog( QgsVectorLayer *layer, QWidget *parent )
  : QWidget( parent, Qt::Window )
  , m_layer( layer )
{
  setObjectName( QStringLiteral( "previewAttributeTableDialog" ) );
  setAttribute( Qt::WA_DeleteOnClose );
  setWindowTitle( QObject::tr( "属性表 — %1" ).arg( layer ? layer->name() : QString() ) );
  resize( 720, 480 );

  auto *lay = new QVBoxLayout( this );
  lay->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm);
  lay->setSpacing(PaleoTheme::tokens().spacingSm);

  auto *bar = new QWidget( this );
  auto *barLay = new QHBoxLayout( bar );
  barLay->setContentsMargins( 0, 0, 0, 0 );
  barLay->setSpacing(PaleoTheme::tokens().spacingSm);

  barLay->addWidget( new QLabel( QObject::tr( "过滤" ), bar ) );
  m_filterEdit = new QLineEdit( bar );
  m_filterEdit->setObjectName( QStringLiteral( "attrFilterEdit" ) );
  m_filterEdit->setPlaceholderText( QObject::tr( "包含文本…" ) );
  barLay->addWidget( m_filterEdit, 1 );
  m_filterColumn = new QComboBox( bar );
  m_filterColumn->setObjectName( QStringLiteral( "attrFilterColumn" ) );
  m_filterColumn->addItem( QObject::tr( "全列" ), -1 );
  if ( m_layer )
  {
    const QgsFields fields = m_layer->fields();
    for ( int i = 0; i < fields.size(); ++i )
      m_filterColumn->addItem( fields.at( i ).name(), i );
  }
  barLay->addWidget( m_filterColumn );
  auto *filterBtn = new QPushButton( QObject::tr( "应用" ), bar );
  filterBtn->setObjectName( QStringLiteral( "attrFilterApply" ) );
  connect( filterBtn, &QPushButton::clicked, this, [this] {
    setFilter( m_filterColumn->currentData().toInt(), m_filterEdit->text() );
  } );
  barLay->addWidget( filterBtn );
  lay->addWidget( bar );

  m_table = new QTableWidget( this );
  m_table->setObjectName( QStringLiteral( "attrFullTable" ) );
  m_table->setEditTriggers( QAbstractItemView::NoEditTriggers );
  m_table->setSortingEnabled( true ); // D7.3 点列头排序
  if ( m_layer )
  {
    QStringList headers;
    const QgsFields fields = m_layer->fields();
    for ( int i = 0; i < fields.size(); ++i )
      headers << fields.at( i ).name();
    headers.prepend( QStringLiteral( "fid" ) );
    m_table->setColumnCount( headers.size() );
    m_table->setHorizontalHeaderLabels( headers );
  }
  m_table->horizontalHeader()->setStretchLastSection( true );
  // goal/ui-experience-polish：静默空表补空态指引（与「0 / 0」页码互补）。
  m_emptyLabel = new PaleoEmptyStateLabel(
      QObject::tr( "没有匹配的要素——清除过滤或换个过滤列" ), m_table );
  m_emptyLabel->setObjectName( QStringLiteral( "attrFullEmptyState" ) );
  lay->addWidget( m_table, 1 );

  auto *pageBar = new QWidget( this );
  auto *pageLay = new QHBoxLayout( pageBar );
  pageLay->setContentsMargins( 0, 0, 0, 0 );
  m_prevBtn = new QPushButton( QObject::tr( "上一页" ), pageBar );
  m_prevBtn->setObjectName( QStringLiteral( "attrPrevPage" ) );
  m_nextBtn = new QPushButton( QObject::tr( "下一页" ), pageBar );
  m_nextBtn->setObjectName( QStringLiteral( "attrNextPage" ) );
  m_pageLabel = new QLabel( pageBar );
  m_pageLabel->setFont( mono9() );
  connect( m_prevBtn, &QPushButton::clicked, this, [this] { goToPage( m_page - 1 ); } );
  connect( m_nextBtn, &QPushButton::clicked, this, [this] { goToPage( m_page + 1 ); } );
  pageLay->addStretch( 1 );
  pageLay->addWidget( m_prevBtn );
  pageLay->addWidget( m_pageLabel );
  pageLay->addWidget( m_nextBtn );
  pageLay->addStretch( 1 );
  lay->addWidget( pageBar );

  rebuild();
}

int PreviewAttributeTableDialog::pageCount() const
{
  const int total = filteredFeatureIds().size();
  return ( total + pageSize() - 1 ) / pageSize();
}

void PreviewAttributeTableDialog::goToPage( int page )
{
  const int pages = pageCount();
  m_page = pages > 0 ? qBound( 0, page, pages - 1 ) : 0;
  rebuild();
}

void PreviewAttributeTableDialog::setFilter( int column, const QString &text )
{
  m_filterColumnIdx = column;
  m_filterText = text;
  m_page = 0;
  rebuild();
}

void PreviewAttributeTableDialog::closeEvent( QCloseEvent *event )
{
  event->accept();
}

QStringList PreviewAttributeTableDialog::filteredFeatureIds() const
{
  QStringList out;
  if ( !m_layer || !m_layer->isValid() )
    return out;
  QgsFeatureRequest req;
  if ( !m_filterText.isEmpty() )
  {
    // 服务端表达式过滤（列包含）——全列时逐字段 OR。
    QStringList parts;
    if ( m_filterColumnIdx >= 0 )
      parts << QStringLiteral( "\"%1\" ILIKE '%%2%'" )
                   .arg( m_layer->fields().at( m_filterColumnIdx ).name(), m_filterText );
    else
    {
      for ( const QgsField &f : m_layer->fields() )
        parts << QStringLiteral( "\"%1\" ILIKE '%%2%'" ).arg( f.name(), m_filterText );
    }
    req.setFilterExpression( parts.join( QStringLiteral( " OR " ) ) );
  }
  QgsFeatureIterator it = m_layer->getFeatures( req );
  QgsFeature f;
  while ( it.nextFeature( f ) )
    out.append( QString::number( f.id() ) );
  return out;
}

void PreviewAttributeTableDialog::rebuild()
{
  m_table->setSortingEnabled( false );
  m_table->setRowCount( 0 );
  const QStringList ids = filteredFeatureIds();
  const int from = m_page * pageSize();
  const int to = qMin( ids.size(), from + pageSize() );
  for ( int i = from; i < to; ++i )
  {
    QgsFeature f;
    if ( !m_layer->getFeatures( QgsFeatureRequest( ids.at( i ).toLongLong() ) ).nextFeature( f ) )
      continue;
    const int row = m_table->rowCount();
    m_table->insertRow( row );
    auto *fidItem = new QTableWidgetItem( ids.at( i ) );
    fidItem->setFont( mono9() );
    m_table->setItem( row, 0, fidItem );
    const QgsAttributes attrs = f.attributes();
    const int nCols = m_table->columnCount();
    for ( int c = 1; c < nCols; ++c )
    {
      const int fieldIdx = c - 1;
      QString text;
      if ( fieldIdx < attrs.size() )
      {
        const QVariant &v = attrs.at( fieldIdx );
        text = v.type() == QVariant::Double ? QString::number( v.toDouble(), 'f', 4 )
                                            : v.toString();
      }
      auto *item = new QTableWidgetItem( text );
      if ( fieldIdx >= 0 && fieldIdx < m_layer->fields().size() &&
           m_layer->fields().at( fieldIdx ).type() == QVariant::Double )
        item->setFont( mono9() );
      m_table->setItem( row, c, item );
    }
  }
  m_table->setSortingEnabled( true );
  const int pages = pageCount();
  m_pageLabel->setText( pages > 0 ? QStringLiteral( "%1 / %2" ).arg( m_page + 1 ).arg( pages )
                                  : QStringLiteral( "0 / 0" ) );
  if ( m_emptyLabel )
    m_emptyLabel->setVisible( filteredFeatureIds().isEmpty() );
  m_prevBtn->setEnabled( m_page > 0 );
  m_nextBtn->setEnabled( m_page + 1 < pages );
}
