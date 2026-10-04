#pragma once

#include <QDir>
#include <QEvent>
#include <QWidget>
#include <QtTest>

#include "../../src/ui/paleotheme.h"

namespace paleo::tests
{
// Observe the actual synchronous loading label as it is shown; never unhide
// a completed-state hook or substitute a synthetic loading widget.
class LoadingVisualCapture : public QObject
{
public:
  LoadingVisualCapture(QWidget *root, const QString &name) : m_root(root), m_name(name)
  { qApp->installEventFilter(this); }
  ~LoadingVisualCapture() override { qApp->removeEventFilter(this); }
  bool saved = false;
protected:
  bool eventFilter(QObject *object, QEvent *event) override
  {
    auto *widget = qobject_cast<QWidget *>(object);
    if (!m_attempted && event->type() == QEvent::Paint && widget &&
        widget->objectName() == QStringLiteral("loadingText") && m_root->isAncestorOf(widget))
    {
      m_attempted = true;
      const QString dir = qEnvironmentVariable("PALEO_VISUAL_CAPTURE");
      saved = QDir().mkpath(dir) && m_root->grab().save(dir + QLatin1Char('/') + m_name + ".png");
      // grab() has rendered the native label. Consume this outer paint only
      // in the evidence hook to avoid starting a second painter on its device.
      return true;
    }
    return false;
  }
private:
  QWidget *m_root;
  QString m_name;
  bool m_attempted = false;
};

// Optional evidence from production widgets. The same fixtures and viewport
// are used on both revisions; ordinary test runs do not create artifacts.
inline bool captureVisual(QWidget *widget, const QString &name,
                          const QSize &size = QSize(1100, 700))
{
  const QString dir = qEnvironmentVariable("PALEO_VISUAL_CAPTURE");
  if (dir.isEmpty())
    return true;
  if (!QDir().mkpath(dir))
    return false;
  const auto originalTheme = PaleoTheme::currentTheme();
  widget->resize(size);
  widget->show();
  bool saved = true;
  for (const auto theme : {PaleoTheme::Theme::Light, PaleoTheme::Theme::Dark})
  {
    if (PaleoTheme::currentTheme() != theme)
      PaleoTheme::applyTheme(theme);
    QTest::qWait(100);
    saved &= widget->grab().save(dir + QLatin1Char('/') + name +
        (theme == PaleoTheme::Theme::Light ? "-light.png" : "-dark.png"));
  }
  widget->hide();
  if (PaleoTheme::currentTheme() != originalTheme)
    PaleoTheme::applyTheme(originalTheme);
  return saved;
}
} // namespace paleo::tests
