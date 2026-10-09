// 层：视图
// constraintpage_constraints — 约束线域 TU（方向 96 自 constraintpage.cpp 拆出，
// 原文行序保持）：约束列表/右键语义切换/逐线参数保存/批量改型/形状与相代码/
// 五类型化绘制入口/顶点编辑与删除/方向线族+打断线族重排，及列表刷新
// refreshConstraintList/selectedLineParams/loadSelectedConstraintLine/
// newConstraintLineParams。
#include "constraintpage.h"
#include "constraintpage_internal.h"

#include "pageshared.h"

#include "../../workflow/workflows.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QPushButton>
#include <QSet>
#include <QSpinBox>
#include <QVBoxLayout>

using namespace paleo::pagesinternal;
using namespace paleo::constraintpage_internal;

void ConstraintPage::buildConstraintArea( QWidget *content, QVBoxLayout *lay, QComboBox *horizons,
                                          QLabel *cellCaption )
{
  // 方向 96：高级参数区五个逐线 spin、其布局 adv 与 methodCaption/cell 在族段
  // 迁移/保存时消费——objectName 命中即取（ctor 期解析，与原 ctor 局部指针
  // 等价；cellCaption 无 objectName，经参数传入）。
  auto *advancedSection = findChild<QWidget *>( QStringLiteral( "factorAdvancedSection" ) );
  auto *adv = advancedSection ? qobject_cast<QVBoxLayout *>( advancedSection->layout() ) : nullptr;
  auto *ratio = findChild<QDoubleSpinBox *>( QStringLiteral( "factorDirectionRatioSpin" ) );
  auto *influence = findChild<QDoubleSpinBox *>( QStringLiteral( "factorInfluenceSpin" ) );
  auto *core = findChild<QDoubleSpinBox *>( QStringLiteral( "factorCoreSpin" ) );
  auto *softStrength = findChild<QDoubleSpinBox *>( QStringLiteral( "factorSoftStrengthSpin" ) );
  auto *softRadius = findChild<QDoubleSpinBox *>( QStringLiteral( "factorSoftRadiusSpin" ) );
  auto *methodCaption = findChild<QLabel *>( QStringLiteral( "factorMethodCaption" ) );
  auto *cell = findChild<QDoubleSpinBox *>( QStringLiteral( "factorCellSizeSpin" ) );

  lay->addSpacing(PaleoTheme::tokens().spacingMd); // spacing.md：单因素区与约束区分组
  auto *constraintCaption = caption(tr("约束线"), this);
  lay->addWidget(constraintCaption);
  auto *list = new QListWidget( this );
  list->setObjectName( QStringLiteral( "constraintList" ) );
  list->setAccessibleName( tr( "约束列表" ) );
  list->setSelectionMode(QAbstractItemView::ExtendedSelection);
  connect(list, &QListWidget::itemSelectionChanged, this, [this, list] {
    QStringList ids;
    for (const auto *item : list->selectedItems())
      ids.append(item->data(Qt::UserRole).toString());
    auto *h = child<QComboBox>(this, "horizonCombo");
    emit constraintSelectionChanged(h ? h->currentText() : QString(), ids);
    updateFactorActionStates();
  });
  lay->addWidget( list, 1 );
  connect( list, &QListWidget::currentItemChanged, this, [this]( QListWidgetItem *, QListWidgetItem * ) {
    loadSelectedConstraintLine();
    if ( auto *removeButton = findChild<QPushButton *>( QStringLiteral( "constraintDeleteButton" ) ) )
    {
      auto *rows = findChild<QListWidget *>( QStringLiteral( "constraintList" ) );
      removeButton->setEnabled( rows && rows->currentItem() );
    }
  } );
  // 方向23：列表右键——语义切换（五种 Semantic 词表）与删除。
  list->setContextMenuPolicy( Qt::CustomContextMenu );
  connect( list, &QListWidget::customContextMenuRequested, this,
           [this, list]( const QPoint &pos ) {
             QListWidgetItem *item = list->itemAt( pos );
             if ( !item )
               return;
             const QString id = item->data( Qt::UserRole ).toString();
             auto *horizons = child<QComboBox>( this, "horizonCombo" );
             const QString horizon = horizons ? horizons->currentText() : QString();
             QMenu menu( this );
             QMenu *semanticMenu = menu.addMenu( tr( "切换语义" ) );
             const QVector<QPair<QString, QString>> semantics{
               { tr( "硬屏障" ), QStringLiteral( "hard_barrier" ) },
               { tr( "方向引导" ), QStringLiteral( "direction_guide" ) },
               { tr( "解释软边界" ), QStringLiteral( "interpretive_boundary" ) },
               { tr( "等值停止" ), QStringLiteral( "contour_stop" ) },
               { tr( "制图绕行" ), QStringLiteral( "cartographic_detour" ) },
             };
             for ( const auto &[label, token] : semantics )
             {
               connect( semanticMenu->addAction( label ), &QAction::triggered, this,
                        [this, horizon, id, token] {
                          emit constraintSemanticChangeRequested( horizon, id, token );
                        } );
             }
             connect( menu.addAction( tr( "删除约束" ) ), &QAction::triggered, this,
                      [this, horizon, id] { emit constraintDeleteRequested( horizon, id ); } );
             menu.exec( list->viewport()->mapToGlobal( pos ) );
           } );

  lay->addWidget( caption( tr( "约束语义" ), content ) );
  auto *semantic = new QComboBox( content );
  semantic->setObjectName( QStringLiteral( "constraintSemanticCombo" ) );
  semantic->addItem( tr( "硬屏障" ), QStringLiteral( "hard_barrier" ) );
  semantic->addItem( tr( "方向引导" ), QStringLiteral( "direction_guide" ) );
  semantic->addItem( tr( "解释软边界" ), QStringLiteral( "interpretive_boundary" ) );
  semantic->addItem( tr( "等值停止" ), QStringLiteral( "contour_stop" ) );
  semantic->addItem( tr( "制图绕行" ), QStringLiteral( "cartographic_detour" ) );
  semantic->setAccessibleName( tr( "约束语义" ) );
  lay->addWidget( semantic );
  auto *blockMode = new QComboBox(content);
  blockMode->setObjectName(QStringLiteral("constraintBlockModeCombo"));
  blockMode->addItem(tr("完全阻断"), QStringLiteral("full_block"));
  blockMode->addItem(tr("仅显示停线"), QStringLiteral("display_only"));
  blockMode->addItem(tr("不阻断"), QStringLiteral("none"));
  blockMode->setAccessibleName(tr("阻断方式"));
  lay->addWidget(blockMode);
  auto *lineAngle = new QDoubleSpinBox(content);
  lineAngle->setObjectName(QStringLiteral("constraintAngleSpin"));
  lineAngle->setRange(-1, 360);
  lineAngle->setSpecialValueText(tr("保留线条方向"));
  lineAngle->setValue(-1);
  lineAngle->setSuffix(tr(" °"));
  lineAngle->setToolTip(tr("从北顺时针；保存时绕线条中心旋转几何，单因素使用同一条线的方向"));
  lineAngle->setAccessibleName(tr("约束方向角"));
  useMono(lineAngle);
  lay->addWidget(lineAngle);
  auto *batchType = new QPushButton(tr("所选约束批量改型"), content);
  batchType->setObjectName(QStringLiteral("constraintBatchTypeButton"));
  batchType->setEnabled(false);
  batchType->setToolTip(tr("在列表中按 Ctrl 或 Shift 多选约束"));
  lay->addWidget(batchType);
  connect(batchType, &QPushButton::clicked, this, [this, list, semantic] {
    QStringList ids;
    for (const auto *item : list->selectedItems())
      ids.append(item->data(Qt::UserRole).toString());
    auto *horizons = child<QComboBox>(this, "horizonCombo");
    emit constraintParametersRequested(horizons ? horizons->currentText() : QString(), ids,
                                       {{QStringLiteral("semantic"), semantic->currentData()}});
  });
  auto *saveLine = new QPushButton( tr( "保存约束参数" ), content );
  saveLine->setObjectName( QStringLiteral( "constraintParamSaveButton" ) );
  saveLine->setEnabled( false );
  saveLine->setToolTip( tr( "先在约束列表中选择一条线" ) );
  lay->addWidget( saveLine );
  connect( saveLine, &QPushButton::clicked, this, [this, semantic, blockMode, lineAngle, ratio, influence, core, softStrength, softRadius] {
    auto *rows = child<QListWidget>( this, "constraintList" );
    QListWidgetItem *item = rows ? rows->currentItem() : nullptr;
    if ( !item )
      return;
    QVariantMap lineParams = selectedLineParams();
    lineParams.insert( QStringLiteral( "semantic" ), semantic->currentData().toString() );
    lineParams.insert( QStringLiteral( "ratio" ), ratio->value() );
    lineParams.insert( QStringLiteral( "influenceRadius" ), influence->value() );
    lineParams.insert( QStringLiteral( "coreRadius" ), core->value() );
    lineParams.insert( QStringLiteral( "softStrength" ), softStrength->value() );
    lineParams.insert( QStringLiteral( "softRadius" ), softRadius->value() );
    if (blockMode->currentIndex() >= 0)
      lineParams.insert(QStringLiteral("blockMode"), blockMode->currentData());
    if (lineAngle->value() >= 0)
      lineParams.insert(QStringLiteral("geometryAzimuth"), lineAngle->value());
    auto *horizons = child<QComboBox>(this, "horizonCombo");
    emit constraintParametersRequested(horizons ? horizons->currentText() : QString(),
                                       {item->data(Qt::UserRole).toString()}, lineParams);
  } );

  lay->addWidget( caption( tr( "相代码" ), content ) );
  auto *spin = new QSpinBox( this );
  spin->setObjectName( QStringLiteral( "faciesCodeSpin" ) );
  spin->setRange( 0, 9999 );
  spin->setAccessibleName( tr( "相代码" ) );
  lay->addWidget( spin );

  // Shape picker feeds ConstraintDrawController::startCapture's tool choice.
  auto *shape = new QComboBox( this );
  shape->setObjectName( QStringLiteral( "shapeCombo" ) );
  shape->addItem( tr( "约束线" ), QStringLiteral( "line" ) );
  shape->addItem( tr( "约束多边形" ), QStringLiteral( "polygon" ) );
  shape->addItem( tr( "约束矩形" ), QStringLiteral( "rect" ) );
  shape->addItem( tr( "约束点" ), QStringLiteral( "point" ) );
  shape->addItem( tr( "约束圆" ), QStringLiteral( "circle" ) );
  shape->addItem( tr( "约束椭圆" ), QStringLiteral( "ellipse" ) );
  lay->addWidget( shape );

  auto *draw = new QPushButton( tr( "绘制约束" ), this );
  draw->setObjectName( QStringLiteral( "drawButton" ) );
  lay->addWidget( draw );
  connect( draw, &QPushButton::clicked, this, [this, horizons, shape, spin] {
    emit drawConstraintRequested( horizons->currentText(),
                                  shape->currentData().toString(), spin->value() );
  } );

  // ---- 类型化约束线两入口（方向线/打断线——03_Constraints 组的类型化
  // 约束；shape 决定画布工具，constraintType 进 ConstraintStore 词表）------
  // 两个类型化入口是同组次要动作——归并为一行工具排，不再各占全宽。
  auto *typedRow = new QHBoxLayout();
  typedRow->setSpacing(PaleoTheme::tokens().spacingXs); // xs
  auto *direction = new QPushButton( tr( "画方向线" ), this );
  direction->setObjectName( QStringLiteral( "directionButton" ) );
  typedRow->addWidget( direction );
  connect( direction, &QPushButton::clicked, this, [this, horizons, spin] {
    emit drawTypedConstraintRequested( horizons->currentText(), QStringLiteral( "line" ),
                                       QStringLiteral( "direction_line" ), spin->value() );
  } );
  auto *breakLine = new QPushButton( tr( "画打断线" ), this );
  breakLine->setObjectName( QStringLiteral( "breakLineButton" ) );
  typedRow->addWidget( breakLine );
  connect( breakLine, &QPushButton::clicked, this, [this, horizons, spin] {
    emit drawTypedConstraintRequested( horizons->currentText(), QStringLiteral( "line" ),
                                       QStringLiteral( "break_line" ), spin->value() );
  } );
  lay->addLayout( typedRow );
  auto *softRow = new QHBoxLayout();
  softRow->setSpacing(PaleoTheme::tokens().spacingXs);
  auto *softButton = new QPushButton( tr( "画软边界" ), content );
  softButton->setObjectName( QStringLiteral( "softBoundaryButton" ) );
  softRow->addWidget( softButton );
  connect( softButton, &QPushButton::clicked, this, [this, horizons, spin] {
    emit drawTypedConstraintRequested( horizons->currentText(), QStringLiteral( "line" ),
                                       QStringLiteral( "interpretive_boundary" ), spin->value() );
  } );
  auto *stopButton = new QPushButton( tr( "画等值停止" ), content );
  stopButton->setObjectName( QStringLiteral( "contourStopButton" ) );
  softRow->addWidget( stopButton );
  connect( stopButton, &QPushButton::clicked, this, [this, horizons, spin] {
    emit drawTypedConstraintRequested( horizons->currentText(), QStringLiteral( "line" ),
                                       QStringLiteral( "contour_stop" ), spin->value() );
  } );
  auto *detourButton = new QPushButton( tr( "画制图绕行" ), content );
  detourButton->setObjectName( QStringLiteral( "cartographicDetourButton" ) );
  softRow->addWidget( detourButton );
  connect( detourButton, &QPushButton::clicked, this, [this, horizons, spin] {
    emit drawTypedConstraintRequested( horizons->currentText(), QStringLiteral( "line" ),
                                       QStringLiteral( "cartographic_detour" ), spin->value() );
  } );
  lay->addLayout( softRow );
  // ---- 方向23：已绘约束线编辑面（顶点编辑 / 删除；语义切换走列表右键）------
  auto *editRow = new QHBoxLayout();
  editRow->setSpacing(PaleoTheme::tokens().spacingXs); // xs
  auto *vertexEdit = new QPushButton( tr( "编辑约束线" ), content );
  vertexEdit->setObjectName( QStringLiteral( "constraintVertexEditButton" ) );
  vertexEdit->setAccessibleName( tr( "编辑约束线" ) );
  editRow->addWidget( vertexEdit );
  connect( vertexEdit, &QPushButton::clicked, this, [this, horizons] {
    emit editConstraintVerticesRequested( horizons->currentText() );
  } );
  auto *removeButton = new QPushButton( tr( "删除选中约束" ), content );
  removeButton->setObjectName( QStringLiteral( "constraintDeleteButton" ) );
  removeButton->setAccessibleName( tr( "删除选中约束" ) );
  removeButton->setEnabled( false );
  editRow->addWidget( removeButton );
  const auto selectedConstraintId = [this]() -> QString {
    auto *list = findChild<QListWidget *>( QStringLiteral( "constraintList" ) );
    if ( !list )
      return QString();
    QListWidgetItem *item = list->currentItem();
    return item ? item->data( Qt::UserRole ).toString() : QString();
  };
  connect( removeButton, &QPushButton::clicked, this, [this, horizons, selectedConstraintId] {
    const QString id = selectedConstraintId();
    if ( !id.isEmpty() )
      emit constraintDeleteRequested( horizons->currentText(), id );
  } );
  lay->addLayout( editRow );
  // ---- 类型化约束线 end ----------------------------------------------------

  auto *constraints = new QWidget(content);
  constraints->setObjectName(QStringLiteral("constraintFamiliesSection"));
  auto *constraintLay = panelLayout(constraints);
  const int start = lay->indexOf(constraintCaption);
  while (lay->count() > start) {
    QLayoutItem *item = lay->takeAt(start);
    constraintLay->addItem(item);
  }
  // 两族入口排在列表/编辑之前；通用形状与逐线编辑留在次级展开面。
  auto *details = new CollapsibleSection(tr("编辑已绘约束 / 其他形状"), constraints);
  details->setObjectName(QStringLiteral("constraintDetailsSection")); details->setExpanded(false);
  while (constraintLay->count() > 1) details->containerLayout()->addItem(constraintLay->takeAt(1));
  const auto family = [constraintLay, constraints](const char *name, const QString &title, const QString &note) {
    auto *group = new QWidget(constraints); group->setObjectName(QString::fromLatin1(name));
    auto *layout = panelLayout(group); layout->addWidget(caption(title, group));
    auto *label = new QLabel(note, group); label->setWordWrap(true);
    PaleoTheme::applyThemedStyleSheet(label, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
    layout->addWidget(label); constraintLay->addWidget(group); return layout;
  };
  auto *directionFamily = family("directionFamilySection", tr("方向线族"),
      tr("方向引导改变沿线权重；软边界减弱跨线影响；制图绕行只调整解释性等值线。"));
  auto *directionType = new QComboBox(constraints); directionType->setObjectName(QStringLiteral("directionFamilyTypeCombo"));
  directionType->setAccessibleName(tr("方向线族语义"));
  directionType->addItem(tr("方向引导"), QStringLiteral("direction_line"));
  directionType->addItem(tr("解释软边界"), QStringLiteral("interpretive_boundary"));
  directionType->addItem(tr("制图绕行"), QStringLiteral("cartographic_detour"));
  directionFamily->addWidget(directionType); typedRow->removeWidget(direction); directionFamily->addWidget(direction);
  moveParamWidget( adv, ratio, directionFamily ); moveParamWidget( adv, influence, directionFamily ); moveParamWidget( adv, core, directionFamily );
  moveParamWidget( adv, softStrength, details->containerLayout() ); moveParamWidget( adv, softRadius, details->containerLayout() );
  auto *breakFamily = family("breakFamilySection", tr("打断线族"),
      tr("硬屏障阻断跨线传播；等值停止终止提线。仅显示停线不改变插值数值。"));
  auto *breakType = new QComboBox(constraints); breakType->setObjectName(QStringLiteral("breakFamilyTypeCombo"));
  breakType->setAccessibleName(tr("打断线族语义"));
  breakType->addItem(tr("硬屏障"), QStringLiteral("break_line"));
  breakType->addItem(tr("等值停止"), QStringLiteral("contour_stop"));
  breakFamily->addWidget(breakType); typedRow->removeWidget(breakLine); breakFamily->addWidget(breakLine);
  details->containerLayout()->removeWidget(blockMode); breakFamily->addWidget(caption(tr("阻断方式"), constraints)); breakFamily->addWidget(blockMode);
  constraintLay->addWidget(details);
  lay->insertWidget(lay->indexOf(methodCaption), constraints);
  lay->removeWidget(cellCaption); lay->removeWidget(cell);
  lay->insertWidget(lay->indexOf(methodCaption) + 2, cellCaption);
  lay->insertWidget(lay->indexOf(cellCaption) + 1, cell);
  // 参数不再藏在高级区，逐线比值仍归约束族（其是否适用由方法联动说明）。
  direction->disconnect(this); breakLine->disconnect(this);
  connect(direction, &QPushButton::clicked, this, [this, horizons, spin, directionType] {
    emit drawTypedConstraintRequested(horizons->currentText(), QStringLiteral("line"), directionType->currentData().toString(), spin->value());
  });
  connect(breakLine, &QPushButton::clicked, this, [this, horizons, spin, breakType] {
    emit drawTypedConstraintRequested(horizons->currentText(), QStringLiteral("line"), breakType->currentData().toString(), spin->value());
  });
  // 将旧三个入口归入次级面，objectName 和信号兼容。
  for (QPushButton *button : {softButton, stopButton, detourButton}) {
    softRow->removeWidget(button); details->containerLayout()->addWidget(button);
  }
}

QVariantMap ConstraintPage::selectedLineParams() const
{
  QVariantMap params;
  auto *rows = child<QListWidget>( const_cast<ConstraintPage *>( this ), "constraintList" );
  const QListWidgetItem *item = rows ? rows->currentItem() : nullptr;
  if ( !item )
    return params;
  const QString json = item->data( Qt::UserRole + 1 ).toString();
  if ( json.isEmpty() )
    return params;
  const QJsonDocument doc = QJsonDocument::fromJson( json.toUtf8() );
  if ( !doc.isObject() )
    return params;
  return doc.object().toVariantMap();
}

void ConstraintPage::loadSelectedConstraintLine()
{
  const QVariantMap params = selectedLineParams();
  if (auto *mode = child<QComboBox>(this, "constraintBlockModeCombo"))
  {
    const QString token = params.value(QStringLiteral("blockMode"), QStringLiteral("full_block")).toString();
    mode->setCurrentIndex(mode->findData(token));
  }
  if (auto *angle = child<QDoubleSpinBox>(this, "constraintAngleSpin"))
    angle->setValue(-1);
  auto *semantic = child<QComboBox>( this, "constraintSemanticCombo" );
  if ( semantic && params.contains( QStringLiteral( "semantic" ) ) )
  {
    const int idx = semantic->findData( params.value( QStringLiteral( "semantic" ) ) );
    if ( idx >= 0 )
      semantic->setCurrentIndex( idx );
  }
  const auto setSpin = [this]( const char *name, const QString &key, const QVariantMap &source ) {
    auto *spin = child<QDoubleSpinBox>( this, name );
    if ( spin && source.contains( key ) )
      spin->setValue( source.value( key ).toDouble() );
  };
  setSpin( "factorDirectionRatioSpin", QStringLiteral( "ratio" ), params );
  setSpin( "factorInfluenceSpin", QStringLiteral( "influenceRadius" ), params );
  setSpin( "factorCoreSpin", QStringLiteral( "coreRadius" ), params );
  setSpin( "factorSoftStrengthSpin", QStringLiteral( "softStrength" ), params );
  setSpin( "factorSoftRadiusSpin", QStringLiteral( "softRadius" ), params );
  updateFactorActionStates();
}

void ConstraintPage::refreshConstraintList()
{
  auto *list = child<QListWidget>( this, "constraintList" );
  if ( !list )
    return;
  const QString keep = list->currentItem() ? list->currentItem()->data( Qt::UserRole ).toString() : QString();
  auto *wf = qobject_cast<ConstraintWorkflow *>( property( kWfProp ).value<QObject *>() );
  auto *horizons = child<QComboBox>( this, "horizonCombo" );
  const QString horizon = horizons ? horizons->currentText() : QString();
  QSet<QString> selected;
  for (const auto *item : list->selectedItems())
    selected.insert(item->data(Qt::UserRole).toString());
  list->blockSignals( true );
  list->clear();
  if ( wf )
  {
    const QVector<QVariantMap> rows = wf->loadConstraints( horizon );
    for ( const QVariantMap &row : rows )
    {
      const QString id = row.value( QStringLiteral( "id" ) ).toString();
      const QString type = row.value( QStringLiteral( "type" ) ).toString();
      auto *item = new QListWidgetItem( tr( "%1 · %2" ).arg( id, type ), list );
      item->setData( Qt::UserRole, id );
      item->setData( Qt::UserRole + 1, row.value( QStringLiteral( "params_json" ) ).toString() );
      if ( id == keep )
        list->setCurrentItem(item, QItemSelectionModel::NoUpdate);
      item->setSelected(selected.contains(id));
    }
  }
  list->blockSignals( false );
  loadSelectedConstraintLine();
}

QVariantMap ConstraintPage::newConstraintLineParams(const QString &type) const {
  const auto spin = [this](const char *name) { return findChild<QDoubleSpinBox *>(QString::fromLatin1(name))->value(); };
  QVariantMap params;
  if (type == QLatin1String("direction_line")) {
    params.insert(QStringLiteral("ratio"), spin("factorDirectionRatioSpin"));
    params.insert(QStringLiteral("influenceRadius"), spin("factorInfluenceSpin"));
    params.insert(QStringLiteral("coreRadius"), spin("factorCoreSpin"));
  }
  if (type == QLatin1String("interpretive_boundary")) {
    params.insert(QStringLiteral("softStrength"), spin("factorSoftStrengthSpin"));
    params.insert(QStringLiteral("softRadius"), spin("factorSoftRadiusSpin"));
  }
  if (type == QLatin1String("break_line") || type == QLatin1String("contour_stop"))
    params.insert(QStringLiteral("blockMode"), findChild<QComboBox *>(QStringLiteral("constraintBlockModeCombo"))->currentData());
  return params;
}
