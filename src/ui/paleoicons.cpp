// 层：视图
#include "paleoicons.h"
#include "paleotheme.h"

#include <QIconEngine>
#include <QPainter>
#include <QPixmap>
#include <qgsapplication.h>

namespace
{
enum class Glyph { Source, Maximize, Restore, Inactive, DataLine };

// QGIS 的 SVG 画布档位，不是 UI 间距：保留上游 16/20/24/32 尺寸契约。
const QList<QSize> glyphSizes{QSize(16, 16), QSize(20, 20), QSize(24, 24), QSize(32, 32)};

class ThemeIconEngine final : public QIconEngine
{
public:
  ThemeIconEngine(QIcon source, Glyph glyph = Glyph::Source, bool forceDark = false, QColor dataColor = {})
      : m_source(std::move(source)), m_glyph(glyph), m_forceDark(forceDark), m_dataColor(dataColor) {}

  QString iconName() override { return QStringLiteral("PaleoThemeIcon"); }
  QIconEngine *clone() const override { return new ThemeIconEngine(*this); }
  QList<QSize> availableSizes(QIcon::Mode mode, QIcon::State state) override
  {
    const auto sizes = m_source.availableSizes(mode, state);
    return sizes.isEmpty() ? glyphSizes : sizes;
  }
  QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override
  {
    return scaledPixmap(size, mode, state, 1.0);
  }
  QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state,
                       qreal scale) override
  {
    const auto theme = m_forceDark ? PaleoTheme::Theme::Dark : PaleoTheme::currentTheme();
    const auto &t = PaleoTheme::tokens(theme);
    QPixmap pm;
    if (m_glyph == Glyph::Source)
    {
      pm = m_source.pixmap(size, scale, QIcon::Normal, state);
      if (pm.isNull()) return pm;
      if (theme == PaleoTheme::Theme::Dark || mode == QIcon::Disabled)
      {
        QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < img.height(); ++y)
          for (int x = 0; x < img.width(); ++x)
          {
            const QRgb c = img.pixel(x, y);
            if (!qAlpha(c)) continue;
            const QColor ink = mode == QIcon::Disabled ? t.textDisabled : t.textMuted;
            img.setPixel(x, y, mode == QIcon::Disabled
                ? qRgba(ink.red(), ink.green(), ink.blue(), qAlpha(c))
                : qRgba(qMin(255, qRed(c) + ink.red()),
                        qMin(255, qGreen(c) + ink.green()),
                        qMin(255, qBlue(c) + ink.blue()), qAlpha(c)));
          }
        pm = QPixmap::fromImage(img);
        pm.setDevicePixelRatio(scale);
      }
      return pm;
    }

    pm = QPixmap(size * scale);
    pm.setDevicePixelRatio(scale);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    // 线稿用 SVG 同级比例（原有形状与留白），随 DPR 光栅化而非放大位图。
    const qreal s = qMin(size.width(), size.height());
    p.translate((size.width() - s) / 2.0, (size.height() - s) / 2.0);
    QPen pen(mode == QIcon::Disabled ? t.textDisabled : t.text);
    pen.setWidthF(qMax(1.1, s * 0.085));
    pen.setJoinStyle(Qt::MiterJoin);
    p.setPen(pen);
    if (m_glyph == Glyph::Inactive)
    {
      p.setPen(Qt::NoPen);
      p.setBrush(t.textDisabled);
      p.drawEllipse(QRectF(s / 6.0, s / 6.0, s * 2.0 / 3.0, s * 2.0 / 3.0));
    }
    else if (m_glyph == Glyph::DataLine)
    {
      const QColor color = mode == QIcon::Disabled ? t.textDisabled : m_dataColor;
      const QColor halo = PaleoTheme::dataHaloColor(color, theme);
      if (halo.isValid())
      {
        p.setPen(QPen(halo, 4.0));
        p.drawLine(QPointF(s * 0.16, s * 0.5), QPointF(s * 0.84, s * 0.5));
      }
      p.setPen(QPen(color, 2.0));
      p.drawLine(QPointF(s * 0.16, s * 0.5), QPointF(s * 0.84, s * 0.5));
    }
    else if (m_glyph == Glyph::Maximize)
    {
      const qreal m = s * 0.16;
      p.setBrush(Qt::NoBrush);
      p.drawRect(QRectF(m, m, s - 2 * m, s - 2 * m));
    }
    else
    {
      p.drawRect(QRectF(s * 0.30, s * 0.10, s * 0.56, s * 0.56));
      p.setBrush(t.surface);
      p.drawRect(QRectF(s * 0.12, s * 0.32, s * 0.56, s * 0.56));
    }
    return pm;
  }
  void paint(QPainter *p, const QRect &rect, QIcon::Mode mode, QIcon::State state) override
  {
    p->drawPixmap(rect, scaledPixmap(rect.size(), mode, state,
                                    p->device()->devicePixelRatioF()));
  }
private:
  QIcon m_source;
  Glyph m_glyph;
  bool m_forceDark;
  QColor m_dataColor;
};
} // namespace

QIcon PaleoIcons::themed(const QIcon &icon)
{
  return icon.isNull() || icon.name() == QStringLiteral("PaleoThemeIcon")
             ? icon : QIcon(new ThemeIconEngine(icon));
}

QIcon PaleoIcons::qgisTheme(const QString &name)
{
  return themed(QgsApplication::getThemeIcon(QStringLiteral("/") + name));
}

QIcon PaleoIcons::tintForDarkTheme(const QIcon &icon)
{
  return icon.isNull() ? icon : QIcon(new ThemeIconEngine(icon, Glyph::Source, true));
}

QIcon PaleoIcons::maximize() { return QIcon(new ThemeIconEngine({}, Glyph::Maximize)); }
QIcon PaleoIcons::restore() { return QIcon(new ThemeIconEngine({}, Glyph::Restore)); }

QSize PaleoIcons::toolbarSize() { return QSize(18, 18); }
QIcon PaleoIcons::inactive() { return QIcon(new ThemeIconEngine({}, Glyph::Inactive)); }
QIcon PaleoIcons::dataLine(const QColor &color)
{ return QIcon(new ThemeIconEngine({}, Glyph::DataLine, false, color)); }
