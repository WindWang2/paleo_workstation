// 层：视图
#include "mapbookpanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTextEdit>
#include <QVBoxLayout>

namespace
{
  // 面板不带样式（DESIGN.md token 单一真源 = paleotheme），只在文案上区分。
  QString orderText( PaleoMapBook::Order order )
  {
    return order == PaleoMapBook::Order::RowMajor ? QObject::tr( "行主序" )
                                                  : QObject::tr( "列主序" );
  }
} // namespace

PaleoMapBookPanel::PaleoMapBookPanel( QWidget *parent )
  : QWidget( parent )
{
  buildUi();
}

void PaleoMapBookPanel::buildUi()
{
  auto *form = new QFormLayout;
  form->setLabelAlignment( Qt::AlignRight );

  m_book = new QLineEdit( QStringLiteral( "mapbook" ), this );
  m_book->setObjectName( QStringLiteral( "mapbookBook" ) );
  form->addRow( tr( "图册名" ), m_book );

  auto *dirRow = new QHBoxLayout;
  m_dir = new QLineEdit( this );
  m_dir->setObjectName( QStringLiteral( "mapbookOutputDir" ) );
  auto *pick = new QPushButton( tr( "浏览…" ), this );
  pick->setObjectName( QStringLiteral( "mapbookPickDir" ) );
  dirRow->addWidget( m_dir, 1 );
  dirRow->addWidget( pick );
  form->addRow( tr( "输出目录" ), dirRow );

  auto *areaRow = new QHBoxLayout;
  m_xMin = new QDoubleSpinBox( this );
  m_yMin = new QDoubleSpinBox( this );
  m_xMax = new QDoubleSpinBox( this );
  m_yMax = new QDoubleSpinBox( this );
  for ( QDoubleSpinBox *box : { m_xMin, m_yMin, m_xMax, m_yMax } )
  {
    box->setRange( -1.0e9, 1.0e9 );
    box->setDecimals( 2 );
    box->setSingleStep( 100.0 );
    areaRow->addWidget( box );
  }
  m_xMin->setObjectName( QStringLiteral( "mapbookXMin" ) );
  m_yMin->setObjectName( QStringLiteral( "mapbookYMin" ) );
  m_xMax->setObjectName( QStringLiteral( "mapbookXMax" ) );
  m_yMax->setObjectName( QStringLiteral( "mapbookYMax" ) );
  m_xMax->setValue( 3000.0 );
  m_yMax->setValue( 3000.0 );
  form->addRow( tr( "AOI 范围（x0 y0 x1 y1）" ), areaRow );

  auto *gridRow = new QHBoxLayout;
  m_cols = new QSpinBox( this );
  m_rows = new QSpinBox( this );
  for ( QSpinBox *box : { m_cols, m_rows } )
  {
    box->setRange( 1, 99 );
    box->setValue( 3 );
    gridRow->addWidget( box );
  }
  m_cols->setObjectName( QStringLiteral( "mapbookCols" ) );
  m_rows->setObjectName( QStringLiteral( "mapbookRows" ) );
  form->addRow( tr( "网格（列 × 行）" ), gridRow );

  m_order = new QComboBox( this );
  m_order->setObjectName( QStringLiteral( "mapbookOrder" ) );
  m_order->addItem( orderText( PaleoMapBook::Order::RowMajor ),
                    static_cast<int>( PaleoMapBook::Order::RowMajor ) );
  m_order->addItem( orderText( PaleoMapBook::Order::ColumnMajor ),
                    static_cast<int>( PaleoMapBook::Order::ColumnMajor ) );
  form->addRow( tr( "遍历次序" ), m_order );

  m_format = new QComboBox( this );
  m_format->setObjectName( QStringLiteral( "mapbookFormat" ) );
  m_format->addItem( QStringLiteral( "PNG" ),
                     static_cast<int>( PaleoMapBookQueue::Format::Png ) );
  m_format->addItem( QStringLiteral( "PDF" ),
                     static_cast<int>( PaleoMapBookQueue::Format::Pdf ) );
  form->addRow( tr( "导出格式" ), m_format );

  m_dpi = new QComboBox( this );
  m_dpi->setObjectName( QStringLiteral( "mapbookDpi" ) );
  for ( const int dpi : { 150, 300, 600 } )
    m_dpi->addItem( tr( "%1 dpi" ).arg( dpi ), dpi );
  m_dpi->setCurrentIndex( 1 );
  form->addRow( tr( "分辨率" ), m_dpi );

  m_title = new QLineEdit( QStringLiteral( "%{book} · %{tile_label}" ), this );
  m_title->setObjectName( QStringLiteral( "mapbookTitlePattern" ) );
  form->addRow( tr( "标题模板" ), m_title );

  m_footer = new QLineEdit(
    QStringLiteral( "%{tile_label} · 中心 %{center_x}, %{center_y} · %{extent} · %{crs}" ), this );
  m_footer->setObjectName( QStringLiteral( "mapbookFooterPattern" ) );
  form->addRow( tr( "页脚模板" ), m_footer );

  m_index = new QCheckBox( tr( "出目录索引页" ), this );
  m_index->setObjectName( QStringLiteral( "mapbookIndexPage" ) );
  m_index->setChecked( true );
  form->addRow( QString(), m_index );

  m_preview = new QSpinBox( this );
  m_preview->setObjectName( QStringLiteral( "mapbookPreviewLimit" ) );
  m_preview->setRange( 0, 999 );
  m_preview->setSpecialValueText( tr( "全部" ) );
  form->addRow( tr( "预演版数" ), m_preview );

  auto *buttons = new QHBoxLayout;
  m_start = new QPushButton( tr( "开始批量导出" ), this );
  m_start->setObjectName( QStringLiteral( "mapbookStart" ) );
  m_cancel = new QPushButton( tr( "取消" ), this );
  m_cancel->setObjectName( QStringLiteral( "mapbookCancel" ) );
  m_cancel->setEnabled( false );
  buttons->addWidget( m_start );
  buttons->addWidget( m_cancel );

  m_status = new QLabel( tr( "待命" ), this );
  m_status->setObjectName( QStringLiteral( "mapbookStatus" ) );
  m_progress = new QProgressBar( this );
  m_progress->setObjectName( QStringLiteral( "mapbookProgress" ) );
  m_progress->setRange( 0, 1 );
  m_progress->setValue( 0 );
  m_progress->setTextVisible( true );

  m_log = new QTextEdit( this );
  m_log->setObjectName( QStringLiteral( "mapbookLog" ) );
  m_log->setReadOnly( true );
  m_log->setPlaceholderText( tr( "逐版结果与失败明细" ) );

  auto *root = new QVBoxLayout( this );
  root->addLayout( form );
  root->addLayout( buttons );
  root->addWidget( m_status );
  root->addWidget( m_progress );
  root->addWidget( m_log, 1 );

  connect( m_start, &QPushButton::clicked, this, &PaleoMapBookPanel::onStart );
  connect( m_cancel, &QPushButton::clicked, this, &PaleoMapBookPanel::onCancel );
  connect( pick, &QPushButton::clicked, this, &PaleoMapBookPanel::onPickDir );
}

PaleoMapBook::Area PaleoMapBookPanel::area() const
{
  PaleoMapBook::Area a;
  a.xMin = m_xMin->value();
  a.yMin = m_yMin->value();
  a.xMax = m_xMax->value();
  a.yMax = m_yMax->value();
  return a;
}

void PaleoMapBookPanel::setArea( const PaleoMapBook::Area &area )
{
  m_xMin->setValue( area.xMin );
  m_yMin->setValue( area.yMin );
  m_xMax->setValue( area.xMax );
  m_yMax->setValue( area.yMax );
}

int PaleoMapBookPanel::cols() const { return m_cols->value(); }

int PaleoMapBookPanel::rows() const { return m_rows->value(); }

void PaleoMapBookPanel::setGrid( int cols, int rows )
{
  m_cols->setValue( cols );
  m_rows->setValue( rows );
}

PaleoMapBook::Order PaleoMapBookPanel::order() const
{
  return static_cast<PaleoMapBook::Order>( m_order->currentData().toInt() );
}

QString PaleoMapBookPanel::bookName() const { return m_book->text().trimmed(); }

void PaleoMapBookPanel::setBookName( const QString &name ) { m_book->setText( name ); }

QString PaleoMapBookPanel::outputDir() const { return m_dir->text().trimmed(); }

void PaleoMapBookPanel::setOutputDir( const QString &dir ) { m_dir->setText( dir ); }

QString PaleoMapBookPanel::titlePattern() const { return m_title->text(); }

QString PaleoMapBookPanel::footerPattern() const { return m_footer->text(); }

double PaleoMapBookPanel::dpi() const { return m_dpi->currentData().toDouble(); }

PaleoMapBookQueue::Format PaleoMapBookPanel::format() const
{
  return static_cast<PaleoMapBookQueue::Format>( m_format->currentData().toInt() );
}

bool PaleoMapBookPanel::withIndexPage() const { return m_index->isChecked(); }

// 0 = 特殊值「全部」→ 队列口径的 -1（≥0 表示只出前 N 版，0 会被当成一版不出）。
int PaleoMapBookPanel::previewLimit() const
{
  const int value = m_preview->value();
  return value <= 0 ? -1 : value;
}

QVector<PaleoMapBook::Tile> PaleoMapBookPanel::previewTiles( QString *error ) const
{
  PaleoMapBook::GridRequest request;
  request.area = area();
  request.cols = cols();
  request.rows = rows();
  request.order = order();
  return PaleoMapBook::buildGrid( request, error );
}

void PaleoMapBookPanel::setBusy( bool busy )
{
  m_start->setEnabled( !busy );
  m_cancel->setEnabled( busy );
  m_status->setText( busy ? tr( "批量导出中…" ) : tr( "待命" ) );
  if ( busy )
  {
    m_progress->setRange( 0, 0 ); // 跑马灯：总量未知前
    clearLog();
  }
  else
  {
    m_progress->setRange( 0, 1 );
    m_progress->setValue( 0 );
  }
}

void PaleoMapBookPanel::setProgress( int done, int total )
{
  if ( total <= 0 )
  {
    m_progress->setRange( 0, 0 );
    return;
  }
  m_progress->setRange( 0, total );
  m_progress->setValue( done );
  m_status->setText( tr( "%1 / %2 版" ).arg( done ).arg( total ) );
}

void PaleoMapBookPanel::appendLog( const QString &line ) { m_log->append( line ); }

void PaleoMapBookPanel::clearLog() { m_log->clear(); }

void PaleoMapBookPanel::onStart()
{
  QString error;
  const QVector<PaleoMapBook::Tile> tiles = previewTiles( &error );
  if ( tiles.isEmpty() )
  {
    appendLog( tr( "参数不可用：%1" ).arg( error.isEmpty() ? tr( "格序列为空" ) : error ) );
    return;
  }
  if ( outputDir().isEmpty() || bookName().isEmpty() )
  {
    appendLog( tr( "参数不可用：图册名与输出目录都要填" ) );
    return;
  }
  appendLog( tr( "请求批量导出：%1 版" ).arg( tiles.size() ) );
  emit batchRequested();
}

void PaleoMapBookPanel::onCancel() { emit cancelRequested(); }

void PaleoMapBookPanel::onPickDir()
{
  const QString dir = QFileDialog::getExistingDirectory( this, tr( "选择地图册输出目录" ),
                                                          outputDir() );
  if ( !dir.isEmpty() )
    setOutputDir( dir );
}
