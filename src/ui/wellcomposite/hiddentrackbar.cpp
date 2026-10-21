// 层：视图
#include "../paleotheme.h"
#include "hiddentrackbar.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QToolButton>

namespace WellComposite
{

HiddenTrackBar::HiddenTrackBar(QWidget *parent)
  : QWidget(parent)
{
  setObjectName(QStringLiteral("wellCompositeHiddenBar"));
  auto *lay = new QHBoxLayout(this);
  lay->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingXs, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingXs);
  lay->setSpacing(PaleoTheme::tokens().spacingXs);
  // rebuild() 动态填充；样式遵循 DESIGN.md chip（胶囊、surface 底、text-muted）
  PaleoTheme::applyThemedStyleSheet(this, [] {
    const auto &t = PaleoTheme::tokens();
    return PaleoTheme::metricStyleSheet(QStringLiteral(
        "QWidget#wellCompositeHiddenBar { background: %1; border-top: 1px solid %2; }"
        "QToolButton { background: %3; border: 1px solid %2; border-radius: %7px;"
        " padding: {spacing.xs}px {spacing.md}px; color: %4; font-size: {typography.label}pt; }"
        "QToolButton:hover { border-color: %5; color: %6; }"
        "QLabel { color: %4; font-size: {typography.label}pt; }"))
        .arg(t.surfaceAlt.name(), t.border.name(), t.surface.name(),
             t.textMuted.name(), t.primary.name(), t.primaryText.name())
        .arg(PaleoTheme::chipRadius(PaleoTheme::bodyFont(t.labelPt)));
  });
  hide();
}

void HiddenTrackBar::setTracks(const QList<TrackSpec> &specs)
{
  m_specs = specs;
  rebuild();
}

int HiddenTrackBar::hiddenCount() const
{
  int n = 0;
  for (const auto &s : m_specs)
    if (!s.visible)
      ++n;
  return n;
}

QString HiddenTrackBar::chipText(const TrackSpec &spec)
{
  const QStringList names = spec.curveNames();
  if (names.isEmpty())
    return spec.title;
  return QStringLiteral("%1 · %2").arg(spec.title, names.join(QStringLiteral("/")));
}

void HiddenTrackBar::rebuild()
{
  // 同时清理旧控件和 stretch，反复切井不累积布局项。
  while (auto *item = layout()->takeAt(0))
  {
    delete item->widget();
    delete item;
  }

  const int hidden = hiddenCount();
  if (hidden == 0)
  {
    hide();
    return;
  }

  auto *lbl = new QLabel(tr("隐藏道:"), this);
  layout()->addWidget(lbl);

  QMenu *moreMenu = nullptr;
  int shown = 0;
  for (int specIndex = 0; specIndex < m_specs.size(); ++specIndex)
  {
    const TrackSpec &spec = m_specs.at(specIndex);
    if (spec.visible) continue;
    // 默认隐藏的辅助道可能很多，余项收进原生菜单，避免撑宽单井视图。
    if (shown++ >= 3 && hidden > 4)
    {
      if (!moreMenu)
      {
        auto *more = new QToolButton(this);
        more->setText(tr("更多隐藏道（%1）").arg(hidden - 3));
        more->setToolTip(tr("选择要恢复显示的图道"));
        more->setPopupMode(QToolButton::InstantPopup);
        moreMenu = new QMenu(more);
        more->setMenu(moreMenu);
        layout()->addWidget(more);
      }
      auto *action = moreMenu->addAction(chipText(spec));
      connect(action, &QAction::triggered, this, [this, title = spec.title, specIndex] {
        emit trackRestoreRequested(title);
        emit trackRestoreIndexRequested(specIndex);
      }, Qt::QueuedConnection);
      continue;
    }
    auto *chip = new QToolButton(this);
    chip->setText(chipText(spec));
    chip->setToolTip(tr("点击恢复显示「%1」道").arg(chipText(spec)));
    chip->setCursor(Qt::PointingHandCursor);
    const QString title = spec.title;
    connect(chip, &QToolButton::clicked, this, [this, title, specIndex]() {
      emit trackRestoreRequested(title);
      emit trackRestoreIndexRequested(specIndex);
    });
    layout()->addWidget(chip);
  }
  dynamic_cast<QHBoxLayout *>(layout())->addStretch(1);

  show();
}

} // namespace WellComposite
