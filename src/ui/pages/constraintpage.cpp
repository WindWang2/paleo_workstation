// 层：视图
#include "ui/paleotheme.h"
#include "constraintpage.h"
#include "constraintpage_internal.h"

#include "pageshared.h"

#include "../../qgis/qgislayerservice.h"
#include "../../services/singlefactordef.h"
#include "../../workflow/workflows.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QShowEvent>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

using namespace paleo::pagesinternal;
using namespace paleo::constraintpage_internal;

// ---------------------------------------------------------------------------
// ConstraintPage — ②约束与单因素（m2(B) 单因素图页）
// 方向 96：本体只留 ctor 编排 + 因子清单面 + 图层服务绑定/生成态/厚度刷新；
// 其余构建段与方法分布 constraintpage_wellfactors/_interpolation/_generate/
// _constraints/_sections.cpp（见 constraintpage.h private 区段图）。
// ---------------------------------------------------------------------------

ConstraintPage::ConstraintPage( ConstraintWorkflow *wf, QWidget *parent )
  : QWidget( parent )
{
  auto *scroll = new QScrollArea( this );
  scroll->setObjectName( QStringLiteral( "constraintPageScroll" ) );
  scroll->setWidgetResizable( true );
  scroll->setFrameShape( QFrame::NoFrame );
  auto *content = new QWidget( scroll );
  content->setObjectName( QStringLiteral( "constraintPageBody" ) );
  auto *lay = panelLayout( content );
  scroll->setWidget( content );
  auto *outer = new QVBoxLayout( this );
  outer->setContentsMargins( 0, 0, 0, 0 );
  outer->addWidget( scroll );

  // 反馈 #19：输入 → 处理 → 输出（IPO）三段式。顶部步骤条始终说明当前所处阶段，
  // 默认只露最少参数；约束、高级插值、兼容入口均为渐进展开。
  auto *stepper = new QLabel( content );
  stepper->setObjectName( QStringLiteral( "factorIpoStepper" ) );
  stepper->setAccessibleName( tr( "单因素成图步骤" ) );
  stepper->setTextFormat( Qt::PlainText );
  stepper->setWordWrap( true );
  lay->addWidget( stepper );
  auto *inputStage = caption( tr( "① 输入 · 层位、因素与井点数据" ), this );
  inputStage->setObjectName( QStringLiteral( "factorInputStageCaption" ) );
  lay->addWidget( inputStage );
  lay->addWidget( caption( tr( "当前层位" ), this ) );
  auto *horizons = new QComboBox( this );
  horizons->setObjectName( QStringLiteral( "horizonCombo" ) );
  horizons->setAccessibleName( tr( "单因素图层位" ) );
  horizons->setToolTip( tr( "选择当前图件所属层位；暂无层位时请先打开工程" ) );
  lay->addWidget( horizons );

  // ---- m2(B)：单因素清单 + 生成 双区（§10 词表驱动）------------------------
  auto *selectionHint = caption(tr("点击一行选择因素；每次生成一张图。"), this);
  selectionHint->setWordWrap(true);
  lay->addWidget(selectionHint);
  auto *factors = new QTableWidget( 0, 4, this );
  factors->setObjectName( QStringLiteral( "factorTable" ) );
  factors->setAccessibleName( tr( "单因素清单" ) );
  factors->setHorizontalHeaderLabels( { tr( "选择" ), tr( "因素" ), tr( "输入" ), tr( "成果状态" ) } );
  factors->verticalHeader()->setVisible( false );
  factors->horizontalHeader()->setStretchLastSection( true );
  factors->setEditTriggers( QAbstractItemView::NoEditTriggers );
  factors->setSelectionBehavior( QAbstractItemView::SelectRows );
  factors->setSelectionMode( QAbstractItemView::SingleSelection );
  factors->setWordWrap(false);
  // 输入说明放到当前因素提示中，窄 dock 保留因素名和成果状态。
  factors->setColumnHidden(2, true);
  factors->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  factors->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  int row = 0;
  for ( const SingleFactorDefinition &def : SingleFactorRegistry::builtins() )
  {
    if (!SingleFactorRegistry::isImplemented(def.factorId)) continue;
    factors->insertRow( row );
    auto *check = new QTableWidgetItem();
    check->setFlags( Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable );
    check->setCheckState( Qt::Unchecked );
    check->setData( Qt::UserRole, def.factorId );
    auto *name = new QTableWidgetItem( def.title );
    const QString inputLabel = def.inputAssetType == QLatin1String("horizon") ? tr("顶底构造面")
                               : def.inputAssetType == QLatin1String("prediction")
                                   ? tr("预测数据")
                                   : tr("当前地层井点数据");
    auto *input = new QTableWidgetItem(inputLabel);
    name->setToolTip(inputLabel);
    auto *status = new QTableWidgetItem( tr( "未生成" ) );
    for ( auto *it : { check, name, input, status } )
      it->setFlags( it->flags() & ~Qt::ItemIsEditable );
    // 名称/输入列挂 UserRole，测试与刷新按 factorId 对行。
    name->setData( Qt::UserRole, def.factorId );
    input->setData( Qt::UserRole, def.factorId );
    status->setData( Qt::UserRole, def.factorId );
    factors->setItem( row, 0, check );
    factors->setItem( row, 1, name );
    factors->setItem( row, 2, input );
    factors->setItem( row, 3, status );
    ++row;
  }
  factors->setFixedHeight(factors->horizontalHeader()->sizeHint().height()
                         + factors->rowCount() * factors->verticalHeader()->defaultSectionSize()
                         + 2 * factors->frameWidth());
  lay->addWidget( factors );
  auto *inputHint = caption(QString(), content);
  inputHint->setObjectName(QStringLiteral("factorInputHint"));
  inputHint->setWordWrap(true);
  lay->addWidget(inputHint);

  // 方向 96：自此处起 ctor 按原文行序分解为构建段（段图见 constraintpage.h；
  // 调用序 = 原文行序，创建序/连接序逐行保持）。
  auto *dataDetails = new CollapsibleSection(tr("数据与取值详情"), content);
  dataDetails->setObjectName(QStringLiteral("factorDataDetailsSection"));
  buildWellFactorSection(dataDetails->container(), dataDetails->containerLayout(), horizons, factors);
  dataDetails->setExpanded(false); lay->addWidget(dataDetails);
  QLabel *cellCaption = buildFieldAndCellRows( lay );
  buildMethodSection( content, lay );
  buildBoundaryRows( content, lay );
  buildAdvancedSection( content, lay );
  buildSurfaceRow( lay );
  auto *surfaceInputs = findChild<QWidget *>(QStringLiteral("factorSurfaceRow"));
  lay->removeWidget(surfaceInputs);
  lay->insertWidget(lay->indexOf(dataDetails) + 1,
                    surfaceInputs);
  auto *outputStage = caption(tr("③ 输出 · 预览、保存与发布"), content);
  outputStage->setObjectName(QStringLiteral("factorOutputStageCaption"));
  lay->addWidget(outputStage);
  auto *contourSection = new CollapsibleSection(tr("可选：提取等值线（等值面保存后）"), content);
  contourSection->setObjectName(QStringLiteral("factorContourSection"));
  contourSection->setExpanded(false);
  lay->addWidget(contourSection);
  buildContourSection( contourSection->container(), contourSection->containerLayout() );
  wireFactorTableSelection( factors );
  buildConstraintArea( content, lay, horizons, cellCaption );
  auto *constraints = findChild<QWidget *>(QStringLiteral("constraintFamiliesSection"));
  lay->removeWidget(constraints);
  auto *methodCaption = findChild<QLabel *>(QStringLiteral("factorMethodCaption"));
  auto *settingsCaption = caption(tr("② 处理 · 成图方法（默认即可直接生成）"), content);
  settingsCaption->setObjectName(QStringLiteral("factorSettingsCaption"));
  lay->insertWidget(lay->indexOf(methodCaption), settingsCaption);
  auto *advanced = findChild<QWidget *>(QStringLiteral("factorAdvancedSection"));
  const int advancedIndex = lay->indexOf(advanced);
  lay->removeWidget(advanced);
  auto *options = new CollapsibleSection(tr("高级插值参数"), content);
  options->setObjectName(QStringLiteral("factorAdvancedOptionsSection"));
  options->containerLayout()->addWidget(advanced);
  options->setExpanded(false);
  lay->insertWidget(advancedIndex, options);
  // 约束（断层/边界/方向线）属于处理阶段的可选项，默认收起；门控需要时
  //「下一步」按钮会展开并定位（见 updateOperationGuidance）。已有约束的
  // 旧工程在 refresh 时自动展开，避免已绘约束被“藏起来”。
  auto *optionalConstraints = new CollapsibleSection(tr("可选：断层 / 边界 / 方向约束"), content);
  optionalConstraints->setObjectName(QStringLiteral("factorConstraintsOptionalSection"));
  optionalConstraints->containerLayout()->addWidget(constraints);
  optionalConstraints->setExpanded(false);
  lay->insertWidget(lay->indexOf(options) + 1, optionalConstraints);
  auto *settings = new CollapsibleSection(tr("成图设置（默认即可）"), content);
  settings->setObjectName(QStringLiteral("factorBasicSettingsSection"));
  for (const char *name : {"factorCellSizeCaption", "factorCellSizeSpin", "factorTaskCaption", "factorTaskCombo",
       "factorSettingsCaption", "factorMethodCaption", "factorMethodCombo", "factorStrategyNote", "factorTaskNote", "factorCoverageCaption", "factorCoverageCombo",
       "factorBoundaryCaption", "factorBoundaryRow", "factorGridCaption", "factorGridResolutionSpin"})
    if (auto *widget = findChild<QWidget *>(QString::fromLatin1(name))) { lay->removeWidget(widget); settings->containerLayout()->addWidget(widget); }
  settings->setExpanded(false); lay->insertWidget(lay->indexOf(options), settings);
  buildFactorResultsSection(content, lay);
  buildTailSections( content, lay );
  // 反馈 #19：兜底工具（原始字段 IDW / 厚度样本 / 不确定性集合）统一收进输出段
  // 末尾的「更多工具」折叠段；各自仍是独立折叠段，objectName 不变。
  auto *moreTools = new CollapsibleSection(tr("更多工具"), content);
  moreTools->setObjectName(QStringLiteral("factorMoreToolsSection"));
  for (const char *name : {"factorLegacySection", "thicknessSection", "realizationSection"})
    if (auto *w = findChild<QWidget *>(QString::fromLatin1(name))) {
      lay->removeWidget(w);
      moreTools->containerLayout()->addWidget(w);
    }
  moreTools->setExpanded(false);
  lay->addWidget(moreTools);
  // 主操作与就绪提示固定在滚动区外，长表单中始终可见。
  auto *actionBar = new QWidget(this);
  actionBar->setObjectName(QStringLiteral("factorActionBar"));
  auto *actionLayout = panelLayout(actionBar);
  buildGenerateSection(actionBar, actionLayout);
  auto *status = findChild<QLabel *>(QStringLiteral("statusLabel"));
  lay->removeWidget(status);
  actionLayout->addWidget(status);
  outer->setSpacing(0);
  outer->addWidget(actionBar);
  lay->addStretch();
  wireWorkflowFeedback( wf );
  wireInputStaleness();
  wireWellFactorTriggers( wf, horizons, factors );
  refreshWellFactorFields();
  refreshConstraintList();
  refreshFactorResults();
  if (factors->rowCount()) factors->item(0, 0)->setCheckState(Qt::Checked);
  updateFactorActionStates();
}

void ConstraintPage::showEvent( QShowEvent *event )
{
  QWidget::showEvent( event );
  refreshThicknessSamples();
  refreshWellFactorFields();
  refreshConstraintList();
  refreshFactorLayers();
  refreshFactorResults();
}

void ConstraintPage::wireFactorTableSelection( QTableWidget *factors )
{
  connect(factors, &QTableWidget::cellClicked, this, [factors](int row, int column) {
    // 原生复选框保留取消语义；名称/状态点击也可选择，避免高亮与勾选脱节。
    if (column != 0 && factors->isEnabled())
      factors->item(row, 0)->setCheckState(Qt::Checked);
  });
  // 勾选互斥单选（业务上同时只看一张单因素图）：勾一行自动取消其它行；
  // 生成过的行随勾选态发 factorVisibilityRequested（互斥上图意图）。
  connect( factors, &QTableWidget::itemChanged, this,
           [this, factors]( QTableWidgetItem *changed ) {
             if ( !changed || changed->column() != 0 )
               return;
             if ( changed->checkState() != Qt::Checked )
             {
               // 用户手动取消勾选：已生成的层发下图意图。
               const QString layerId = property( kFactorGenProp )
                                           .toMap()
                                           .value( factorIdOfRow( factors, changed->row() ) )
                                           .toString();
               if ( !layerId.isEmpty() )
                 emit factorVisibilityRequested( layerId, false );
               updateFactorActionStates();
               return;
             }
             // 新勾选：屏蔽信号地取消其它行（单选语义）。
             const int newRow = changed->row();
             factors->setCurrentCell(newRow, 1);
             factors->blockSignals( true );
             for ( int r = 0; r < factors->rowCount(); ++r )
             {
               if ( r != newRow && factors->item( r, 0 ) &&
                    factors->item( r, 0 )->checkState() == Qt::Checked )
               {
                 const QString prevId = factorIdOfRow( factors, r );
                 factors->item( r, 0 )->setCheckState( Qt::Unchecked );
                 const QString prevLayer =
                     property( kFactorGenProp ).toMap().value( prevId ).toString();
                 if ( !prevLayer.isEmpty() )
                   emit factorVisibilityRequested( prevLayer, false );
               }
             }
             factors->blockSignals( false );
             const QString newLayer =
                 property( kFactorGenProp ).toMap().value( factorIdOfRow( factors, newRow ) ).toString();
             if ( !newLayer.isEmpty() )
               emit factorVisibilityRequested( newLayer, true );
             // 换因素后先把逐井表按已保存提取恢复到新因素（#544 口径在
             // itemChanged 路径的漏网：不恢复则 applySuggestedContourInterval
             // 按旧因素的值域算等值距默认值，错误默认会被静默提交）。
             restoreWellFactorDisplay();
             // 再按该因素的井点值极差重设等值距；没有值域时，
             // 非结构因素若残留自动态 0 则回到 20，避免间距必须为正却提交 0。
             applySuggestedContourInterval();
             updateFactorActionStates();
           } );
  // ---- m2(B) 双区 end ------------------------------------------------------
}

void ConstraintPage::setHorizons( const QStringList &horizons )
{
  auto *combo = child<QComboBox>( this, "horizonCombo" );
  if ( !combo )
    return;
  const QString keep = combo->currentText();
  combo->clear();
  combo->addItems( horizons ); // clear/addItems 触发 currentIndexChanged → 约束列表/井因子随层位刷新
  if ( !keep.isEmpty() )
    if ( const int idx = combo->findText( keep ); idx >= 0 && idx != combo->currentIndex() )
      combo->setCurrentIndex( idx );
}

void ConstraintPage::refreshThicknessSamples()
{
  auto *table = child<QTableWidget>( this, "thicknessTable" );
  auto *hint = child<QLabel>( this, "thicknessHint" );
  if ( !table )
    return;
  auto *wf = qobject_cast<ConstraintWorkflow *>( property( kWfProp ).value<QObject *>() );
  // ARCH-06：typed 面直读（原 paleo.thickness.* 动态属性暗道已撤）。
  const QVariantList rows = wf ? wf->thicknessSampleRows() : QVariantList();
  const QString message = wf ? wf->thicknessSampleMessage() : QString();

  table->setRowCount( 0 );
  for ( const QVariant &v : rows )
  {
    const QVariantMap m = v.toMap();
    const int r = table->rowCount();
    table->insertRow( r );
    auto *name = new QTableWidgetItem( m.value( QStringLiteral( "well_name" ) ).toString() );
    const QString tvdTop = m.contains( QStringLiteral( "tvd_top" ) )
                               ? QString::number( m.value( QStringLiteral( "tvd_top" ) ).toDouble(), 'f', 1 )
                               : QStringLiteral( "—" );
    const QString tvdBase = m.contains( QStringLiteral( "tvd_base" ) )
                                ? QString::number( m.value( QStringLiteral( "tvd_base" ) ).toDouble(), 'f', 1 )
                                : QStringLiteral( "—" );
    // 贡献井 → 层间速度（m/s）；否则 → 原因文案。
    const QString last = m.value( QStringLiteral( "contributing" ) ).toBool()
                             ? tr( "%1 m/s" ).arg( m.value( QStringLiteral( "vint" ) ).toDouble(), 0, 'f', 0 )
                             : m.value( QStringLiteral( "reason" ) ).toString();
    auto *itTop = new QTableWidgetItem( tvdTop );
    auto *itBase = new QTableWidgetItem( tvdBase );
    auto *itV = new QTableWidgetItem( last );
    for ( auto *it : { name, itTop, itBase, itV } )
      it->setFlags( it->flags() & ~Qt::ItemIsEditable );
    table->setItem( r, 0, name );
    table->setItem( r, 1, itTop );
    table->setItem( r, 2, itBase );
    table->setItem( r, 3, itV );
  }
  if ( hint )
    hint->setText( message );
}

// ---- m2(B) 单因素状态面 ------------------------------------------------------

void ConstraintPage::bindLayerService( QObject *layers )
{
  auto *svc = qobject_cast<QgisLayerService *>( layers );
  if ( !svc )
  {
    setProperty( kLayersProp, QVariant() );
    return;
  }
  setProperty( kLayersProp, QVariant::fromValue( static_cast<QObject *>( svc ) ) );
  connect(svc, &QgisLayerService::layerDeclared, this, [this](const QString &id) {
    updateEngineRows();
    if (id.startsWith(QStringLiteral("wells")))
      invalidateWellFactors();
    if (id.isEmpty() || id.startsWith(QStringLiteral("factor.")))
      refreshFactorLayers();
    refreshFactorResults();
  });
  refreshFactorLayers();
}

void ConstraintPage::refreshFactorLayers() {
  auto *svc = qobject_cast<QgisLayerService *>(property(kLayersProp).value<QObject *>());
  auto *horizons = child<QComboBox>(this, "horizonCombo");
  auto *table = child<QTableWidget>(this, "factorTable");
  if (!table || !horizons)
    return;
  const QString prefix = QStringLiteral("factor.%1.").arg(horizons->currentText());
  QVariantMap generated;
  if (svc)
    for (const auto &d : svc->declared())
      if (d.horizon == horizons->currentText() && d.layerId.startsWith(prefix))
        generated.insert(d.layerId.mid(prefix.size()), d.layerId);
  setProperty(kFactorGenProp, generated);
  // 「（旧输入）」按层位记在属性里：refreshFactorLayers 任何一次重声明都会
  // 重写状态列文本，后缀写在文本里会被静默抹掉（D11）。切层位天然隔离。
  const QString staleKey = QStringLiteral("paleo.page.factorstale.%1").arg(horizons->currentText());
  const QStringList stale = property(staleKey.toUtf8().constData()).toStringList();
  const QString staleSuffix = tr("（旧输入）");
  const QSignalBlocker blocker(table);
  for (int r = 0; r < table->rowCount(); ++r) {
    const QString rowId = factorIdOfRow(table, r);
    const QString id = generated.value(rowId).toString();
    table->item(r, 3)->setText(id.isEmpty() ? tr("未生成")
        : stale.contains(rowId) ? tr("等值面已保存") + staleSuffix
        : tr("等值面已保存"));
  }
  updateFactorActionStates();
}

void ConstraintPage::noteFactorLayer( const QString &factorId, const QString &layerId )
{
  auto *horizons = child<QComboBox>(this, "horizonCombo");
  if (!horizons || layerId != QStringLiteral("factor.%1.%2").arg(horizons->currentText(), factorId))
    return;
  QVariantMap generated = property( kFactorGenProp ).toMap();
  generated.insert( factorId, layerId );
  setProperty( kFactorGenProp, generated );
  // 新输入已生成：清掉该因素的「（旧输入）」标记（D11 同族，按层位属性）。
  {
    const QString staleKey =
        QStringLiteral( "paleo.page.factorstale.%1" ).arg( horizons->currentText() );
    QStringList stale = property( staleKey.toUtf8().constData() ).toStringList();
    if ( stale.removeAll( factorId ) > 0 )
      setProperty( staleKey.toUtf8().constData(), stale );
  }

  auto *table = child<QTableWidget>( this, "factorTable" );
  if ( table )
  {
    for ( int r = 0; r < table->rowCount(); ++r )
    {
      if ( factorIdOfRow( table, r ) != factorId )
        continue;
      QTableWidgetItem *status = table->item( r, 3 );
      if ( status )
      {
        table->blockSignals( true ); // 状态文本不是勾选态，不触发单选逻辑
        status->setText(tr("等值面已保存"));
        status->setToolTip(layerId);
        table->blockSignals( false );
      }
      break;
    }
  }

  // 该行正被勾选而图层刚生成 → 上图意图（否则新生成层不进互斥显示）。
  // 注意：这里不重置等值距——S5-m3：无法区分「自动态残留」与「用户显式
  // 输入的间距」，静默压回 0 会吞掉用户输入；勾选切换回调（wireFactorTableSelection）
  // 已覆盖跨类型残留，structural 先生成后计算的用户随时可手动改回「自动」。
  auto *factors = child<QTableWidget>( this, "factorTable" );
  if ( factors && checkedRow( factors ) >= 0 &&
       factorIdOfRow( factors, checkedRow( factors ) ) == factorId )
  {
    emit factorVisibilityRequested( layerId, true );
  }
  updateFactorActionStates();
}

QString ConstraintPage::checkedFactorLayerId() const
{
  auto *factors = child<QTableWidget>( const_cast<ConstraintPage *>( this ), "factorTable" );
  if ( !factors )
    return QString();
  const int r = checkedRow( factors );
  if ( r < 0 )
    return QString();
  return property( kFactorGenProp ).toMap().value( factorIdOfRow( factors, r ) ).toString();
}

// structural 判定 = 已声明因素栅格的 .structural.json 侧卡存在（该引擎
// 必落侧卡，其它引擎不会产出同名文件）。
bool ConstraintPage::checkedFactorIsStructural() const
{
  const QString layerId = checkedFactorLayerId();
  if ( layerId.isEmpty() )
    return false;
  auto *svc = qobject_cast<QgisLayerService *>(
      property( kLayersProp ).value<QObject *>() );
  if ( !svc )
    return false;
  const QVector<LayerDeclaration> declared = svc->declared();
  for ( const LayerDeclaration &d : declared )
  {
    if ( d.layerId != layerId )
      continue;
    const QString path = d.source.section( QLatin1Char( '|' ), 0, 0 );
    const int dot = path.lastIndexOf( QLatin1Char( '.' ) );
    if ( dot < 0 )
      return false;
    return QFileInfo::exists( path.left( dot ) + QStringLiteral( ".structural.json" ) );
  }
  return false;
}

void ConstraintPage::markInputsStale()
{
  auto *factors = child<QTableWidget>( this, "factorTable" );
  if ( !factors )
    return;
  const int r = checkedRow( factors );
  if ( r < 0 )
    return;
  const QString factorId = factorIdOfRow( factors, r );
  if ( property( kFactorGenProp ).toMap().value( factorId ).toString().isEmpty() )
    return;
  // stale 记入按层位的动态属性（D11）：refreshFactorLayers 重写状态列时据此
  // 恢复后缀——此前只写在文本里，任何 layerDeclared 重声明都会静默抹掉。
  if ( auto *horizons = child<QComboBox>( this, "horizonCombo" ) )
  {
    const QString staleKey =
        QStringLiteral( "paleo.page.factorstale.%1" ).arg( horizons->currentText() );
    QStringList stale = property( staleKey.toUtf8().constData() ).toStringList();
    if ( !stale.contains( factorId ) )
    {
      stale << factorId;
      setProperty( staleKey.toUtf8().constData(), stale );
    }
  }
  QTableWidgetItem *status = factors->item( r, 3 );
  if ( !status )
    return;
  const QString suffix = tr( "（旧输入）" );
  if ( !status->text().contains( suffix ) )
  {
    factors->blockSignals( true );
    status->setText( status->text() + suffix );
    factors->blockSignals( false );
  }
  updateOperationGuidance();
}
