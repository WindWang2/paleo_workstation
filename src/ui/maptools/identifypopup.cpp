// 层：视图
#include "identifypopup.h"

#include "../paleotheme.h"

#include <QEnterEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>

IdentifyPopup::IdentifyPopup(QWidget *parent)
  : QFrame(parent)
{
  setObjectName(QStringLiteral("identifyPopup"));
  const auto &tokens = PaleoTheme::tokens();
  setStyleSheet(QStringLiteral(
      "QFrame#identifyPopup { background: %1; border: 1px solid %2; border-radius: %3px; }"
      "QLabel { color: %4; background: transparent; }"
      "QLabel#identifySubtitle, QLabel#identifyCounter { color: %5; }")
                    .arg(tokens.surface.name(), tokens.border.name())
                    .arg(tokens.radiusMd)
                    .arg(tokens.text.name(), tokens.textMuted.name()));
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(tokens.spacingSm, tokens.spacingSm, tokens.spacingSm, tokens.spacingSm);
  layout->setSpacing(tokens.spacingXs);
  m_title = new QLabel(this);
  m_title->setObjectName(QStringLiteral("identifyTitle"));
  m_title->setWordWrap(true);
  QFont titleFont = m_title->font();
  titleFont.setBold(true);
  m_title->setFont(titleFont);
  layout->addWidget(m_title);
  m_subtitle = new QLabel(this);
  m_subtitle->setObjectName(QStringLiteral("identifySubtitle"));
  m_subtitle->setWordWrap(true);
  layout->addWidget(m_subtitle);
  m_switchRow = new QWidget(this);
  auto *switches = new QHBoxLayout(m_switchRow);
  switches->setContentsMargins(0, 0, 0, 0);
  m_prev = new QToolButton(m_switchRow);
  m_prev->setObjectName(QStringLiteral("identifyPrev"));
  m_prev->setText(QStringLiteral("‹"));
  m_prev->setAccessibleName(tr("上一处"));
  m_next = new QToolButton(m_switchRow);
  m_next->setObjectName(QStringLiteral("identifyNext"));
  m_next->setText(QStringLiteral("›"));
  m_next->setAccessibleName(tr("下一处"));
  m_counter = new QLabel(m_switchRow);
  m_counter->setObjectName(QStringLiteral("identifyCounter"));
  switches->addWidget(m_prev);
  switches->addWidget(m_counter, 1);
  switches->addWidget(m_next);
  layout->addWidget(m_switchRow);
  auto *fields = new QWidget(this);
  m_fields = new QVBoxLayout(fields);
  m_fields->setContentsMargins(0, 0, 0, 0);
  m_fields->setSpacing(tokens.spacingXs);
  layout->addWidget(fields);
  setMinimumWidth(220);
  setMaximumWidth(320);
  connect(m_prev, &QToolButton::clicked, this, [this] { showIndex(m_index - 1); });
  connect(m_next, &QToolButton::clicked, this, [this] { showIndex(m_index + 1); });
  hide();
}

void IdentifyPopup::setHits(const QVector<IdentifyHit> &hits)
{
  const QString keep = m_hits.value(m_index).subtitle;
  m_hits = hits;
  int next = 0;
  for (int i = 0; i < m_hits.size(); ++i)
  {
    if (m_hits.at(i).subtitle == keep)
    {
      next = i;
      break;
    }
  }
  if (m_hits.isEmpty())
  {
    hide();
    m_index = 0;
    return;
  }
  showIndex(next);
}

void IdentifyPopup::showIndex(int index)
{
  if (m_hits.isEmpty())
    return;
  m_index = (index + m_hits.size()) % m_hits.size();
  const IdentifyHit &hit = m_hits.at(m_index);
  m_title->setText(hit.title);
  m_subtitle->setText(hit.subtitle);
  m_switchRow->setVisible(m_hits.size() > 1);
  m_counter->setText(tr("%1 / %2").arg(m_index + 1).arg(m_hits.size()));
  while (QLayoutItem *item = m_fields->takeAt(0))
  {
    delete item->widget();
    delete item;
  }
  const int limit = qMin(6, hit.fields.size());
  for (int i = 0; i < limit; ++i)
  {
    auto *row = new QLabel(tr("%1：%2").arg(hit.fields.at(i).first, hit.fields.at(i).second), this);
    row->setWordWrap(true);
    row->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_fields->addWidget(row);
  }
  adjustSize();
}

void IdentifyPopup::showNear(const QPoint &anchor)
{
  adjustSize();
  QPoint pos = anchor + QPoint(16, 16);
  if (QWidget *host = parentWidget())
  {
    const QRect bounds = host->rect();
    if (pos.x() + width() > bounds.right())
      pos.setX(qMax(0, anchor.x() - width() - 8));
    if (pos.y() + height() > bounds.bottom())
      pos.setY(qMax(0, anchor.y() - height() - 8));
  }
  move(pos);
  show();
  raise();
}

void IdentifyPopup::enterEvent(QEnterEvent *event)
{
  QFrame::enterEvent(event);
  emit pointerInside(true);
}

void IdentifyPopup::leaveEvent(QEvent *event)
{
  QFrame::leaveEvent(event);
  emit pointerInside(false);
}
