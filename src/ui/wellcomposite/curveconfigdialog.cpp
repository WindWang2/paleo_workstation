// 层：视图
#include "curveconfigdialog.h"
#include "ui/paleoicons.h"
#include "../paleotheme.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include "ui/notifications/notificationmanager.h"
#include <QVBoxLayout>

namespace WellComposite
{

static QString trackTypeLabel(TrackType type)
{
  switch (type)
  {
    case TrackType::DepthScale: return QCoreApplication::translate("CurveConfigDialog", "[深度标尺]");
    case TrackType::StratigraphyCompound: return QCoreApplication::translate("CurveConfigDialog", "[地层系统组]");
    case TrackType::FaciesCompound: return QCoreApplication::translate("CurveConfigDialog", "[沉积相]");
    case TrackType::Formation: return QCoreApplication::translate("CurveConfigDialog", "[地层分层]");
    case TrackType::Lithology: return QCoreApplication::translate("CurveConfigDialog", "[岩性分析]");
    case TrackType::Core: return QCoreApplication::translate("CurveConfigDialog", "[取心数据]");
    case TrackType::Symbol: return QCoreApplication::translate("CurveConfigDialog", "[沉积旋回/符号]");
    case TrackType::Text: return QCoreApplication::translate("CurveConfigDialog", "[解释文本]");
    case TrackType::Image: return QCoreApplication::translate("CurveConfigDialog", "[微观图像]");
    case TrackType::Curve: return QCoreApplication::translate("CurveConfigDialog", "[测井曲线]");
  }
  return QCoreApplication::translate("CurveConfigDialog", "[井道]");
}

CurveConfigDialog::CurveConfigDialog(WellCompositeCanvas *canvas, QWidget *parent)
  : QDialog(parent)
  , m_canvas(canvas)
{
  setWindowTitle(tr("测井道配置与排列管理"));
  resize(880, 560);
  setupUi();
  loadFromCanvas();
  populateTree();
  updateButtonStates();
}

void CurveConfigDialog::setupUi()
{
  auto *rootLay = new QVBoxLayout(this);
  rootLay->setContentsMargins(PaleoTheme::tokens().spacingMd, PaleoTheme::tokens().spacingMd, PaleoTheme::tokens().spacingMd, PaleoTheme::tokens().spacingMd);
  rootLay->setSpacing(PaleoTheme::tokens().spacingMd);

  // 顶部说明提示
  m_lblInfo = new QLabel(
      tr("提示：在左侧列表中可直接调整所有井道的排列顺序（上移/下移/置顶/置底）与显示勾选；多选 2-4 根曲线可合并为同道渲染，亦支持解散多曲线道。"),
      this);
  PaleoTheme::applyThemedStyleSheet(m_lblInfo, [] {
    const auto &t = PaleoTheme::tokens();
    return PaleoTheme::metricStyleSheet(QStringLiteral(
               "QLabel { background: %1; color: %2; border: 1px solid %3; "
               "border-radius: {rounded.sm}px; padding: {spacing.sm}px {spacing.md}px; font-size: {typography.body}pt; }"))
        .arg(t.surfaceAlt.name(), t.text.name(), t.border.name());
  });
  rootLay->addWidget(m_lblInfo);

  // 中间区域：左侧树形列表 + 右侧操作工具栏
  auto *midWidget = new QWidget(this);
  auto *midLay = new QHBoxLayout(midWidget);
  midLay->setContentsMargins(0, 0, 0, 0);
  midLay->setSpacing(PaleoTheme::tokens().spacingMd);

  m_tree = new QTreeWidget(midWidget);
  m_tree->setHeaderLabels({tr("井道 / 测井曲线"), tr("井道类别 / 单位"), tr("道宽 / 显示范围"), tr("形态"), tr("颜色"), tr("数据点数")});
  m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_tree->setAlternatingRowColors(true);
  m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  m_tree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
  m_tree->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
  m_tree->header()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
  // 选中态/hover 由壳级 PaleoTheme::itemViewStyleSheet 统一（primary 底 +
  // onPrimary 字）——不再自写 ::item:selected（原 surfaceAltRaised 变体与
  // 全局三分叉，goal/ui-experience-polish 收敛）。
  PaleoTheme::applyThemedStyleSheet(m_tree, [] {
    const auto &t = PaleoTheme::tokens();
    return PaleoTheme::metricStyleSheet(QStringLiteral(
               "QTreeWidget { border: 1px solid %1; border-radius: {rounded.sm}px; background: %2; font-size: {typography.body}pt; }"))
        .arg(t.border.name(), t.surface.name());
  });
  midLay->addWidget(m_tree, 1);

  // 右侧按钮栏
  auto *btnBox = new QWidget(midWidget);
  auto *btnLay = new QVBoxLayout(btnBox);
  btnLay->setContentsMargins(0, 0, 0, 0);
  btnLay->setSpacing(PaleoTheme::tokens().spacingSm);

  const auto themedBtnStyle = [] {
    const auto &t = PaleoTheme::tokens();
    return PaleoTheme::metricStyleSheet(QStringLiteral(
               "QPushButton { background: %1; border: 1px solid %2; border-radius: {rounded.sm}px; "
               "padding: {spacing.sm}px {spacing.md}px; font-size: {typography.body}pt; color: %3; text-align: left; }"
               "QPushButton:hover { background: %4; border-color: %5; }"
               "QPushButton:pressed { background: %2; }"
               "QPushButton:disabled { color: %5; background: %4; border-color: %2; }"))
        .arg(t.surface.name(), t.border.name(), t.text.name(),
             t.surfaceAlt.name(), t.textDisabled.name());
  };

  auto *lblOrderTitle = new QLabel(tr("井道顺序管理"), btnBox);
  PaleoTheme::applyThemedStyleSheet(lblOrderTitle, [] {
    return PaleoTheme::mutedCaptionStyleSheet() +
           PaleoTheme::metricStyleSheet(QStringLiteral(" font-weight: bold; font-size: {typography.label}pt; margin-top: {spacing.xs}px;"));
  });
  btnLay->addWidget(lblOrderTitle);

  m_btnMoveTop = new QPushButton(tr("置顶 ⬆"), btnBox);
  PaleoTheme::applyThemedStyleSheet(m_btnMoveTop, themedBtnStyle);
  m_btnMoveTop->setToolTip(tr("将选中的井道移动到综合柱状图最左侧"));
  m_btnMoveTop->setProperty("hint", m_btnMoveTop->toolTip());
  btnLay->addWidget(m_btnMoveTop);

  m_btnMoveUp = new QPushButton(tr("井道上移 ▲"), btnBox);
  PaleoTheme::applyThemedStyleSheet(m_btnMoveUp, themedBtnStyle);
  m_btnMoveUp->setToolTip(tr("将选中的井道向左/向上移动一个位置"));
  m_btnMoveUp->setProperty("hint", m_btnMoveUp->toolTip());
  btnLay->addWidget(m_btnMoveUp);

  m_btnMoveDown = new QPushButton(tr("井道下移 ▼"), btnBox);
  PaleoTheme::applyThemedStyleSheet(m_btnMoveDown, themedBtnStyle);
  m_btnMoveDown->setToolTip(tr("将选中的井道向右/向下移动一个位置"));
  m_btnMoveDown->setProperty("hint", m_btnMoveDown->toolTip());
  btnLay->addWidget(m_btnMoveDown);

  m_btnMoveBottom = new QPushButton(tr("置底 ⬇"), btnBox);
  PaleoTheme::applyThemedStyleSheet(m_btnMoveBottom, themedBtnStyle);
  m_btnMoveBottom->setToolTip(tr("将选中的井道移动到综合柱状图最右侧（如沉积相道）"));
  m_btnMoveBottom->setProperty("hint", m_btnMoveBottom->toolTip());
  btnLay->addWidget(m_btnMoveBottom);

  m_btnRename = new QPushButton(tr("重命名井道..."), btnBox);
  PaleoTheme::applyThemedStyleSheet(m_btnRename, themedBtnStyle);
  m_btnRename->setToolTip(tr("修改所选井道的道头显示标题"));
  m_btnRename->setProperty("hint", m_btnRename->toolTip());
  btnLay->addWidget(m_btnRename);

  btnLay->addSpacing(PaleoTheme::tokens().spacingSm);

  auto *lblCurveTitle = new QLabel(tr("曲线合并与解散"), btnBox);
  PaleoTheme::applyThemedStyleSheet(lblCurveTitle, [] {
    return PaleoTheme::mutedCaptionStyleSheet() +
           PaleoTheme::metricStyleSheet(QStringLiteral(" font-weight: bold; font-size: {typography.label}pt; margin-top: {spacing.xs}px;"));
  });
  btnLay->addWidget(lblCurveTitle);

  m_btnCombine = new QPushButton(tr("合并所选曲线 (2-4根)..."), btnBox);
  PaleoTheme::applyThemedStyleSheet(m_btnCombine, themedBtnStyle);
  m_btnCombine->setToolTip(tr("选中多根独立曲线后点击，将其合并入同一个曲线道（最多4根）"));
  m_btnCombine->setProperty("hint", m_btnCombine->toolTip());
  btnLay->addWidget(m_btnCombine);

  m_btnDissolve = new QPushButton(tr("解散所选道 (独立单道)"), btnBox);
  PaleoTheme::applyThemedStyleSheet(m_btnDissolve, themedBtnStyle);
  m_btnDissolve->setToolTip(tr("将选中的多曲线道拆开，每根曲线独占一道"));
  m_btnDissolve->setProperty("hint", m_btnDissolve->toolTip());
  btnLay->addWidget(m_btnDissolve);

  m_btnExtract = new QPushButton(tr("拆出为独立道"), btnBox);
  PaleoTheme::applyThemedStyleSheet(m_btnExtract, themedBtnStyle);
  m_btnExtract->setToolTip(tr("将选中的单根曲线移出当前道，成为单独井道"));
  m_btnExtract->setProperty("hint", m_btnExtract->toolTip());
  btnLay->addWidget(m_btnExtract);

  btnLay->addSpacing(PaleoTheme::tokens().spacingSm);

  m_btnResetDefault = new QPushButton(tr("恢复标准测井组合"), btnBox);
  PaleoTheme::applyThemedStyleSheet(m_btnResetDefault, themedBtnStyle);
  m_btnResetDefault->setToolTip(tr("按标准地质类别（岩性/三孔隙/电阻率）重置曲线道分道"));
  btnLay->addWidget(m_btnResetDefault);

  btnLay->addStretch(1);
  midLay->addWidget(btnBox);
  rootLay->addWidget(midWidget, 1);

  // 底部标准对话框操作栏
  auto *bottomLay = new QHBoxLayout();
  bottomLay->setContentsMargins(0, 0, 0, 0);
  bottomLay->setSpacing(PaleoTheme::tokens().spacingSm);

  m_btnApply = new QPushButton(tr("应用"), this);
  PaleoTheme::applyThemedStyleSheet(m_btnApply, [] {
    const auto &t = PaleoTheme::tokens();
    return PaleoTheme::metricStyleSheet(QStringLiteral(
               "QPushButton { background: %1; border: 1px solid %2; border-radius: {rounded.sm}px; padding: {spacing.sm}px {spacing.md}px; font-size: {typography.body}pt; color: %3; }"
               "QPushButton:hover { background: %4; border-color: %5; }"))
        .arg(t.surface.name(), t.border.name(), t.text.name(),
             t.surfaceAlt.name(), t.textDisabled.name());
  });
  bottomLay->addWidget(m_btnApply);

  bottomLay->addStretch(1);

  auto *dialogButtons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  dialogButtons->button(QDialogButtonBox::Ok)->setText(tr("确定"));
  dialogButtons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
  // 确定 = 主按钮（primary 合法三用途之一）
  PaleoTheme::applyThemedStyleSheet(dialogButtons->button(QDialogButtonBox::Ok), [] {
    const auto &t = PaleoTheme::tokens();
    return PaleoTheme::metricStyleSheet(QStringLiteral(
               "QPushButton { background: %1; color: %2; border: 1px solid %3; border-radius: {rounded.sm}px; padding: {spacing.sm}px {spacing.lg}px; font-size: {typography.body}pt; font-weight: bold; }"
               "QPushButton:hover { background: %3; }"))
        .arg(t.primary.name(), t.onPrimary.name(), t.primaryHover.name());
  });
  PaleoTheme::applyThemedStyleSheet(dialogButtons->button(QDialogButtonBox::Cancel), [] {
    const auto &t = PaleoTheme::tokens();
    return PaleoTheme::metricStyleSheet(QStringLiteral(
               "QPushButton { background: %1; border: 1px solid %2; border-radius: {rounded.sm}px; padding: {spacing.sm}px {spacing.md}px; font-size: {typography.body}pt; color: %3; }"
               "QPushButton:hover { background: %4; }"))
        .arg(t.surface.name(), t.border.name(), t.text.name(), t.surfaceAlt.name());
  });
  bottomLay->addWidget(dialogButtons);
  rootLay->addLayout(bottomLay);

  // 事件信号绑定
  connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &CurveConfigDialog::onTreeSelectionChanged);
  connect(m_tree, &QTreeWidget::itemChanged, this, &CurveConfigDialog::onTreeItemChanged);
  connect(m_btnMoveTop, &QPushButton::clicked, this, &CurveConfigDialog::onMoveTrackToTop);
  connect(m_btnMoveUp, &QPushButton::clicked, this, &CurveConfigDialog::onMoveTrackUp);
  connect(m_btnMoveDown, &QPushButton::clicked, this, &CurveConfigDialog::onMoveTrackDown);
  connect(m_btnMoveBottom, &QPushButton::clicked, this, &CurveConfigDialog::onMoveTrackToBottom);
  connect(m_btnRename, &QPushButton::clicked, this, &CurveConfigDialog::onRenameSelected);
  connect(m_btnCombine, &QPushButton::clicked, this, &CurveConfigDialog::onCombineSelected);
  connect(m_btnDissolve, &QPushButton::clicked, this, &CurveConfigDialog::onDissolveSelected);
  connect(m_btnExtract, &QPushButton::clicked, this, &CurveConfigDialog::onExtractCurve);
  connect(m_btnResetDefault, &QPushButton::clicked, this, &CurveConfigDialog::onResetDefault);

  connect(m_btnApply, &QPushButton::clicked, this, &CurveConfigDialog::applyConfiguration);
  connect(dialogButtons, &QDialogButtonBox::accepted, this, [this]() {
    applyConfiguration();
    accept();
  });
  connect(dialogButtons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void CurveConfigDialog::loadFromCanvas()
{
  m_tracks.clear();
  m_allCurves.clear();

  if (!m_canvas)
    return;

  for (const auto &t : m_canvas->tracks())
  {
    if (!t)
      continue;

    WellTrackConfigItem item;
    item.trackRef = t;
    item.type = t->type();
    item.title = t->title();
    item.width = t->width();
    item.visible = t->isVisible();

    if (t->type() == TrackType::Curve)
    {
      auto ct = std::dynamic_pointer_cast<CurveTrack>(t);
      if (ct)
      {
        item.curves = ct->curves();
        for (const auto &c : ct->curves())
          m_allCurves.append(c);
      }
    }

    m_tracks.append(item);
  }
}

void CurveConfigDialog::populateTree(int selectTrackIdx)
{
  m_tree->blockSignals(true);
  m_tree->clear();

  QTreeWidgetItem *targetItemToSelect = nullptr;

  for (int trackIdx = 0; trackIdx < m_tracks.size(); ++trackIdx)
  {
    const auto &trk = m_tracks.at(trackIdx);
    auto *trackItem = new QTreeWidgetItem(m_tree);

    const QString titleDisplay = trk.title.isEmpty() ? tr("（未命名）") : trk.title;
    if (trk.type == TrackType::Curve)
    {
      trackItem->setText(0, tr("道 %1: %2 %3 [%4根曲线]")
                                .arg(trackIdx + 1)
                                .arg(trackTypeLabel(trk.type))
                                .arg(titleDisplay)
                                .arg(trk.curves.size()));
      trackItem->setText(1, tr("测井曲线道"));
    }
    else
    {
      trackItem->setText(0, tr("道 %1: %2 %3")
                                .arg(trackIdx + 1)
                                .arg(trackTypeLabel(trk.type))
                                .arg(titleDisplay));
      trackItem->setText(1, trackTypeLabel(trk.type));
    }

    trackItem->setText(2, tr("宽 %1 px").arg(qRound(trk.width)));
    trackItem->setCheckState(0, trk.visible ? Qt::Checked : Qt::Unchecked);
    trackItem->setData(0, Qt::UserRole, trackIdx);
    trackItem->setData(0, Qt::UserRole + 1, -1); // -1 表示道本身

    QFont f = trackItem->font(0);
    f.setBold(true);
    trackItem->setFont(0, f);
    trackItem->setExpanded(true);

    if (trackIdx == selectTrackIdx)
    {
      targetItemToSelect = trackItem;
    }

    if (trk.type == TrackType::Curve)
    {
      for (int curveIdx = 0; curveIdx < trk.curves.size(); ++curveIdx)
      {
        const auto &c = trk.curves.at(curveIdx);
        auto *curveItem = new QTreeWidgetItem(trackItem);
        curveItem->setText(0, QStringLiteral("  ● %1").arg(c.name));
        curveItem->setText(1, c.unit.isEmpty() ? QStringLiteral("—") : c.unit);
        curveItem->setText(2, QStringLiteral("%1 ~ %2").arg(c.minScale).arg(c.maxScale));
        curveItem->setText(3, c.mode == CurveDisplayMode::Discrete ? tr("离散点") :
                               (c.mode == CurveDisplayMode::Histogram ? tr("直方图") : tr("连续曲线")));
        curveItem->setText(4, c.color.name());
        curveItem->setText(5, QString::number(c.depths.size()));

        curveItem->setData(0, Qt::UserRole, trackIdx);
        curveItem->setData(0, Qt::UserRole + 1, curveIdx);
        curveItem->setIcon(0, PaleoIcons::dataLine(c.color));
        PaleoTheme::setItemTextColor(curveItem, 0, PaleoTheme::ItemTextColor::Normal);
      }
    }
  }

  m_tree->blockSignals(false);

  if (targetItemToSelect)
  {
    m_tree->setCurrentItem(targetItemToSelect);
    targetItemToSelect->setSelected(true);
  }

  updateButtonStates();
}

void CurveConfigDialog::onTreeSelectionChanged()
{
  updateButtonStates();
}

void CurveConfigDialog::onTreeItemChanged(QTreeWidgetItem *item, int column)
{
  if (!item || column != 0)
    return;

  const int trkIdx = item->data(0, Qt::UserRole).toInt();
  const int crvIdx = item->data(0, Qt::UserRole + 1).toInt();
  if (crvIdx == -1 && trkIdx >= 0 && trkIdx < m_tracks.size())
  {
    m_tracks[trkIdx].visible = (item->checkState(0) == Qt::Checked);
  }
}

void CurveConfigDialog::updateButtonStates()
{
  const auto sel = m_tree->selectedItems();

  int curveCount = 0;
  int trackCount = 0;
  int selectedTrackIdx = -1;

  for (auto *it : sel)
  {
    const int curveIdx = it->data(0, Qt::UserRole + 1).toInt();
    const int trkIdx = it->data(0, Qt::UserRole).toInt();
    if (curveIdx == -1)
    {
      trackCount++;
      if (selectedTrackIdx == -1)
        selectedTrackIdx = trkIdx;
    }
    else
    {
      curveCount++;
      if (selectedTrackIdx == -1)
        selectedTrackIdx = trkIdx;
    }
  }

  // 移动井道条件：选中了 1 个道或选中的曲线所在道
  const bool hasSingleTrackSelection = (selectedTrackIdx >= 0 && selectedTrackIdx < m_tracks.size() &&
                                        (trackCount == 1 || (trackCount == 0 && curveCount > 0)));

  // §35：禁用控件必须带 reason tooltip——启用时恢复功能提示，禁用时说明原因。
  // （范式同 edittools/editingtoolbar.cpp 的 gate()。）
  const auto gate = [](QPushButton *btn, bool enabled, const QString &reason) {
    if (enabled)
    {
      btn->setEnabled(true);
      btn->setToolTip(btn->property("hint").toString());
    }
    else
    {
      btn->setEnabled(false);
      btn->setToolTip(reason);
    }
  };

  const QString reasonNoTrack = tr("请先在左侧列表选中一个井道");
  gate(m_btnMoveUp, hasSingleTrackSelection && selectedTrackIdx > 0,
       hasSingleTrackSelection ? tr("所选井道已在最前，无法再上移") : reasonNoTrack);
  gate(m_btnMoveDown, hasSingleTrackSelection && selectedTrackIdx < m_tracks.size() - 1,
       hasSingleTrackSelection ? tr("所选井道已在最后，无法再下移") : reasonNoTrack);
  gate(m_btnMoveTop, hasSingleTrackSelection && selectedTrackIdx > 0,
       hasSingleTrackSelection ? tr("所选井道已在最前，无需置顶") : reasonNoTrack);
  gate(m_btnMoveBottom, hasSingleTrackSelection && selectedTrackIdx < m_tracks.size() - 1,
       hasSingleTrackSelection ? tr("所选井道已在最后，无需置底") : reasonNoTrack);
  gate(m_btnRename, hasSingleTrackSelection, reasonNoTrack);

  // 合并条件：选中了 2 至 4 根曲线
  gate(m_btnCombine, curveCount >= 2 && curveCount <= 4,
       tr("需先在列表中选中 2-4 根曲线（当前选中 %1 根）").arg(curveCount));

  // 解散条件：选中的道包含 > 1 根曲线
  bool canDissolve = false;
  if (selectedTrackIdx >= 0 && selectedTrackIdx < m_tracks.size())
  {
    const auto &trk = m_tracks.at(selectedTrackIdx);
    if (trk.type == TrackType::Curve && trk.curves.size() > 1)
      canDissolve = true;
  }
  gate(m_btnDissolve, canDissolve && (trackCount == 1 || curveCount > 0),
       canDissolve ? reasonNoTrack : tr("所选井道不是多曲线道，无需解散"));

  // 拆出单根条件：选中的是 1 根曲线且其所在道有 > 1 根曲线
  gate(m_btnExtract, curveCount == 1 && canDissolve,
       curveCount == 1 ? tr("该曲线所在道只有一根曲线，无法拆出")
                       : tr("请先选中多曲线道中的单根曲线"));
}

void CurveConfigDialog::moveTrack(int fromIdx, int toIdx)
{
  if (fromIdx < 0 || fromIdx >= m_tracks.size() || toIdx < 0 || toIdx >= m_tracks.size() || fromIdx == toIdx)
    return;

  m_tracks.move(fromIdx, toIdx);
  populateTree(toIdx);
}

void CurveConfigDialog::setTrackVisible(int trackIdx, bool visible)
{
  if (trackIdx < 0 || trackIdx >= m_tracks.size())
    return;

  m_tracks[trackIdx].visible = visible;
  populateTree(trackIdx);
}

void CurveConfigDialog::onMoveTrackUp()
{
  const auto sel = m_tree->selectedItems();
  if (sel.isEmpty())
    return;
  const int trkIdx = sel.front()->data(0, Qt::UserRole).toInt();
  if (trkIdx > 0 && trkIdx < m_tracks.size())
  {
    m_tracks.swapItemsAt(trkIdx, trkIdx - 1);
    populateTree(trkIdx - 1);
  }
}

void CurveConfigDialog::onMoveTrackDown()
{
  const auto sel = m_tree->selectedItems();
  if (sel.isEmpty())
    return;
  const int trkIdx = sel.front()->data(0, Qt::UserRole).toInt();
  if (trkIdx >= 0 && trkIdx < m_tracks.size() - 1)
  {
    m_tracks.swapItemsAt(trkIdx, trkIdx + 1);
    populateTree(trkIdx + 1);
  }
}

void CurveConfigDialog::onMoveTrackToTop()
{
  const auto sel = m_tree->selectedItems();
  if (sel.isEmpty())
    return;
  const int trkIdx = sel.front()->data(0, Qt::UserRole).toInt();
  if (trkIdx > 0 && trkIdx < m_tracks.size())
  {
    moveTrack(trkIdx, 0);
  }
}

void CurveConfigDialog::onMoveTrackToBottom()
{
  const auto sel = m_tree->selectedItems();
  if (sel.isEmpty())
    return;
  const int trkIdx = sel.front()->data(0, Qt::UserRole).toInt();
  if (trkIdx >= 0 && trkIdx < m_tracks.size() - 1)
  {
    moveTrack(trkIdx, m_tracks.size() - 1);
  }
}

bool CurveConfigDialog::renameTrack(int trkIdx, const QString &newTitle)
{
  if (trkIdx < 0 || trkIdx >= m_tracks.size() || newTitle.trimmed().isEmpty())
    return false;

  m_tracks[trkIdx].title = newTitle.trimmed();
  if (m_tracks[trkIdx].trackRef)
    m_tracks[trkIdx].trackRef->setTitle(newTitle.trimmed());
  populateTree(trkIdx);
  return true;
}

void CurveConfigDialog::onRenameSelected()
{
  const auto sel = m_tree->selectedItems();
  if (sel.isEmpty())
    return;
  const int trkIdx = sel.front()->data(0, Qt::UserRole).toInt();
  if (trkIdx < 0 || trkIdx >= m_tracks.size())
    return;

  bool ok = false;
  const QString currentTitle = m_tracks[trkIdx].title;
  const QString newTitle = QInputDialog::getText(
      this, tr("重命名井道"), tr("请输入井道名称:"), QLineEdit::Normal, currentTitle, &ok);
  if (ok && !newTitle.trimmed().isEmpty())
  {
    renameTrack(trkIdx, newTitle);
  }
}

bool CurveConfigDialog::combineCurves(const QList<QPair<int, int>> &indices, const QString &title)
{
  if (indices.size() < 2 || indices.size() > 4 || title.trimmed().isEmpty())
    return false;

  QVector<CurveData> toMerge;
  auto sortedIndices = indices;
  for (const auto &p : indices)
  {
    if (p.first < 0 || p.first >= m_tracks.size() || p.second < 0 || p.second >= m_tracks[p.first].curves.size())
      return false;
    toMerge.append(m_tracks[p.first].curves.at(p.second));
  }

  int insertIdx = sortedIndices.front().first;

  // 按照 trackIdx 降序、curveIdx 降序排序
  std::sort(sortedIndices.begin(), sortedIndices.end(), [](const QPair<int, int> &a, const QPair<int, int> &b) {
    if (a.first != b.first) return a.first > b.first;
    return a.second > b.second;
  });

  for (const auto &p : sortedIndices)
  {
    m_tracks[p.first].curves.removeAt(p.second);
  }

  // 清除变空的曲线道
  for (int i = m_tracks.size() - 1; i >= 0; --i)
  {
    if (m_tracks[i].type == TrackType::Curve && m_tracks[i].curves.isEmpty())
    {
      m_tracks.removeAt(i);
      if (i < insertIdx)
        insertIdx--;
    }
  }

  // 新增合并道
  WellTrackConfigItem newItem;
  newItem.type = TrackType::Curve;
  newItem.title = title.trimmed();
  newItem.width = 180.0;
  newItem.visible = true;
  newItem.curves = toMerge;

  insertIdx = qBound(0, insertIdx, static_cast<int>(m_tracks.size()));
  m_tracks.insert(insertIdx, newItem);

  populateTree(insertIdx);
  return true;
}

void CurveConfigDialog::onCombineSelected()
{
  const auto sel = m_tree->selectedItems();
  QList<QPair<int, int>> indices; // (trackIdx, curveIdx)
  QStringList curveNames;

  for (auto *it : sel)
  {
    const int trkIdx = it->data(0, Qt::UserRole).toInt();
    const int crvIdx = it->data(0, Qt::UserRole + 1).toInt();
    if (crvIdx >= 0 && trkIdx >= 0 && trkIdx < m_tracks.size() && crvIdx < m_tracks[trkIdx].curves.size())
    {
      indices.append(qMakePair(trkIdx, crvIdx));
      curveNames.append(m_tracks[trkIdx].curves.at(crvIdx).name);
    }
  }

  if (indices.size() < 2 || indices.size() > 4)
  {
    paleo::ui::NotificationManager::showWarning(this, tr("提示"), tr("请在列表中选择 2 至 4 根曲线进行合并！"));
    return;
  }

  bool ok = false;
  const QString defaultTitle = tr("组合道: %1").arg(curveNames.join(QLatin1String("+")));
  const QString title = QInputDialog::getText(
      this, tr("合并曲线道"), tr("请输入新建曲线道的名称:"), QLineEdit::Normal, defaultTitle, &ok);
  if (!ok || title.trimmed().isEmpty())
    return;

  combineCurves(indices, title.trimmed());
}

bool CurveConfigDialog::dissolveTrack(int targetTrackIdx)
{
  if (targetTrackIdx < 0 || targetTrackIdx >= m_tracks.size())
    return false;

  const auto targetItem = m_tracks.at(targetTrackIdx);
  if (targetItem.type != TrackType::Curve || targetItem.curves.size() <= 1)
    return false;

  // 解散为每个独立单道
  m_tracks.removeAt(targetTrackIdx);
  int insertIdx = targetTrackIdx;
  for (const auto &c : targetItem.curves)
  {
    WellTrackConfigItem standalone;
    standalone.type = TrackType::Curve;
    standalone.title = tr("%1 测井").arg(c.name);
    standalone.width = 140.0;
    standalone.visible = true;
    standalone.curves.append(c);
    m_tracks.insert(insertIdx++, standalone);
  }

  populateTree(targetTrackIdx);
  return true;
}

void CurveConfigDialog::onDissolveSelected()
{
  const auto sel = m_tree->selectedItems();
  if (sel.isEmpty())
    return;

  const int targetTrackIdx = sel.front()->data(0, Qt::UserRole).toInt();
  if (!dissolveTrack(targetTrackIdx))
  {
    paleo::ui::NotificationManager::showInfo(this, tr("提示"), tr("该井道非多曲线道，无需解散。"));
  }
}

bool CurveConfigDialog::extractCurve(int trkIdx, int crvIdx)
{
  if (trkIdx < 0 || trkIdx >= m_tracks.size() || crvIdx < 0 || crvIdx >= m_tracks[trkIdx].curves.size())
    return false;

  if (m_tracks[trkIdx].curves.size() <= 1)
    return false;

  const CurveData c = m_tracks[trkIdx].curves.takeAt(crvIdx);

  WellTrackConfigItem standalone;
  standalone.type = TrackType::Curve;
  standalone.title = tr("%1 测井").arg(c.name);
  standalone.width = 140.0;
  standalone.visible = true;
  standalone.curves.append(c);
  m_tracks.insert(trkIdx + 1, standalone);

  populateTree(trkIdx + 1);
  return true;
}

void CurveConfigDialog::onExtractCurve()
{
  const auto sel = m_tree->selectedItems();
  if (sel.isEmpty())
    return;

  const int trkIdx = sel.front()->data(0, Qt::UserRole).toInt();
  const int crvIdx = sel.front()->data(0, Qt::UserRole + 1).toInt();
  extractCurve(trkIdx, crvIdx);
}

void CurveConfigDialog::onResetDefault()
{
  if (m_allCurves.isEmpty())
    return;

  // 找到第一个曲线道的位置
  int firstCurveIdx = -1;
  for (int i = 0; i < m_tracks.size(); ++i)
  {
    if (m_tracks[i].type == TrackType::Curve)
    {
      firstCurveIdx = i;
      break;
    }
  }
  if (firstCurveIdx == -1)
    firstCurveIdx = m_tracks.size();

  // 移除所有曲线道
  for (int i = m_tracks.size() - 1; i >= 0; --i)
  {
    if (m_tracks[i].type == TrackType::Curve)
      m_tracks.removeAt(i);
  }

  const auto isLitho = [](const QString &name) {
    const QString n = name.toUpper();
    return n.startsWith(QStringLiteral("GR")) || n.startsWith(QStringLiteral("CAL")) ||
           n.startsWith(QStringLiteral("SP")) || n.startsWith(QStringLiteral("BS")) ||
           n.startsWith(QStringLiteral("AZIM"));
  };
  const auto isPorosity = [](const QString &name) {
    const QString n = name.toUpper();
    return n.startsWith(QStringLiteral("AC")) || n.startsWith(QStringLiteral("DEN")) ||
           n.startsWith(QStringLiteral("CNL")) || n.startsWith(QStringLiteral("POR")) ||
           n.startsWith(QStringLiteral("CPOR")) || n.startsWith(QStringLiteral("PHIF"));
  };
  const auto isResistivity = [](const QString &name) {
    const QString n = name.toUpper();
    return n.startsWith(QStringLiteral("RT")) || n.startsWith(QStringLiteral("RXO")) ||
           n.startsWith(QStringLiteral("RD")) || n.startsWith(QStringLiteral("RS")) ||
           n.startsWith(QStringLiteral("ILD")) || n.startsWith(QStringLiteral("ILM")) ||
           n.startsWith(QStringLiteral("AT"));
  };

  QVector<CurveData> lithoCurves;
  QVector<CurveData> poroCurves;
  QVector<CurveData> resCurves;
  QVector<CurveData> otherCurves;

  for (const auto &c : m_allCurves)
  {
    if (isLitho(c.name)) lithoCurves.append(c);
    else if (isPorosity(c.name)) poroCurves.append(c);
    else if (isResistivity(c.name)) resCurves.append(c);
    else otherCurves.append(c);
  }

  QList<WellTrackConfigItem> newCurveTracks;
  const auto addGroup = [&newCurveTracks](const QString &baseTitle, const QVector<CurveData> &group) {
    for (int i = 0; i < group.size(); i += 4)
    {
      WellTrackConfigItem item;
      item.type = TrackType::Curve;
      item.title = (group.size() > 4) ? QStringLiteral("%1 (%2)").arg(baseTitle).arg(i / 4 + 1) : baseTitle;
      item.width = 180.0;
      item.visible = true;
      for (int j = 0; j < 4 && (i + j) < group.size(); ++j)
        item.curves.append(group.at(i + j));
      newCurveTracks.append(item);
    }
  };

  if (!lithoCurves.isEmpty()) addGroup(tr("岩性测井"), lithoCurves);
  if (!poroCurves.isEmpty()) addGroup(tr("三孔隙测井"), poroCurves);
  if (!resCurves.isEmpty()) addGroup(tr("电阻率测井"), resCurves);
  if (!otherCurves.isEmpty()) addGroup(tr("辅助曲线"), otherCurves);

  if (lithoCurves.isEmpty() && poroCurves.isEmpty() && resCurves.isEmpty() && otherCurves.isEmpty())
  {
    for (int i = 0; i < m_allCurves.size(); i += 4)
    {
      WellTrackConfigItem item;
      item.type = TrackType::Curve;
      item.title = (i == 0) ? tr("常规测井") : tr("辅助曲线");
      item.width = 180.0;
      item.visible = true;
      for (int j = 0; j < 4 && (i + j) < m_allCurves.size(); ++j)
        item.curves.append(m_allCurves.at(i + j));
      newCurveTracks.append(item);
    }
  }

  int insertPos = qBound(0, firstCurveIdx, static_cast<int>(m_tracks.size()));
  for (const auto &item : newCurveTracks)
  {
    m_tracks.insert(insertPos++, item);
  }

  populateTree(firstCurveIdx);
}

void CurveConfigDialog::applyConfiguration()
{
  if (!m_canvas)
    return;

  QList<std::shared_ptr<WellTrack>> assembledTracks;

  for (const auto &trkCfg : m_tracks)
  {
    if (trkCfg.type == TrackType::Curve)
    {
      if (trkCfg.curves.isEmpty())
        continue;

      auto ct = std::make_shared<CurveTrack>(trkCfg.title, trkCfg.width);
      ct->setVisible(trkCfg.visible);
      for (const auto &c : trkCfg.curves)
        ct->addCurve(c);
      assembledTracks.append(ct);
    }
    else
    {
      if (trkCfg.trackRef)
      {
        trkCfg.trackRef->setVisible(trkCfg.visible);
        trkCfg.trackRef->setTitle(trkCfg.title);
        assembledTracks.append(trkCfg.trackRef);
      }
    }
  }

  m_canvas->setTracks(assembledTracks);
}

} // namespace WellComposite
