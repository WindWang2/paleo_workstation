#include "seismicpreviewpanel.h"

#include "../io/segyreader.h"
#include "../linkage/seismicmaplink.h"
#include "../linkage/selectioncontext.h"

#include <QGraphicsLineItem>
#include <QGraphicsPathItem>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsView>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QtMath>

struct SeismicLineData
{
  QVector<SegyTrace> traces;
  int samplesPerTrace = 0;
  float sampleIntervalUs = 0.0f;
};

// ---------------------------------------------------------------------------
// Scene scaffold: a rounded-rect plot card, asset label top-left, a TWT axis,
// the line's baseline with CDP tick marks, per-tick wiggle glyphs and a
// deterministic pseudo-trace profile + faint horizon bands hanging below.
// Real trace rendering replaces this stub later — until then the prototype
// intent (剖面子面板) must stay visible. Colors are DESIGN.md tokens (same
// palette as correlationpanel.cpp).
// ---------------------------------------------------------------------------
namespace
{
  const char kCtxProp[] = "paleo.seismic.ctx"; // QObject* (SelectionContext) on the link
  const char kOrigin[]  = "seismicpanel";      // SelectionContext origin tag

  constexpr qreal kSceneW    = 640.0;
  constexpr qreal kSceneH    = 360.0;
  constexpr qreal kMargin    = 20.0;
  constexpr qreal kLabelBand = 34.0;
  constexpr qreal kAxisW     = 44.0;
  constexpr qreal kRadius    = 4.0;   // rounded.sm
  constexpr qreal kBaselineY = 86.0;  // surface datum inside the plot card
  constexpr qreal kTickEvery = 44.0;  // CDP tick spacing
  constexpr qreal kGlyphLen  = 46.0;  // wiggle glyph length hanging off baseline
  constexpr double kPi       = 3.14159265358979323846;

  const QColor kSurface(QStringLiteral("#FFFFFF"));
  const QColor kSurfaceAlt(QStringLiteral("#EDF1F5"));
  const QColor kBorder(QStringLiteral("#DFE5EC"));
  const QColor kText(QStringLiteral("#24303E"));
  const QColor kTextMuted(QStringLiteral("#5D6E80"));

  // DESIGN.md `label` token: 8pt muted captions mark panel groups.
  QLabel *caption(const QString &text, QWidget *parent)
  {
    auto *l = new QLabel(text, parent);
    QFont f = l->font();
    f.setPointSize(8);
    l->setFont(f);
    l->setStyleSheet(QStringLiteral("color: #5D6E80;")); // text-muted
    return l;
  }

  QGraphicsSimpleTextItem *sceneText(QGraphicsScene *scene, const QString &text,
                                     int pointSize, const QColor &color)
  {
    auto *t = scene->addSimpleText(text);
    QFont f = t->font();
    f.setPointSize(pointSize);
    t->setFont(f);
    t->setBrush(color);
    return t;
  }
} // namespace

SeismicPreviewPanel::SeismicPreviewPanel(SeismicMapLink *link, QWidget *parent)
  : QWidget(parent), m_link(link)
{
  auto *lay = new QHBoxLayout(this);
  lay->setContentsMargins(8, 8, 8, 8); // spacing.sm
  lay->setSpacing(8);

  // ---- left: seismic line list ----
  auto *left = new QWidget(this);
  auto *leftLay = new QVBoxLayout(left);
  leftLay->setContentsMargins(0, 0, 0, 0);
  leftLay->setSpacing(4); // spacing.xs
  leftLay->addWidget(caption(tr("地震测线"), left));
  m_list = new QListWidget(left);
  m_list->setObjectName(QStringLiteral("seismicList"));
  m_list->setAccessibleName(tr("地震测线列表"));
  m_list->setMinimumWidth(160);
  m_list->setMaximumWidth(240);
  leftLay->addWidget(m_list, 1);
  lay->addWidget(left);

  // ---- right: preview cell — view + overlaid empty label share one grid
  // cell so the label never shifts the view's geometry (correlationpanel idiom).
  auto *cell = new QWidget(this);
  auto *grid = new QGridLayout(cell);
  grid->setContentsMargins(0, 0, 0, 0);
  grid->setSpacing(0);

  m_scene = new QGraphicsScene(this);
  m_view = new QGraphicsView(m_scene, cell);
  m_view->setObjectName(QStringLiteral("seismicPreview"));
  m_view->setAccessibleName(tr("地震剖面预览"));
  m_view->setFrameShape(QFrame::NoFrame);
  m_view->setRenderHint(QPainter::Antialiasing);
  m_view->setBackgroundBrush(kSurfaceAlt);
  m_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  m_view->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  grid->addWidget(m_view, 0, 0);

  m_emptyLabel = new QLabel(tr("还没有地震数据 — 通过「数据导入」添加测线"), cell);
  m_emptyLabel->setObjectName(QStringLiteral("seismicEmptyLabel"));
  m_emptyLabel->setAlignment(Qt::AlignCenter);
  m_emptyLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
  m_emptyLabel->setStyleSheet(QStringLiteral("color: %1;").arg(kTextMuted.name()));
  grid->addWidget(m_emptyLabel, 0, 0);
  m_emptyLabel->setVisible(true); // born with no assets — §42.4 guidance

  lay->addWidget(cell, 1);

  connect(m_list, &QListWidget::currentItemChanged, this,
          [this](QListWidgetItem *cur, QListWidgetItem *) { onListSelection(cur); });

  context();                  // resolve + subscribe if wiring is already in place
  updatePreview(QString());   // idle scaffold — never a blank panel
}

// ---------------------------------------------------------------------------
// Context resolution — the link owns no public selection API, so picks go to
// the SelectionContext it was built with. Discovery order: documented
// "paleo.seismic.ctx" dynamic property → SelectionContext parent → child of
// the link → found under the link's parent. Resolved lazily so the property
// may be set before or after panel construction.
// ---------------------------------------------------------------------------
SelectionContext *SeismicPreviewPanel::context()
{
  if (m_ctx || !m_link)
    return m_ctx;

  if (auto *ctx = qobject_cast<SelectionContext *>(
          m_link->property(kCtxProp).value<QObject *>()))
    m_ctx = ctx;
  else if (auto *ctx = qobject_cast<SelectionContext *>(m_link->parent()))
    m_ctx = ctx;
  else if (auto *ctx = m_link->findChild<SelectionContext *>())
    m_ctx = ctx;
  else if (m_link->parent())
    m_ctx = m_link->parent()->findChild<SelectionContext *>();

  if (m_ctx)
    connect(m_ctx, &SelectionContext::selectionChanged, this,
            [this](const QStringList &ids, const QString &origin) {
              if (origin != QLatin1String(kOrigin)) // own echo — skip (correlationpanel pattern)
                applyExternalSelection(ids);
            });
  return m_ctx;
}

void SeismicPreviewPanel::setSeismicAssets(const QVector<QPair<QString, QString>> &assets)
{
  m_assets = assets;
  rebuildList();
  if (auto *ctx = context()) // a selection may predate the assets — honor it
    applyExternalSelection(ctx->selectedIds());
  else
    updatePreview(hasAsset(m_currentId) ? m_currentId : QString());
}

void SeismicPreviewPanel::addSeismicAsset(const QString &id, const QString &label)
{
  m_assets.append({id, label});
  rebuildList();
}

void SeismicPreviewPanel::loadLineFromFile(const QString &assetId, const QString &segyPath)
{
  if (assetId.isEmpty() || segyPath.isEmpty())
    return;

  SegyReader reader;
  QString err;
  if (!reader.open(segyPath, &err))
  {
    qWarning("SeismicPreviewPanel::loadLineFromFile: failed to open '%s': %s",
             qPrintable(segyPath), qPrintable(err));
    return;
  }

  auto data = std::make_shared<SeismicLineData>();
  // §2/§7：底 dock 预览同样只解码一条测线（首条 inline），不再全量 readAll。
  const QVector<qint32> inlines = reader.inlineNumbers();
  QVector<SegyTrace> line;
  if (!inlines.isEmpty())
    reader.readInline(inlines.front(), &line, &err);
  if (line.isEmpty())
    line = reader.traces(); // 无 inline 头的退化文件兜底（小文件）
  data->traces = line;
  data->samplesPerTrace = reader.samplesPerTrace();
  data->sampleIntervalUs = reader.sampleIntervalUs();

  m_lineTraces.insert(assetId, data);

  if (!hasAsset(assetId))
  {
    addSeismicAsset(assetId, assetId);
  }

  if (m_currentId.isEmpty() || m_currentId == assetId)
  {
    if (m_list)
    {
      const QSignalBlocker blocker(m_list);
      for (int i = 0; i < m_list->count(); ++i)
      {
        if (m_list->item(i)->data(Qt::UserRole).toString() == assetId)
        {
          m_list->setCurrentRow(i);
          break;
        }
      }
    }
    updatePreview(assetId);
  }
}

void SeismicPreviewPanel::rebuildList()
{
  if (!m_list)
    return;
  const QSignalBlocker blocker(m_list); // repopulating must not fire a pick
  m_list->clear();
  for (const auto &a : m_assets)
  {
    auto *it = new QListWidgetItem(a.second, m_list);
    it->setData(Qt::UserRole, a.first);
  }
  if (m_emptyLabel)
    m_emptyLabel->setVisible(m_assets.isEmpty());
}

void SeismicPreviewPanel::onListSelection(QListWidgetItem *current)
{
  if (!current)
    return; // cleared selection keeps the last preview — ctx clear() handles idles
  const QString id = current->data(Qt::UserRole).toString();
  if (id.isEmpty())
    return;

  // Selection first, signal last — when seismicSelected reaches observers the
  // context broadcast has already landed and the link's layer is consistent.
  if (auto *ctx = context())
  {
    ctx->setSelection({id}, QLatin1String(kOrigin)); // link applies it (direction B)
  }
  else if (m_link)
  {
    // Degraded wiring: no context reachable — drive the link's real selection
    // slot directly so the layer pick still lands. The hub is bypassed, so
    // other SelectionContext consumers won't hear it (documented fallback).
    QMetaObject::invokeMethod(m_link, "onContextSelection", Qt::DirectConnection,
                              Q_ARG(QStringList, QStringList{id}),
                              Q_ARG(QString, QString::fromLatin1(kOrigin)));
  }
  updatePreview(id);
  emit seismicSelected(id);
}

void SeismicPreviewPanel::applyExternalSelection(const QStringList &ids)
{
  QString match;
  for (const QString &id : ids)
  {
    if (hasAsset(id))
    {
      match = id;
      break;
    }
  }

  if (m_list) // sync the row without re-entering onListSelection
  {
    const QSignalBlocker blocker(m_list);
    int row = -1;
    if (!match.isEmpty())
      for (int i = 0; i < m_list->count(); ++i)
        if (m_list->item(i)->data(Qt::UserRole).toString() == match)
        {
          row = i;
          break;
        }
    m_list->setCurrentRow(row);
  }
  updatePreview(match);
}

bool SeismicPreviewPanel::hasAsset(const QString &id) const
{
  for (const auto &a : m_assets)
    if (a.first == id)
      return true;
  return false;
}

QString SeismicPreviewPanel::assetLabel(const QString &id) const
{
  for (const auto &a : m_assets)
    if (a.first == id)
      return a.second;
  return {};
}

// ---------------------------------------------------------------------------
// Preview render — stubbed seismic section. Idle state (empty/unknown id)
// draws a card + guidance hint; a known line draws the full scaffold.
// ---------------------------------------------------------------------------
void SeismicPreviewPanel::updatePreview(const QString &assetId)
{
  m_currentId = assetId;
  if (!m_scene)
    return;
  m_scene->clear();
  m_scene->setSceneRect(0, 0, kSceneW, kSceneH);

  const qreal l = kMargin + kAxisW;
  const qreal r = kSceneW - kMargin;
  const qreal t = kLabelBand;
  const qreal b = kSceneH - kMargin;

  // Plot card — surface on the view's surface-alt background.
  QPainterPath framePath;
  framePath.addRoundedRect(QRectF(l - 8, t - 10, (r - l) + 16, (b - t) + 10),
                           kRadius, kRadius);
  auto *frame = m_scene->addPath(framePath, QPen(kBorder, 1.0), QBrush(kSurface));
  frame->setZValue(-1);

  const QString label = assetLabel(assetId);
  if (label.isEmpty())
  {
    // Idle scaffold — a ghost baseline telegraphs where the section lands.
    const qreal midY = t + (b - t) / 2.0;
    m_scene->addLine(l + 8, midY, r - 8, midY,
                     QPen(kBorder, 1.0, Qt::DashLine));
    auto *hint = sceneText(m_scene, tr("选择测线以预览剖面"), 9, kTextMuted);
    const QRectF hb = hint->boundingRect();
    hint->setPos(l + (r - l - hb.width()) / 2.0, midY - hb.height() - 10);
    return;
  }

  // Title: line label + asset id + honest "stub" note.
  auto it = m_lineTraces.constFind(assetId);
  const bool hasRealData = (it != m_lineTraces.constEnd() && it.value() &&
                            !it.value()->traces.isEmpty() &&
                            it.value()->samplesPerTrace > 0);

  if (hasRealData)
  {
    const auto &lineData = *(it.value());
    const auto &traces = lineData.traces;
    const int traceCount = traces.size();
    const int samplesPerTrace = lineData.samplesPerTrace;

    // =========================================================================
    // Design Decision: Variable Density (VD / 变密度) Profile vs. Wiggle Trace
    // =========================================================================
    // Variable Density (VD) raster rendering is selected over traditional
    // wiggle-trace / variable-area (WT/VA) polylines for three core reasons:
    // 1. Rendering Performance & Scalability: A real seismic profile contains hundreds
    //    to thousands of traces, each with thousands of sample points. Rendering
    //    wiggle traces with QPainterPath polylines and filled polygon loops creates
    //    tens of thousands of scene items / path nodes, causing significant UI
    //    framerate drops and memory bloat during pan/zoom. In contrast, VD renders
    //    the entire section into a single QImage / QGraphicsPixmapItem with O(1)
    //    scene complexity and hardware-accelerated raster blits.
    // 2. Information Density & 2D Overview: In a compact preview panel (640x360),
    //    adjacent wiggle traces heavily overlap, creating illegible black ink
    //    saturation ("trace clutter"). Grayscale VD provides high dynamic range
    //    visualization of continuous impedance reflectors, stratigraphic unconformities,
    //    and fault offsets across the full section width.
    // 3. DESIGN.md Compliance: Conforms to the Paleo Workbench design system's
    //    restrained aesthetic (order, density, and professional GIS utility without
    //    decorative visual clutter).
    // =========================================================================

    // Title: line label
    auto *title = sceneText(m_scene, label, 9, kText);
    title->setPos(l, t - 4);

    // Calculate total duration in ms
    const float dtUs = (lineData.sampleIntervalUs > 0.0f) ? lineData.sampleIntervalUs : 2000.0f;
    const double totalMs = static_cast<double>(samplesPerTrace) * dtUs / 1000.0;
    const int totalMsInt = static_cast<int>(std::round(totalMs));

    // Caption text: e.g. %1 — %2 traces × %3 samples (%4 ms)
    auto *sub = sceneText(m_scene,
                          tr("%1 — %2 traces × %3 samples (%4 ms)")
                            .arg(assetId)
                            .arg(traceCount)
                            .arg(samplesPerTrace)
                            .arg(totalMsInt),
                          8, kTextMuted);
    sub->setPos(l + title->boundingRect().width() + 12, t - 2);

    // TWT axis (left): actual time labels based on samples * dt / 1000 ms
    const qreal axisX = l + 2;
    m_scene->addLine(axisX, kBaselineY, axisX, b - 4, QPen(kTextMuted, 1.0));
    const int twtTicks[3] = {
      0,
      static_cast<int>(std::round(totalMs * 0.5)),
      totalMsInt
    };
    for (int i = 0; i < 3; ++i)
    {
      const qreal y = kBaselineY + i * (b - 4 - kBaselineY) / 2.0;
      m_scene->addLine(axisX - 4, y, axisX, y, QPen(kTextMuted, 1.0));
      auto *tl = sceneText(m_scene, QString::number(twtTicks[i]), 8, kTextMuted);
      const QRectF tb = tl->boundingRect();
      tl->setPos(axisX - 6 - tb.width(), y - tb.height() / 2.0);
    }
    auto *axisCap = sceneText(m_scene, tr("TWT(ms)"), 8, kTextMuted);
    axisCap->setPos(l - kAxisW + 4, t - 4);

    // Plot area rect: QRectF(l + 8, kBaselineY, r - l - 16, b - 4 - kBaselineY)
    const QRectF plotRect(l + 8, kBaselineY, r - l - 16, b - 4 - kBaselineY);

    // Find amplitude extrema for symmetric normalization
    float maxAbs = 0.0f;
    for (const auto &tr : traces)
    {
      for (float s : tr.samples)
      {
        float a = qAbs(s);
        if (a > maxAbs)
          maxAbs = a;
      }
    }

    // Build QImage for Variable Density profile (width = traceCount, height = samplesPerTrace)
    QImage img(traceCount, samplesPerTrace, QImage::Format_RGB32);
    for (int y = 0; y < samplesPerTrace; ++y)
    {
      QRgb *scanLine = reinterpret_cast<QRgb *>(img.scanLine(y));
      for (int x = 0; x < traceCount; ++x)
      {
        const auto &smps = traces[x].samples;
        const float s = (y < smps.size()) ? smps[y] : 0.0f;
        float norm = (maxAbs > 1e-6f) ? (s / maxAbs) : 0.0f;
        norm = qBound(-1.0f, norm, 1.0f);
        // Map 0 -> mid-gray 128, positive -> dark, negative -> light
        const int gray = qBound(0, static_cast<int>(128.0f - norm * 127.0f + 0.5f), 255);
        scanLine[x] = qRgb(gray, gray, gray);
      }
    }

    // Add QGraphicsPixmapItem to scene, scaled to the plot area
    if (plotRect.width() > 0 && plotRect.height() > 0 && img.width() > 0 && img.height() > 0)
    {
      QPixmap pixmap = QPixmap::fromImage(img).scaled(
          plotRect.size().toSize(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
      auto *pixItem = m_scene->addPixmap(pixmap);
      pixItem->setPos(plotRect.topLeft());
    }

    // Border around plot area
    m_scene->addRect(plotRect, QPen(kBorder, 1.0));

    // Baseline (the line's surface datum at top of section)
    m_scene->addLine(plotRect.left(), kBaselineY, plotRect.right(), kBaselineY,
                     QPen(kText, 1.2));

    // Real CDP tick marks at top with actual CDP numbers from trace.cdp
    const int numTicks = qMin(traceCount, 8);
    const int step = (numTicks > 1) ? (traceCount - 1) / (numTicks - 1) : 1;
    for (int i = 0; i < traceCount; i += (step > 0 ? step : 1))
    {
      const qreal u = (traceCount > 1) ? (static_cast<qreal>(i) / (traceCount - 1)) : 0.5;
      const qreal x = plotRect.left() + u * plotRect.width();
      m_scene->addLine(x, kBaselineY - 4, x, kBaselineY, QPen(kTextMuted, 1.0));
      auto *cl = sceneText(m_scene, QString::number(traces[i].cdp), 8, kTextMuted);
      cl->setPos(x - cl->boundingRect().width() / 2.0, kBaselineY - 16);
    }
    if (traceCount > 1 && ((traceCount - 1) % step != 0))
    {
      const int i = traceCount - 1;
      const qreal x = plotRect.right();
      m_scene->addLine(x, kBaselineY - 4, x, kBaselineY, QPen(kTextMuted, 1.0));
      auto *cl = sceneText(m_scene, QString::number(traces[i].cdp), 8, kTextMuted);
      cl->setPos(x - cl->boundingRect().width() / 2.0, kBaselineY - 16);
    }
    auto *cdpCap = sceneText(m_scene, tr("CDP"), 8, kTextMuted);
    cdpCap->setPos(plotRect.left() - 24, kBaselineY - 16);
  }
  else
  {
    // Title: line label + asset id + honest "stub" note.
    auto *title = sceneText(m_scene, label, 9, kText);
    title->setPos(l, t - 4);
    auto *sub = sceneText(m_scene,
                          tr("%1 — 预览占位，道数据待接入").arg(assetId),
                          8, kTextMuted);
    sub->setPos(l + title->boundingRect().width() + 12, t - 2);

    // TWT axis (left): 0/500/1000 ms.
    const qreal axisX = l + 2;
    m_scene->addLine(axisX, kBaselineY, axisX, b - 4, QPen(kTextMuted, 1.0));
    const int twtMs[] = {0, 500, 1000};
    for (int i = 0; i < 3; ++i)
    {
      const qreal y = kBaselineY + i * (b - 4 - kBaselineY) / 2.0;
      m_scene->addLine(axisX - 4, y, axisX, y, QPen(kTextMuted, 1.0));
      auto *tl = sceneText(m_scene, QString::number(twtMs[i]), 8, kTextMuted);
      const QRectF tb = tl->boundingRect();
      tl->setPos(axisX - 6 - tb.width(), y - tb.height() / 2.0);
    }
    auto *axisCap = sceneText(m_scene, tr("TWT(ms)"), 8, kTextMuted);
    axisCap->setPos(l - kAxisW + 4, t - 4);

    // Baseline (the line's surface datum) + CDP ticks + wiggle glyphs.
    m_scene->addLine(l + 8, kBaselineY, r - 8, kBaselineY, QPen(kText, 1.2));

    const quint32 seed = qHash(assetId);
    const double p1 = static_cast<double>(seed % 628) / 100.0;
    const double p2 = static_cast<double>((seed >> 8) % 314) / 100.0;

    int cdp = 0;
    for (qreal x = l + 12; x <= r - 10; x += kTickEvery, ++cdp)
    {
      const bool major = (cdp % 5 == 0);
      m_scene->addLine(x, kBaselineY, x, kBaselineY + (major ? 8 : 4),
                       QPen(kTextMuted, 1.0));

      // Per-tick wiggle glyph hanging off the baseline — the section stub.
      QPainterPath glyph;
      glyph.moveTo(x, kBaselineY);
      for (qreal dy = 0; dy <= kGlyphLen; dy += 2.0)
        glyph.lineTo(x + 3.0 * qSin(dy / kGlyphLen * 3 * kPi + p1), kBaselineY + dy);
      m_scene->addPath(glyph, QPen(kText, 0.8));

      if (major)
      {
        auto *cl = sceneText(m_scene, QString::number(cdp * 50), 8, kTextMuted);
        cl->setPos(x - cl->boundingRect().width() / 2.0, kBaselineY + 10);
      }
    }

    // Baseline profile + faint horizon bands below — deterministic per asset.
    const qreal span = r - 8 - (l + 8);
    auto band = [&](qreal bandY, qreal amp, double f1, double f2, const QPen &pen) {
      QPainterPath path;
      path.moveTo(l + 8, bandY);
      for (qreal dx = 0; dx <= span; dx += 3.0)
      {
        const qreal u = dx / span;
        const qreal y = bandY
            + amp * (0.62 * qSin(u * f1 * 2 * kPi + p1)
                   + 0.38 * qSin(u * f2 * 2 * kPi + p2))
                  * qSin(u * kPi); // taper to zero at the edges
        path.lineTo(l + 8 + dx, y);
      }
      m_scene->addPath(path, pen);
    };
    band(kBaselineY + 74.0, 12.0, 3.0, 7.0, QPen(kText, 1.3));                 // profile
    band(kBaselineY + 128.0, 8.0, 4.0, 9.0, QPen(kTextMuted, 1.0, Qt::DashLine)); // horizon
    band(kBaselineY + 178.0, 10.0, 5.0, 11.0, QPen(kTextMuted, 1.0, Qt::DashLine));
  }
}
