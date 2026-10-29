// 层：视图
#include "correlationpanel.h"
#include "correlation/correlation_internal.h"
#include "paleotheme.h"

#include "../linkage/selectioncontext.h"
#include "../io/lasdoc.h" // LasCurve（方向 59：previewdoc.h 瘦身后契约值类型直取白名单门面）
#include "../services/paleotaskservice.h" // B1：quiet LAS 异步任务服务
#include "../services/previewdoc.h" // LasParser 的唯一 UI 出口（W1）

#include "correlation/correlationwellcolumn.h"
#include "correlation/curvebrowser.h"
#include "correlation/depthruler.h"

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

#include "correlation/correlationpanel_scene_internal.h"

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
  // goal/ui-experience-polish：空态/错误态双档活体样式（errorState 属性供
  // 测试与 a11y 分辨；错误文案带下一步指引）。
  m_emptyLabel->setProperty("errorState", false);
  PaleoTheme::applyThemedStyleSheet(m_emptyLabel, [this] {
    if (m_emptyLabel->property("errorState").toBool())
      return QStringLiteral("color: %1;")
          .arg(PaleoTheme::tokens().errorText.name());
    return PaleoTheme::mutedCaptionStyleSheet();
  });
  grid->addWidget(m_emptyLabel, 0, 0);
  m_emptyLabel->setVisible(true);
  auto *sharedScaleBox = new QCheckBox(tr("同名曲线共用刻度"), this);
  sharedScaleBox->setObjectName(QStringLiteral("sharedCurveScaleBox"));
  sharedScaleBox->setToolTip(tr("跨井对同一助记符使用统一横向刻度（电阻率类对数），便于形态对比"));
  connect(sharedScaleBox, &QCheckBox::toggled, this, &WellCorrelationPanel::setSharedCurveScale);
  grid->addWidget(sharedScaleBox, 1, 0);

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

  m_lasDoc = new PreviewDocService(nullptr, this);
  connect(m_lasDoc, &PreviewDocService::lasReady, this,
          [this](const QString &wellId, const QStringList &names, const QList<LasCurve> &curves) {
    if (!m_pendingLas.contains(wellId)) return;
    const auto presentation = m_pendingLas.take(wellId);
    if (curves.isEmpty()) {
      emit lasLoadFinished(wellId, false);
      emit lasLoadError(wellId, tr("LAS 无数据曲线"));
      showEmptyErrorIfIdle(wellId, tr("LAS 无数据曲线"));
    } else if (presentation.withTrack) {
      const bool ok = applyLasTrack(wellId, names, curves, presentation.mnemonic);
      emit lasLoadFinished(wellId, ok);
      if (!ok) emit lasLoadError(wellId, tr("曲线不存在：%1").arg(presentation.mnemonic));
    } else {
      applyLasCurves(wellId, curves);
      emit lasLoadFinished(wellId, true);
    }
  });
  const auto failed = [this](const QString &wellId, const QString &reason) {
    if (!m_pendingLas.remove(wellId)) return;
    emit lasLoadFinished(wellId, false);
    emit lasLoadError(wellId, reason);
    showEmptyErrorIfIdle(wellId, reason);
  };
  connect(m_lasDoc, &PreviewDocService::lasFailed, this, failed);
  connect(m_lasDoc, &PreviewDocService::lasCancelled, this,
          [this, failed](const QString &wellId) { failed(wellId, tr("测井读取已取消")); });
}

WellCorrelationPanel::~WellCorrelationPanel()
{
  m_lasDoc->resetProjectState();
  qDeleteAll(m_columns);
  delete m_ruler;
}

void WellCorrelationPanel::resetProject()
{
  m_lasDoc->resetProjectState();
  m_pendingLas.clear();
  m_lasByWell.clear();
  if (m_browser)
    m_browser->setCurves(QString(), {});
  setWells({});
}

void WellCorrelationPanel::setTaskService(PaleoTaskService *svc)
{
  m_taskSvc = svc;
  m_lasDoc->setTaskService(svc);
}

bool WellCorrelationPanel::isLasLoadPending(const QString &wellId) const
{
  return m_pendingLas.contains(wellId);
}

// B1：解析结果的回填出口——browser 列表 + m_lasByWell 快照。
void WellCorrelationPanel::applyLasCurves(const QString &wellId,
                                          const QList<LasCurve> &curves)
{
  m_lasByWell.insert(wellId, curves);
  m_browser->setCurves(wellId, curves);
}

// goal/petrophysics-logs：计算产物并入曲线集（同名替换旧算，异名追加；
// 上轨不动——用户勾选驱动）。
void WellCorrelationPanel::mergeComputedCurves(const QString &wellId,
                                               const QList<LasCurve> &computed)
{
  if (computed.isEmpty())
    return;
  QList<LasCurve> merged = m_lasByWell.value(wellId);
  for (const LasCurve &c : computed)
  {
    bool replaced = false;
    for (LasCurve &m : merged)
    {
      if (m.name.compare(c.name, Qt::CaseInsensitive) == 0)
      {
        m = c;
        replaced = true;
        break;
      }
    }
    if (!replaced)
      merged.append(c);
  }
  applyLasCurves(wellId, merged);
}

// B1：按曲线名上轨（loadWellLas 的解析后动作，同步/异步共用）。
bool WellCorrelationPanel::applyLasTrack(const QString &wellId,
                                         const QStringList &names,
                                         const QList<LasCurve> &curves,
                                         const QString &curveMnemonic)
{
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

  applyLasCurves(wellId, curves);

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

// B1：提交 quiet 异步解析。withTrack=false = setLasForWell（只填 browser）；
// true = loadWellLas（再按 mnemonic 上轨）。同井新请求协作取消旧任务、
// 世代号发射前丢弃陈旧结果——与 PreviewDocService::requestLas 同一纪律。
bool WellCorrelationPanel::submitLasLoad(const QString &wellId,
                                         const QString &lasPath, bool withTrack,
                                         const QString &mnemonic)
{
  if (lasPath.isEmpty() || !QFile::exists(lasPath))
    return false; // 快速失败留在同步侧：调用方当场拿到 false
  if (!m_taskSvc) return false;
  m_pendingLas.insert(wellId, {withTrack, mnemonic});
  m_lasDoc->requestLas(wellId, lasPath);
  return true;
}

bool WellCorrelationPanel::setLasForWell(const QString &wellId, const QString &lasPath)
{
  if (m_taskSvc)
    return submitLasLoad(wellId, lasPath, /*withTrack=*/false, QString());
  QStringList names;
  QList<LasCurve> curves;
  if (!PreviewDocService::lasAt(lasPath, &names, &curves) || curves.isEmpty())
    return false;
  applyLasCurves(wellId, curves);
  return true;
}

bool WellCorrelationPanel::loadWellLas(const QString &wellId, const QString &lasPath,
                                       const QString &curveMnemonic)
{
  if (m_taskSvc)
    return submitLasLoad(wellId, lasPath, /*withTrack=*/true, curveMnemonic);
  QStringList names;
  QList<LasCurve> curves;
  if (!PreviewDocService::lasAt(lasPath, &names, &curves) || curves.isEmpty())
    return false;
  if (!applyLasTrack(wellId, names, curves, curveMnemonic))
    return false;
  return true;
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

// goal/ui-experience-polish：LAS 异步失败的面板内可见化——面板尚无任何井
// 列时，把空态标签切错误档（有井列的局部失败不遮画布，由壳层状态栏提示）。
bool WellCorrelationPanel::columnsEmpty() const
{
  return m_columns.isEmpty();
}

void WellCorrelationPanel::showEmptyErrorIfIdle(const QString &wellId,
                                                const QString &reason)
{
  if (!m_emptyLabel)
    return;
  if (!columnsEmpty())
    return; // 已有成功井列：不遮挡内容
  m_emptyLabel->setProperty("errorState", true);
  m_emptyLabel->setText(tr("测井 %1 曲线加载失败：%2\n重新选择井，或检查 LAS 文件后重试")
                            .arg(wellId, reason));
  // 活体样式已在构造注册（读 errorState 属性）——这里即时重算一次。
  m_emptyLabel->setStyleSheet(QStringLiteral("color: %1;")
                                  .arg(PaleoTheme::tokens().errorText.name()));
  m_emptyLabel->setVisible(true);
}

void WellCorrelationPanel::resetEmptyLabel()
{
  if (!m_emptyLabel)
    return;
  m_emptyLabel->setProperty("errorState", false);
  m_emptyLabel->setText(tr("选择井以构建剖面"));
  m_emptyLabel->setStyleSheet(PaleoTheme::mutedCaptionStyleSheet());
}

void WellCorrelationPanel::setWells(const QList<QPair<QString, QString>> &wells)
{
  m_wells = wells;
  resetEmptyLabel();

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
void WellCorrelationPanel::setSharedCurveScale(bool on)
{
  if (m_sharedScale == on)
    return;
  m_sharedScale = on;
  if (auto *box = findChild<QCheckBox *>(QStringLiteral("sharedCurveScaleBox")))
  {
    const QSignalBlocker block(box);
    box->setChecked(on);
  }
  rebuildScene();
}

void WellCorrelationPanel::applyCurveScales()
{
  QHash<QString, QPair<float, float>> ranges;
  if (m_sharedScale)
  {
    for (CorrelationWellColumn *col : std::as_const(m_columns))
      for (const QString &mn : col->mnemonics())
      {
        const CorrelationTrack *t = col->track(mn);
        if (!t || t->isEmpty())
          continue;
        const QPair<float, float> vr = t->valueRange();
        auto it = ranges.find(mn);
        if (it == ranges.end())
          ranges.insert(mn, vr);
        else
          *it = {qMin(it->first, vr.first), qMax(it->second, vr.second)};
      }
  }
  for (CorrelationWellColumn *col : std::as_const(m_columns))
    for (const QString &mn : col->mnemonics())
    {
      CorrelationTrack *t = col->track(mn);
      if (!t)
        continue;
      const auto it = ranges.constFind(mn);
      if (!m_sharedScale || it == ranges.constEnd())
      {
        t->clearScale();
        continue;
      }
      float lo = it->first, hi = it->second;
      const bool log = CorrelationTrack::isLogMnemonic(mn) && lo > 0.0f;
      if (!(hi > lo))
        hi = lo + 1e-6f;
      t->setScale(lo, hi, log);
    }
}

void WellCorrelationPanel::rebuildScene()
{
  if (!m_updatesEnabled)
  {
    m_dirty = true;
    return;
  }

  m_emptyLabel->setVisible(m_wells.isEmpty());

  static_cast<CorrelationScene *>(m_scene)->cancelDrag(); // K-1：防悬垂
  for (QGraphicsPathItem *it : m_columnItems)
  {
    m_scene->removeItem(it);
    delete it;
  }
  m_columnItems.clear();

  computeDepthAxis();
  applyCurveScales();

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
