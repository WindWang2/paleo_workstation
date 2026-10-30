// 层：视图
#include "subintervalselectorbar.h"

#include "../linkage/selectioncontext.h"
#include "../services/stratigraphicsubintervals.h"
#include "../services/subintervalfactors.h"
#include "paleoicons.h"
#include "paleotheme.h"

#include <QComboBox>
#include <QCompleter>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>
#include <QVBoxLayout>

using paleo::strat::UnitSelection;
using paleo::strat::kSubIntervalCount;

namespace {

void styleSubButton( QToolButton *btn )
{
  PaleoTheme::applyThemedStyleSheet( btn, [btn] {
    const PaleoTheme::ThemeTokens &t = PaleoTheme::tokens();
    return PaleoTheme::metricStyleSheet( QStringLiteral(
               "QToolButton { border-radius: %6px; padding: {spacing.xs}px {spacing.sm}px;"
               " background: %1; border: 1px solid %2; color: %3; }"
               "QToolButton:checked { background: %4; border-color: %4; color: %5; }"
               "QToolButton:hover:!checked { border-color: %4; }"
               "QToolButton:disabled { color: %7; }" ) )
        .arg( t.surface.name(), t.border.name(), t.text.name(), t.primary.name(),
              t.onPrimary.name() )
        .arg( PaleoTheme::chipRadius( btn->font() ) )
        .arg( t.textDisabled.name() );
  } );
}

QString shortRevision( const QString &rev )
{
  return rev.size() > 10 ? rev.left( 8 ) + QStringLiteral( "…" ) : rev;
}

void enableSearchableCombo( QComboBox *box )
{
  if ( !box )
    return;
  box->setEditable( true );
  box->setInsertPolicy( QComboBox::NoInsert );
  if ( QCompleter *c = box->completer() )
  {
    c->setCaseSensitivity( Qt::CaseInsensitive );
    c->setFilterMode( Qt::MatchContains );
    c->setCompletionMode( QCompleter::PopupCompletion );
  }
  if ( QLineEdit *edit = box->lineEdit() )
    edit->setClearButtonEnabled( true );
}

QToolButton *makePageButton( QWidget *parent, const QString &objectName, const QString &iconName,
                             const QString &tip )
{
  auto *btn = new QToolButton( parent );
  btn->setObjectName( objectName );
  btn->setAutoRaise( true );
  btn->setIcon( PaleoIcons::qgisTheme( iconName ) );
  btn->setIconSize( PaleoIcons::toolbarSize() );
  btn->setToolTip( tip );
  btn->setVisible( false );
  return btn;
}

QLabel *makeIconLabel( QWidget *parent, const QString &objectName, const QString &iconName,
                       const QString &tip )
{
  auto *lab = new QLabel( parent );
  lab->setObjectName( objectName );
  lab->setToolTip( tip );
  if ( const QIcon ic = PaleoIcons::qgisTheme( iconName ); !ic.isNull() )
    lab->setPixmap( ic.pixmap( PaleoIcons::toolbarSize() ) );
  return lab;
}

} // namespace

SubIntervalSelectorBar::SubIntervalSelectorBar( SelectionContext *selection, QWidget *parent )
  : QWidget( parent )
  , m_selection( selection )
{
  setObjectName( QStringLiteral( "subIntervalSelectorBar" ) );
  setAccessibleName( tr( "层段分析选择" ) );

  auto *root = new QVBoxLayout( this );
  root->setContentsMargins( 0, 0, 0, 0 );
  root->setSpacing( PaleoTheme::tokens().spacingXs );

  auto *row = new QWidget( this );
  row->setObjectName( QStringLiteral( "subIntervalSelectorRow" ) );
  auto *lay = new QHBoxLayout( row );
  lay->setContentsMargins( 0, 0, 0, 0 );
  lay->setSpacing( PaleoTheme::tokens().spacingSm );

  m_modeLabel = new QLabel( tr( "层段分析" ), row );
  m_modeLabel->setObjectName( QStringLiteral( "subIntervalModeLabel" ) );
  m_modeLabel->setToolTip( tr( "层段分析模式：选择方案/修订、父单元与 S1–S4；"
                               "与上方「界面编辑」层位条独立，不切换地图图层或中断编辑。" ) );
  if ( const QIcon ic = PaleoIcons::qgisTheme( QStringLiteral( "mIconPolygonLayer.svg" ) ); !ic.isNull() )
  {
    m_modeLabel->setPixmap( ic.pixmap( PaleoIcons::toolbarSize() ) );
    m_modeLabel->setText( tr( " 层段分析" ) );
  }
  lay->addWidget( m_modeLabel );

  m_schemeIcon = makeIconLabel( row, QStringLiteral( "subIntervalSchemeIcon" ),
                                QStringLiteral( "mIconPolygonLayer.svg" ),
                                tr( "地层解释方案" ) );
  lay->addWidget( m_schemeIcon );
  m_schemeCombo = new QComboBox( row );
  m_schemeCombo->setObjectName( QStringLiteral( "subIntervalSchemeCombo" ) );
  m_schemeCombo->setAccessibleName( tr( "地层方案" ) );
  m_schemeCombo->setToolTip( tr( "地层解释方案。可输入关键字过滤；列表超过一页时可翻页。"
                                 "切换后分析选择写入该方案稳定 ID；无提交方案时显示工程草稿。" ) );
  m_schemeCombo->setWhatsThis( tr( "<b>地层方案</b><p>选择解释方案并写入 UnitSelection.schemeId。"
                                   "输入关键字可过滤；超多方案时分页浏览（每页 %1 条）。"
                                   "</p>" )
                                   .arg( paleo::strat::kSchemeRevisionPageSize ) );
  m_schemeCombo->setMinimumContentsLength( 8 );
  m_schemeCombo->setSizeAdjustPolicy( QComboBox::AdjustToContents );
  enableSearchableCombo( m_schemeCombo );
  connect( m_schemeCombo, QOverload<int>::of( &QComboBox::currentIndexChanged ), this,
           &SubIntervalSelectorBar::onSchemeChanged );
  if ( QLineEdit *edit = m_schemeCombo->lineEdit() )
    connect( edit, &QLineEdit::textEdited, this, &SubIntervalSelectorBar::onSchemeFilterEdited );
  lay->addWidget( m_schemeCombo );
  m_schemePrev = makePageButton( row, QStringLiteral( "subIntervalSchemePrev" ),
                                 QStringLiteral( "mActionArrowLeft.svg" ),
                                 tr( "上一页方案" ) );
  connect( m_schemePrev, &QToolButton::clicked, this, [this] {
    --m_schemePage;
    refillSchemeComboPage();
  } );
  lay->addWidget( m_schemePrev );
  m_schemeNext = makePageButton( row, QStringLiteral( "subIntervalSchemeNext" ),
                                 QStringLiteral( "mActionArrowRight.svg" ),
                                 tr( "下一页方案" ) );
  connect( m_schemeNext, &QToolButton::clicked, this, [this] {
    ++m_schemePage;
    refillSchemeComboPage();
  } );
  lay->addWidget( m_schemeNext );
  m_schemePageLabel = new QLabel( row );
  m_schemePageLabel->setObjectName( QStringLiteral( "subIntervalSchemePageLabel" ) );
  m_schemePageLabel->setVisible( false );
  lay->addWidget( m_schemePageLabel );

  m_revisionIcon = makeIconLabel( row, QStringLiteral( "subIntervalRevisionIcon" ),
                                  QStringLiteral( "mActionHistory.svg" ),
                                  tr( "解释修订快照" ) );
  lay->addWidget( m_revisionIcon );
  m_revisionCombo = new QComboBox( row );
  m_revisionCombo->setObjectName( QStringLiteral( "subIntervalRevisionCombo" ) );
  m_revisionCombo->setAccessibleName( tr( "解释修订" ) );
  m_revisionCombo->setToolTip( tr( "方案修订快照（不可变）。可输入关键字过滤；超多修订时分页。"
                                   "选择后 UnitSelection.revision 写入该修订；分析成果按修订对齐。" ) );
  m_revisionCombo->setWhatsThis( tr( "<b>解释修订</b><p>选择修订快照并写入 UnitSelection.revision。"
                                     "输入关键字可按修订号/备注/时间过滤；超过 %1 条时分页。"
                                     "</p>" )
                                     .arg( paleo::strat::kSchemeRevisionPageSize ) );
  m_revisionCombo->setMinimumContentsLength( 8 );
  m_revisionCombo->setSizeAdjustPolicy( QComboBox::AdjustToContents );
  enableSearchableCombo( m_revisionCombo );
  connect( m_revisionCombo, QOverload<int>::of( &QComboBox::currentIndexChanged ), this,
           &SubIntervalSelectorBar::onRevisionChanged );
  if ( QLineEdit *edit = m_revisionCombo->lineEdit() )
    connect( edit, &QLineEdit::textEdited, this, &SubIntervalSelectorBar::onRevisionFilterEdited );
  lay->addWidget( m_revisionCombo );
  m_revisionPrev = makePageButton( row, QStringLiteral( "subIntervalRevisionPrev" ),
                                   QStringLiteral( "mActionArrowLeft.svg" ),
                                   tr( "上一页修订" ) );
  connect( m_revisionPrev, &QToolButton::clicked, this, [this] {
    --m_revisionPage;
    refillRevisionComboPage();
  } );
  lay->addWidget( m_revisionPrev );
  m_revisionNext = makePageButton( row, QStringLiteral( "subIntervalRevisionNext" ),
                                   QStringLiteral( "mActionArrowRight.svg" ),
                                   tr( "下一页修订" ) );
  connect( m_revisionNext, &QToolButton::clicked, this, [this] {
    ++m_revisionPage;
    refillRevisionComboPage();
  } );
  lay->addWidget( m_revisionNext );
  m_revisionPageLabel = new QLabel( row );
  m_revisionPageLabel->setObjectName( QStringLiteral( "subIntervalRevisionPageLabel" ) );
  m_revisionPageLabel->setVisible( false );
  lay->addWidget( m_revisionPageLabel );

  m_parent = new QComboBox( row );
  m_parent->setObjectName( QStringLiteral( "subIntervalParentCombo" ) );
  m_parent->setAccessibleName( tr( "父层间单元" ) );
  m_parent->setToolTip( tr( "父层间单元（相邻两条界面，如 T70–T71）。"
                            "展开「状态」可查看四个子层段的井出现与成果红绿灯。" ) );
  m_parent->setMinimumContentsLength( 10 );
  m_parent->setSizeAdjustPolicy( QComboBox::AdjustToContents );
  connect( m_parent, QOverload<int>::of( &QComboBox::currentIndexChanged ), this,
           &SubIntervalSelectorBar::onParentChanged );
  lay->addWidget( m_parent );

  for ( int i = 1; i <= kSubIntervalCount; ++i )
  {
    auto *btn = new QToolButton( row );
    btn->setObjectName( QStringLiteral( "subIntervalS%1" ).arg( i ) );
    btn->setText( QStringLiteral( "S%1" ).arg( i ) );
    btn->setCheckable( true );
    btn->setToolButtonStyle( Qt::ToolButtonTextBesideIcon );
    btn->setIcon( PaleoIcons::qgisTheme( QStringLiteral( "mIconRasterLayer.svg" ) ) );
    btn->setIconSize( PaleoIcons::toolbarSize() );
    btn->setToolTip( tr( "子层段 S%1（自上而下第 %1 段）。选中后单因素/编图/相分析"
                         "按稳定子层段 ID 归属，不拆分界面名。" )
                         .arg( i ) );
    btn->setStatusTip( tr( "分析层段：S%1" ).arg( i ) );
    styleSubButton( btn );
    connect( btn, &QToolButton::clicked, this, [this, i] { onSubClicked( i ); } );
    m_subs.push_back( btn );
    lay->addWidget( btn );
  }

  m_agg = new QToolButton( row );
  m_agg->setObjectName( QStringLiteral( "subIntervalAggregate" ) );
  m_agg->setText( tr( "综合" ) );
  m_agg->setCheckable( true );
  m_agg->setToolButtonStyle( Qt::ToolButtonTextBesideIcon );
  m_agg->setIcon( PaleoIcons::qgisTheme( QStringLiteral( "processingAlgorithm.svg" ) ) );
  m_agg->setIconSize( PaleoIcons::toolbarSize() );
  m_agg->setToolTip( tr( "父单元显式综合（厚度加权）。不是任一子层段；"
                         "缺任一子层段成果时综合结果不可用。" ) );
  m_agg->setStatusTip( tr( "分析层段：父单元综合" ) );
  styleSubButton( m_agg );
  connect( m_agg, &QToolButton::clicked, this, [this] { onSubClicked( 0 ); } );
  lay->addWidget( m_agg );

  m_expand = new QToolButton( row );
  m_expand->setObjectName( QStringLiteral( "subIntervalExpand" ) );
  m_expand->setCheckable( true );
  m_expand->setToolButtonStyle( Qt::ToolButtonTextBesideIcon );
  m_expand->setIcon( PaleoIcons::qgisTheme( QStringLiteral( "mActionIdentify.svg" ) ) );
  m_expand->setIconSize( PaleoIcons::toolbarSize() );
  m_expand->setText( tr( "状态" ) );
  m_expand->setToolTip( tr( "展开查看当前父单元四个子层段的完整性/可用资料/受影响状态（红绿灯）。" ) );
  m_expand->setStatusTip( tr( "展开子层段状态" ) );
  connect( m_expand, &QToolButton::toggled, this, [this]( bool on ) {
    if ( m_expandStatus )
      m_expandStatus->setVisible( on );
    rebuildStatusPanel();
  } );
  lay->addWidget( m_expand );

  m_status = new QLabel( row );
  m_status->setObjectName( QStringLiteral( "subIntervalStatus" ) );
  m_status->setTextInteractionFlags( Qt::TextSelectableByMouse );
  lay->addWidget( m_status, 1 );

  root->addWidget( row );

  m_expandStatus = new QLabel( this );
  m_expandStatus->setObjectName( QStringLiteral( "subIntervalExpandStatus" ) );
  m_expandStatus->setWordWrap( true );
  m_expandStatus->setVisible( false );
  m_expandStatus->setTextInteractionFlags( Qt::TextSelectableByMouse );
  root->addWidget( m_expandStatus );

  if ( m_selection )
  {
    connect( m_selection, &SelectionContext::activeUnitChanged, this,
             &SubIntervalSelectorBar::syncFromContext );
    syncFromContext( m_selection->activeUnit() );
  }
  m_status->setText( readinessText() );
}

QString SubIntervalSelectorBar::currentDisplayName() const
{
  return m_current.displayName;
}

QStringList SubIntervalSelectorBar::subIntervalStatusLines() const
{
  QStringList lines;
  if ( m_scheme.intervals.empty() )
  {
    lines << tr( "尚未加载界面名单——请先打开工程并确认层序界面。" );
    return lines;
  }
  const QString iid = m_parent && m_parent->currentIndex() >= 0
                          ? m_parent->currentData().toString()
                          : QString();
  if ( iid.isEmpty() )
  {
    lines << tr( "请选择父层间单元。" );
    return lines;
  }
  const auto rows = paleo::strat::summarizeSubIntervalOccurrences(
      m_scheme, iid, m_bindings.empty() ? nullptr : &m_bindings );
  if ( rows.isEmpty() )
  {
    lines << tr( "当前父单元尚未派生四子层段。" );
    return lines;
  }
  for ( const auto &r : rows )
  {
    const bool selected = m_current.isValid() && !m_current.aggregate && m_current.subIntervalId == r.subIntervalId;
    lines << ( selected ? r.line + tr( " · 当前选中" ) : r.line );
  }
  if ( m_current.aggregate && m_current.intervalId == iid )
    lines << tr( "综合：当前选中（需 S1–S4 齐全才可出综合成果）" );
  else
    lines << tr( "综合：未选中" );
  if ( !m_fromDataService )
    lines << tr( "提示：当前为工程草稿方案，井出现来自界面名单；打开含层段方案的工区后可切换修订。" );
  return lines;
}

void SubIntervalSelectorBar::reloadFromBoundaries( const QStringList &orderedBoundaries,
                                                   const QString &schemeId, const QString &schemeName )
{
  m_fallbackBoundaries = orderedBoundaries;
  m_fromDataService = false;
  m_bindings.clear();
  m_scheme = paleo::strat::draftScheme( schemeId, schemeName, orderedBoundaries, {} );
  rebuildSchemeCombo();
  rebuildRevisionCombo();
  rebuildParentCombo();
  rebuildSubButtons();
  rebuildStatusPanel();
  m_status->setText( readinessText() );
  if ( orderedBoundaries.size() < 2 )
    emit readinessMessage( tr( "界面不足两条，无法构成父层间单元。请在工程参数中配置层序界面。" ) );
}

void SubIntervalSelectorBar::reloadFromProject( const QString &projectDir,
                                                const QStringList &fallbackBoundaries )
{
  m_projectDir = projectDir;
  m_fallbackBoundaries = fallbackBoundaries;
  paleo::data::StratigraphicScheme loaded;
  std::vector<paleo::data::SubIntervalResultBinding> bindings;
  QString err;
  if ( !projectDir.isEmpty()
       && paleo::strat::loadProjectSchemeFromDir( projectDir, {}, {}, loaded, &bindings, &err ) )
  {
    m_fromDataService = true;
    m_bindings = std::move( bindings );
    applyLoadedScheme( loaded, true ); // 写入 SelectionContext（含 schemeId/revision）
    rebuildSchemeCombo();
    rebuildRevisionCombo();
    emit readinessMessage( tr( "已加载地层方案「%1」修订 %2" )
                               .arg( QString::fromStdString( loaded.name ),
                                     shortRevision( QString::fromStdString( loaded.revision.value ) ) ) );
    return;
  }
  if ( !err.isEmpty() )
    emit readinessMessage( err );
  reloadFromBoundaries( fallbackBoundaries );
}

void SubIntervalSelectorBar::setSchemeSnapshot(
    const paleo::data::StratigraphicScheme &scheme,
    const std::vector<paleo::data::SubIntervalResultBinding> &bindings )
{
  m_fromDataService = !scheme.id.empty();
  m_bindings = bindings;
  applyLoadedScheme( scheme, false );
  rebuildSchemeCombo();
  rebuildRevisionCombo();
}

void SubIntervalSelectorBar::applyLoadedScheme( const paleo::data::StratigraphicScheme &scheme, bool broadcast )
{
  m_scheme = scheme;
  rebuildParentCombo();
  // 尽量保留当前子序号偏好。
  int prefer = ( m_current.isValid() && !m_current.aggregate && m_current.subIndex >= 1 ) ? m_current.subIndex : 1;
  if ( m_parent && m_parent->count() > 0 )
  {
    const QString iid = m_parent->currentData().toString();
    const UnitSelection sel = paleo::strat::makeSelection( m_scheme, iid, prefer );
    if ( sel.isValid() )
      applySelection( sel, broadcast );
    else
    {
      m_current = {};
      rebuildSubButtons();
      m_status->setText( readinessText() );
      rebuildStatusPanel();
    }
  }
  else
  {
    m_current = {};
    rebuildSubButtons();
    m_status->setText( readinessText() );
    rebuildStatusPanel();
  }
}

void SubIntervalSelectorBar::rebuildSchemeCombo()
{
  if ( !m_schemeCombo )
    return;
  m_schemeChoices.clear();
  m_schemeFilter.clear();
  m_schemePage = 0;
  if ( m_fromDataService && !m_projectDir.isEmpty() )
  {
    QVector<paleo::strat::SchemeChoice> schemes;
    QString err;
    if ( paleo::strat::listProjectSchemesFromDir( m_projectDir, schemes, &err ) )
    {
      for ( const auto &c : schemes )
      {
        SubIntervalLabeledChoice row;
        row.id = c.id;
        row.label = c.name.isEmpty() ? c.id : c.name;
        m_schemeChoices.push_back( row );
      }
    }
  }
  if ( m_schemeChoices.isEmpty() )
  {
    SubIntervalLabeledChoice row;
    row.id = QString::fromStdString( m_scheme.id );
    const QString name = QString::fromStdString( m_scheme.name );
    row.label = name.isEmpty() ? tr( "工程草稿" ) : name;
    m_schemeChoices.push_back( row );
  }
  refillSchemeComboPage();
}

void SubIntervalSelectorBar::rebuildRevisionCombo()
{
  if ( !m_revisionCombo )
    return;
  m_revisionChoices.clear();
  m_revisionFilter.clear();
  m_revisionPage = 0;
  const QString schemeId = m_schemeCombo ? m_schemeCombo->currentData().toString()
                                         : QString::fromStdString( m_scheme.id );
  if ( m_fromDataService && !m_projectDir.isEmpty() && !schemeId.isEmpty() )
  {
    QVector<paleo::strat::SchemeRevisionChoice> revs;
    QString err;
    if ( paleo::strat::listProjectSchemeRevisionsFromDir( m_projectDir, schemeId, revs, &err ) )
    {
      for ( const auto &r : revs )
      {
        SubIntervalLabeledChoice row;
        row.id = r.revision;
        row.label = shortRevision( r.revision );
        if ( !r.note.isEmpty() )
          row.label += QStringLiteral( " · " ) + r.note;
        else if ( !r.created.isEmpty() )
          row.label += QStringLiteral( " · " ) + r.created;
        m_revisionChoices.push_back( row );
      }
    }
  }
  if ( m_revisionChoices.isEmpty() )
  {
    SubIntervalLabeledChoice row;
    row.id = QString::fromStdString( m_scheme.revision.value );
    row.label = row.id.isEmpty() ? tr( "（草稿）" ) : shortRevision( row.id );
    m_revisionChoices.push_back( row );
  }
  refillRevisionComboPage();
}

void SubIntervalSelectorBar::refillSchemeComboPage()
{
  if ( !m_schemeCombo )
    return;
  QVector<paleo::strat::LabeledIdChoice> all;
  all.reserve( m_schemeChoices.size() );
  for ( const auto &c : m_schemeChoices )
    all.push_back( { c.label, c.id } );
  const QString keep = QString::fromStdString( m_scheme.id );
  // 无过滤时跳到含当前方案的页，避免「当前项不在本页」被误当成第 0 项。
  if ( m_schemeFilter.trimmed().isEmpty() && !keep.isEmpty() )
  {
    int found = -1;
    for ( int i = 0; i < all.size(); ++i )
    {
      if ( all.at( i ).id == keep )
      {
        found = i;
        break;
      }
    }
    if ( found >= 0 )
      m_schemePage = found / paleo::strat::kSchemeRevisionPageSize;
  }
  const auto page = paleo::strat::pageLabeledChoices( all, m_schemeFilter, m_schemePage );
  m_schemePage = page.pageIndex;
  m_block = true;
  m_schemeCombo->clear();
  const QIcon ic = PaleoIcons::qgisTheme( QStringLiteral( "mIconPolygonLayer.svg" ) );
  for ( const auto &c : page.items )
    m_schemeCombo->addItem( ic, c.label, c.id );
  const int idx = m_schemeCombo->findData( keep );
  if ( idx >= 0 )
    m_schemeCombo->setCurrentIndex( idx );
  else
    m_schemeCombo->setCurrentIndex( -1 );
  m_block = false;
  updateSchemePageChrome();
  if ( QLineEdit *edit = m_schemeCombo->lineEdit() )
  {
    edit->setPlaceholderText( tr( "搜索方案…" ) );
    if ( !m_schemeFilter.isEmpty() )
      edit->setText( m_schemeFilter );
  }
}

void SubIntervalSelectorBar::refillRevisionComboPage()
{
  if ( !m_revisionCombo )
    return;
  QVector<paleo::strat::LabeledIdChoice> all;
  all.reserve( m_revisionChoices.size() );
  for ( const auto &c : m_revisionChoices )
    all.push_back( { c.label, c.id } );
  const QString keep = QString::fromStdString( m_scheme.revision.value );
  if ( m_revisionFilter.trimmed().isEmpty() && !keep.isEmpty() )
  {
    int found = -1;
    for ( int i = 0; i < all.size(); ++i )
    {
      if ( all.at( i ).id == keep )
      {
        found = i;
        break;
      }
    }
    if ( found >= 0 )
      m_revisionPage = found / paleo::strat::kSchemeRevisionPageSize;
  }
  const auto page = paleo::strat::pageLabeledChoices( all, m_revisionFilter, m_revisionPage );
  m_revisionPage = page.pageIndex;
  m_block = true;
  m_revisionCombo->clear();
  const QIcon ic = PaleoIcons::qgisTheme( QStringLiteral( "mActionHistory.svg" ) );
  for ( const auto &c : page.items )
    m_revisionCombo->addItem( ic, c.label, c.id );
  const int idx = m_revisionCombo->findData( keep );
  if ( idx >= 0 )
    m_revisionCombo->setCurrentIndex( idx );
  else
    m_revisionCombo->setCurrentIndex( -1 );
  m_block = false;
  updateRevisionPageChrome();
  if ( QLineEdit *edit = m_revisionCombo->lineEdit() )
  {
    edit->setPlaceholderText( tr( "搜索修订…" ) );
    if ( !m_revisionFilter.isEmpty() )
      edit->setText( m_revisionFilter );
  }
}

void SubIntervalSelectorBar::updateSchemePageChrome()
{
  QVector<paleo::strat::LabeledIdChoice> all;
  for ( const auto &c : m_schemeChoices )
    all.push_back( { c.label, c.id } );
  const auto page = paleo::strat::pageLabeledChoices( all, m_schemeFilter, m_schemePage );
  const bool multi = page.pageCount > 1;
  if ( m_schemePrev )
  {
    m_schemePrev->setVisible( multi );
    m_schemePrev->setEnabled( multi && page.pageIndex > 0 );
  }
  if ( m_schemeNext )
  {
    m_schemeNext->setVisible( multi );
    m_schemeNext->setEnabled( multi && page.pageIndex + 1 < page.pageCount );
  }
  if ( m_schemePageLabel )
  {
    m_schemePageLabel->setVisible( multi || !m_schemeFilter.isEmpty() );
    if ( page.filteredTotal == 0 )
      m_schemePageLabel->setText( tr( "无匹配方案" ) );
    else if ( multi )
      m_schemePageLabel->setText( tr( "%1/%2页 · 共%3" )
                                      .arg( page.pageIndex + 1 )
                                      .arg( page.pageCount )
                                      .arg( page.filteredTotal ) );
    else
      m_schemePageLabel->setText( tr( "共%1" ).arg( page.filteredTotal ) );
  }
}

void SubIntervalSelectorBar::updateRevisionPageChrome()
{
  QVector<paleo::strat::LabeledIdChoice> all;
  for ( const auto &c : m_revisionChoices )
    all.push_back( { c.label, c.id } );
  const auto page = paleo::strat::pageLabeledChoices( all, m_revisionFilter, m_revisionPage );
  const bool multi = page.pageCount > 1;
  if ( m_revisionPrev )
  {
    m_revisionPrev->setVisible( multi );
    m_revisionPrev->setEnabled( multi && page.pageIndex > 0 );
  }
  if ( m_revisionNext )
  {
    m_revisionNext->setVisible( multi );
    m_revisionNext->setEnabled( multi && page.pageIndex + 1 < page.pageCount );
  }
  if ( m_revisionPageLabel )
  {
    m_revisionPageLabel->setVisible( multi || !m_revisionFilter.isEmpty() );
    if ( page.filteredTotal == 0 )
      m_revisionPageLabel->setText( tr( "无匹配修订" ) );
    else if ( multi )
      m_revisionPageLabel->setText( tr( "%1/%2页 · 共%3" )
                                        .arg( page.pageIndex + 1 )
                                        .arg( page.pageCount )
                                        .arg( page.filteredTotal ) );
    else
      m_revisionPageLabel->setText( tr( "共%1" ).arg( page.filteredTotal ) );
  }
}

void SubIntervalSelectorBar::rebuildParentCombo()
{
  m_block = true;
  const QString keep = m_parent->currentData().toString();
  m_parent->clear();
  const auto opts = paleo::strat::selectorOptions( m_scheme );
  for ( const auto &p : opts )
    m_parent->addItem( p.label, p.intervalId );
  int idx = m_parent->findData( keep );
  if ( idx < 0 && m_current.isValid() )
    idx = m_parent->findData( m_current.intervalId );
  if ( idx < 0 && m_parent->count() > 0 )
    idx = 0;
  if ( idx >= 0 )
    m_parent->setCurrentIndex( idx );
  m_block = false;
  const bool ok = m_parent->count() > 0;
  for ( auto *b : m_subs )
    b->setEnabled( ok );
  if ( m_agg )
    m_agg->setEnabled( ok );
}

void SubIntervalSelectorBar::rebuildSubButtons()
{
  const bool ok = m_parent && m_parent->count() > 0;
  for ( auto *b : m_subs )
  {
    b->setEnabled( ok );
    b->setChecked( false );
  }
  if ( m_agg )
  {
    m_agg->setEnabled( ok );
    m_agg->setChecked( false );
  }
  if ( !m_current.isValid() )
    return;
  if ( m_current.aggregate )
  {
    if ( m_agg )
      m_agg->setChecked( true );
    return;
  }
  const QString iid = m_current.intervalId;
  for ( int i = 1; i <= kSubIntervalCount; ++i )
  {
    const UnitSelection probe = paleo::strat::makeSelection( m_scheme, iid, i );
    if ( probe.isValid() && probe.subIntervalId == m_current.subIntervalId && i - 1 < m_subs.size() )
      m_subs.at( i - 1 )->setChecked( true );
  }
}

void SubIntervalSelectorBar::rebuildStatusPanel()
{
  if ( !m_expandStatus )
    return;
  if ( !m_expand || !m_expand->isChecked() )
  {
    m_expandStatus->clear();
    return;
  }
  m_expandStatus->setText( subIntervalStatusLines().join( QChar( '\n' ) ) );
}

QString SubIntervalSelectorBar::readinessText() const
{
  if ( m_scheme.boundaries.empty() )
    return tr( "未加载界面——打开工程后自动按层序界面生成父单元。" );
  if ( m_scheme.intervals.empty() )
    return tr( "界面不足——至少需要两条相邻界面才能分析层段。" );
  const QString schemeBit = m_fromDataService
                                ? tr( "方案 %1 / 修订 %2" )
                                      .arg( QString::fromStdString( m_scheme.name ),
                                            shortRevision( QString::fromStdString( m_scheme.revision.value ) ) )
                                : tr( "工程草稿" );
  if ( !m_current.isValid() )
    return tr( "%1 · 请选择父单元与子层段（S1–S4）" ).arg( schemeBit );
  if ( m_current.aggregate )
    return tr( "%1 · 当前：%2（综合需各子层段成果齐全）" ).arg( schemeBit, m_current.displayName );
  return tr( "%1 · 当前：%2" ).arg( schemeBit, m_current.displayName );
}

bool SubIntervalSelectorBar::loadSelectedScheme( bool broadcast )
{
  if ( !m_fromDataService || m_projectDir.isEmpty() || !m_schemeCombo || !m_revisionCombo )
    return false;
  const QString sid = m_schemeCombo->currentData().toString();
  const QString rev = m_revisionCombo->currentData().toString();
  paleo::data::StratigraphicScheme loaded;
  std::vector<paleo::data::SubIntervalResultBinding> bindings;
  QString err;
  if ( !paleo::strat::loadProjectSchemeFromDir( m_projectDir, sid, rev, loaded, &bindings, &err ) )
  {
    if ( !err.isEmpty() )
      emit readinessMessage( err );
    return false;
  }
  m_bindings = std::move( bindings );
  applyLoadedScheme( loaded, broadcast );
  return true;
}

void SubIntervalSelectorBar::onSchemeFilterEdited( const QString &text )
{
  if ( m_block )
    return;
  m_schemeFilter = text;
  m_schemePage = 0;
  refillSchemeComboPage();
}

void SubIntervalSelectorBar::onRevisionFilterEdited( const QString &text )
{
  if ( m_block )
    return;
  m_revisionFilter = text;
  m_revisionPage = 0;
  refillRevisionComboPage();
}

void SubIntervalSelectorBar::onSchemeChanged( int index )
{
  if ( m_block || index < 0 )
    return;
  m_schemeFilter.clear();
  rebuildRevisionCombo();
  if ( !loadSelectedScheme( true ) && !m_fromDataService )
    rebuildParentCombo();
}

void SubIntervalSelectorBar::onRevisionChanged( int index )
{
  if ( m_block || index < 0 )
    return;
  m_revisionFilter.clear();
  loadSelectedScheme( true );
}

void SubIntervalSelectorBar::onParentChanged( int )
{
  if ( m_block )
    return;
  int prefer = 1;
  if ( m_current.isValid() && !m_current.aggregate )
  {
    for ( int i = 1; i <= kSubIntervalCount; ++i )
    {
      if ( i - 1 < m_subs.size() && m_subs.at( i - 1 )->isChecked() )
        prefer = i;
    }
  }
  onSubClicked( prefer );
  rebuildStatusPanel();
}

void SubIntervalSelectorBar::onSubClicked( int subIndex )
{
  if ( !m_parent || m_parent->currentIndex() < 0 )
  {
    emit readinessMessage( tr( "请先选择父层间单元。" ) );
    return;
  }
  const QString iid = m_parent->currentData().toString();
  const UnitSelection sel = paleo::strat::makeSelection( m_scheme, iid, subIndex );
  if ( !sel.isValid() )
  {
    emit readinessMessage( tr( "该父单元尚未派生完整四子层段，请检查解释方案。" ) );
    rebuildSubButtons();
    return;
  }
  applySelection( sel, true );
}

void SubIntervalSelectorBar::applySelection( const UnitSelection &sel, bool broadcast )
{
  m_current = sel;
  rebuildSubButtons();
  m_status->setText( readinessText() );
  rebuildStatusPanel();
  if ( broadcast && m_selection )
    m_selection->setActiveUnit( sel );
  emit unitSelectionCommitted( sel );
}

void SubIntervalSelectorBar::syncFromContext( const UnitSelection &unit )
{
  if ( unit == m_current )
    return;
  m_current = unit;
  if ( unit.isValid() && m_parent )
  {
    m_block = true;
    const int idx = m_parent->findData( unit.intervalId );
    if ( idx >= 0 )
      m_parent->setCurrentIndex( idx );
    m_block = false;
  }
  rebuildSubButtons();
  m_status->setText( readinessText() );
  rebuildStatusPanel();
}
