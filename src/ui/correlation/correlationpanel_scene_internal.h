// 层：视图
#pragma once
// correlationpanel 场景/视图脚手架（CorrelationScene/CorrelationView 与布局常量），
// 由 correlationpanel.cpp 与 correlationpanel_layout.cpp 共用（file-budget 拆分）。
// 匿名命名空间：每个包含它的 TU 各持一份内部实现，不导出符号。

#include "../correlationpanel.h"
#include "ui/common/wheelzoom.h"
#include "correlation_internal.h"
#include "../paleotheme.h"
#include "../../linkage/selectioncontext.h"
#include "../../io/lasdoc.h" // LasCurve（方向 59：previewdoc.h 瘦身后契约值类型直取白名单门面）
#include "../../services/paleotaskservice.h" // B1：quiet LAS 异步任务服务
#include "../../services/previewdoc.h" // LasParser 的唯一 UI 出口（W1）
#include "correlationwellcolumn.h"
#include "curvebrowser.h"
#include "depthruler.h"
#include <QCheckBox>
#include <QEvent>
#include <QSignalBlocker>
#include <QFile>
#include <QFileInfo>
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
#include <memory>

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
using paleo::ui_detail::kBorder;

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
  QColor kPrimary() { return PaleoTheme::tokens().primary; }
  QColor kText() { return PaleoTheme::tokens().text; }

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

      // K-1（=20261020 D-06）：删列项前作废拖拽裸指针（防 UAF）。
      void cancelDrag() { m_dragColumn = nullptr; m_dragIndex = -1; m_dragging = false; }

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

      // 背景刷在构造期固化会跟丢主题切换——随 ApplicationPaletteChange 重取。
      bool event(QEvent *e) override
      {
        if (e->type() == QEvent::ApplicationPaletteChange)
          setBackgroundBrush(kSurfaceAlt());
        return QGraphicsView::event(e);
      }

      DepthRuler *ruler = nullptr;
      bool rulerEnabled = false;
      qreal rulerTop = 0.0;                       // scene y of the ruler strip top
      float axisMin = 0.0f, axisMax = 100.0f;
      std::function<float(qreal)> depthOfY;

      void wheelEvent(QWheelEvent *e) override
      {
        if (e->modifiers() & Qt::ControlModifier)
        {
          const qreal f = paleo::ui::wheelZoomFactor(e->angleDelta().y(), 1.15);
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
