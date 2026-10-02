// 层：视图
#include "../paleotheme.h"
#include "hiddentrackbar.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>

namespace WellComposite
{

HiddenTrackBar::HiddenTrackBar(QWidget *parent)
  : QWidget(parent)
{
  setObjectName(QStringLiteral("wellCompositeHiddenBar"));
  auto *lay = new QHBoxLayout(this);
  lay->setContentsMargins(8, 2, 8, 2);
  lay->setSpacing(4);
  // rebuild() 动态填充；样式遵循 DESIGN.md chip（胶囊、surface 底、text-muted）
  PaleoTheme::applyThemedStyleSheet(this, [] {
    const auto &t = PaleoTheme::tokens();
    return QStringLiteral(
        "HiddenTrackBar { background: %1; border-top: 1px solid %2; }"
        "QToolButton { background: %3; border: 1px solid %2; border-radius: 9px;"
        " padding: 1px 10px; color: %4; font-size: 8pt; }"
        "QToolButton:hover { border-color: %5; color: %6; }"
        "QLabel { color: %4; font-size: 8pt; }")
        .arg(t.surfaceAlt.name(), t.border.name(), t.surface.name(),
             t.textMuted.name(), t.primary.name(), t.primaryText.name());
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

void HiddenTrackBar::rebuild()
{
  // 清掉旧 chip（保留无——本控件没有固定子件）
  const auto children_ = findChildren<QToolButton *>();
  for (auto *btn : children_)
  {
    btn->setParent(nullptr);
    delete btn;
  }
  const auto labels = findChildren<QLabel *>();
  for (auto *lbl : labels)
  {
    lbl->setParent(nullptr);
    delete lbl;
  }

  const int hidden = hiddenCount();
  if (hidden == 0)
  {
    hide();
    return;
  }

  auto *lbl = new QLabel(tr("隐藏道:"), this);
  layout()->addWidget(lbl);

  for (const auto &spec : m_specs)
  {
    if (spec.visible)
      continue;
    auto *chip = new QToolButton(this);
    chip->setText(spec.title);
    chip->setToolTip(tr("点击恢复显示「%1」道").arg(spec.title));
    chip->setCursor(Qt::PointingHandCursor);
    const QString title = spec.title;
    connect(chip, &QToolButton::clicked, this, [this, title]() {
      emit trackRestoreRequested(title);
    });
    layout()->addWidget(chip);
  }
  dynamic_cast<QHBoxLayout *>(layout())->addStretch(1);

  show();
}

} // namespace WellComposite
