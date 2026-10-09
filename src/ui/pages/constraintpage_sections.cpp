// 层：视图
// constraintpage_sections — 页尾段域 TU（方向 96 自 constraintpage.cpp 拆出，
// 原文行序保持）：兼容 IDW 段、状态行、厚度样本段（阶段C §5C）、不确定性
// 集合段（方向 47），及 workflow 反馈接线与输入陈旧接线。
#include "constraintpage.h"
#include "constraintpage_internal.h"

#include "pageshared.h"

#include "../../domain/arearules.h" // 历史存量 include（勿增新 io include）
#include "../../domain/mappinghorizons.h"
#include "../../workflow/workflows.h"
#include "../realization/realizationpanel.h" // 方向 47：集合查看面（视图同层）

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

using namespace paleo::pagesinternal;
using namespace paleo::constraintpage_internal;

void ConstraintPage::buildTailSections( QWidget *content, QVBoxLayout *lay )
{
  // 旧 IDW 行（objectName 保留；runIdwRequested 原语义不动）。
  lay->addSpacing(PaleoTheme::tokens().spacingMd); // spacing.md：约束区与 IDW 区分组
  auto *legacyCaption = caption( tr( "插值（IDW）" ), content );
  legacyCaption->setObjectName( QStringLiteral( "factorLegacyIdwCaption" ) );
  lay->addWidget( legacyCaption );
  auto *idwField = new QLineEdit( QStringLiteral( "z" ), this );
  idwField->setObjectName( QStringLiteral( "idwField" ) );
  idwField->setPlaceholderText( tr( "井属性字段" ) );
  idwField->setAccessibleName( tr( "插值字段" ) );
  lay->addWidget( idwField );

  auto *idwCell = new QDoubleSpinBox( this );
  idwCell->setObjectName( QStringLiteral( "idwCellSize" ) );
  idwCell->setRange( 0.0001, 1.0e9 );
  idwCell->setDecimals( 4 );
  idwCell->setValue( 1.0 );
  idwCell->setAccessibleName( tr( "像元大小" ) );
  lay->addWidget( idwCell );

  auto *idw = new QPushButton( tr( "插值" ), this );
  idw->setObjectName( QStringLiteral( "runIdwButton" ) );
  lay->addWidget( idw );
  connect( idw, &QPushButton::clicked, this, [this] {
    auto *horizons = findChild<QComboBox *>( QStringLiteral( "horizonCombo" ) );
    emit runIdwRequested( horizons->currentText() );
  } );
  auto *legacySection = new CollapsibleSection(tr("兼容入口：原始井字段 IDW"), content);
  legacySection->setObjectName(QStringLiteral("factorLegacySection"));
  legacySection->setExpanded(false);
  for (QWidget *widget : QVector<QWidget *>{legacyCaption, idwField, idwCell, idw}) {
    lay->removeWidget(widget); legacySection->containerLayout()->addWidget(widget);
  }
  lay->addWidget(legacySection);

  auto *status = new QLabel( this );
  status->setObjectName( QStringLiteral( "statusLabel" ) );
  status->setWordWrap( true );
  lay->addWidget( status );

  // ---- 阶段C 厚度样本表（autoplan §5C）-------------------------------------
  // 逐井：井名 / D61 TVD / D62 TVD / 层间速度或原因。行表由 MappingWorkflow
  // typed 镜像到 ConstraintWorkflow::setThicknessSamples；不足样本的两句
  // （「厚度样本不足以成面」/「没有厚度样本」）渲染在 thicknessHint，不弹框。
  // m2(B)：整段挪进 CollapsibleSection（objectName 全保留，默认展开）。
  lay->addSpacing(PaleoTheme::tokens().spacingMd); // spacing.md
  // 厚度样本的层位/基面名随工程参数（AreaRules targetHorizon + 有序集合的
  // 下一界面）——文案在工程打开时由 refreshAreaParamLabels 重写。
  auto *section = new CollapsibleSection( tr( "厚度样本" ), this );
  section->setObjectName( QStringLiteral( "thicknessSection" ) );
  section->setExpanded( true );
  lay->addWidget( section, 1 );
  auto *thLay = section->containerLayout();
  auto *thCap = caption(
      tr( "%1→%2 厚度样本" )
          .arg( AreaRules::active().targetHorizon,
                baseHorizonFor( AreaRules::active().targetHorizon ) ),
      section->container() );
  thCap->setObjectName( QStringLiteral( "thicknessCaption" ) );
  thLay->addWidget( thCap );
  auto *thTable = new QTableWidget( 0, 4, section->container() );
  thTable->setObjectName( QStringLiteral( "thicknessTable" ) );
  thTable->setAccessibleName( tr( "厚度样本表" ) );
  thTable->setHorizontalHeaderLabels(
      { tr( "井名" ), tr( "%1 TVD" ).arg( AreaRules::active().targetHorizon ),
        tr( "%1 TVD" ).arg( baseHorizonFor( AreaRules::active().targetHorizon ) ),
        tr( "层间速度或原因" ) } );
  thTable->verticalHeader()->setVisible( false );
  thTable->horizontalHeader()->setStretchLastSection( true );
  thLay->addWidget( thTable, 1 );
  auto *thHint = new QLabel( section->container() );
  thHint->setObjectName( QStringLiteral( "thicknessHint" ) );
  thHint->setWordWrap( true );
  // text-muted 活体（随主题重算）。
  PaleoTheme::applyThemedStyleSheet(
      thHint, [] { return PaleoTheme::mutedCaptionStyleSheet(); } );
  thLay->addWidget( thHint );

  // ---- 方向 47：realization 集合查看面 -------------------------------------
  // SGS「保留实现集合」勾选后的产物面：成员切换/统计面/集合差值/动画帧全是
  // intent 信号，catalog 换绑与上图编排由壳侧 attachConstraintPage 接
  // RealizationWorkflow。默认收起——副产物面不抢单因素主流程的视觉权重。
  lay->addSpacing( PaleoTheme::tokens().spacingMd );
  auto *rsSection = new CollapsibleSection( tr( "不确定性集合" ), this );
  rsSection->setObjectName( QStringLiteral( "realizationSection" ) );
  rsSection->setExpanded( false );
  lay->addWidget( rsSection );
  auto *rsPanel = new RealizationPanel( rsSection->container() );
  rsPanel->setObjectName( QStringLiteral( "realizationPanel" ) );
  rsSection->containerLayout()->addWidget( rsPanel );
}

void ConstraintPage::wireWorkflowFeedback( ConstraintWorkflow *wf )
{
  auto *status = findChild<QLabel *>( QStringLiteral( "statusLabel" ) );
  if ( wf ) // workflow feedback lands on the status label
  {
    setProperty( kWfProp, QVariant::fromValue( static_cast<QObject *>( wf ) ) );
    connect( wf, &ConstraintWorkflow::constraintAdded, status, [this, status]( const QString &id ) {
      status->setText( tr( "已添加约束 %1" ).arg( id ) );
      refreshConstraintList();
    } );
    connect( wf, &ConstraintWorkflow::constraintLineUpdated, this, [this]( const QString & ) {
      refreshConstraintList();
    } );
    connect( wf, &ConstraintWorkflow::constraintRemoved, this, [this, status]( const QString &id ) {
      status->setText( tr( "已删除约束 %1" ).arg( id ) );
      refreshConstraintList();
    } );
    connect( wf, &ConstraintWorkflow::factorDone, status,
             [status]( const QString &h, const QString &layerId ) {
               status->setText( tr( "单因素完成：%1 → %2" ).arg( h, layerId ) );
             } );
    // m2(B)：新生成链回执 → 状态列/状态行刷新。
    connect( wf, &ConstraintWorkflow::factorGenerated, status,
             [this, status]( const QString &h, const QString &fid, const QString &layerId ) {
               noteFactorLayer( fid, layerId );
               status->setText( tr( "单因素完成：%1 %2 → %3" ).arg( h, fid, layerId ) );
             } );
    connect( wf, &ConstraintWorkflow::contoursGenerated, status,
             [status]( const QString &h, const QString &, const QString &layerId ) {
               status->setText( tr( "等值线完成：%1 → %2" ).arg( h, layerId ) );
             } );
    connect( wf, &ConstraintWorkflow::cartographicWorkGenerated, status,
             [status]( const QString &h, const QString &, const QString &layerId ) {
               status->setText( tr( "制图工作场完成：%1 → %2" ).arg( h, layerId ) );
             } );
    connect( wf, &ConstraintWorkflow::interpretiveContoursGenerated, status,
             [status]( const QString &h, const QString &, const QString &layerId ) {
               status->setText( tr( "解释性等值线完成：%1 → %2" ).arg( h, layerId ) );
             } );
  }
}

void ConstraintPage::wireInputStaleness()
{
  const auto mark = [this] { markInputsStale(); };
  if ( auto *edit = child<QLineEdit>( this, "factorFieldEdit" ) )
    connect( edit, &QLineEdit::textEdited, this, mark );
  if ( auto *spinBox = child<QDoubleSpinBox>( this, "factorCellSizeSpin" ) )
    connect( spinBox, qOverload<double>( &QDoubleSpinBox::valueChanged ), this, [mark]( double ) { mark(); } );
  if ( auto *combo = child<QComboBox>( this, "factorMethodCombo" ) )
    connect( combo, qOverload<int>( &QComboBox::currentIndexChanged ), this,
             [this, mark]( int ) {
               mark();
               // 结构 IDW 默认铺满测区边界（上游默认）——联动覆盖选项。
               auto *method = child<QComboBox>( this, "factorMethodCombo" );
               auto *coverage = child<QComboBox>( this, "factorCoverageCombo" );
               if ( method && coverage
                    && method->currentData().toString()
                           == QLatin1String( "structural_idw" ) )
               {
                 const int idx = coverage->findData(
                     QStringLiteral( "domain_extrapolation" ) );
                 if ( idx >= 0 )
                   coverage->setCurrentIndex( idx );
               }
               updateEngineRows();
               updateFactorActionStates();
             } );
  if ( auto *combo = child<QComboBox>( this, "factorCoverageCombo" ) )
    connect( combo, qOverload<int>( &QComboBox::currentIndexChanged ), this, [mark]( int ) { mark(); } );
  if ( auto *combo = child<QComboBox>( this, "factorBoundaryCombo" ) )
    connect( combo, qOverload<int>( &QComboBox::currentIndexChanged ), this,
             [this, mark]( int ) {
               mark();
               updateFactorActionStates(); // 边界必选门随选择刷新
             } );
  if ( auto *spin = child<QSpinBox>( this, "factorGridResolutionSpin" ) )
    connect( spin, qOverload<int>( &QSpinBox::valueChanged ), this,
             [mark]( int ) { mark(); } );
  if ( auto *horizons = child<QComboBox>( this, "horizonCombo" ) )
    connect( horizons, qOverload<int>( &QComboBox::currentIndexChanged ), this, [this]( int ) { refreshConstraintList(); } );
}
