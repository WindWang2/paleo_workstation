#include "curveconfigdialog.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QMessageBox>
#include <QVBoxLayout>

namespace WellComposite
{

CurveConfigDialog::CurveConfigDialog(WellCompositeCanvas *canvas, QWidget *parent)
  : QDialog(parent)
  , m_canvas(canvas)
{
  setWindowTitle(tr("测井曲线道组合与管理 (ResFormStar 规范)"));
  resize(820, 540);
  setupUi();
  loadFromCanvas();
  populateTree();
  updateButtonStates();
}

void CurveConfigDialog::setupUi()
{
  auto *rootLay = new QVBoxLayout(this);
  rootLay->setContentsMargins(16, 16, 16, 16);
  rootLay->setSpacing(12);

  // 顶部说明提示
  m_lblInfo = new QLabel(
      tr("提示：按住 Ctrl/Shift 键可在列表中多选 2-4 根曲线合并为同一道渲染；选定多曲线道可一键解散为独立单道。"),
      this);
  m_lblInfo->setStyleSheet(QStringLiteral(
      "QLabel { background: #EDF1F5; color: #24303E; border: 1px solid #DFE5EC; "
      "border-radius: 4px; padding: 8px 12px; font-size: 8.5pt; }"));
  rootLay->addWidget(m_lblInfo);

  // 中间区域：左侧树形列表 + 右侧操作工具栏
  auto *midWidget = new QWidget(this);
  auto *midLay = new QHBoxLayout(midWidget);
  midLay->setContentsMargins(0, 0, 0, 0);
  midLay->setSpacing(12);

  m_tree = new QTreeWidget(midWidget);
  m_tree->setHeaderLabels({tr("井道 / 测井曲线"), tr("单位"), tr("显示范围"), tr("形态"), tr("颜色"), tr("点数")});
  m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_tree->setAlternatingRowColors(true);
  m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  m_tree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
  m_tree->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
  m_tree->header()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
  m_tree->setStyleSheet(QStringLiteral(
      "QTreeWidget { border: 1px solid #DFE5EC; border-radius: 4px; background: #FFFFFF; font-size: 8.5pt; }"
      "QTreeWidget::item { padding: 4px 0; }"
      "QTreeWidget::item:selected { background-color: #E6F0FA; color: #1B73D0; }"));
  midLay->addWidget(m_tree, 1);

  // 右侧按钮栏
  auto *btnBox = new QWidget(midWidget);
  auto *btnLay = new QVBoxLayout(btnBox);
  btnLay->setContentsMargins(0, 0, 0, 0);
  btnLay->setSpacing(8);

  const QString btnStyle = QStringLiteral(
      "QPushButton { background: #FFFFFF; border: 1px solid #DFE5EC; border-radius: 4px; "
      "padding: 6px 12px; font-size: 8.5pt; color: #24303E; text-align: left; }"
      "QPushButton:hover { background: #EDF1F5; border-color: #9AA7B4; }"
      "QPushButton:pressed { background: #DFE5EC; }"
      "QPushButton:disabled { color: #9AA7B4; background: #F8FAFC; border-color: #E2E8F0; }");

  m_btnCombine = new QPushButton(tr("合并所选曲线 (2-4根)..."), btnBox);
  m_btnCombine->setStyleSheet(btnStyle);
  m_btnCombine->setToolTip(tr("选中多根独立曲线后点击，将其合并入同一个曲线道（最多4根）"));
  btnLay->addWidget(m_btnCombine);

  m_btnDissolve = new QPushButton(tr("解散所选道 (独立单道)"), btnBox);
  m_btnDissolve->setStyleSheet(btnStyle);
  m_btnDissolve->setToolTip(tr("将选中的多曲线道拆开，每根曲线独占一道"));
  btnLay->addWidget(m_btnDissolve);

  m_btnExtract = new QPushButton(tr("拆出为独立道"), btnBox);
  m_btnExtract->setStyleSheet(btnStyle);
  m_btnExtract->setToolTip(tr("将选中的单根曲线移出当前道，成为单独井道"));
  btnLay->addWidget(m_btnExtract);

  btnLay->addSpacing(8);

  m_btnMoveUp = new QPushButton(tr("井道上移 ▲"), btnBox);
  m_btnMoveUp->setStyleSheet(btnStyle);
  btnLay->addWidget(m_btnMoveUp);

  m_btnMoveDown = new QPushButton(tr("井道下移 ▼"), btnBox);
  m_btnMoveDown->setStyleSheet(btnStyle);
  btnLay->addWidget(m_btnMoveDown);

  btnLay->addSpacing(8);

  m_btnResetDefault = new QPushButton(tr("恢复标准地质组合"), btnBox);
  m_btnResetDefault->setStyleSheet(btnStyle);
  m_btnResetDefault->setToolTip(tr("按标准地质类别（岩性/三孔隙/电阻率）重置分道"));
  btnLay->addWidget(m_btnResetDefault);

  btnLay->addStretch(1);
  midLay->addWidget(btnBox);
  rootLay->addWidget(midWidget, 1);

  // 底部标准对话框操作栏
  auto *bottomLay = new QHBoxLayout();
  bottomLay->setContentsMargins(0, 0, 0, 0);
  bottomLay->setSpacing(8);

  m_btnApply = new QPushButton(tr("应用"), this);
  m_btnApply->setStyleSheet(QStringLiteral(
      "QPushButton { background: #FFFFFF; border: 1px solid #DFE5EC; border-radius: 4px; padding: 6px 16px; font-size: 9pt; color: #24303E; }"
      "QPushButton:hover { background: #EDF1F5; border-color: #9AA7B4; }"));
  bottomLay->addWidget(m_btnApply);

  bottomLay->addStretch(1);

  auto *dialogButtons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  dialogButtons->button(QDialogButtonBox::Ok)->setText(tr("确定"));
  dialogButtons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
  dialogButtons->button(QDialogButtonBox::Ok)->setStyleSheet(QStringLiteral(
      "QPushButton { background: #1B73D0; color: #FFFFFF; border: 1px solid #1565B8; border-radius: 4px; padding: 6px 20px; font-size: 9pt; font-weight: bold; }"
      "QPushButton:hover { background: #1565B8; }"));
  dialogButtons->button(QDialogButtonBox::Cancel)->setStyleSheet(QStringLiteral(
      "QPushButton { background: #FFFFFF; border: 1px solid #DFE5EC; border-radius: 4px; padding: 6px 16px; font-size: 9pt; color: #24303E; }"
      "QPushButton:hover { background: #EDF1F5; }"));
  bottomLay->addWidget(dialogButtons);
  rootLay->addLayout(bottomLay);

  // 事件信号绑定
  connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &CurveConfigDialog::onTreeSelectionChanged);
  connect(m_btnCombine, &QPushButton::clicked, this, &CurveConfigDialog::onCombineSelected);
  connect(m_btnDissolve, &QPushButton::clicked, this, &CurveConfigDialog::onDissolveSelected);
  connect(m_btnExtract, &QPushButton::clicked, this, &CurveConfigDialog::onExtractCurve);
  connect(m_btnMoveUp, &QPushButton::clicked, this, &CurveConfigDialog::onMoveTrackUp);
  connect(m_btnMoveDown, &QPushButton::clicked, this, &CurveConfigDialog::onMoveTrackDown);
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
    if (t && t->type() == TrackType::Curve)
    {
      auto ct = std::dynamic_pointer_cast<CurveTrack>(t);
      if (ct)
      {
        TrackConfigItem item;
        item.title = ct->title();
        item.width = ct->width();
        item.curves = ct->curves();
        m_tracks.append(item);

        for (const auto &c : ct->curves())
          m_allCurves.append(c);
      }
    }
  }
}

void CurveConfigDialog::populateTree()
{
  m_tree->clear();

  for (int trackIdx = 0; trackIdx < m_tracks.size(); ++trackIdx)
  {
    const auto &trk = m_tracks.at(trackIdx);
    auto *trackItem = new QTreeWidgetItem(m_tree);
    trackItem->setText(0, tr("道 %1: %2  [%3根曲线]").arg(trackIdx + 1).arg(trk.title).arg(trk.curves.size()));
    trackItem->setData(0, Qt::UserRole, trackIdx);
    trackItem->setData(0, Qt::UserRole + 1, -1); // -1 表示道本身
    QFont f = trackItem->font(0);
    f.setBold(true);
    trackItem->setFont(0, f);
    trackItem->setExpanded(true);

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
      curveItem->setForeground(0, c.color);
    }
  }

  m_tree->expandAll();
  updateButtonStates();
}

void CurveConfigDialog::onTreeSelectionChanged()
{
  updateButtonStates();
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
      selectedTrackIdx = trkIdx;
    }
    else
    {
      curveCount++;
      selectedTrackIdx = trkIdx;
    }
  }

  // 合并条件：选中了 2 至 4 根曲线
  m_btnCombine->setEnabled(curveCount >= 2 && curveCount <= 4);

  // 解散条件：选中的道包含 > 1 根曲线
  bool canDissolve = false;
  if (trackCount == 1 && selectedTrackIdx >= 0 && selectedTrackIdx < m_tracks.size())
  {
    canDissolve = (m_tracks[selectedTrackIdx].curves.size() > 1);
  }
  else if (curveCount > 0 && selectedTrackIdx >= 0 && selectedTrackIdx < m_tracks.size())
  {
    canDissolve = (m_tracks[selectedTrackIdx].curves.size() > 1);
  }
  m_btnDissolve->setEnabled(canDissolve);

  // 拆出单根条件：选中的是 1 根曲线且其所在道有 > 1 根曲线
  m_btnExtract->setEnabled(curveCount == 1 && canDissolve);

  // 移动条件：选中了道
  m_btnMoveUp->setEnabled(selectedTrackIdx > 0 && trackCount == 1);
  m_btnMoveDown->setEnabled(selectedTrackIdx >= 0 && selectedTrackIdx < m_tracks.size() - 1 && trackCount == 1);
}

void CurveConfigDialog::onCombineSelected()
{
  const auto sel = m_tree->selectedItems();
  QVector<CurveData> toMerge;
  QList<QPair<int, int>> indices; // (trackIdx, curveIdx)

  for (auto *it : sel)
  {
    const int trkIdx = it->data(0, Qt::UserRole).toInt();
    const int crvIdx = it->data(0, Qt::UserRole + 1).toInt();
    if (crvIdx >= 0 && trkIdx >= 0 && trkIdx < m_tracks.size() && crvIdx < m_tracks[trkIdx].curves.size())
    {
      indices.append(qMakePair(trkIdx, crvIdx));
    }
  }

  if (indices.size() < 2 || indices.size() > 4)
  {
    QMessageBox::warning(this, tr("提示"), tr("请在列表中选择 2 至 4 根曲线进行合并！"));
    return;
  }

  QStringList curveNames;
  for (const auto &p : indices)
  {
    const auto &c = m_tracks[p.first].curves.at(p.second);
    toMerge.append(c);
    curveNames.append(c.name);
  }

  bool ok = false;
  const QString defaultTitle = QStringLiteral("组合道: %1").arg(curveNames.join(QLatin1String("+")));
  const QString title = QInputDialog::getText(
      this, tr("合并曲线道"), tr("请输入新建曲线道的名称:"), QLineEdit::Normal, defaultTitle, &ok);
  if (!ok || title.trimmed().isEmpty())
    return;

  // 从原道中移除所选曲线（按逆序移除避免索引错乱）
  // 按照 trackIdx 降序、curveIdx 降序排序
  std::sort(indices.begin(), indices.end(), [](const QPair<int, int> &a, const QPair<int, int> &b) {
    if (a.first != b.first) return a.first > b.first;
    return a.second > b.second;
  });

  for (const auto &p : indices)
  {
    m_tracks[p.first].curves.removeAt(p.second);
  }

  // 清除空道
  for (int i = m_tracks.size() - 1; i >= 0; --i)
  {
    if (m_tracks[i].curves.isEmpty())
      m_tracks.removeAt(i);
  }

  // 新增合并道
  TrackConfigItem newItem;
  newItem.title = title.trimmed();
  newItem.width = 180.0;
  newItem.curves = toMerge;
  m_tracks.append(newItem);

  populateTree();
}

void CurveConfigDialog::onDissolveSelected()
{
  const auto sel = m_tree->selectedItems();
  if (sel.isEmpty())
    return;

  const int targetTrackIdx = sel.front()->data(0, Qt::UserRole).toInt();
  if (targetTrackIdx < 0 || targetTrackIdx >= m_tracks.size())
    return;

  const auto targetItem = m_tracks.at(targetTrackIdx);
  if (targetItem.curves.size() <= 1)
  {
    QMessageBox::information(this, tr("提示"), tr("该井道仅有 1 根曲线，无需解散。"));
    return;
  }

  // 解散为每个独立单道
  m_tracks.removeAt(targetTrackIdx);
  int insertIdx = targetTrackIdx;
  for (const auto &c : targetItem.curves)
  {
    TrackConfigItem standalone;
    standalone.title = QStringLiteral("%1 测井").arg(c.name);
    standalone.width = 140.0;
    standalone.curves.append(c);
    m_tracks.insert(insertIdx++, standalone);
  }

  populateTree();
}

void CurveConfigDialog::onExtractCurve()
{
  const auto sel = m_tree->selectedItems();
  if (sel.isEmpty())
    return;

  const int trkIdx = sel.front()->data(0, Qt::UserRole).toInt();
  const int crvIdx = sel.front()->data(0, Qt::UserRole + 1).toInt();
  if (trkIdx < 0 || trkIdx >= m_tracks.size() || crvIdx < 0 || crvIdx >= m_tracks[trkIdx].curves.size())
    return;

  if (m_tracks[trkIdx].curves.size() <= 1)
    return;

  const CurveData c = m_tracks[trkIdx].curves.takeAt(crvIdx);

  TrackConfigItem standalone;
  standalone.title = QStringLiteral("%1 测井").arg(c.name);
  standalone.width = 140.0;
  standalone.curves.append(c);
  m_tracks.insert(trkIdx + 1, standalone);

  populateTree();
}

void CurveConfigDialog::onMoveTrackUp()
{
  const auto sel = m_tree->selectedItems();
  if (sel.isEmpty()) return;
  const int trkIdx = sel.front()->data(0, Qt::UserRole).toInt();
  if (trkIdx > 0 && trkIdx < m_tracks.size())
  {
    m_tracks.swapItemsAt(trkIdx, trkIdx - 1);
    populateTree();
  }
}

void CurveConfigDialog::onMoveTrackDown()
{
  const auto sel = m_tree->selectedItems();
  if (sel.isEmpty()) return;
  const int trkIdx = sel.front()->data(0, Qt::UserRole).toInt();
  if (trkIdx >= 0 && trkIdx < m_tracks.size() - 1)
  {
    m_tracks.swapItemsAt(trkIdx, trkIdx + 1);
    populateTree();
  }
}

void CurveConfigDialog::onResetDefault()
{
  if (m_allCurves.isEmpty())
    return;

  m_tracks.clear();

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

  const auto addGroup = [this](const QString &baseTitle, const QVector<CurveData> &group) {
    for (int i = 0; i < group.size(); i += 4)
    {
      TrackConfigItem item;
      item.title = (group.size() > 4) ? QStringLiteral("%1 (%2)").arg(baseTitle).arg(i / 4 + 1) : baseTitle;
      item.width = 180.0;
      for (int j = 0; j < 4 && (i + j) < group.size(); ++j)
        item.curves.append(group.at(i + j));
      m_tracks.append(item);
    }
  };

  if (!lithoCurves.isEmpty()) addGroup(QStringLiteral("岩性测井"), lithoCurves);
  if (!poroCurves.isEmpty()) addGroup(QStringLiteral("三孔隙测井"), poroCurves);
  if (!resCurves.isEmpty()) addGroup(QStringLiteral("电阻率测井"), resCurves);
  if (!otherCurves.isEmpty()) addGroup(QStringLiteral("辅助曲线"), otherCurves);

  if (lithoCurves.isEmpty() && poroCurves.isEmpty() && resCurves.isEmpty() && otherCurves.isEmpty())
  {
    for (int i = 0; i < m_allCurves.size(); i += 4)
    {
      TrackConfigItem item;
      item.title = (i == 0) ? QStringLiteral("常规测井") : QStringLiteral("辅助曲线");
      item.width = 180.0;
      for (int j = 0; j < 4 && (i + j) < m_allCurves.size(); ++j)
        item.curves.append(m_allCurves.at(i + j));
      m_tracks.append(item);
    }
  }

  populateTree();
}

void CurveConfigDialog::applyConfiguration()
{
  if (!m_canvas)
    return;

  // 保留非曲线道（标尺道、地层道、岩性道、取芯道、图片道、符号道、文本道）
  QList<std::shared_ptr<WellTrack>> nonCurveBefore;
  QList<std::shared_ptr<WellTrack>> nonCurveAfter;
  bool passedCurves = false;

  for (const auto &t : m_canvas->tracks())
  {
    if (t->type() == TrackType::Curve)
    {
      passedCurves = true;
    }
    else
    {
      if (!passedCurves)
        nonCurveBefore.append(t);
      else
        nonCurveAfter.append(t);
    }
  }

  // 重新组装井道
  QList<std::shared_ptr<WellTrack>> assembledTracks;
  assembledTracks.append(nonCurveBefore);

  for (const auto &trkCfg : m_tracks)
  {
    auto ct = std::make_shared<CurveTrack>(trkCfg.title, trkCfg.width);
    for (const auto &c : trkCfg.curves)
      ct->addCurve(c);
    assembledTracks.append(ct);
  }

  assembledTracks.append(nonCurveAfter);

  m_canvas->setTracks(assembledTracks);
}

} // namespace WellComposite
