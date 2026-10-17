// 层：视图
#include "editingtoolbar.h"
#include "../../qgis/faciespartitioneditor.h"
#include "../../qgis/faciespartitiontool.h"
#include "../paleoicons.h"
#include <QAction>
#include <QActionGroup>
#include <QComboBox>
#include <QMenu>
#include <QSignalBlocker>
#include <QToolBar>
#include <QToolButton>

void PaleoEditingToolbar::buildPartitionControls() {
  auto *menu = new QMenu(this);
  mActionSplitFacies = menu->addAction(PaleoIcons::qgisTheme("mActionSplitFeatures.svg"), tr("分割所选相面"));
  mActionSplitFacies->setObjectName("actionSplitFacies"); mActionSplitFacies->setCheckable(true);
  mActionSplitFacies->setProperty("hint", tr("先选面，再画分割线；各部分继承三级归属，公共边界同步插点"));
  mActionPaintFacies = menu->addAction(PaleoIcons::qgisTheme("mActionAddPolygon.svg"), tr("新建 / 覆盖相面"));
  mActionPaintFacies->setObjectName("actionPaintFacies"); mActionPaintFacies->setCheckable(true);
  mActionPaintFacies->setProperty("hint", tr("选择目标微相后画面；自动裁切原面，绘制范围限制在现有相图覆盖内"));
  mActionMergeFacies = menu->addAction(PaleoIcons::qgisTheme("mActionMergeFeatures.svg"), tr("合并所选相面"));
  mActionMergeFacies->setObjectName("actionMergeFacies");
  mActionMergeFacies->setProperty("hint", tr("合并相邻面；不同微相必须明确选择目标微相，所有来源与证据保留待复核"));
  connect(mActionMergeFacies, &QAction::triggered, this, &PaleoEditingToolbar::mergePartition);
  mActionFaciesOperations = new QAction(PaleoIcons::qgisTheme("mActionAddPolygon.svg"), tr("相面编辑"), this);
  mActionFaciesOperations->setObjectName("actionFaciesOperations"); mActionFaciesOperations->setMenu(menu);
  mPartitionButton = new QToolButton(mToolBar); mPartitionButton->setObjectName("partitionToolsButton");
  mPartitionButton->setDefaultAction(mActionFaciesOperations);
  mPartitionButton->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
  mPartitionButton->setMenu(menu); mPartitionButton->setPopupMode(QToolButton::InstantPopup);
  mToolBar->addWidget(mPartitionButton);
  mPartitionCode = new QComboBox(mToolBar); mPartitionCode->setObjectName("partitionFaciesCode");
  mPartitionCode->setMinimumContentsLength(14); mPartitionCode->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  mPartitionCode->setAccessibleName(tr("新建或合并后的目标微相")); mToolBar->addWidget(mPartitionCode);
  connect(mPartitionCode, &QComboBox::currentIndexChanged, this, [this] { updateActionStates(); });
}
void PaleoEditingToolbar::wireToolActions() {
  auto *group = new QActionGroup(this); group->setExclusive(true);
  for (auto *action : {mActionSelect, mActionAddPoint, mActionAddLine, mActionAddPolygon,
       mActionReshape, mActionMove, mActionDeleteFeatures, mActionVertexEdit, mActionSplitFacies, mActionPaintFacies}) {
    group->addAction(action);
    connect(action, &QAction::triggered, this, &PaleoEditingToolbar::onEditToolTriggered);
  }
}
void PaleoEditingToolbar::uncheckTools() {
  for (auto *action : {mActionSelect, mActionAddPoint, mActionAddLine, mActionAddPolygon,
       mActionReshape, mActionMove, mActionDeleteFeatures, mActionVertexEdit, mActionSplitFacies, mActionPaintFacies})
    action->setChecked(false);
}
void PaleoEditingToolbar::updatePartitionControls(const QString &noEdit) {
  auto *layer = currentLayer();
  const bool partition = FaciesPartitionEditor::isPartition(layer);
  const auto rows = FaciesPartitionEditor::choices(layer);
  const auto identity = FaciesPartitionEditor::layerKey(layer);
  if (mPartitionCode->property("schema").toList() != rows || mPartitionCode->property("layer").toString() != identity) {
    const QSignalBlocker blocked(mPartitionCode);
    mPartitionCode->clear(); mPartitionCode->addItem(tr("选择目标微相（合并可保留原类）"), 0);
    for (const auto &value : rows) { const auto row = value.toMap();
      mPartitionCode->addItem(row.value("name").toString(), row.value("code"));
      mPartitionCode->setItemData(mPartitionCode->count() - 1, row.value("name"), Qt::ToolTipRole); }
    mPartitionCode->setProperty("schema", rows); mPartitionCode->setProperty("layer", identity);
  }
  const auto reason = !noEdit.isEmpty() ? noEdit : !partition ? tr("请选择相面工作副本") : QString();
  mPartitionButton->setEnabled(reason.isEmpty()); mPartitionButton->setToolTip(reason.isEmpty() ? tr("保持三级归属与完整覆盖的相面编辑") : reason);
  mActionFaciesOperations->setEnabled(reason.isEmpty()); mActionFaciesOperations->setToolTip(mPartitionButton->toolTip());
  mPartitionCode->setVisible(partition); mPartitionCode->setEnabled(reason.isEmpty());
  mPartitionCode->setToolTip(reason.isEmpty() ? tr("相 → 亚相 → 微相；新建面须选择目标，分割直接继承原类") : reason);
  const auto gate = [](QAction *a, const QString &why) { a->setEnabled(why.isEmpty());
    a->setToolTip(why.isEmpty() ? a->property("hint").toString() : why); };
  gate(mActionSplitFacies, !reason.isEmpty() ? reason : FaciesPartitionEditor::selectionSize(layer) == 0 ? tr("请先选择需要分割的相面") : QString());
  gate(mActionPaintFacies, !reason.isEmpty() ? reason : mPartitionCode->currentData().toInt() == 0 ? tr("请先选择目标微相") : QString());
  gate(mActionMergeFacies, !reason.isEmpty() ? reason : FaciesPartitionEditor::selectionSize(layer) < 2 ? tr("请至少选择两个相邻相面") : QString());
  if (partition) {
    for (auto *action : {mActionMove, mActionReshape, mActionDeleteFeatures})
      gate(action, tr("相图须保持完整分区；请用共边节点、分割、覆盖绘制或合并编辑"));
    gate(mActionCrossLayerTopo, tr("三级相面共用基础分区，跨层独立撤销会破坏一致性；请在当前工作副本编辑"));
    gate(mActionAddPolygon, mActionPaintFacies->isEnabled() ? QString() : mActionPaintFacies->toolTip());
  }
}
void PaleoEditingToolbar::mergePartition() {
  if (!mActionMergeFacies->isEnabled() || !startEditing()) return;
  auto *layer = currentLayer(); QString error;
  if (!FaciesPartitionEditor::merge(layer, mPartitionCode->currentData().toInt(), &error)) emit editRefused(error);
  else emit featureEdited(FaciesPartitionEditor::layerKey(layer));
  updateActionStates();
}
