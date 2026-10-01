// 层：视图
#include "../paleotheme.h"
#include "multiwellview.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QSplitter>
#include <QVBoxLayout>

namespace WellComposite
{

namespace {
// overlay：透明覆盖层画 correlation 线（D5.3）
class CorrelationOverlay : public QWidget
{
public:
  explicit CorrelationOverlay(MultiWellView *owner)
    : QWidget(owner), m_owner(owner)
  {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    raise();
  }

  void paintEvent(QPaintEvent *) override
  {
    if (!m_owner || !m_owner->showCorrelationLines())
      return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(QStringLiteral("#B45309")), 1.4, Qt::DashDotLine));

    // 相邻可见面板之间：同名标志层 (x1,y1)-(x2,y2) 直线
    QList<QPair<WellCompositePanel *, QRect>> geom;
    for (int i = 0; i < 4; ++i)
    {
      WellCompositePanel *panel = m_owner->panelAt(i);
      if (!panel || !panel->isVisible())
        continue;
      // 面板画布 body 区域映射到容器坐标
      QWidget *canvas = panel->canvas();
      if (!canvas)
        continue;
      const QPoint tl = canvas->mapTo(m_owner, QPoint(0, 0));
      geom << qMakePair(panel, QRect(tl, canvas->size()));
    }

    for (int g = 1; g < geom.size(); ++g)
    {
      const auto &left = geom.at(g - 1);
      const auto &right = geom.at(g);
      WellCompositeCanvas *lc = left.first->canvas();
      WellCompositeCanvas *rc = right.first->canvas();

      for (const auto &lm : lc->markerLines())
      {
        for (const auto &rm : rc->markerLines())
        {
          if (lm.second != rm.second || lm.second.isEmpty())
            continue;
          if (lm.first < lc->visibleTopDepth() || lm.first > lc->visibleBottomDepth())
            continue;
          if (rm.first < rc->visibleTopDepth() || rm.first > rc->visibleBottomDepth())
            continue;
          const qreal y1 = left.second.top() + lc->depthToY(lm.first);
          const qreal y2 = right.second.top() + rc->depthToY(rm.first);
          p.drawLine(QPointF(left.second.right(), y1), QPointF(right.second.left(), y2));
        }
      }
    }
  }

private:
  MultiWellView *m_owner = nullptr;
};
} // namespace

// ----------------------------------------------------------------------------
// MultiWellView
// ----------------------------------------------------------------------------
MultiWellView::MultiWellView(QWidget *parent)
  : QWidget(parent), m_panels(4, nullptr)
{
  setObjectName(QStringLiteral("wellCompositeMultiWellView"));
  auto *lay = new QGridLayout(this);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(4);

  m_overlay = new CorrelationOverlay(this);
  m_overlay->hide();
  relayout();
}

int MultiWellView::capacity() const
{
  switch (m_mode)
  {
  case LayoutMode::Single: return 1;
  case LayoutMode::Dual: return 2;
  case LayoutMode::Quad: return 4;
  }
  return 1;
}

void MultiWellView::setLayoutMode(LayoutMode mode)
{
  if (m_mode == mode)
    return;
  m_mode = mode;
  relayout();
  emit layoutModeChanged(m_mode);
}

void MultiWellView::relayout()
{
  // 清出既有布局里的面板（reparent 保持存活）
  for (auto *panel : m_panels)
    if (panel)
      panel->setParent(this);

  auto *lay = static_cast<QGridLayout *>(layout());
  // 清空布局项
  while (lay->count())
  {
    QLayoutItem *item = lay->takeAt(0);
    delete item;
  }

  const int cap = capacity();
  for (int i = 0; i < cap; ++i)
  {
    auto *panel = m_panels.at(i);
    if (!panel)
      continue;
    const int row = (m_mode == LayoutMode::Quad) ? (i / 2) : 0;
    const int col = (m_mode == LayoutMode::Quad) ? (i % 2) : i;
    lay->addWidget(panel, row, col);
    panel->show();
  }
  // 占位（空槽位也拉住网格形状）
  for (int i = 0; i < cap; ++i)
  {
    if (m_panels.at(i))
      continue;
    const int row = (m_mode == LayoutMode::Quad) ? (i / 2) : 0;
    const int col = (m_mode == LayoutMode::Quad) ? (i % 2) : i;
    auto *placeholder = new QLabel(tr("槽位 %1：从井选择器加入井").arg(i + 1), this);
    placeholder->setAlignment(Qt::AlignCenter);
    PaleoTheme::applyThemedStyleSheet(placeholder, [] {
      const auto &t = PaleoTheme::tokens();
      return QStringLiteral(
          "color: %1; border: 1px dashed %2; border-radius: 4px; font-size: 9pt;")
          .arg(t.textDisabled.name(), t.border.name());
    });
    lay->addWidget(placeholder, row, col);
  }

  m_overlay->setGeometry(rect());
  m_overlay->setVisible(m_showCorrelation && cap > 1);
  m_overlay->raise();
}

int MultiWellView::setPanel(int slot, WellCompositePanel *panel)
{
  const int cap = capacity();
  if (slot < 0 || slot >= cap)
    slot = cap - 1;

  // 解绑旧面板
  if (m_panels.at(slot))
  {
    m_panels.at(slot)->setParent(nullptr);
    m_panels[slot] = nullptr;
  }

  m_panels[slot] = panel;
  if (panel)
  {
    panel->setParent(this);
    connect(panel->canvas(), &WellCompositeCanvas::viewportChanged,
            this, &MultiWellView::onPanelViewportChanged);
  }
  relayout();
  return slot;
}

WellCompositePanel *MultiWellView::panelAt(int slot) const
{
  return (slot >= 0 && slot < m_panels.size()) ? m_panels.at(slot) : nullptr;
}

int MultiWellView::panelCount() const
{
  int n = 0;
  for (auto *p : m_panels)
    if (p)
      ++n;
  return n;
}

void MultiWellView::setLinkScroll(bool link)
{
  if (m_linkScroll == link)
    return;
  m_linkScroll = link;
  emit linkScrollChanged(m_linkScroll);
}

void MultiWellView::setShowCorrelationLines(bool on)
{
  if (m_showCorrelation == on)
    return;
  m_showCorrelation = on;
  m_overlay->setVisible(on && capacity() > 1);
  m_overlay->update();
}

WellCompositePanel *MultiWellView::panelForSender(QObject *sender) const
{
  for (auto *p : m_panels)
    if (p && p->canvas() == sender)
      return p;
  return nullptr;
}

void MultiWellView::onPanelViewportChanged(double top, double bottom, double span)
{
  // D5.1/D2.9 锁步：源面板视口顶深广播到其它面板（防重入）
  if (!m_linkScroll || m_syncingScroll)
    return;

  WellCompositePanel *src = panelForSender(sender());
  if (!src)
    return;

  m_syncingScroll = true;
  for (auto *p : m_panels)
  {
    if (!p || p == src)
      continue;
    p->canvas()->setScrollDepth(top);
  }
  m_syncingScroll = false;
  m_overlay->update();
}

QVector<QPair<QString, QString>> MultiWellView::correlationPairs() const
{
  QVector<QPair<QString, QString>> pairs;
  QList<QPair<int, WellCompositeCanvas *>> active;
  for (int i = 0; i < m_panels.size() && i < capacity(); ++i)
    if (m_panels.at(i) && m_panels.at(i)->canvas())
      active << qMakePair(i, m_panels.at(i)->canvas());

  for (int g = 1; g < active.size(); ++g)
  {
    const auto *lc = active.at(g - 1).second;
    const auto *rc = active.at(g).second;
    for (const auto &lm : lc->markerLines())
      for (const auto &rm : rc->markerLines())
        if (lm.second == rm.second && !lm.second.isEmpty())
          pairs << qMakePair(lm.second, rm.second);
  }
  return pairs;
}

bool MultiWellView::alignToDatum(const QString &markerName)
{
  bool any = false;
  // 校平是「各井各滚各的」——临时断开锁步广播，否则后设的井滚动会把前面的井拉回
  const bool wasLinked = m_linkScroll;
  m_linkScroll = false;
  // D5.4 语义修正：同名标志层「同屏高」按像素对齐——共同深度偏移 K 取第一
  // 个有该标志层井的 0.4×span，各井 scroll = marker − K。旧实现各井用自己
  // 的 span，双栏体高差几像素（布局圆整/滚动条不对称）就把 40% 位错开
  //（tst_wellcomposite_multiwell::testDatumAlign 实测 y1−y2≈5.6px）。
  double commonOffset = -1.0;
  for (auto *p : m_panels)
  {
    if (!p)
      continue;
    WellCompositeCanvas *canvas = p->canvas();
    const double markerDepth = [canvas, markerName]() {
      for (const auto &m : canvas->markerLines())
        if (m.second == markerName)
          return m.first;
      return -1.0;
    }();
    if (markerDepth < 0)
      continue;
    if (commonOffset < 0.0)
      commonOffset = canvas->visibleDepthSpan() * 0.4;
    // 视口使标志层位于共同基准位（同 ppm 下像素级同高）
    canvas->setScrollDepth(markerDepth - commonOffset);
    any = true;
  }
  m_linkScroll = wasLinked;
  m_overlay->update();
  if (any)
    m_datumMarker = markerName;
  return any;
}

void MultiWellView::clearDatum()
{
  m_datumMarker.clear();
}

void MultiWellView::setAvailableWells(const QStringList &wells)
{
  m_availableWells = wells;
}

void MultiWellView::setSelectedWells(const QStringList &wells)
{
  m_selectedWells = wells;
}

QString MultiWellView::deltaTableText() const
{
  // D5.7 井间层段厚度差表：行 = 标志层段（上->下），列 = 各井厚度
  // 标志层并集按深度排序；厚度 = 相邻同名标志层深度差
  struct Col
  {
    QString well;
    WellCompositeCanvas *canvas;
  };
  QList<Col> cols;
  QStringList header;
  header << QStringLiteral("层段");
  for (int i = 0; i < m_panels.size() && i < capacity(); ++i)
  {
    auto *p = m_panels.at(i);
    if (!p || !p->canvas())
      continue;
    cols << Col{p->wellName(), p->canvas()};
    header << p->wellName();
  }
  if (cols.size() < 2)
    return QString();

  // 相邻标志层段并集（按 (upper,lower) 名对）
  QMap<QString, QVector<double>> thickness; // "T32->T35" → 各井厚度（NaN 缺）
  QStringList rowOrder;
  for (int c = 0; c < cols.size(); ++c)
  {
    const auto markers = cols.at(c).canvas->markerLines();
    for (int i = 1; i < markers.size(); ++i)
    {
      const QString key = QStringLiteral("%1->%2").arg(markers.at(i - 1).second, markers.at(i).second);
      if (!thickness.contains(key))
      {
        thickness.insert(key, QVector<double>(cols.size(), std::numeric_limits<double>::quiet_NaN()));
        rowOrder << key;
      }
      thickness[key][c] = markers.at(i).first - markers.at(i - 1).first;
    }
  }

  QStringList lines;
  lines << header.join(QLatin1Char('\t'));
  for (const QString &key : rowOrder)
  {
    QStringList row;
    row << key;
    for (double v : thickness.value(key))
      row << (std::isnan(v) ? QStringLiteral("—") : QString::number(v, 'f', 1));
    lines << row.join(QLatin1Char('\t'));
  }
  return lines.join(QLatin1Char('\n'));
}

// ----------------------------------------------------------------------------
// D5.5 WellSelectionDialog
// ----------------------------------------------------------------------------
WellSelectionDialog::WellSelectionDialog(const QStringList &wells, const QStringList &referenceWells,
                                         const QStringList &initialSelection, QWidget *parent)
  : QDialog(parent), m_wells(wells), m_referenceWells(referenceWells), m_selected(initialSelection)
{
  setObjectName(QStringLiteral("wellCompositeWellSelectionDialog"));
  setWindowTitle(tr("选择对比井"));
  setMinimumSize(QSize(320, 380));

  auto *root = new QVBoxLayout(this);
  root->addWidget(new QLabel(tr("测区井（蓝）/ 参考井（琥珀）"), this));

  m_list = new QListWidget(this);
  m_list->setObjectName(QStringLiteral("wellSelectionList"));
  for (const QString &w : wells)
  {
    auto *item = new QListWidgetItem(w, m_list);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    const bool ref = referenceWells.contains(w);
    item->setCheckState(m_selected.contains(w) ? Qt::Checked : Qt::Unchecked);
    // D7.10 参考井/测区井视觉语义保持：琥珀/蓝
    item->setForeground(ref ? QBrush(QColor(QStringLiteral("#92400E")))
                            : QBrush(QColor(QStringLiteral("#1B73D0"))));
    item->setData(Qt::UserRole, ref ? QStringLiteral("ref") : QStringLiteral("survey"));
  }
  root->addWidget(m_list, 1);

  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  root->addWidget(buttons);

  connect(m_list, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
    if (item->checkState() == Qt::Checked)
    {
      if (!m_selected.contains(item->text()))
        m_selected << item->text();
    }
    else
    {
      m_selected.removeAll(item->text());
    }
  });
}

void WellSelectionDialog::toggleWell(const QString &name, bool checked)
{
  for (int i = 0; i < m_list->count(); ++i)
  {
    auto *item = m_list->item(i);
    if (item->text() == name)
    {
      item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
      return;
    }
  }
}

} // namespace WellComposite
