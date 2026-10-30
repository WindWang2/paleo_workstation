// 层：视图
#include "paleoicons.h"
#include "paleotheme.h"

#include <QAbstractButton>
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
        // #481 P3-2：逐像素 CPU 重着色按（尺寸/DPR/模式/状态/主题）缓存
        //——ribbon hover 反复取图不再每像素重算；键空间受限于标准档位
        //（尺寸×模式×状态×主题），有界。
        const QString key = QStringLiteral("%1x%2@%3|%4|%5|%6")
                                .arg(size.width()).arg(size.height())
                                .arg(double(scale))
                                .arg(int(mode)).arg(int(state)).arg(int(theme));
        const auto hit = m_recolorCache.constFind(key);
        if (hit != m_recolorCache.constEnd())
          return hit.value();
        QImage img = pm.toImage().convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < img.height(); ++y)
          for (int x = 0; x < img.width(); ++x)
          {
            const QRgb c = img.pixel(x, y);
            if (!qAlpha(c)) continue;
            const QColor ink = mode == QIcon::Disabled ? t.textDisabled : t.textMuted;
            img.setPixel(x, y, mode == QIcon::Disabled
                ? qRgba(ink.red(), ink.green(), ink.blue(), qAlpha(c))
                // 保留原生图标的亮部、色彩与内部轮廓；只提亮近黑的中性描线。
                : (qMax(qRed(c), qMax(qGreen(c), qBlue(c))) -
                       qMin(qRed(c), qMin(qGreen(c), qBlue(c))) < 48 &&
                   qGray(c) < qGray(ink.rgb()) / 2
                    ? qRgba(ink.red(), ink.green(), ink.blue(), qAlpha(c)) : c));
          }
        pm = QPixmap::fromImage(img);
        pm.setDevicePixelRatio(scale);
        m_recolorCache.insert(key, pm);
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
  mutable QHash<QString, QPixmap> m_recolorCache; // #481 P3-2：重着色缓存
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

QIcon PaleoIcons::commandIcon(const QString &command)
{
  static const struct Entry { const char *command; const char *icon; } entries[] = {
      {"baseMapShow", "mActionShowAllLayers.svg"},
      {"baseMapStart", "mActionStart.svg"},
      {"cancelRunButton", "mActionCancelEdits.svg"},
      {"dataRedoButton", "mActionRedo.svg"},
      {"dataUndoButton", "mActionUndo.svg"},
      {"extractWellFactorsButton", "mIconPointLayer.svg"},
      {"factorCancelButton", "mActionCancelEdits.svg"},
      {"factorNextStepButton", "mActionZoomToSelected.svg"},
      {"factorOneClickButton", "processingModel.svg"},
      {"factorTaskCenterButton", "mActionHistory.svg"},
      {"generateFactorButton", "mActionStart.svg"},
      {"healthCheckButton", "mActionIdentify.svg"},
      {"importAddData", "mActionAdd.svg"},
      {"importBoundary", "mActionCapturePolygon.svg"},
      {"importFiles", "mActionFileOpen.svg"},
      {"importFolder", "mIconFolderOpen.svg"},
      {"importLedgerButton", "mActionHistory.svg"},
      {"importOfflineBasemap", "mIconRasterLayer.svg"},
      {"importPaleomap", "mActionAddLayer.svg"},
      {"importPaleomapFolder", "mIconFolderOpen.svg"},
      {"importSeismic", "mIconRasterLayer.svg"},
      {"importWellLogs", "mLayoutItemElevationProfile.svg"},
      {"importWells", "mActionCapturePoint.svg"},
      {"listViewButton", "mActionOpenTable.svg"},
      {"maintainWellFactorsButton", "mActionOpenTable.svg"},
      {"mamclButton", "processingAlgorithm.svg"},
      {"openSeismicSectionButton", "mIconRasterLayer.svg"},
      {"openWellFaciesButton", "mLayoutItemElevationProfile.svg"},
      {"recomputeWellFactorsButton", "mActionRefresh.svg"},
      {"runButton", "mActionStart.svg"},
      {"subIntervalS1", "mIconRasterLayer.svg"},
      {"subIntervalS2", "mIconRasterLayer.svg"},
      {"subIntervalS3", "mIconRasterLayer.svg"},
      {"subIntervalS4", "mIconRasterLayer.svg"},
      {"subIntervalAggregate", "processingAlgorithm.svg"},
      {"subIntervalExpand", "mActionIdentify.svg"},
      {"subIntervalSchemeCombo", "mIconPolygonLayer.svg"},
      {"subIntervalRevisionCombo", "mActionHistory.svg"},

      {"storageGovernanceButton", "mActionOptions.svg"},
      {"treeViewButton", "mActionShowAllLayers.svg"},
      {"workbench.cancel", "mActionCancelEdits.svg"},
      {"workbench.checkTopology", "mActionIdentify.svg"},
      {"workbench.compose", "processingAlgorithm.svg"},
      {"workbench.consistency", "mActionOpenTable.svg"},
      {"workbench.contours", "mActionAddLayer.svg"},
      {"workbench.copy", "mActionEditCopy.svg"},
      {"workbench.delete", "mActionDeleteSelected.svg"},
      {"workbench.draw", "mActionCapturePolygon.svg"},
      {"workbench.exportMap", "mActionSaveAsPDF.svg"},
      {"workbench.exportVector", "mActionSharingExport.svg"},
      {"workbench.factor", "processingModel.svg"},
      {"workbench.import", "mActionAddLayer.svg"},
      {"workbench.polygonize", "mActionCapturePolygon.svg"},
      {"workbench.predict", "mActionStart.svg"},
      {"workbench.referencePanel", "mActionShowAllLayers.svg"},
      {"workbench.show", "mActionZoomToSelected.svg"},
      {"workbench.welltracks", "mLayoutItemElevationProfile.svg"},
  };
  for (const auto &entry : entries)
    if (command == QLatin1String(entry.command))
      return qgisTheme(QLatin1String(entry.icon));
  return {};
}

int PaleoIcons::applyCommandIcons(QWidget *root)
{
  if (!root)
    return 0;
  int applied = 0;
  for (auto *button : root->findChildren<QAbstractButton *>())
  {
    if (!button->icon().isNull())
      continue;
    const QIcon icon = commandIcon(button->objectName());
    if (icon.isNull())
      continue;
    button->setIcon(icon);
    button->setIconSize(toolbarSize());
    ++applied;
  }
  return applied;
}

QIcon PaleoIcons::inactive() { return QIcon(new ThemeIconEngine({}, Glyph::Inactive)); }
QIcon PaleoIcons::dataLine(const QColor &color)
{ return QIcon(new ThemeIconEngine({}, Glyph::DataLine, false, color)); }
