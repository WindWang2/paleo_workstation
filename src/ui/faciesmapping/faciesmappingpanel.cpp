// 层：视图
#include "faciesmappingpanel.h"

#include "../paleotheme.h"

#include <QFont>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

FaciesMappingPanel::FaciesMappingPanel( QWidget *parent )
  : QWidget( parent )
{
  buildUi();
  syncEnabledState();
}

void FaciesMappingPanel::buildUi()
{
  auto *lay = new QVBoxLayout( this );
  lay->setContentsMargins( PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm );
  lay->setSpacing( PaleoTheme::tokens().spacingSm );

  auto *form = new QFormLayout();
  form->setSpacing( PaleoTheme::tokens().spacingSm );
  m_horizon = new QLineEdit();
  m_horizon->setObjectName( QStringLiteral( "faciesHorizonEdit" ) );
  m_horizon->setPlaceholderText( tr( "目标层位，如 D61" ) );
  form->addRow( tr( "层位" ), m_horizon );

  m_wellWeight = new QDoubleSpinBox();
  m_wellWeight->setObjectName( QStringLiteral( "faciesWellWeight" ) );
  m_wellWeight->setRange( 0.0, 10.0 );
  m_wellWeight->setDecimals( 2 );
  m_wellWeight->setSingleStep( 0.1 );
  m_wellWeight->setValue( 1.0 );
  m_wellWeight->setToolTip( tr( "井点优势相证据的票权（0 = 不参与合成）。" ) );
  form->addRow( tr( "井相权重" ), m_wellWeight );

  m_factorWeight = new QDoubleSpinBox();
  m_factorWeight->setObjectName( QStringLiteral( "faciesFactorWeight" ) );
  m_factorWeight->setRange( 0.0, 10.0 );
  m_factorWeight->setDecimals( 2 );
  m_factorWeight->setSingleStep( 0.1 );
  m_factorWeight->setValue( 1.0 );
  m_factorWeight->setToolTip( tr( "单因素图采样证据的票权（0 = 不参与合成）。" ) );
  form->addRow( tr( "单因素权重" ), m_factorWeight );

  m_predictionWeight = new QDoubleSpinBox();
  m_predictionWeight->setObjectName( QStringLiteral( "faciesPredictionWeight" ) );
  m_predictionWeight->setRange( 0.0, 10.0 );
  m_predictionWeight->setDecimals( 2 );
  m_predictionWeight->setSingleStep( 0.1 );
  m_predictionWeight->setValue( 1.0 );
  m_predictionWeight->setToolTip( tr( "远程井道预测证据的票权（0 = 不参与合成）。" ) );
  form->addRow( tr( "预测相权重" ), m_predictionWeight );

  m_assignThreshold = new QDoubleSpinBox();
  m_assignThreshold->setObjectName( QStringLiteral( "faciesAssignThreshold" ) );
  m_assignThreshold->setRange( 0.0, 1.0 );
  m_assignThreshold->setDecimals( 2 );
  m_assignThreshold->setSingleStep( 0.05 );
  m_assignThreshold->setValue( 0.5 );
  m_assignThreshold->setToolTip( tr( "加权得分份额低于该阈值时不赋相（如实留未定）。" ) );
  form->addRow( tr( "赋相阈值" ), m_assignThreshold );

  m_minRegionArea = new QDoubleSpinBox();
  m_minRegionArea->setObjectName( QStringLiteral( "faciesMinRegionArea" ) );
  m_minRegionArea->setRange( 0.0, 1000000.0 );
  m_minRegionArea->setDecimals( 1 );
  m_minRegionArea->setValue( 0.0 );
  m_minRegionArea->setToolTip( tr( "面积小于该值的候选碎片直接丢弃（0 = 不丢弃）。" ) );
  form->addRow( tr( "最小单元面积" ), m_minRegionArea );

  m_minIslandArea = new QDoubleSpinBox();
  m_minIslandArea->setObjectName( QStringLiteral( "faciesMinIslandArea" ) );
  m_minIslandArea->setRange( 0.0, 1000000.0 );
  m_minIslandArea->setDecimals( 1 );
  m_minIslandArea->setValue( 0.0 );
  m_minIslandArea->setToolTip( tr( "孤岛检测阈值，面积小于该值的单元报 QA（0 = 关闭检测）。" ) );
  form->addRow( tr( "孤岛阈值" ), m_minIslandArea );

  m_coverageRadius = new QDoubleSpinBox();
  m_coverageRadius->setObjectName( QStringLiteral( "faciesCoverageRadius" ) );
  m_coverageRadius->setRange( 0.0, 1000000.0 );
  m_coverageRadius->setDecimals( 1 );
  m_coverageRadius->setValue( 0.0 );
  m_coverageRadius->setToolTip( tr( "缺井覆盖检测的缓冲半径（0 = 井点必须在单元内）。" ) );
  form->addRow( tr( "井控半径" ), m_coverageRadius );
  lay->addLayout( form );

  auto *row = new QHBoxLayout();
  row->setSpacing( PaleoTheme::tokens().spacingSm );
  m_generate = new QToolButton( this );
  m_generate->setObjectName( QStringLiteral( "faciesGenerateButton" ) );
  m_generate->setText( tr( "生成草稿相图" ) );
  m_generate->setToolTip(
    tr( "按当前权重/阈值发一键成图意图：优势相→相界→合成→QA→草稿图层。计算在功能层。" ) );
  {
    PaleoTheme::applyThemedStyleSheet( m_generate, [] {
      return PaleoTheme::toolButtonStyleSheet() +
          QStringLiteral( "QToolButton { color: %1; } QToolButton:disabled { color: %2; }" )
              .arg( PaleoTheme::tokens().primaryText.name(),
                    PaleoTheme::tokens().textDisabled.name() );
    } );
    QFont font = m_generate->font();
    font.setWeight( QFont::Medium );
    m_generate->setFont( font );
  }
  m_cancel = new QToolButton( this );
  m_cancel->setObjectName( QStringLiteral( "faciesCancelButton" ) );
  m_cancel->setText( tr( "取消" ) );
  m_cancel->setToolTip( tr( "取消在途生成。未落盘的草稿不登记。" ) );
  row->addWidget( m_generate );
  row->addWidget( m_cancel );
  row->addStretch();
  lay->addLayout( row );

  m_progress = new QProgressBar();
  m_progress->setObjectName( QStringLiteral( "faciesProgress" ) );
  m_progress->setRange( 0, 100 );
  m_progress->setValue( 0 );
  m_progress->setTextVisible( true );
  lay->addWidget( m_progress );

  m_status = new QLabel( tr( "填层位并调整权重后生成" ) );
  m_status->setObjectName( QStringLiteral( "faciesStatus" ) );
  m_status->setWordWrap( true );
  lay->addWidget( m_status );

  auto *qaCaption = new QLabel( tr( "编图 QA 报告" ) );
  qaCaption->setObjectName( QStringLiteral( "faciesQaCaption" ) );
  lay->addWidget( qaCaption );
  m_qaTable = new QTableWidget( 0, 4, this );
  m_qaTable->setObjectName( QStringLiteral( "faciesQaTable" ) );
  m_qaTable->setHorizontalHeaderLabels(
    { tr( "类型" ), tr( "单元" ), tr( "度量" ), tr( "相关" ) } );
  m_qaTable->horizontalHeader()->setSectionResizeMode( 0, QHeaderView::ResizeToContents );
  m_qaTable->horizontalHeader()->setStretchLastSection( true );
  m_qaTable->setSelectionBehavior( QAbstractItemView::SelectRows );
  m_qaTable->setEditTriggers( QAbstractItemView::NoEditTriggers );
  lay->addWidget( m_qaTable, 1 );
  m_qaSummary = new QLabel( tr( "尚无报告" ) );
  m_qaSummary->setObjectName( QStringLiteral( "faciesQaSummary" ) );
  lay->addWidget( m_qaSummary );

  connect( m_generate, &QToolButton::clicked, this, [this]() {
    const SourceWeights w = weights();
    emit generateRequested( m_horizon->text().trimmed(), w.well, w.factor, w.prediction,
                            assignThreshold(), minRegionArea(), minIslandArea(),
                            wellCoverageRadius() );
  } );
  connect( m_cancel, &QToolButton::clicked, this, &FaciesMappingPanel::cancelRequested );
  connect( m_qaTable, &QTableWidget::cellClicked, this, [this]( int row, int ) {
    const QTableWidgetItem *idItem = m_qaTable->item( row, 1 );
    if ( !idItem )
      return;
    const QTableWidgetItem *xItem = m_qaTable->item( row, 4 );
    const QTableWidgetItem *yItem = m_qaTable->item( row, 5 );
    if ( !xItem || !yItem )
      return;
    emit issueSelected( idItem->text(), xItem->text().toDouble(), yItem->text().toDouble() );
  } );
}

FaciesMappingPanel::SourceWeights FaciesMappingPanel::weights() const
{
  SourceWeights w;
  w.well = m_wellWeight->value();
  w.factor = m_factorWeight->value();
  w.prediction = m_predictionWeight->value();
  return w;
}

double FaciesMappingPanel::assignThreshold() const
{
  return m_assignThreshold->value();
}

double FaciesMappingPanel::minIslandArea() const
{
  return m_minIslandArea->value();
}

double FaciesMappingPanel::wellCoverageRadius() const
{
  return m_coverageRadius->value();
}

double FaciesMappingPanel::minRegionArea() const
{
  return m_minRegionArea->value();
}

void FaciesMappingPanel::setQaRows( const QList<QaRow> &rows )
{
  m_qaTable->setRowCount( 0 );
  // 隐匿定位列（4/5）：x/y 存纯文本供点击定位，不参与显示。
  m_qaTable->setColumnCount( 6 );
  for ( const QaRow &row : rows )
    addQaRow( row );
  m_qaSummary->setText( rows.isEmpty() ? tr( "无 QA 问题" )
                                       : tr( "共 %1 条问题，点击行定位" ).arg( rows.size() ) );
}

int FaciesMappingPanel::addQaRow( const QaRow &row )
{
  const int r = m_qaTable->rowCount();
  m_qaTable->insertRow( r );
  // 类型 token（算法层稳定标识）→ 本地化文案；token 原文挂 tooltip 供核对。
  QString typeText = row.type;
  if ( row.type == QStringLiteral( "unclosed_ring" ) )
    typeText = tr( "环未闭合" );
  else if ( row.type == QStringLiteral( "overlap" ) )
    typeText = tr( "单元重叠" );
  else if ( row.type == QStringLiteral( "small_island" ) )
    typeText = tr( "孤岛小面" );
  else if ( row.type == QStringLiteral( "constraint_conflict" ) )
    typeText = tr( "穿越硬约束" );
  else if ( row.type == QStringLiteral( "no_well_coverage" ) )
    typeText = tr( "缺井覆盖" );
  auto *typeItem = new QTableWidgetItem( typeText );
  typeItem->setToolTip( row.type );
  m_qaTable->setItem( r, 0, typeItem );
  m_qaTable->setItem( r, 1, new QTableWidgetItem( row.regionIds ) );
  m_qaTable->setItem( r, 2, new QTableWidgetItem( QString::number( row.metric, 'g', 6 ) ) );
  m_qaTable->setItem( r, 3, new QTableWidgetItem( row.related ) );
  m_qaTable->setItem( r, 4, new QTableWidgetItem( QString::number( row.x, 'g', 12 ) ) );
  m_qaTable->setItem( r, 5, new QTableWidgetItem( QString::number( row.y, 'g', 12 ) ) );
  m_qaTable->hideColumn( 4 );
  m_qaTable->hideColumn( 5 );
  return r;
}

void FaciesMappingPanel::setBusy( bool busy )
{
  m_busy = busy;
  syncEnabledState();
  if ( busy )
    m_progress->setValue( 0 );
}

void FaciesMappingPanel::syncEnabledState()
{
  m_generate->setEnabled( !m_busy );
  m_cancel->setEnabled( m_busy );
}

void FaciesMappingPanel::setSummary( const QString &text )
{
  m_status->setText( text );
}

void FaciesMappingPanel::updateProgress( int percent, const QString &stageLabel )
{
  m_progress->setValue( percent );
  if ( !stageLabel.isEmpty() )
    m_status->setText( stageLabel );
}

void FaciesMappingPanel::showResult( bool ok, const QString &summary )
{
  m_busy = false;
  syncEnabledState();
  m_progress->setValue( ok ? 100 : m_progress->value() );
  m_status->setText( summary );
}
