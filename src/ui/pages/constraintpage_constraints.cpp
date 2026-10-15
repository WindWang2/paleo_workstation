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
#include "../../workflow/constraintuseplan.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHash>
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
  auto *constraintCaption = caption(tr("1 · 绘制或导入间隔线、方向线"), this);
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
  auto *direction = new QPushButton(tr("绘制方向线"), this);
  direction->setObjectName( QStringLiteral( "directionButton" ) );
  typedRow->addWidget( direction );
  connect( direction, &QPushButton::clicked, this, [this, horizons, spin] {
    emit drawTypedConstraintRequested( horizons->currentText(), QStringLiteral( "line" ),
                                       QStringLiteral( "direction_line" ), spin->value() );
  } );
  auto *breakLine = new QPushButton(tr("绘制间隔线"), this);
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
  auto *breakFamily =
      family("breakFamilySection", tr("间隔线族"),
             tr("硬屏障阻断跨线传播；等值停止终止提线。仅显示停线不改变插值数值。"));
  auto *breakType = new QComboBox(constraints); breakType->setObjectName(QStringLiteral("breakFamilyTypeCombo"));
  breakType->setAccessibleName(tr("间隔线族语义"));
  breakType->addItem(tr("硬屏障"), QStringLiteral("break_line"));
  breakType->addItem(tr("等值停止"), QStringLiteral("contour_stop"));
  breakFamily->addWidget(breakType); typedRow->removeWidget(breakLine); breakFamily->addWidget(breakLine);
  details->containerLayout()->removeWidget(blockMode); breakFamily->addWidget(caption(tr("阻断方式"), constraints)); breakFamily->addWidget(blockMode);
  for (const auto &entry :
       QVector<QPair<QVBoxLayout *, QString>>{{breakFamily, QStringLiteral("barrier")},
                                              {directionFamily, QStringLiteral("direction")}}) {
    auto *import = new QPushButton(entry.second == QLatin1String("barrier") ? tr("导入间隔线…")
                                                                            : tr("导入方向线…"),
                                   constraints);
    import->setObjectName(entry.second + QStringLiteral("ImportButton"));
    import->setToolTip(
        tr("导入本工程局部米制坐标的 GPKG、SHP 或 GeoJSON；原文件和约束快照均保存到数据管理。"));
    entry.first->addWidget(import);
    connect(import, &QPushButton::clicked, this, [this, horizons, role = entry.second] {
      emit constraintImportRequested(horizons->currentText(), role);
    });
  }
  auto *hint = new QLabel(constraints);
  hint->setObjectName(QStringLiteral("factorConstraintHint"));
  hint->setWordWrap(true);
  constraintLay->addWidget(hint);
  constraintLay->removeWidget(breakFamily->parentWidget());
  constraintLay->insertWidget(1, breakFamily->parentWidget());
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

namespace
{

QString constraintRoleLabel( const QString &role )
{
  if ( role == QLatin1String( "hard_barrier" ) )
    return ConstraintPage::tr( "硬屏障" );
  if ( role == QLatin1String( "direction_guide" ) )
    return ConstraintPage::tr( "局部方向" );
  if ( role == QLatin1String( "global_direction" ) )
    return ConstraintPage::tr( "全域平均方向" );
  if ( role == QLatin1String( "soft_boundary" ) )
    return ConstraintPage::tr( "软边界" );
  if ( role == QLatin1String( "hull_clip" ) )
    return ConstraintPage::tr( "凸包裁剪" );
  return role;
}

QString constraintReasonLabel( const QString &reason )
{
  const QString token = reason.section( QLatin1Char( ':' ), 0, 0 );
  static const QHash<QString, QString> labels = {
      { QStringLiteral( "disabled" ), ConstraintPage::tr( "已停用" ) },
      { QStringLiteral( "non_blocking" ), ConstraintPage::tr( "非阻断模式" ) },
      { QStringLiteral( "invalid_params" ), ConstraintPage::tr( "参数非法" ) },
      { QStringLiteral( "short_line" ), ConstraintPage::tr( "几何无效：少于 2 个顶点" ) },
      { QStringLiteral( "zero_length" ), ConstraintPage::tr( "几何无效：零长度" ) },
      { QStringLiteral( "self_intersecting" ), ConstraintPage::tr( "几何无效：自相交" ) },
      { QStringLiteral( "non_finite_coordinate" ), ConstraintPage::tr( "几何无效：坐标非有限" ) },
      { QStringLiteral( "empty_geometry" ), ConstraintPage::tr( "几何为空" ) },
      { QStringLiteral( "not_line" ), ConstraintPage::tr( "不是线几何" ) },
      { QStringLiteral( "method_does_not_read_line_constraints" ), ConstraintPage::tr( "当前方法不读约束线" ) },
      { QStringLiteral( "unsupported_by_method" ), ConstraintPage::tr( "当前方法不支持该语义" ) },
      { QStringLiteral( "cartographic_only" ), ConstraintPage::tr( "仅作用于制图" ) },
      { QStringLiteral( "contour_stop_does_not_change_analysis" ), ConstraintPage::tr( "仅作用于制图（停线）" ) },
      { QStringLiteral( "cartographic_detour_not_analysis_constraint" ), ConstraintPage::tr( "仅作用于制图（绕行）" ) },
      { QStringLiteral( "direction_ratio_not_above_one" ), ConstraintPage::tr( "方向比值 ≤ 1" ) },
      { QStringLiteral( "direction_ratio_out_of_range" ), ConstraintPage::tr( "方向比值越界（运行将被拒绝）" ) },
      { QStringLiteral( "soft_strength_zero" ), ConstraintPage::tr( "软边界强度为 0" ) },
      { QStringLiteral( "soft_strength_out_of_range" ), ConstraintPage::tr( "软边界强度越界（运行将被拒绝）" ) },
      { QStringLiteral( "global_aniso_ratio_not_above_one" ), ConstraintPage::tr( "全局各向异性比值 ≤ 1" ) },
      { QStringLiteral( "outside_compute_domain" ), ConstraintPage::tr( "在计算域外" ) },
      // U6：constraintsemantics 实际发出、此前缺译的 token（行号语境：
      // unknown_type 见 resolveConstraintSemantics 旧行只认 type 列；ignored
      // 是 semantic 与 ignoredReason 双空的兜底；unknown_method 是词表外方法
      // 落到 decideByMethod 尾部；其余三个是 structural_idw 开关关闭路径）。
      { QStringLiteral( "unknown_type" ), ConstraintPage::tr( "未识别的约束类型" ) },
      { QStringLiteral( "other_horizon" ), ConstraintPage::tr( "属于其他层位，当前层位不参与" ) },
      { QStringLiteral( "ignored" ), ConstraintPage::tr( "已忽略（未记录原因）" ) },
      { QStringLiteral( "unknown_method" ), ConstraintPage::tr( "未知的成图方法" ) },
      { QStringLiteral( "directions_disabled_by_request" ), ConstraintPage::tr( "本次请求已停用方向线" ) },
      { QStringLiteral( "interpretive_strength_zero_or_barriers_disabled" ),
        ConstraintPage::tr( "解释性强度为 0 或屏障已停用（仅停等值线）" ) },
  };
  // 前缀 token（token:detail）：detail 是域层产出——unknown_type 带裸 type
  //（通用「绘制约束」无 params_json 的主链可达原因），invalid_params 带中文
  // 错误详情。拼进译文/直显详情，不整段回显英文 token。
  if ( token == QLatin1String( "unknown_type" ) )
  {
    const QString type = reason.section( QLatin1Char( ':' ), 1 );
    return type.isEmpty() ? labels.value( token )
                          : ConstraintPage::tr( "未识别的约束类型：%1" ).arg( type );
  }
  if ( token == QLatin1String( "invalid_params" ) )
  {
    const QString detail = reason.section( QLatin1Char( ':' ), 1 );
    return detail.isEmpty() ? reason : detail;
  }
  return labels.value( token, reason );
}

} // namespace

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
  QVariantList signatureRows;
  if ( wf )
  {
    const QVector<QVariantMap> rows = wf->loadConstraints( horizon );
    for ( const QVariantMap &row : rows )
    {
      signatureRows << row;
      const QString id = row.value( QStringLiteral( "id" ) ).toString();
      const QString type = row.value( QStringLiteral( "type" ) ).toString();
      auto *item = new QListWidgetItem( tr( "%1 · %2" ).arg( id, type ), list );
      item->setData( Qt::UserRole, id );
      item->setData( Qt::UserRole + 1, row.value( QStringLiteral( "params_json" ) ).toString() );
      item->setData( Qt::UserRole + 3, type );
      if ( id == keep )
        list->setCurrentItem(item, QItemSelectionModel::NoUpdate);
      item->setSelected(selected.contains(id));
    }
  }
  list->blockSignals( false );
  setProperty( "paleo.page.constraintrows", signatureRows );
  applyConstraintUsePlan();
  const QByteArray signature =
      QJsonDocument::fromVariant(signatureRows).toJson(QJsonDocument::Compact);
  const auto key = QStringLiteral("paleo.page.constraintsignature.") + horizon;
  if (property(key.toUtf8().constData()).isValid() &&
      property(key.toUtf8().constData()).toByteArray() != signature)
    markInputsStale();
  setProperty(key.toUtf8().constData(), signature);
  loadSelectedConstraintLine();
}

void ConstraintPage::applyConstraintUsePlan()
{
  // P1-A：计数/就绪门/逐条状态全部来自统一语义 + 能力判定（不数 type 字符串）。
  // 「可用」= 启用 + 参数合法 + 几何有效（与方法无关，决定就绪门）；
  // 「参与」= 当前成图方法实际数值消费（与 QC constraint_receipts 同口径）。
  QVector<QVariantMap> rows;
  for ( const QVariant &row : property( "paleo.page.constraintrows" ).toList() )
    rows << row.toMap();
  auto *horizons = child<QComboBox>( this, "horizonCombo" );
  auto *method = child<QComboBox>( this, "factorMethodCombo" );
  paleo::singlefactor::ConstraintMethodContext context;
  context.methodId = method ? method->currentData().toString() : QString();
  context.horizon = horizons ? horizons->currentText() : QString();
  // U6：结构面三开关页面暂无专属控件——从 paleo.page.* 动态属性注入计划
  // 上下文（缺省与引擎参数一致：constraintfactorjobs_factor.cpp 的
  // enableBarriers/enableDirections/interpretiveStrength 均缺省 true）。
  // 仍只喂唯一解释器 planConstraintUse，不在页面另立判定；壳侧将来接真实
  // 开关时写同名属性即可。
  const auto structuralFlag = [this]( const char *name ) {
    const QVariant value = property( name );
    return !value.isValid() || value.toBool();
  };
  context.structuralEnableBarriers = structuralFlag( "paleo.page.structuralEnableBarriers" );
  context.structuralEnableDirections = structuralFlag( "paleo.page.structuralEnableDirections" );
  context.structuralInterpretiveStrengthPositive =
      structuralFlag( "paleo.page.structuralInterpretiveStrengthPositive" );
  const paleo::singlefactor::ConstraintUsePlan plan = paleo::singlefactor::planConstraintUse( rows, context );
  setProperty( "paleo.page.constraintuseplan", plan.summary );
  int untyped = 0; // U6 计数盲区：semantic 为空的行（unknown_type/non_blocking）不进任何可用桶
  if ( auto *list = child<QListWidget>( this, "constraintList" ) )
  {
    for ( int i = 0; i < list->count(); ++i )
    {
      QListWidgetItem *item = list->item( i );
      const QString id = item->data( Qt::UserRole ).toString();
      int parts = 0, used = 0;
      bool typed = false;
      QStringList roles, reasons;
      for ( const paleo::singlefactor::ConstraintUseDecision &d : plan.decisions )
      {
        if ( d.featureId != id )
          continue;
        ++parts;
        if ( !d.semantic.isEmpty() )
          typed = true;
        if ( d.consumed )
        {
          ++used;
          if ( !roles.contains( constraintRoleLabel( d.role ) ) )
            roles << constraintRoleLabel( d.role );
        }
        else if ( !reasons.contains( constraintReasonLabel( d.reason ) ) )
        {
          reasons << constraintReasonLabel( d.reason );
        }
      }
      if ( parts > 0 && !typed )
        ++untyped;
      const QString type = item->data( Qt::UserRole + 3 ).toString();
      QString status;
      if ( parts > 0 && used == parts )
        status = tr( "参与：%1" ).arg( roles.join( QStringLiteral( "、" ) ) );
      else if ( used > 0 )
        status = tr( "部分参与 %1/%2：%3；%4" ).arg( used ).arg( parts ).arg( roles.join( QStringLiteral( "、" ) ),
                                                                         reasons.join( QStringLiteral( "、" ) ) );
      else
        status = tr( "不参与：%1" ).arg( reasons.join( QStringLiteral( "、" ) ) );
      item->setText( tr( "%1 · %2 · %3" ).arg( id, type, status ) );
      item->setData( Qt::UserRole + 2, used > 0 );
      item->setToolTip( status );
    }
  }
  setProperty( "paleo.page.constraintsready", plan.availableBarriers > 0 && plan.availableDirections > 0 );
  if ( auto *hint = child<QLabel>( this, "factorConstraintHint" ) )
  {
    QString text = tr( "本层位：%1 条间隔线、%2 条方向线可用" ).arg( plan.availableBarriers ).arg( plan.availableDirections );
    if ( plan.availableSoft > 0 )
      text += tr( "、%1 条软边界" ).arg( plan.availableSoft );
    if ( plan.availableCartographic > 0 )
      text += tr( "、%1 条仅制图线" ).arg( plan.availableCartographic );
    if ( plan.disabled > 0 )
      text += tr( "；%1 条已停用" ).arg( plan.disabled );
    if ( plan.invalid > 0 )
      text += tr( "；%1 条无效（不参与计算）" ).arg( plan.invalid );
    if ( untyped > 0 )
      text += tr( "；%1 条未类型化" ).arg( untyped );
    text += tr( "。当前方法实际参与 %1 条" ).arg( plan.consumed );
    if ( plan.blocking > 0 )
      text += tr( "，%1 条参数越界会使运行被拒绝" ).arg( plan.blocking );
    text += tr( "。%1 绘制、导入自动保存；顶点编辑需保存编辑会话。准备两类线后，再提取井点数据。" )
                .arg( plan.capabilityNote );
    hint->setText( text );
  }
  updateFactorActionStates();
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
