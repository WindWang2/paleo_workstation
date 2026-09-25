#include "correlationpanel.h"

#include "../linkage/selectioncontext.h"

#include <QGraphicsLineItem>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsView>
#include <QGridLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QSet>

// ---------------------------------------------------------------------------
// Scene scaffold: one rounded-rect placeholder column per well (~120px wide,
// full strip height), name label on top, horizon tick near the bottom.
// Colors come from DESIGN.md tokens: surface fill, border stroke, primary for
// the selected state, text/text-muted for labels.
// ---------------------------------------------------------------------------
namespace
{
  constexpr int kWellIdRole = 0;   // QGraphicsItem::data key → well id
  constexpr int kHighlightRole = 1; // QGraphicsItem::data key → bool

  constexpr qreal kColumnWidth = 120.0;
  constexpr qreal kColumnGap   = 16.0; // spacing.md
  constexpr qreal kMargin      = 12.0;
  constexpr qreal kLabelBand   = 24.0;
  constexpr qreal kSceneHeight = 380.0;
  constexpr qreal kTickLift    = 24.0; // horizon tick sits this far above the bottom edge
  constexpr qreal kRadius      = 4.0;  // rounded.sm

  const QColor kSurface(QStringLiteral("#FFFFFF"));
  const QColor kSurfaceAlt(QStringLiteral("#EDF1F5"));
  const QColor kBorder(QStringLiteral("#DFE5EC"));
  const QColor kPrimary(QStringLiteral("#1B73D0"));
  const QColor kText(QStringLiteral("#24303E"));
  const QColor kTextMuted(QStringLiteral("#5D6E80"));

  QPen columnPen(bool highlighted)
  {
    return highlighted ? QPen(kPrimary, 2.0) : QPen(kBorder, 1.0);
  }

  // Scene subclass detects which column a click landed on during normal
  // dispatch — the same path a viewport mouse event takes, so tests exercise
  // it by clicking the viewport (or sending the scene event directly).
  class CorrelationScene : public QGraphicsScene
  {
    public:
      using QGraphicsScene::QGraphicsScene;
      std::function<void(const QString &)> clicked;
      std::function<void(const QString &)> doubleClicked;

    protected:
      void mousePressEvent(QGraphicsSceneMouseEvent *e) override
      {
        QGraphicsScene::mousePressEvent(e);
        if (clicked && e->button() == Qt::LeftButton)
        {
          const QString id = columnIdAt(e->scenePos());
          if (!id.isEmpty())
            clicked(id);
        }
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
      // Children (label, tick) resolve to their parent column.
      QString columnIdAt(const QPointF &pos) const
      {
        for (QGraphicsItem *it : items(pos))
          for (QGraphicsItem *c = it; c; c = c->parentItem())
          {
            const QVariant v = c->data(kWellIdRole);
            if (v.isValid())
              return v.toString();
          }
        return {};
      }
  };
} // namespace

WellCorrelationPanel::WellCorrelationPanel(SelectionContext *ctx, QWidget *parent)
  : QWidget(parent), m_ctx(ctx)
{
  auto *lay = new QGridLayout(this);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(0);

  auto *scene = new CorrelationScene(this);
  scene->clicked = [this](const QString &id) { emit wellClicked(id); };
  scene->doubleClicked = [this](const QString &id) { emit wellDoubleClicked(id); };
  m_scene = scene;

  m_view = new QGraphicsView(scene, this);
  m_view->setObjectName(QStringLiteral("correlationView"));
  m_view->setAccessibleName(tr("连井剖面"));
  m_view->setFrameShape(QFrame::NoFrame);
  m_view->setRenderHint(QPainter::Antialiasing);
  m_view->setBackgroundBrush(kSurfaceAlt);
  m_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  m_view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  lay->addWidget(m_view, 0, 0);

  // §42.4 empty state — guidance text overlaid on the strip area; stacked in
  // the same grid cell so it never shifts the view's geometry.
  m_emptyLabel = new QLabel(tr("选择井以构建剖面"), this);
  m_emptyLabel->setObjectName(QStringLiteral("emptyLabel"));
  m_emptyLabel->setAlignment(Qt::AlignCenter);
  m_emptyLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
  m_emptyLabel->setStyleSheet(QStringLiteral("color: %1;").arg(kTextMuted.name()));
  lay->addWidget(m_emptyLabel, 0, 0);
  m_emptyLabel->setVisible(true);

  if (m_ctx)
  {
    connect(m_ctx, &SelectionContext::selectionChanged, this,
            [this](const QStringList &ids, const QString &origin) {
              if (origin != QLatin1String("correlation")) // own echo — skip
                applySelection(ids);
            });
  }
}

void WellCorrelationPanel::setWells(const QList<QPair<QString, QString>> &wells)
{
  m_wells = wells;
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
    const QVariant id = it->data(kWellIdRole);
    if (id.isValid() && id.toString() == wellId)
      return it->data(kHighlightRole).toBool();
  }
  return false;
}

void WellCorrelationPanel::rebuildScene()
{
  m_scene->clear();
  m_emptyLabel->setVisible(m_wells.isEmpty());

  const int n = m_wells.size();
  const qreal width = kMargin * 2 + n * kColumnWidth + (n > 0 ? (n - 1) * kColumnGap : 0);
  m_scene->setSceneRect(0, 0, width, kSceneHeight);

  const qreal colH = kSceneHeight - kLabelBand - kMargin;
  const qreal tickY = kSceneHeight - kMargin - kTickLift;
  for (int i = 0; i < n; ++i)
  {
    const qreal x = kMargin + i * (kColumnWidth + kColumnGap);

    QPainterPath path;
    path.addRoundedRect(QRectF(x, kLabelBand, kColumnWidth, colH), kRadius, kRadius);
    auto *column = m_scene->addPath(path, columnPen(false), QBrush(kSurface));
    column->setData(kWellIdRole, m_wells.at(i).first);
    column->setData(kHighlightRole, false);

    // Well name centered above the column.
    auto *label = new QGraphicsSimpleTextItem(m_wells.at(i).second, column);
    QFont f = label->font();
    f.setPointSize(9); // body token
    label->setFont(f);
    label->setBrush(kText);
    const QRectF lb = label->boundingRect();
    label->setPos(x + (kColumnWidth - lb.width()) / 2.0,
                  kLabelBand - lb.height() - 2.0);

    // Horizon tick placeholder near the bottom edge (activeHorizon binding
    // arrives with real log rendering).
    auto *tick = new QGraphicsLineItem(x + 8, tickY, x + kColumnWidth - 8, tickY, column);
    QPen tickPen(kTextMuted, 1.0, Qt::DashLine);
    tick->setPen(tickPen);
  }
}

void WellCorrelationPanel::applySelection(const QStringList &ids)
{
  const QSet<QString> selected(ids.begin(), ids.end());
  const auto items = m_scene->items();
  for (QGraphicsItem *it : items)
  {
    if (!it->data(kWellIdRole).isValid())
      continue; // label/tick children carry no id
    const bool on = selected.contains(it->data(kWellIdRole).toString());
    if (it->data(kHighlightRole).toBool() == on)
      continue;
    it->setData(kHighlightRole, on);
    static_cast<QGraphicsPathItem *>(it)->setPen(columnPen(on));
  }
}
