// 层：视图
#include "paleoemptystate.h"

#include "paleotheme.h"

#include <QEvent>

PaleoEmptyStateLabel::PaleoEmptyStateLabel(const QString &text, QWidget *host,
                                           Kind kind)
  : QLabel(text, host), m_kind(kind)
{
    setAlignment(Qt::AlignCenter);
    setTextFormat(Qt::PlainText);
    setWordWrap(true);
    setObjectName(kind == Kind::Error     ? QStringLiteral("emptyStateCardError")
                  : kind == Kind::Degraded ? QStringLiteral("emptyStateCardDegraded")
                                           : QStringLiteral("emptyStateCard"));
    // 空态卡片 = 语义色字 + surface 卡片底（失败/降级用 error/warning 字），
    // 圆角 8px（DESIGN.md 卡片档）。活体样式：换主题自动重算。
    PaleoTheme::applyThemedStyleSheet(this, [this] {
      const auto &t = PaleoTheme::tokens();
      const QColor fg = m_kind == Kind::Error    ? t.errorText
                        : m_kind == Kind::Degraded ? t.warningText
                                                    : t.textMuted;
      return QStringLiteral(
                 "background: rgba(%1,%2,%3,0.9); color: %4; padding: 12px 16px;"
                 " border: 1px solid %5; border-radius: 8px;")
          .arg(t.surface.red())
          .arg(t.surface.green())
          .arg(t.surface.blue())
          .arg(fg.name().toUpper(), t.border.name().toUpper());
    });
    if (host)
    {
      host->installEventFilter(this);
      recenter(host->size());
    }
}

void PaleoEmptyStateLabel::setDetailText(const QString &text)
{
    setText(text);
    if (parentWidget())
      recenter(parentWidget()->size());
}

bool PaleoEmptyStateLabel::eventFilter(QObject *obj, QEvent *ev)
{
    if (ev->type() == QEvent::Resize)
      if (auto *w = qobject_cast<QWidget *>(obj))
        recenter(w->size());
    return QLabel::eventFilter(obj, ev);
}

void PaleoEmptyStateLabel::recenter(const QSize &host)
{
    // Bound after measuring: adjustSize() after resize() restores the wide
    // size hint and used to clip guidance in narrow docks.
    const int maxW = qMax(1, host.width() - 24);
    const int w = qMin(maxW, sizeHint().width());
    const int h = qMin(qMax(1, host.height() - 8), qMax(40, heightForWidth(w)));
    resize(w, h);
    move(qMax(0, (host.width() - w) / 2),
         qMax(0, (host.height() - h) / 2));
}
