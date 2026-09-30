// 层：视图
#include "exportengine.h"

#include <QDate>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QPdfWriter>
#include <QClipboard>
#include <QApplication>
#include <QPrinterInfo>
#include <QSvgGenerator>

#include "wellcompositecanvas.h"

namespace WellComposite
{

namespace {

// 参与导出的道（可见 + 打印开关开）
QList<std::shared_ptr<WellTrack>> exportableTracks(const WellCompositeCanvas &canvas)
{
  QList<std::shared_ptr<WellTrack>> out;
  for (const auto &t : canvas.tracks())
    if (t && t->isVisible() && t->isPrintIncluded())
      out << t;
  return out;
}

qreal exportTracksWidth(const QList<std::shared_ptr<WellTrack>> &tracks)
{
  qreal w = 0.0;
  for (const auto &t : tracks)
    w += t->width();
  return w;
}

// D4.12 导出档曲线线宽（屏幕 1px → 导出 +1 加粗）
class CurvePenGuard
{
public:
  explicit CurvePenGuard(const QList<std::shared_ptr<WellTrack>> &tracks) : m_tracks(tracks)
  {
    for (const auto &t : tracks)
    {
      if (auto ct = std::dynamic_pointer_cast<CurveTrack>(t))
      {
        QVector<CurveData> cs = ct->curves();
        for (auto &c : cs)
        {
          m_saved.append(c.penWidth);
          c.penWidth = c.penWidth + 1.0f;
        }
        ct->setCurves(cs);
      }
    }
  }
  ~CurvePenGuard()
  {
    for (const auto &t : m_tracks)
    {
      if (auto ct = std::dynamic_pointer_cast<CurveTrack>(t))
      {
        QVector<CurveData> cs = ct->curves();
        int i = 0;
        for (auto &c : cs)
        {
          if (i < m_saved.size())
            c.penWidth = m_saved.at(i++);
        }
        ct->setCurves(cs);
      }
    }
  }

private:
  QList<std::shared_ptr<WellTrack>> m_tracks;
  QVector<float> m_saved;
};

} // namespace

void ExportEngine::applyExportRenderHints(QPainter &painter)
{
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setRenderHint(QPainter::TextAntialiasing, true);
  painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
}

QStringList ExportEngine::headerLines(const Options &opt)
{
  return {
      QStringLiteral("井名: %1").arg(opt.wellName.isEmpty() ? QStringLiteral("—") : opt.wellName),
      QStringLiteral("项目: %1").arg(opt.projectName.isEmpty() ? QStringLiteral("—") : opt.projectName),
      QStringLiteral("日期: %1").arg(QDate::currentDate().toString(Qt::ISODate)),
      QStringLiteral("比例尺: %1").arg(opt.scaleRatio),
  };
}

// ----------------------------------------------------------------------------
// D4.4 LegendGenerator
// ----------------------------------------------------------------------------
QList<LegendGenerator::LegendEntry> LegendGenerator::collect(
    const ComprehensiveWellData &data, const QList<std::shared_ptr<WellTrack>> &tracks)
{
  QList<LegendEntry> entries;

  // 岩性（花纹名去重按首现序）
  QStringList seenLitho;
  for (const auto &li : data.lithologyIntervals)
  {
    if (li.lithoName.isEmpty() || seenLitho.contains(li.lithoName))
      continue;
    seenLitho << li.lithoName;
    const QBrush b = LithologyPatternFactory::getBrush(li.lithoName);
    QPixmap swatch = b.texture().isNull() ? QPixmap(14, 14) : b.texture().scaled(14, 14);
    if (swatch.isNull())
    {
      swatch = QPixmap(14, 14);
      swatch.fill(li.baseColor);
    }
    LegendEntry e;
    e.symbol = li.lithoName;
    e.detail = QStringLiteral("岩性");
    e.swatch = li.baseColor;
    entries << e;
  }

  // 微相纹理（去重）
  QStringList seenFacies;
  for (const auto &fi : data.faciesIntervals)
  {
    const QString name = fi.microFacies.isEmpty() ? fi.subFacies : fi.microFacies;
    if (name.isEmpty() || seenFacies.contains(name))
      continue;
    seenFacies << name;
    LegendEntry e;
    e.symbol = name;
    e.detail = QStringLiteral("微相");
    e.swatch = fi.microColor;
    entries << e;
  }

  // 曲线（量程 + 单位；以画布上实际在场的道为准）
  for (const auto &t : tracks)
  {
    auto ct = std::dynamic_pointer_cast<CurveTrack>(t);
    if (!ct)
      continue;
    for (const auto &c : ct->curves())
    {
      LegendEntry e;
      e.symbol = c.name;
      e.detail = QStringLiteral("%1~%2 %3")
                     .arg(QString::number(c.minScale, 'f', 0), QString::number(c.maxScale, 'f', 0),
                          c.unit.isEmpty() ? QStringLiteral("—") : c.unit);
      e.swatch = c.color;
      e.isLine = true;
      entries << e;
    }
  }

  // 标志层
  for (const auto &m : data.standardHorizons)
  {
    LegendEntry e;
    e.symbol = m.second;
    e.detail = QStringLiteral("%1 m 标志层").arg(QString::number(m.first, 'f', 1));
    e.swatch = QColor(QStringLiteral("#B45309"));
    entries << e;
  }

  return entries;
}

qreal LegendGenerator::paint(QPainter &painter, const QRectF &rect,
                             const QList<LegendEntry> &entries)
{
  if (entries.isEmpty())
    return 0.0;

  painter.save();
  ExportEngine::applyExportRenderHints(painter);

  QFont f = painter.font();
  f.setPointSize(8);
  painter.setFont(f);
  const QFontMetrics fm(f);

  const qreal rowH = 20.0;
  const qreal colW = rect.width() / 2.0;
  const int rowsPerCol = qMax(1, static_cast<int>(rect.height() / rowH));
  const int cols = 2;

  painter.setPen(QColor(QStringLiteral("#24303E")));
  for (int i = 0; i < entries.size(); ++i)
  {
    const int col = i / rowsPerCol;
    const int row = i % rowsPerCol;
    if (col >= cols)
      break;
    const qreal x = rect.left() + col * colW;
    const qreal y = rect.top() + row * rowH;
    const QRectF symbolRect(x + 2, y + 3, 14, 14);

    if (entries.at(i).isLine)
    {
      painter.setPen(QPen(entries.at(i).swatch, 2.0));
      painter.drawLine(QPointF(symbolRect.left(), symbolRect.center().y()),
                       QPointF(symbolRect.right(), symbolRect.center().y()));
    }
    else
    {
      painter.fillRect(symbolRect, entries.at(i).swatch);
      painter.setPen(QColor(QStringLiteral("#9AA7B4")));
      painter.drawRect(symbolRect);
    }

    painter.setPen(QColor(QStringLiteral("#24303E")));
    const QString text = QStringLiteral("%1 %2").arg(entries.at(i).symbol, entries.at(i).detail);
    painter.drawText(QRectF(symbolRect.right() + 5, y, colW - 25, rowH),
                     Qt::AlignVCenter | Qt::ElideRight, text);
  }
  painter.restore();
  return rowsPerCol * rowH;
}

QStringList LegendGenerator::textLines(const QList<LegendEntry> &entries)
{
  QStringList lines;
  for (const auto &e : entries)
    lines << QStringLiteral("%1\t%2").arg(e.symbol, e.detail);
  return lines;
}

// ----------------------------------------------------------------------------
// 道区域绘制（PDF/PNG/SVG 共用）
// ----------------------------------------------------------------------------
namespace {

void paintTracksRegion(QPainter &p, const QList<std::shared_ptr<WellTrack>> &tracks,
                       double topDepth, double bottomDepth, double pxPerMeter,
                       const QPointF &origin, qreal headerRowH)
{
  qreal x = origin.x();
  for (const auto &t : tracks)
  {
    const QRectF headerRect(x, origin.y(), t->width(), headerRowH);
    t->paintHeader(p, headerRect, -1.0);

    const QRectF bodyRect(x, origin.y() + headerRowH, t->width(),
                          (bottomDepth - topDepth) * pxPerMeter);
    t->paintBody(p, bodyRect, topDepth, bottomDepth, pxPerMeter);
    x += t->width();
  }
}

void paintPageHeader(QPainter &p, const QRectF &rect, const ExportEngine::Options &opt,
                     int pageNo, int totalPages)
{
  p.save();
  const QStringList lines = ExportEngine::headerLines(opt);
  QFont f = p.font();
  f.setPointSize(9);
  f.setBold(true);
  p.setFont(f);
  p.setPen(QColor(QStringLiteral("#24303E")));
  p.drawText(rect.adjusted(0, 0, -60, -rect.height() + 18), Qt::AlignLeft | Qt::AlignVCenter,
             lines.join(QStringLiteral("   |   ")));

  f.setPointSize(8);
  f.setBold(false);
  p.setFont(f);
  p.setPen(QColor(QStringLiteral("#5D6E80")));
  p.drawText(QRectF(rect.right() - 120, rect.top(), 120, 18), Qt::AlignRight | Qt::AlignVCenter,
             QStringLiteral("页 %1/%2").arg(pageNo).arg(totalPages));
  p.setPen(QColor(QStringLiteral("#DFE5EC")));
  p.drawLine(rect.bottomLeft(), rect.bottomRight());
  p.restore();
}

} // namespace

// ----------------------------------------------------------------------------
// D4.6 PNG 渲染
// ----------------------------------------------------------------------------
QImage ExportEngine::renderToImage(const WellCompositeCanvas &canvas,
                                   const ComprehensiveWellData &data, const Options &opt,
                                   QString *errorOut)
{
  const QList<std::shared_ptr<WellTrack>> tracks = exportableTracks(canvas);
  if (tracks.isEmpty())
  {
    if (errorOut)
      *errorOut = QStringLiteral("无可导出的道（全部隐藏或关闭打印）");
    return QImage();
  }

  bool okDenom = false;
  const int denom = opt.scaleRatio.startsWith(QLatin1String("1:"))
                        ? opt.scaleRatio.mid(2).toInt(&okDenom)
                        : 500;
  const double pxPerMeter = 39.3701 * opt.dpi / (okDenom && denom > 0 ? denom : 500);

  const double span = qMax(1.0, opt.bottomDepth - opt.topDepth);
  const qreal headerRowH = 72.0;
  const qreal width = exportTracksWidth(tracks) + 2.0;

  // QImage 高度上限保护（>16384px 时按上限折减等效分辨率）
  const double maxH = 16384.0;
  const double effPpm = std::min(pxPerMeter, (maxH - headerRowH) / span);
  const qreal height = headerRowH + span * effPpm;

  QImage img(qMax(64, qRound(width)), qMax(64, qRound(height)), QImage::Format_ARGB32_Premultiplied);
  img.fill(Qt::white);

  QPainter p(&img);
  applyExportRenderHints(p);
  CurvePenGuard penGuard(tracks);

  if (opt.includeHeader)
  {
    paintPageHeader(p, QRectF(1, 1, width - 2, 20), opt, 1, 1);
  }

  const QPointF origin(1.0, opt.includeHeader ? 24.0 : 2.0);
  paintTracksRegion(p, tracks, opt.topDepth, opt.bottomDepth, effPpm, origin, headerRowH);

  if (opt.includeLegend)
  {
    const QList<LegendGenerator::LegendEntry> entries = LegendGenerator::collect(data, tracks);
    const QRectF legendRect(1.0, img.height() - 150.0, width - 2.0, 148.0);
    p.fillRect(legendRect, Qt::white);
    LegendGenerator::paint(p, legendRect, entries);
  }

  p.end();
  return img;
}

// ----------------------------------------------------------------------------
// D4.9 预览
// ----------------------------------------------------------------------------
QImage ExportEngine::renderPreview(const WellCompositeCanvas &canvas,
                                   const ComprehensiveWellData &data, int heightPx)
{
  Options opt;
  opt.topDepth = canvas.minDepth();
  opt.bottomDepth = canvas.maxDepth();
  opt.includeHeader = false;
  opt.includeLegend = false;
  opt.dpi = 96;
  opt.scaleRatio = QStringLiteral("1:5000"); // 预览快览低清档

  // 按给定高度反推等效 ppm
  const QList<std::shared_ptr<WellTrack>> tracks = exportableTracks(canvas);
  if (tracks.isEmpty())
    return QImage();

  QImage img = renderToImage(canvas, data, opt);
  if (img.isNull())
    return img;
  if (heightPx > 0 && img.height() != heightPx)
    img = img.scaledToHeight(heightPx, Qt::SmoothTransformation);
  return img;
}

// ----------------------------------------------------------------------------
// D4.5 PDF 矢量多页导出
// ----------------------------------------------------------------------------
QString ExportEngine::exportCanvas(const WellCompositeCanvas &canvas,
                                   const ComprehensiveWellData &data, Format format,
                                   const QString &path, const Options &opt)
{
  const QList<std::shared_ptr<WellTrack>> tracks = exportableTracks(canvas);
  if (tracks.isEmpty())
    return QStringLiteral("无可导出的道（全部隐藏或关闭打印）");

  const qreal headerRowH = 72.0;

  if (format == Format::Png)
  {
    QString err;
    const QImage img = renderToImage(canvas, data, opt, &err);
    if (img.isNull())
      return err.isEmpty() ? QStringLiteral("渲染失败") : err;
    if (!img.save(path))
      return QStringLiteral("无法写入文件: %1").arg(path);
    return QString();
  }

  if (format == Format::Svg)
  {
    QSvgGenerator gen;
    gen.setFileName(path);
    gen.setSize(QSize(qMax(64, qRound(exportTracksWidth(tracks) + 2)),
                      qMax(64, qRound(headerRowH + (opt.bottomDepth - opt.topDepth) * 8.0))));
    gen.setViewBox(QRect(0, 0, gen.size().width(), gen.size().height()));
    gen.setTitle(QStringLiteral("综合柱状图 %1").arg(opt.wellName));
    QPainter p(&gen);
    applyExportRenderHints(p);
    CurvePenGuard penGuard(tracks);
    if (opt.includeHeader)
      paintPageHeader(p, QRectF(1, 1, gen.size().width() - 2, 20), opt, 1, 1);
    paintTracksRegion(p, tracks, opt.topDepth, opt.bottomDepth, 8.0,
                      QPointF(1.0, opt.includeHeader ? 24.0 : 2.0), headerRowH);
    if (opt.includeLegend)
    {
      const auto entries = LegendGenerator::collect(data, tracks);
      LegendGenerator::paint(p, QRectF(1, gen.size().height() - 150, gen.size().width() - 2, 148), entries);
    }
    p.end();
    return QString();
  }

  // ---- PDF（QPdfWriter 矢量，多页；D3 后与原生打印共用分页管线）----
  QPdfWriter writer(path);
  writer.setResolution(opt.dpi);
  writer.setPageLayout(QPageLayout(QPageSize(QPageSize::A4), QPageLayout::Portrait,
                                   QMarginsF(12, 14, 12, 14), QPageLayout::Millimeter));
  writer.setTitle(QStringLiteral("综合柱状图 %1").arg(opt.wellName));
  writer.setCreator(QStringLiteral("Paleo Workstation"));
  return exportToPagedDevice(canvas, data, writer, opt);
}

QString ExportEngine::exportToPagedDevice(const WellCompositeCanvas &canvas,
                                          const ComprehensiveWellData &data,
                                          QPagedPaintDevice &device, const Options &opt)
{
  const QList<std::shared_ptr<WellTrack>> tracks = exportableTracks(canvas);
  if (tracks.isEmpty())
    return QStringLiteral("无可导出的道（全部隐藏或关闭打印）");

  const qreal headerRowH = 72.0;

  QPainter p(&device);
  applyExportRenderHints(p);
  CurvePenGuard penGuard(tracks);

  // 设备实际分辨率优先（QPrinter 300/QPrinterInfo 高分辨率档与 opt.dpi 可能
  // 不同——比例尺语义按设备 dpi 换算，不按导出参数硬套）。
  const int dpi = device.logicalDpiY() > 0 ? device.logicalDpiY() : opt.dpi;
  bool okDenom = false;
  const int denom = opt.scaleRatio.startsWith(QLatin1String("1:"))
                        ? opt.scaleRatio.mid(2).toInt(&okDenom)
                        : 500;
  const double pxPerMeter = 39.3701 * dpi / (okDenom && denom > 0 ? denom : 500);

  const QRectF pageRect = QRectF(0, 0, device.width(), device.height());
  const qreal headBlockH = opt.includeHeader ? 26.0 : 2.0;
  const qreal legendBlockH = opt.includeLegend ? 150.0 : 2.0;
  const qreal contentH = pageRect.height() - headBlockH - legendBlockH - 4.0;

  // D4.8 分页策略：metersPerPage 自适应（0 = 按页面容量）
  const double pageMeters = opt.metersPerPage > 0.0
                                ? opt.metersPerPage
                                : std::max(10.0, contentH / pxPerMeter);
  const double totalSpan = std::max(1.0, opt.bottomDepth - opt.topDepth);
  const int totalPages = qMax(1, static_cast<int>(std::ceil(totalSpan / pageMeters)));

  for (int page = 0; page < totalPages; ++page)
  {
    if (page > 0)
      device.newPage();

    const double pTop = opt.topDepth + page * pageMeters;
    const double pBottom = std::min(opt.bottomDepth, pTop + pageMeters);
    const QRectF thisPageRect = QRectF(0, 0, device.width(), device.height());

    if (opt.includeHeader)
      paintPageHeader(p, QRectF(thisPageRect.left() + 1, thisPageRect.top() + 2,
                                thisPageRect.width() - 2, 20),
                      opt, page + 1, totalPages);

    paintTracksRegion(p, tracks, pTop, pBottom, pxPerMeter,
                      QPointF(1.0, headBlockH), headerRowH);

    if (opt.includeLegend && page == totalPages - 1)
    {
      const auto entries = LegendGenerator::collect(data, tracks);
      const QRectF legendRect(thisPageRect.left() + 1,
                              thisPageRect.bottom() - legendBlockH - 2,
                              thisPageRect.width() - 2, legendBlockH);
      p.fillRect(legendRect, Qt::white);
      LegendGenerator::paint(p, legendRect, entries);
    }
  }
  p.end();
  return QString();
}

bool ExportEngine::nativePrintAvailable()
{
  return !QPrinterInfo::availablePrinters().isEmpty();
}

} // namespace WellComposite
