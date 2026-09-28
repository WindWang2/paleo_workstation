// 层：视图
#include "correlationpanel.h"
#include "paleotheme.h"

#include "../linkage/selectioncontext.h"
#include "../services/previewdoc.h" // LasParser 的唯一 UI 出口（W1）

#include "correlation/correlationwellcolumn.h"
#include "correlation/curvebrowser.h"
#include "correlation/depthruler.h"

#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsView>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPen>
#include <QSet>
#include <QWheelEvent>

#include <cmath>
#include <limits>

// ---------------------------------------------------------------------------
// Scene/view scaffold. Columns are the ONLY top-level scene items — the
// depth ruler paints through the view's background/foreground passes and
// horizon lines parent under a chrome rect that exists only while markers
// are visible, so click resolution (parent-chain walk to the well-id data
// role) and the column-count scaffolding stay exact.
// Colors follow DESIGN.md tokens: surface fill, border stroke, primary for
// the selected state, text/text-muted for labels.
// ---------------------------------------------------------------------------
namespace
{
  constexpr qreal kColumnGap     = 16.0; // spacing.md
  constexpr qreal kMargin        = 12.0;
  constexpr qreal kLabelBand     = 24.0;
  constexpr qreal kSceneHeight   = 380.0;
  constexpr qreal kRulerWidth    = 46.0;
  constexpr qreal kHeaderDragBand = 24.0; // column header height = drag affordance
  constexpr qreal kDragThreshold  = 6.0;  // px before a press becomes a reorder drag
  constexpr int   kBrowserWidth   = 232;

  // Columns are placed at y = kLabelBand and paint a header of the same
  // height, so the shared depth body starts one header lower.
  qreal depthBodyTop() { return kLabelBand + kLabelBand; }
  qreal depthBodyHeight() { return kSceneHeight - depthBodyTop() - kMargin; }

  bool isWellColumnItem(const QGraphicsItem *it)
  {
    return it
        && it->data(CorrelationItemRoles::WellId).isValid()
        && !it->data(CorrelationItemRoles::HorizonMarker).isValid();
  }

  // DESIGN.md token 出口（调用时现取——暗色翻案后随主题翻转）。
  QColor kSurfaceAlt() { return PaleoTheme::tokens().surfaceAlt; }
  QColor kBorder() { return PaleoTheme::tokens().border; }
  QColor kPrimary() { return PaleoTheme::tokens().primary; }
  QColor kText() { return PaleoTheme::tokens().text; }
  QColor kTextMuted() { return PaleoTheme::tokens().textMuted; }

  QPen columnPen(bool highlighted)
  {
    return highlighted ? QPen(kPrimary(), 2.0) : QPen(kBorder(), 1.0);
  }

  // Click/double-click intents (QListWidget semantics: every press reports
  // a click) + header-band drag reorder: pressing a column's header band
  // and dragging past the threshold slides that column horizontally; on
  // release the drop index is derived from the column centers and routed
  // to reorder() with QList::move semantics.
  class CorrelationScene : public QGraphicsScene
  {
    public:
      using QGraphicsScene::QGraphicsScene;
      std::function<void(const QString &)> clicked;
      std::function<void(const QString &)> doubleClicked;
      std::function<void(int, int)> reorderRequested;

    protected:
      void mousePressEvent(QGraphicsSceneMouseEvent *e) override
      {
        m_press = e->scenePos();
        m_dragColumn = nullptr;
        m_dragIndex = -1;
        m_dragging = false;
        if (e->button() == Qt::LeftButton)
        {
          QGraphicsItem *col = columnItemAt(e->scenePos());
          if (col && e->scenePos().y() <= col->sceneBoundingRect().top() + kHeaderDragBand)
          {
            m_dragColumn = col; // header press: potential reorder drag
            m_dragIndex = indexByX(col);
            m_dragOrigin = col->pos();
          }
        }
        QGraphicsScene::mousePressEvent(e);
        if (clicked && e->button() == Qt::LeftButton)
        {
          const QString id = columnIdAt(e->scenePos());
          if (!id.isEmpty())
            clicked(id);
        }
      }

      void mouseMoveEvent(QGraphicsSceneMouseEvent *e) override
      {
        if (m_dragColumn && m_dragIndex >= 0)
        {
          const qreal dx = e->scenePos().x() - m_press.x();
          if (!m_dragging && std::abs(dx) > kDragThreshold)
            m_dragging = true;
          if (m_dragging)
            m_dragColumn->setPos(m_dragOrigin + QPointF(dx, 0)); // horizontal only
        }
        QGraphicsScene::mouseMoveEvent(e);
      }

      void mouseReleaseEvent(QGraphicsSceneMouseEvent *e) override
      {
        if (m_dragging && m_dragColumn && m_dragIndex >= 0 && reorderRequested)
        {
          // Drop index: how many OTHER columns sit left of the dragged
          // column's center — matches QList::move insertion semantics.
          const qreal cx = m_dragColumn->sceneBoundingRect().center().x();
          int target = 0;
          for (QGraphicsItem *it : items())
          {
            if (it == m_dragColumn || !isWellColumnItem(it))
              continue;
            if (it->sceneBoundingRect().center().x() < cx)
              ++target;
          }
          reorderRequested(m_dragIndex, target);
        }
        m_dragColumn = nullptr;
        m_dragIndex = -1;
        m_dragging = false;
        QGraphicsScene::mouseReleaseEvent(e);
      }

      void mouseDoubleClickEvent(QGraphicsSceneMouseEvent *e) override
      {
        QGraphicsScene::mouseDoubleClickEvent(e);
        if (e->button() == Qt::LeftButton)
        {
          const QString id = columnIdAt(e->scenePos());
          if (!id.isEmpty())
          {
            if (clicked) // QListWidget-style: every press reports a click
              clicked(id);
            if (doubleClicked)
              doubleClicked(id);
          }
        }
      }

    private:
      // Children (labels, tracks) resolve to their parent column.
      QGraphicsItem *columnItemAt(const QPointF &pos) const
      {
        for (QGraphicsItem *it : items(pos))
          for (QGraphicsItem *c = it; c; c = c->parentItem())
            if (isWellColumnItem(c))
              return c;
        return nullptr;
      }

      QString columnIdAt(const QPointF &pos) const
      {
        const QGraphicsItem *col = columnItemAt(pos);
        return col ? col->data(CorrelationItemRoles::WellId).toString() : QString();
      }

      int indexByX(QGraphicsItem *col) const
      {
        int idx = 0;
        for (QGraphicsItem *it : items())
        {
          if (it == col || !isWellColumnItem(it))
            continue;
          if (it->sceneBoundingRect().center().x() < col->sceneBoundingRect().center().x())
            ++idx;
        }
        return idx;
      }

      QPointF m_press;
      QPointF m_dragOrigin;
      QGraphicsItem *m_dragColumn = nullptr;
      int m_dragIndex = -1;
      bool m_dragging = false;
  };

  // Depth ruler host: ticks/labels paint in the foreground pass, grid lines
  // in the background pass (below the columns), both in scene coordinates
  // so the ruler stays glued to the data. The visible depth window — not
  // the full axis — drives tick selection, so Ctrl+wheel vertical zoom
  // re-nices the ticks ("follows view zoom").
  class CorrelationView : public QGraphicsView
  {
    public:
      CorrelationView(QGraphicsScene *scene, QWidget *parent)
        : QGraphicsView(scene, parent) {}

      DepthRuler *ruler = nullptr;
      bool rulerEnabled = false;
      qreal rulerTop = 0.0;                       // scene y of the ruler strip top
      float axisMin = 0.0f, axisMax = 100.0f;
      std::function<float(qreal)> depthOfY;

      void wheelEvent(QWheelEvent *e) override
      {
        if (e->modifiers() & Qt::ControlModifier)
        {
          const qreal f = e->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
          const qreal cur = transform().m22();
          if ((f > 1.0 && cur < 20.0) || (f < 1.0 && cur > 0.25))
          {
            setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
            scale(1.0, f);
          }
          e->accept();
          return;
        }
        QGraphicsView::wheelEvent(e);
      }

      void drawBackground(QPainter *p, const QRectF &rect) override
      {
        QGraphicsView::drawBackground(p, rect);
        paintRuler(p, rect, true);
      }

      void drawForeground(QPainter *p, const QRectF &rect) override
      {
        paintRuler(p, rect, false);
      }

    private:
      void paintRuler(QPainter *p, const QRectF &rect, bool gridPass)
      {
        if (!ruler || !rulerEnabled || !depthOfY)
          return;

        // Tick window = currently visible depth span (clamped to the axis).
        float lo = axisMin, hi = axisMax;
        if (viewport()->width() > 1)
        {
          const QRectF vis = mapToScene(viewport()->rect()).boundingRect();
          lo = qMax(axisMin, depthOfY(vis.top()));
          hi = qMin(axisMax, depthOfY(vis.bottom()));
        }
        if (hi > lo + 1e-4f)
          ruler->setRange(lo, hi);

        const QRectF bb = ruler->boundingRect();
        const double labelW = bb.width() - ruler->gridWidth();
        p->save();
        p->translate(0.0, rulerTop);
        const QRectF zone = gridPass
            ? QRectF(labelW, bb.top(), ruler->gridWidth(), bb.height())
            : QRectF(0.0, bb.top(), labelW, bb.height());
        p->setClipRect(zone.intersected(rect.translated(0, -rulerTop)));
        ruler->paint(p, nullptr, nullptr);
        p->restore();
      }
  };
} // namespace

WellCorrelationPanel::WellCorrelationPanel(SelectionContext *ctx, QWidget *parent)
  : QWidget(parent), m_ctx(ctx)
{
  m_markers = new HorizonMarkerSet(this);
  m_ruler = new DepthRuler(); // view-painted; never inserted into the scene
  m_browser = new CurveBrowser(this);

  auto *scene = new CorrelationScene(this);
  scene->clicked = [this](const QString &id) { emit wellClicked(id); };
  scene->doubleClicked = [this](const QString &id) { emit wellDoubleClicked(id); };
  scene->reorderRequested = [this](int from, int to) { reorder(from, to); };
  m_scene = scene;

  auto *view = new CorrelationView(scene, this);
  view->setObjectName(QStringLiteral("correlationView"));
  view->setAccessibleName(tr("连井剖面"));
  view->setFrameShape(QFrame::NoFrame);
  view->setRenderHint(QPainter::Antialiasing);
  view->setBackgroundBrush(kSurfaceAlt());
  view->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  view->ruler = m_ruler;
  view->rulerTop = depthBodyTop();
  view->depthOfY = [this](qreal y) { return depthAtY(y); };
  m_view = view;

  // §42.4 empty state — guidance text overlaid on the strip area; stacked
  // in the same grid cell so it never shifts the view's geometry.
  auto *grid = new QGridLayout;
  grid->setContentsMargins(0, 0, 0, 0);
  grid->setSpacing(0);
  grid->addWidget(m_view, 0, 0);
  m_emptyLabel = new QLabel(tr("选择井以构建剖面"), this);
  m_emptyLabel->setObjectName(QStringLiteral("emptyLabel"));
  m_emptyLabel->setAlignment(Qt::AlignCenter);
  m_emptyLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
  m_emptyLabel->setStyleSheet(QStringLiteral("color: %1;").arg(kTextMuted().name()));
  grid->addWidget(m_emptyLabel, 0, 0);
  m_emptyLabel->setVisible(true);

  // Section strip + LAS curve browser on the right.
  auto *hbox = new QHBoxLayout(this);
  hbox->setContentsMargins(0, 0, 0, 0);
  hbox->setSpacing(0);
  hbox->addLayout(grid, 1);
  m_browser->setFixedWidth(kBrowserWidth);
  hbox->addWidget(m_browser);

  if (m_ctx)
  {
    connect(m_ctx, &SelectionContext::selectionChanged, this,
            [this](const QStringList &ids, const QString &origin) {
              if (origin != QLatin1String("correlation")) // own echo — skip
                applySelection(ids);
            });
    // Active horizon emphasizes the matching correlation line.
    connect(m_ctx, &SelectionContext::activeHorizonChanged, this,
            [this](const QString &) { relayoutMarkers(); });
  }

  // Browser checkboxes drive the well's track set.
  connect(m_browser, &CurveBrowser::mnemonicToggled, this,
          [this](const QString &wellId, const QString &mnemonic, bool on) {
            const QList<LasCurve> curves = m_lasByWell.value(wellId);
            if (!on)
            {
              removeWellTrack(wellId, mnemonic);
              return;
            }
            for (const LasCurve &c : curves)
              if (c.name.compare(mnemonic, Qt::CaseInsensitive) == 0)
              {
                const LasCurve &depthCurve = curves.first(); // ~C column 0 = DEPT
                QVector<float> depths(depthCurve.values.size());
                for (qsizetype i = 0; i < depthCurve.values.size(); ++i)
                  depths[i] = static_cast<float>(depthCurve.values.at(i));
                QVector<float> values(c.values.size());
                for (qsizetype i = 0; i < c.values.size(); ++i)
                  values[i] = static_cast<float>(c.values.at(i));
                addWellTrack(wellId, c.name, c.unit, depths, values);
                return;
              }
          });

  // Pick moved (drag or programmatic): relayout keeps the marker drag
  // grabber alive (rebuildScene never clears the chrome layer).
  connect(m_markers, &HorizonMarkerSet::markerDepthChanged, this,
          [this](const QString &, const QString &, float) { rebuildScene(); });
}

WellCorrelationPanel::~WellCorrelationPanel()
{
  qDeleteAll(m_columns);
  delete m_ruler;
}

void WellCorrelationPanel::setUpdatesEnabled(bool enabled)
{
  QWidget::setUpdatesEnabled(enabled);
  if (m_updatesEnabled == enabled)
    return;
  m_updatesEnabled = enabled;
  if (m_updatesEnabled && m_dirty)
  {
    m_dirty = false;
    rebuildScene();
    if (m_ctx)
      applySelection(m_ctx->selectedIds());
  }
}

void WellCorrelationPanel::setWells(const QList<QPair<QString, QString>> &wells)
{
  m_wells = wells;

  QSet<QString> ids;
  for (const auto &w : wells)
    ids.insert(w.first);

  for (auto it = m_columns.begin(); it != m_columns.end();)
  {
    if (!ids.contains(it.key()))
    {
      delete it.value();
      it = m_columns.erase(it);
    }
    else
      ++it;
  }

  for (const auto &w : wells)
  {
    CorrelationWellColumn *col = m_columns.value(w.first);
    if (col && col->wellName() != w.second)
    {
      // Display name changed: rebuild the column object, carrying tracks.
      auto *nc = new CorrelationWellColumn(w.first, w.second);
      for (const QString &mn : col->mnemonics())
        nc->addTrack(*col->track(mn));
      delete col;
      m_columns.insert(w.first, nc);
    }
    else if (!col)
    {
      m_columns.insert(w.first, new CorrelationWellColumn(w.first, w.second));
    }
  }

  rebuildScene();
  if (m_ctx) // selection may predate the wells — honor current state
    applySelection(m_ctx->selectedIds());
}

QString WellCorrelationPanel::wellAt(int index) const
{
  return (index >= 0 && index < m_wells.size()) ? m_wells.at(index).first : QString();
}

void WellCorrelationPanel::reorder(int from, int to)
{
  if (from == to || from < 0 || to < 0 || from >= m_wells.size() || to >= m_wells.size())
    return;
  m_wells.move(from, to);
  rebuildScene();
  if (m_ctx)
    applySelection(m_ctx->selectedIds());
}

bool WellCorrelationPanel::isWellHighlighted(const QString &wellId) const
{
  if (wellId.isEmpty() || !m_scene)
    return false;
  const auto items = m_scene->items();
  for (QGraphicsItem *it : items)
  {
    if (!isWellColumnItem(it))
      continue;
    if (it->data(CorrelationItemRoles::WellId).toString() == wellId)
      return it->data(CorrelationItemRoles::Highlight).toBool();
  }
  return false;
}

bool WellCorrelationPanel::anyTracks() const
{
  for (CorrelationWellColumn *col : m_columns)
    if (col->trackCount() > 0)
      return true;
  return false;
}

void WellCorrelationPanel::setWellCurves(const QString &wellId, const QVector<float> &depths,
                                         const QVector<float> &values, const QString &curveName)
{
  CorrelationWellColumn *col = m_columns.value(wellId);
  if (!col)
    return;
  col->clearTracks(); // compat semantics: the well's set becomes this ONE track
  const qsizetype n = qMin(depths.size(), values.size());
  if (!wellId.isEmpty() && n > 0)
  {
    // Legacy callers may pass no curveName; tracks key on mnemonic, so an
    // anonymous curve gets the neutral fallback "LOG".
    const QString mnem = curveName.isEmpty() ? QStringLiteral("LOG") : curveName;
    col->addTrack(CorrelationTrack(mnem, QString(), depths.first(n), values.first(n)));
  }

  rebuildScene();
  if (m_ctx) // rebuild resets pens — restore selection state like reorder()
    applySelection(m_ctx->selectedIds());
}

void WellCorrelationPanel::clearWellCurves()
{
  for (CorrelationWellColumn *col : m_columns)
    col->clearTracks();
  rebuildScene();
  if (m_ctx)
    applySelection(m_ctx->selectedIds());
}

int WellCorrelationPanel::curveItemCount(const QString &wellId) const
{
  if (!m_scene)
    return 0;
  int count = 0;
  const auto items = m_scene->items();
  for (QGraphicsItem *it : items)
    if (it->data(CorrelationItemRoles::CurveOwner).isValid() &&
        it->data(CorrelationItemRoles::CurveOwner).toString() == wellId)
      ++count;
  return count;
}

bool WellCorrelationPanel::addWellTrack(const QString &wellId, const QString &mnemonic,
                                        const QString &unit, const QVector<float> &depths,
                                        const QVector<float> &values)
{
  CorrelationWellColumn *col = m_columns.value(wellId);
  if (!col || wellId.isEmpty() || mnemonic.isEmpty())
    return false;
  const qsizetype n = qMin(depths.size(), values.size());
  if (n < 1)
    return false;
  col->addTrack(CorrelationTrack(mnemonic, unit, depths.first(n), values.first(n)));
  rebuildScene();
  if (m_ctx)
    applySelection(m_ctx->selectedIds());
  return true;
}

bool WellCorrelationPanel::removeWellTrack(const QString &wellId, const QString &mnemonic)
{
  CorrelationWellColumn *col = m_columns.value(wellId);
  if (!col || !col->removeTrack(mnemonic))
    return false;
  rebuildScene();
  if (m_ctx)
    applySelection(m_ctx->selectedIds());
  return true;
}

QStringList WellCorrelationPanel::wellTrackMnemonics(const QString &wellId) const
{
  const CorrelationWellColumn *col = m_columns.value(wellId);
  return col ? col->mnemonics() : QStringList();
}

bool WellCorrelationPanel::setLasForWell(const QString &wellId, const QString &lasPath)
{
  QStringList names;
  QList<LasCurve> curves;
  if (!PreviewDocService::lasAt(lasPath, &names, &curves) || curves.isEmpty())
    return false;
  m_lasByWell.insert(wellId, curves);
  m_browser->setCurves(wellId, curves);
  return true;
}

bool WellCorrelationPanel::loadWellLas(const QString &wellId, const QString &lasPath,
                                       const QString &curveMnemonic)
{
  QStringList names;
  QList<LasCurve> curves;
  if (!PreviewDocService::lasAt(lasPath, &names, &curves) || curves.isEmpty())
    return false;

  const QString want = curveMnemonic.trimmed();
  int idx = -1;
  for (int i = 0; i < names.size(); ++i)
    if (names.at(i).compare(want, Qt::CaseInsensitive) == 0)
    {
      idx = i;
      break;
    }
  if (idx < 0)
    return false;

  m_lasByWell.insert(wellId, curves);
  m_browser->setCurves(wellId, curves);

  const LasCurve &depthCurve = curves.first(); // ~C column 0 is the DEPT index
  const LasCurve &valueCurve = curves.at(idx);
  QVector<float> depths(depthCurve.values.size());
  for (qsizetype i = 0; i < depthCurve.values.size(); ++i)
    depths[i] = static_cast<float>(depthCurve.values.at(i));
  QVector<float> values(valueCurve.values.size());
  for (qsizetype i = 0; i < valueCurve.values.size(); ++i)
    values[i] = static_cast<float>(valueCurve.values.at(i));

  addWellTrack(wellId, valueCurve.name, valueCurve.unit, depths, values);
  m_browser->setChecked(valueCurve.name, true); // reflect state (idempotent when already on)
  return true;
}

void WellCorrelationPanel::setManifestHorizons(const QStringList &names)
{
  m_markers->setManifestHorizons(names);
  rebuildScene();
}

void WellCorrelationPanel::setFlattenMarker(const QString &name)
{
  m_markers->setFlattenMarker(name);
  rebuildScene();
}

bool WellCorrelationPanel::isFlattened() const
{
  return m_markers->isFlattened();
}

// Depth axis = the display-space window: every track sample and every
// horizon pick shifted by its well's flatten offset, so flattened markers
// (display depth 0) stay inside the section and unflattened wells keep
// absolute depths.
void WellCorrelationPanel::computeDepthAxis()
{
  float lo = std::numeric_limits<float>::infinity();
  float hi = -lo;
  for (const auto &w : m_wells)
  {
    const QString id = w.first;
    const float off = m_markers->displayOffset(id);
    const CorrelationWellColumn *col = m_columns.value(id);
    if (col)
      for (const QString &mn : col->mnemonics())
      {
        const CorrelationTrack *t = col->track(mn);
        for (const float d : t->depths())
          if (std::isfinite(d))
          {
            lo = qMin(lo, d - off);
            hi = qMax(hi, d - off);
          }
      }
    for (const QString &name : m_markers->markerNames())
    {
      const float p = m_markers->wellDepth(name, id);
      if (std::isfinite(p))
      {
        lo = qMin(lo, p - off);
        hi = qMax(hi, p - off);
      }
    }
  }
  if (lo > hi)
  {
    m_axisMin = 0.0f;
    m_axisMax = 100.0f;
  }
  else
  {
    m_axisMin = lo;
    m_axisMax = qMax(hi, lo + 1e-3f);
  }
}

qreal WellCorrelationPanel::yForDepth(float displayDepth) const
{
  const qreal bodyH = depthBodyHeight();
  const float span = m_axisMax - m_axisMin;
  return depthBodyTop() + static_cast<qreal>(displayDepth - m_axisMin) / span * bodyH;
}

float WellCorrelationPanel::depthAtY(qreal y) const
{
  const qreal bodyH = depthBodyHeight();
  const float span = m_axisMax - m_axisMin;
  return static_cast<float>(m_axisMin + (y - depthBodyTop()) / bodyH * span);
}

// Selective relayout: old column items are deleted (children follow), the
// chrome layer and marker items survive — HorizonMarkerSet::rebuild
// self-cleans its lines by role scan and preserves the drag grabber, so
// mid-drag relayouts (markerDepthChanged) never kill an in-flight drag.
void WellCorrelationPanel::rebuildScene()
{
  if (!m_updatesEnabled)
  {
    m_dirty = true;
    return;
  }

  m_emptyLabel->setVisible(m_wells.isEmpty());

  for (QGraphicsPathItem *it : m_columnItems)
  {
    m_scene->removeItem(it);
    delete it;
  }
  m_columnItems.clear();

  computeDepthAxis();

  const qreal bodyH = depthBodyHeight();
  QSet<QString> selected;
  if (m_ctx)
  {
    const QStringList ids = m_ctx->selectedIds(); // one temporary, valid iterator pair
    selected = QSet<QString>(ids.begin(), ids.end());
  }

  qreal x = kRulerWidth + kMargin;
  QList<HorizonMarkerSet::ColumnGeom> geoms;
  for (const auto &w : m_wells)
  {
    CorrelationWellColumn *col = m_columns.value(w.first);
    if (!col)
      continue;
    col->setHeaderHeight(kLabelBand);
    const float off = m_markers->displayOffset(w.first);
    QGraphicsPathItem *item = col->rebuild(m_scene, QPointF(x, kLabelBand), bodyH,
                                           m_axisMin, m_axisMax, selected.contains(w.first), off);
    m_columnItems.append(item);
    geoms.append({w.first, QRectF(x, depthBodyTop(), col->width(), bodyH)});
    x += col->width() + kColumnGap;
  }
  m_lastGeoms = geoms;

  const qreal sceneW = m_wells.isEmpty() ? 200.0 : x - kColumnGap + kMargin;
  m_scene->setSceneRect(0, 0, sceneW, kSceneHeight);

  // Ruler: full-axis ticks by default; the view narrows the window to the
  // visible span when zoomed. Grid spans the section width.
  m_ruler->setHeight(bodyH);
  m_ruler->setRange(m_axisMin, m_axisMax);
  m_ruler->setGridWidth(qMax(0.0, sceneW - kRulerWidth - kMargin));
  auto *view = static_cast<CorrelationView *>(m_view);
  view->axisMin = m_axisMin;
  view->axisMax = m_axisMax;
  view->rulerEnabled = !m_wells.isEmpty();
  view->viewport()->update();

  relayoutMarkers();
}

void WellCorrelationPanel::relayoutMarkers()
{
  bool anyPicked = false;
  if (!m_wells.isEmpty())
    for (const QString &name : m_markers->markerNames())
      if (!m_markers->wellsPicked(name).isEmpty())
      {
        anyPicked = true;
        break;
      }

  if (anyPicked)
  {
    if (!m_chrome)
    {
      // Invisible, non-interactive parent above the columns: marker lines
      // must not resolve as columns (no well-id role) but stay top-most.
      m_chrome = m_scene->addRect(QRectF(0, 0, 1, 1), QPen(Qt::NoPen), QBrush(Qt::NoBrush));
      m_chrome->setAcceptedMouseButtons(Qt::NoButton);
      m_chrome->setZValue(50.0);
    }
    m_chrome->setRect(m_scene->sceneRect().adjusted(-2.0, -2.0, 2.0, 2.0));
    m_markers->rebuild(m_scene, m_chrome, m_lastGeoms,
                       [this](float d) { return yForDepth(d); },
                       [this](qreal y) { return depthAtY(y); },
                       true, m_ctx ? m_ctx->activeHorizon() : QString());
  }
  else
  {
    if (m_chrome)
    {
      m_scene->removeItem(m_chrome);
      delete m_chrome; // marker items (children) follow
      m_chrome = nullptr;
    }
    // Still run the role-scan cleanup so orphaned lines die with the wells.
    m_markers->rebuild(m_scene, nullptr, {},
                       [this](float d) { return yForDepth(d); },
                       [this](qreal y) { return depthAtY(y); },
                       false, QString());
  }
}

void WellCorrelationPanel::applySelection(const QStringList &ids)
{
  const QSet<QString> selected(ids.begin(), ids.end());
  const auto items = m_scene->items();
  for (QGraphicsItem *it : items)
  {
    if (!isWellColumnItem(it))
      continue; // labels, tracks, and horizon lines are not columns
    auto *column = qgraphicsitem_cast<QGraphicsPathItem *>(it);
    if (!column)
      continue;
    const bool on = selected.contains(it->data(CorrelationItemRoles::WellId).toString());
    if (it->data(CorrelationItemRoles::Highlight).toBool() == on)
      continue;
    it->setData(CorrelationItemRoles::Highlight, on);
    column->setPen(columnPen(on));
  }
}

// ---------------------------------------------------------------------------
// 三视图联动（wave/mapping-pipeline 阶段C 本链路新增）
// ---------------------------------------------------------------------------
void WellCorrelationPanel::scrollToWellTop(const QString &wellId, const QString &horizon)
{
  m_scrollWell = wellId;
  m_scrollHorizon = horizon;

  // Resolve the well's column rect; unknown well → target recorded, view kept.
  QRectF geom;
  for (const HorizonMarkerSet::ColumnGeom &g : m_lastGeoms)
  {
    if (g.wellId == wellId)
    {
      geom = g.rect;
      break;
    }
  }
  if (geom.isNull())
    return;

  const qreal x = geom.center().x();
  qreal y = geom.center().y();
  const float pick = m_markers->wellDepth(horizon, wellId);
  if (!qIsNaN(pick))
  {
    // Pick line renders at the DISPLAY depth (flatten offset applies).
    const qreal pickY = yForDepth(m_markers->displayDepth(wellId, pick));
    if (pickY >= depthBodyTop() && pickY <= depthBodyTop() + geom.height())
      y = pickY;
  }
  m_view->centerOn(x, y);
}
