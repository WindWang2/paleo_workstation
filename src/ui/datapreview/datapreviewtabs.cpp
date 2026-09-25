#include "datapreviewtabs.h"

#include "../../catalog/datacatalog.h"
#include "../../io/dataimportservice.h"
#include "../../io/lasparser.h"
#include "../../io/segyreader.h"
#include "../../io/wellfileparsers.h"

#include <QComboBox>
#include <QHeaderView>
#include <QDesktopServices>
#include <QFile>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QPdfDocument>
#include <QPdfView>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <cmath>
#include <limits>

// ---------------------------------------------------------------------------
// §4 状态文案与九类资产面板。DESIGN.md dock 面板 tokens：
// surface #FFFFFF、border #DFE5EC、text #24303E、text-muted #5D6E80、8pt captions。
// ---------------------------------------------------------------------------
namespace
{
  QLabel *caption8(const QString &text, QWidget *parent)
  {
    auto *l = new QLabel(text, parent);
    QFont f = l->font();
    f.setPointSize(8);
    l->setFont(f);
    l->setStyleSheet(QStringLiteral("color: #5D6E80;"));
    return l;
  }

  QLabel *stateLabel(const QString &text, QWidget *parent)
  {
    auto *l = new QLabel(text, parent);
    l->setAlignment(Qt::AlignCenter);
    l->setWordWrap(true);
    l->setStyleSheet(QStringLiteral("color: #5D6E80;"));
    l->setObjectName(QStringLiteral("stateText"));
    return l;
  }

  // 单道曲线（§4：现有单道绘制风格，不搬连井面板）。y=深度向下，NaN 断线。
  class CurvePanel : public QWidget
  {
  public:
    CurvePanel(QWidget *parent = nullptr) : QWidget(parent) { setMinimumSize(260, 320); }
    void setCurve(const QString &name, const QString &unit, const QVector<double> &values,
                  const QVector<double> &depths)
    {
      m_name = name;
      m_unit = unit;
      m_pts.clear();
      double vMin = std::numeric_limits<double>::max(), vMax = std::numeric_limits<double>::lowest();
      double dMin = std::numeric_limits<double>::max(), dMax = std::numeric_limits<double>::lowest();
      for (int i = 0; i < qMin(values.size(), depths.size()); ++i)
      {
        if (std::isnan(values.at(i)) || std::isnan(depths.at(i)))
          continue;
        m_pts.append(QPointF(values.at(i), depths.at(i)));
        vMin = qMin(vMin, values.at(i));
        vMax = qMax(vMax, values.at(i));
        dMin = qMin(dMin, depths.at(i));
        dMax = qMax(dMax, depths.at(i));
      }
      if (m_pts.isEmpty())
      {
        update();
        return;
      }
      m_vRange = {vMin == vMax ? vMin - 1.0 : vMin, vMax == vMin ? vMax + 1.0 : vMax};
      m_dRange = {dMin, dMax};
      update();
    }
    int pointCount() const { return m_pts.size(); }

  protected:
    void paintEvent(QPaintEvent *) override
    {
      QPainter p(this);
      p.fillRect(rect(), Qt::white);
      const QRect plot = rect().adjusted(48, 26, -12, -32);
      p.setPen(QColor(QStringLiteral("#DFE5EC")));
      p.drawRect(plot);
      p.setPen(QColor(QStringLiteral("#5D6E80")));
      QFont f = p.font();
      f.setPointSize(8);
      p.setFont(f);
      p.drawText(rect().adjusted(48, 4, -12, -18), Qt::AlignLeft,
                 m_name + (m_unit.isEmpty() ? QString() : QStringLiteral(" · ") + m_unit));
      if (m_pts.isEmpty())
      {
        p.drawText(plot, Qt::AlignCenter, QObject::tr("无有效采样"));
        return;
      }
      const auto mapX = [&](double v) {
        return plot.left() + (v - m_vRange.first) / (m_vRange.second - m_vRange.first) * plot.width();
      };
      const auto mapY = [&](double d) {
        return plot.bottom() -
               (d - m_dRange.first) / (m_dRange.second - m_dRange.first) * plot.height();
      };
      p.setPen(QPen(QColor(QStringLiteral("#1B73D0")), 1.4));
      bool first = true;
      QPointF prev;
      for (const QPointF &pt : m_pts)
      {
        const QPointF mapped(mapX(pt.x()), mapY(pt.y()));
        if (!first)
          p.drawLine(prev, mapped);
        prev = mapped;
        first = false;
      }
      p.drawText(QRect(0, plot.top() - 2, 44, 20), Qt::AlignRight,
                 QString::number(m_dRange.first, 'f', 0));
      p.drawText(QRect(0, rect().bottom() - 26, 44, 20), Qt::AlignRight,
                 QString::number(m_dRange.second, 'f', 0));
    }

  private:
    QString m_name, m_unit;
    QVector<QPointF> m_pts;
    QPair<double, double> m_vRange{0, 1}, m_dRange{0, 1};
  };

  // 地震剖面：一条 inline/crossline 的变密度灰度渲染（§4/§7：只解码这一条）。
  class SectionPanel : public QWidget
  {
  public:
    SectionPanel(QWidget *parent = nullptr) : QWidget(parent) { setMinimumSize(320, 260); }
    void setTraces(const QVector<SegyTrace> &traces, float dtUs, double t0Ms)
    {
      const int w = qMax(1, traces.size());
      const int h = traces.isEmpty() ? 1 : qMax(1, traces.front().samples.size());
      m_img = QImage(w, h, QImage::Format_Grayscale8);
      m_img.fill(255);
      float amp = 1e-6f;
      for (const SegyTrace &t : traces)
        for (float s : t.samples)
          amp = qMax(amp, qAbs(s));
      for (int x = 0; x < w; ++x)
      {
        const SegyTrace &t = traces.at(x);
        for (int y = 0; y < h; ++y)
        {
          const int sy = qBound(0, y, t.samples.size() - 1);
          const float v = t.samples.at(sy) / amp;
          const int g = qRound((v * 0.5f + 0.5f) * 255.0f);
          m_img.setPixel(x, y, static_cast<uchar>(g));
        }
      }
      m_caption = QObject::tr("%1 道 · %2 样点 · %3 ms 采样 · t0 = %4 ms")
                      .arg(traces.size())
                      .arg(traces.isEmpty() ? 0 : traces.front().samples.size())
                      .arg(dtUs / 1000.0f, 0, 'f', 1)
                      .arg(t0Ms, 0, 'f', 1);
      update();
    }
    bool hasImage() const { return !m_img.isNull(); }

  protected:
    void paintEvent(QPaintEvent *) override
    {
      QPainter p(this);
      p.fillRect(rect(), Qt::white);
      if (m_img.isNull())
      {
        p.setPen(QColor(QStringLiteral("#5D6E80")));
        p.drawText(rect(), Qt::AlignCenter, QObject::tr("尚未解码剖面"));
        return;
      }
      const QRect dst = rect().adjusted(6, 22, -6, -20);
      p.drawImage(dst, m_img.scaled(dst.size(), Qt::IgnoreAspectRatio, Qt::FastTransformation));
      p.setPen(QColor(QStringLiteral("#5D6E80")));
      QFont f = p.font();
      f.setPointSize(8);
      p.setFont(f);
      p.drawText(rect().adjusted(6, 2, -6, -2), Qt::AlignLeft, m_caption);
    }

  private:
    QImage m_img;
    QString m_caption;
  };

  void walkCoords(const QJsonArray &arr, double *minX, double *minY, double *maxX, double *maxY)
  {
    if (arr.isEmpty())
      return;
    if (arr.at(0).isArray())
    {
      for (const QJsonValue &v : arr)
        walkCoords(v.toArray(), minX, minY, maxX, maxY);
      return;
    }
    const double x = arr.at(0).toDouble();
    const double y = arr.size() > 1 ? arr.at(1).toDouble() : 0.0;
    *minX = qMin(*minX, x);
    *maxX = qMax(*maxX, x);
    *minY = qMin(*minY, y);
    *maxY = qMax(*maxY, y);
  }
} // namespace

DataPreviewTabs::DataPreviewTabs(QWidget *parent)
  : QWidget(parent)
{
  auto *lay = new QVBoxLayout(this);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(4);

  m_tabs = new QTabWidget(this);
  m_tabs->setObjectName(QStringLiteral("dataPreviewTabs"));
  m_tabs->setTabsClosable(true);
  // dock 面板样式（DESIGN.md）：无工作流蓝下划线，安静边框。
  m_tabs->setStyleSheet(QStringLiteral(
      "QTabWidget::pane { border: 1px solid #DFE5EC; background: #FFFFFF; top: -1px; }"
      "QTabBar::tab { padding: 4px 10px; color: #5D6E80; border: 1px solid #DFE5EC;"
      " border-bottom: none; background: #FFFFFF; }"
      "QTabBar::tab:selected { color: #24303E; font-weight: 600; }"));
  connect(m_tabs, &QTabWidget::tabCloseRequested, this, [this](int index) {
    const QString assetId = assetIdAt(index);
    if (!assetId.isEmpty())
      closeAssetTab(assetId);
  });
  connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index) {
    if (index >= 0)
      focusWellIfNeeded(assetIdAt(index), m_tabs->widget(index));
  });
  lay->addWidget(m_tabs);

  m_emptyLabel = stateLabel(tr("还没有打开的预览 — 在列表中选择一条数据"), this);
  m_emptyLabel->setObjectName(QStringLiteral("previewEmptyLabel"));
  lay->addWidget(m_emptyLabel);
  m_tabs->setVisible(false);
}

void DataPreviewTabs::setImportService(DataImportService *svc)
{
  if (m_svc)
    disconnect(m_svc, nullptr, this, nullptr);
  m_svc = svc;
  if (!m_svc)
    return;
  // 文档 PDF 转换完成/失败 → 重建该资产标签（「转换中」→ 预览或降级面）。
  connect(m_svc, &DataImportService::documentPdfReady, this,
          [this](const QString &assetId) { rebuildAssetTab(assetId); });
  connect(m_svc, &DataImportService::documentPdfFailed, this,
          [this](const QString &assetId, const QString &) { rebuildAssetTab(assetId); });
}

int DataPreviewTabs::tabCount() const
{
  return m_tabs->count();
}

QString DataPreviewTabs::assetIdAt(int index) const
{
  QWidget *w = m_tabs->widget(index);
  if (!w)
    return QString();
  for (auto it = m_pageOfAsset.constBegin(); it != m_pageOfAsset.constEnd(); ++it)
    if (it.value() == w)
      return it.key();
  return QString();
}

void DataPreviewTabs::closeAssetTab(const QString &assetId)
{
  QWidget *page = m_pageOfAsset.value(assetId);
  if (!page)
    return;
  const int idx = m_tabs->indexOf(page);
  if (idx >= 0)
    m_tabs->removeTab(idx);
  m_pageOfAsset.remove(assetId);
  page->deleteLater();
  if (m_tabs->count() == 0)
  {
    m_tabs->setVisible(false);
    m_emptyLabel->setVisible(true);
  }
}

bool DataPreviewTabs::isMissingSourceState(const QString &assetId) const
{
  QWidget *page = m_pageOfAsset.value(assetId);
  if (!page)
    return false;
  auto *lbl = page->findChild<QLabel *>(QStringLiteral("stateText"));
  return lbl && lbl->text().contains(tr("找不到源文件"));
}

void DataPreviewTabs::focusWellIfNeeded(const QString &assetId, QWidget *page)
{
  Q_UNUSED(page);
  if (!m_svc || assetId.isEmpty())
    return;
  const auto links = m_svc->catalog()->linksForAsset(assetId);
  for (const EntityAssetLink &l : links)
    if (l.role == QLatin1String("well_head") && !l.unresolved)
    {
      emit wellSelected(l.entityId);
      return;
    }
}

void DataPreviewTabs::rebuildAssetTab(const QString &assetId)
{
  QWidget *page = m_pageOfAsset.value(assetId);
  if (!page)
    return;
  auto *pageLay = qobject_cast<QVBoxLayout *>(page->layout());
  if (!pageLay)
    return;
  while (QLayoutItem *it = pageLay->takeAt(0))
  {
    if (QWidget *w = it->widget())
      w->deleteLater();
    delete it;
  }
  QString title, wellEntity, horizonLayer;
  QWidget *content = buildContent(assetId, &title, &wellEntity, &horizonLayer);
  pageLay->addWidget(content ? content : stateLabel(tr("无法生成预览"), page), 1);
  const int idx = m_tabs->indexOf(page);
  if (idx >= 0 && !title.isEmpty())
    m_tabs->setTabText(idx, title);
  if (!horizonLayer.isEmpty())
    if (auto *btn = page->findChild<QPushButton *>(QStringLiteral("showOnMapBtn")))
      connect(btn, &QPushButton::clicked, this,
              [this, horizonLayer] { emit showHorizonOnMapRequested(horizonLayer); });
}

void DataPreviewTabs::openAsset(const QString &assetId)
{
  if (!m_svc || assetId.isEmpty())
    return;
  if (QWidget *existing = m_pageOfAsset.value(assetId))
  {
    m_tabs->setCurrentIndex(m_tabs->indexOf(existing)); // 重选聚焦（§4）
    focusWellIfNeeded(assetId, existing);
    return;
  }

  QWidget *page = new QWidget(this);
  auto *pageLay = new QVBoxLayout(page);
  pageLay->setContentsMargins(8, 8, 8, 8);

  QString title, wellEntity, horizonLayer;
  QWidget *content = buildContent(assetId, &title, &wellEntity, &horizonLayer);
  pageLay->addWidget(content ? content : stateLabel(tr("无法生成预览"), page), 1);

  const int idx = m_tabs->addTab(page, title.isEmpty() ? assetId : title);
  m_pageOfAsset.insert(assetId, page);
  m_tabs->setVisible(true);
  m_emptyLabel->setVisible(false);
  m_tabs->setCurrentIndex(idx);
  focusWellIfNeeded(assetId, page);

  // horizon 标签的「在地图上显示」（§4）
  if (!horizonLayer.isEmpty())
    if (auto *btn = page->findChild<QPushButton *>(QStringLiteral("showOnMapBtn")))
      connect(btn, &QPushButton::clicked, this,
              [this, horizonLayer] { emit showHorizonOnMapRequested(horizonLayer); });
}

QWidget *DataPreviewTabs::buildContent(const QString &assetId, QString *titleOut,
                                       QString *wellEntityOut, QString *horizonLayerOut)
{
  DataCatalog *cat = m_svc->catalog();
  const CatalogAsset asset = cat->assetById(assetId);
  if (asset.id.isEmpty())
    return nullptr;
  const CatalogVersion v = cat->currentVersion(assetId);
  QString abs = m_svc->absolutePath(assetId);
  // 文档资产：RAW 原件是规范来源——currentVersion 可能已指向 DERIVED
  // PDF 转换件，缺失检查与「用系统程序打开」必须锚在原件上。
  if (asset.type == QLatin1String("document"))
    for (const CatalogVersion &cv : cat->versionsForAsset(assetId))
      if (cv.stage == QLatin1String("RAW"))
      {
        abs = m_svc->absolutePathForVersion(cv);
        break;
      }
  if (titleOut)
    *titleOut = asset.displayName;

  const auto links = cat->linksForAsset(assetId);
  QString linkedWell, linkedBoundary;
  for (const EntityAssetLink &l : links)
  {
    if (l.entityType == QLatin1String("well") && !l.unresolved && linkedWell.isEmpty())
      linkedWell = l.entityId;
    if (l.entityType == QLatin1String("sequence_boundary") && linkedBoundary.isEmpty())
      linkedBoundary = l.entityId;
  }
  if (wellEntityOut)
    *wellEntityOut = linkedWell;
  if (horizonLayerOut)
    *horizonLayerOut = linkedBoundary;

  QWidget *host = new QWidget(this);
  auto *lay = new QVBoxLayout(host);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(6);

  // 外链/受管缺失态（§4：「找不到源文件」+路径）
  if (abs.isEmpty() || !QFile::exists(abs))
  {
    lay->addWidget(stateLabel(tr("找不到源文件\n%1").arg(abs.isEmpty() ? v.path : abs), host), 1);
    return host;
  }

  const QString wellName =
      linkedWell.isEmpty() ? QString() : cat->entityById(linkedWell).name;

  if (asset.type == QLatin1String("well_log"))
  {
    QStringList names;
    QList<LasCurve> curves;
    QString perr;
    if (!LasParser::parse(abs, names, curves, &perr))
    {
      lay->addWidget(stateLabel(tr("%1\n%2").arg(perr, asset.displayName), host), 1);
      return host;
    }
    auto *panel = new CurvePanel(host);
    auto *combo = new QComboBox(host);
    combo->setObjectName(QStringLiteral("curveCombo"));
    for (int i = 1; i < names.size(); ++i) // curves[0] 是深度道
      combo->addItem(names.at(i));
    const int def = combo->findText(QStringLiteral("GR"));
    if (def >= 0)
      combo->setCurrentIndex(def);
    const auto apply = [panel, combo, names, curves]() {
      const int ci = combo->currentIndex() + 1;
      if (ci <= 0 || ci >= names.size())
        return;
      panel->setCurve(names.at(ci), curves.at(ci).unit, curves.at(ci).values,
                      curves.at(0).values);
    };
    connect(combo, &QComboBox::currentIndexChanged, host, apply);
    apply();
    lay->addWidget(caption8(tr("曲线（GR 可切换）"), host));
    lay->addWidget(combo);
    lay->addWidget(panel, 1);
    return host;
  }

  if (asset.type == QLatin1String("well_stratification"))
  {
    QFile f(abs);
    if (!f.open(QIODevice::ReadOnly))
    {
      lay->addWidget(stateLabel(tr("无法读取 %1").arg(asset.displayName), host), 1);
      return host;
    }
    const QVector<WellTopRecord> tops = parseWellTopsText(f.readAll());
    auto *table = new QTableWidget(0, 4, host);
    table->setObjectName(QStringLiteral("topsTable"));
    table->setHorizontalHeaderLabels({tr("层名"), tr("MD"), tr("TVD"), tr("Time(ms)")});
    table->verticalHeader()->setVisible(false);
    for (const WellTopRecord &t : tops)
    {
      if (!wellName.isEmpty() && t.wellName != wellName)
        continue; // 多井文件按当前井过滤，不拆文件（§3）
      const int r = table->rowCount();
      table->insertRow(r);
      table->setItem(r, 0, new QTableWidgetItem(t.topName));
      table->setItem(r, 1, new QTableWidgetItem(t.hasMd ? QString::number(t.md, 'f', 1) : QString()));
      table->setItem(r, 2,
                     new QTableWidgetItem(t.hasTvd ? QString::number(t.tvd, 'f', 1) : QString()));
      // Time 空（-99999）就显示空，不填假时间（§4）
      table->setItem(r, 3,
                     new QTableWidgetItem(t.hasTime ? QString::number(t.timeMs, 'f', 1) : QString()));
    }
    table->horizontalHeader()->setStretchLastSection(true);
    lay->addWidget(caption8(wellName.isEmpty() ? tr("分层表")
                                               : tr("%1 的分层表").arg(wellName),
                            host));
    lay->addWidget(table, 1);
    return host;
  }

  if (asset.type == QLatin1String("time_depth"))
  {
    QFile f(abs);
    if (!f.open(QIODevice::ReadOnly))
    {
      lay->addWidget(stateLabel(tr("无法读取 %1").arg(asset.displayName), host), 1);
      return host;
    }
    const TimeDepthTable td = parseTimeDepthText(f.readAll());
    auto *panel = new CurvePanel(host);
    QVector<double> tvds, times;
    for (const TdRow &r : td.rows)
    {
      if (!r.hasTvd)
        continue;
      tvds.append(r.tvd);
      times.append(r.timeMs);
    }
    panel->setCurve(tr("TIME–TVD"), QStringLiteral("ms"), times, tvds);
    lay->addWidget(panel, 1);
    return host;
  }

  if (asset.type == QLatin1String("well_head"))
  {
    QFile f(abs);
    if (!f.open(QIODevice::ReadOnly))
    {
      lay->addWidget(stateLabel(tr("无法读取 %1").arg(asset.displayName), host), 1);
      return host;
    }
    const QVector<WellHeadRecord> rows = parseWellHeadText(f.readAll());
    auto *info = new QWidget(host);
    auto *grid = new QVBoxLayout(info);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(4);
    const auto addRow = [&](const QString &k, const QString &val) {
      auto *row = new QWidget(info);
      auto *rl = new QHBoxLayout(row);
      rl->setContentsMargins(0, 0, 0, 0);
      rl->addWidget(caption8(k, row));
      auto *v = new QLabel(val, row);
      v->setStyleSheet(QStringLiteral("color: #24303E;"));
      rl->addWidget(v, 1);
      grid->addWidget(row);
    };
    // 预览按当前井过滤（多井井位文件）
    const WellHeadRecord *rec = nullptr;
    for (const WellHeadRecord &r : rows)
      if (!wellName.isEmpty() && r.name == wellName)
        rec = &r;
    if (!rec && rows.size() == 1)
      rec = &rows.front();
    if (!rec && !wellName.isEmpty())
    {
      lay->addWidget(stateLabel(tr("井 %1 不在该井位文件中").arg(wellName), host), 1);
      return host;
    }
    if (rec)
    {
      addRow(tr("井名"), rec->name);
      addRow(tr("X"), QString::number(rec->x, 'f', 2));
      addRow(tr("Y"), QString::number(rec->y, 'f', 2));
      addRow(tr("KB"), QString::number(rec->kb, 'f', 2));
      addRow(tr("TD"), QString::number(rec->td, 'f', 2));
    }
    const QString status = linkedWell.isEmpty()
                               ? QString()
                               : cat->entityById(linkedWell).coordinateStatus;
    addRow(tr("coordinate_status"), status);
    lay->addWidget(info);
    lay->addWidget(caption8(tr("选中时地图同时高亮该井"), host));
    lay->addStretch(1);
    return host;
  }

  if (asset.type == QLatin1String("horizon"))
  {
    const CatalogEntity sb =
        linkedBoundary.isEmpty() ? CatalogEntity() : cat->entityById(linkedBoundary);
    const QString pendingNote = sb.extra.value(QStringLiteral("pending")).toBool()
                                    ? tr("未决层位 — 不进入编图 chip")
                                    : QString();
    const CatalogVersion derived = [cat, &assetId]() {
      CatalogVersion best;
      for (const CatalogVersion &cv : cat->versionsForAsset(assetId))
        if (cv.stage == QLatin1String("DERIVED") && cv.versionNumber >= best.versionNumber)
          best = cv;
      return best;
    }();
    const QString gridTxt = derived.id.isEmpty()
                                ? tr("派生栅格：未生成")
                                : tr("网格 %1×%2 · Z %3 %4–%5 · 碰撞 %6")
                                      .arg(derived.extra.value(QStringLiteral("grid_rows")).toInt())
                                      .arg(derived.extra.value(QStringLiteral("grid_cols")).toInt())
                                      .arg(derived.extra.value(QStringLiteral("z_units")).toString(),
                                           QString::number(derived.extra.value(QStringLiteral("z_min")).toDouble(), 'f', 1),
                                           QString::number(derived.extra.value(QStringLiteral("z_max")).toDouble(), 'f', 1))
                                      .arg(derived.extra.value(QStringLiteral("collisions")).toInt());
    lay->addWidget(caption8(tr("层位 %1").arg(sb.name.isEmpty() ? asset.displayName : sb.name), host));
    auto *grid = new QLabel(gridTxt, host);
    grid->setStyleSheet(QStringLiteral("color: #24303E;"));
    grid->setWordWrap(true);
    lay->addWidget(grid);
    if (!pendingNote.isEmpty())
    {
      auto *p = new QLabel(pendingNote, host);
      p->setStyleSheet(QStringLiteral("color: #F29900;"));
      lay->addWidget(p);
    }
    if (!derived.id.isEmpty())
    {
      auto *btn = new QPushButton(tr("在地图上显示"), host);
      btn->setObjectName(QStringLiteral("showOnMapBtn"));
      lay->addWidget(btn, 0, Qt::AlignLeft);
      if (horizonLayerOut)
        *horizonLayerOut = QStringLiteral("horizon.%1").arg(sb.name);
    }
    lay->addStretch(1);
    return host;
  }

  if (asset.type == QLatin1String("seismic"))
  {
    // survey 几何（导入时冻结）驱动测线选择；只解码选中的一条（§7）。
    QString surveyId;
    for (const EntityAssetLink &l : links)
      if (l.role == QLatin1String("seismic_volume"))
        surveyId = l.entityId;
    const CatalogEntity survey =
        surveyId.isEmpty() ? CatalogEntity() : cat->entityById(surveyId);

    auto *bar = new QWidget(host);
    auto *barLay = new QHBoxLayout(bar);
    barLay->setContentsMargins(0, 0, 0, 0);
    auto *mode = new QComboBox(bar);
    mode->addItem(tr("Inline"), QStringLiteral("inline"));
    mode->addItem(tr("Crossline"), QStringLiteral("crossline"));
    auto *no = new QSpinBox(bar);
    no->setRange(static_cast<int>(survey.inlineMin), static_cast<int>(qMax(survey.inlineMax, survey.inlineMin)));
    auto *panel = new SectionPanel(host);
    const auto decode = [this, abs, survey, mode, no, panel]() {
      SegyReader r;
      QString err;
      if (!r.open(abs, &err))
      {
        panel->setTraces({}, r.sampleIntervalUs(), 0.0);
        return;
      }
      QVector<SegyTrace> line;
      const bool isInline = mode->currentData().toString() == QLatin1String("inline");
      if (isInline)
        r.readInline(no->value(), &line, &err);
      else
        r.readCrossline(no->value(), &line, &err);
      panel->setTraces(line, r.sampleIntervalUs(), r.geometry().startTimeMs);
    };
    connect(mode, &QComboBox::currentIndexChanged, host, [mode, no, survey, decode]() {
      const bool isInline = mode->currentData().toString() == QLatin1String("inline");
      no->setRange(isInline ? static_cast<int>(survey.inlineMin) : static_cast<int>(survey.xlineMin),
                   isInline ? static_cast<int>(qMax(survey.inlineMax, survey.inlineMin))
                            : static_cast<int>(qMax(survey.xlineMax, survey.xlineMin)));
      decode();
    });
    connect(no, &QSpinBox::valueChanged, host, decode);
    if (survey.inlineMin == 0 && survey.inlineMax == 0) // 无 survey 元数据时放开范围
      no->setRange(0, 1000000);
    decode();
    barLay->addWidget(mode);
    barLay->addWidget(no);
    barLay->addStretch(1);
    lay->addWidget(caption8(tr("选择一条 inline 或 crossline 解码"), host));
    lay->addWidget(bar);
    lay->addWidget(panel, 1);
    return host;
  }

  if (asset.type == QLatin1String("image_reference"))
  {
    auto *scroll = new QScrollArea(host);
    scroll->setWidgetResizable(true);
    auto *imgLabel = new QLabel(scroll);
    QPixmap pm(abs);
    if (pm.isNull())
    {
      lay->addWidget(stateLabel(tr("无法解析图片 %1").arg(asset.displayName), host), 1);
      return host;
    }
    imgLabel->setPixmap(pm.scaledToWidth(560, Qt::SmoothTransformation)); // 按面板宽缩放
    scroll->setWidget(imgLabel);
    lay->addWidget(scroll, 1);
    return host;
  }

  if (asset.type == QLatin1String("document"))
  {
    lay->addWidget(caption8(tr("文件"), host));
    auto *name = new QLabel(asset.displayName, host);
    name->setStyleSheet(QStringLiteral("color: #24303E;"));
    lay->addWidget(name);
    lay->addWidget(caption8(tr("类型"), host));
    auto *fmt = new QLabel(asset.format.toUpper(), host);
    fmt->setStyleSheet(QStringLiteral("color: #24303E;"));
    lay->addWidget(fmt);
    auto *btn = new QPushButton(tr("用系统程序打开"), host);
    connect(btn, &QPushButton::clicked, host,
            [abs] { QDesktopServices::openUrl(QUrl::fromLocalFile(abs)); });
    lay->addWidget(btn, 0, Qt::AlignLeft);

    // PDF 预览：pdf 原件直接渲染；office 格式经 soffice → DERIVED 懒转换。
    QString pdfAbs;
    if (asset.format == QLatin1String("pdf"))
      pdfAbs = abs;
    else if (m_svc)
    {
      m_svc->ensureDocumentPdf(assetId);
      switch (m_svc->documentPdfState(assetId))
      {
        case DataImportService::DocPdfState::Ready:
          pdfAbs = m_svc->documentPdfPath(assetId);
          break;
        case DataImportService::DocPdfState::Failed:
          lay->addWidget(
              stateLabel(tr("无 PDF 预览：%1").arg(m_svc->documentPdfError(assetId)),
                         host),
              1);
          break;
        default: // Pending（None 不可达——ensure 刚入队或已记失败）
          lay->addWidget(stateLabel(tr("正在转换为 PDF 预览…"), host), 1);
          break;
      }
    }
    else
      lay->addWidget(stateLabel(tr("无法生成 PDF 预览"), host), 1);

    if (!pdfAbs.isEmpty())
    {
      auto *doc = new QPdfDocument(host);
      if (doc->load(pdfAbs) == QPdfDocument::Error::None)
      {
        auto *view = new QPdfView(host);
        view->setObjectName(QStringLiteral("pdfView"));
        view->setDocument(doc);
        view->setPageMode(QPdfView::PageMode::MultiPage);
        lay->addWidget(view, 1);
        if (asset.format != QLatin1String("pdf"))
          lay->addWidget(
              caption8(tr("预览为 PDF 转换件；原件经「用系统程序打开」"), host));
      }
      else
        lay->addWidget(
            stateLabel(tr("PDF 转换件无法加载\n%1").arg(pdfAbs), host), 1);
    }
    return host;
  }

  if (asset.type == QLatin1String("geojson"))
  {
    QFile f(abs);
    if (!f.open(QIODevice::ReadOnly))
    {
      lay->addWidget(stateLabel(tr("无法读取 %1").arg(asset.displayName), host), 1);
      return host;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject())
    {
      lay->addWidget(stateLabel(tr("GeoJSON 解析失败\n%1").arg(asset.displayName), host), 1);
      return host;
    }
    const QJsonArray features = doc.object().value(QStringLiteral("features")).toArray();
    double minX = std::numeric_limits<double>::max(), minY = minX;
    double maxX = std::numeric_limits<double>::lowest(), maxY = maxX;
    QStringList propKeys;
    for (const QJsonValue &fv : features)
    {
      const QJsonObject feature = fv.toObject();
      const QJsonObject props = feature.value(QStringLiteral("properties")).toObject();
      for (auto it = props.begin(); it != props.end(); ++it)
        if (!propKeys.contains(it.key()))
          propKeys.append(it.key());
      walkCoords(feature.value(QStringLiteral("geometry")).toObject()
                     .value(QStringLiteral("coordinates"))
                     .toArray(),
                 &minX, &minY, &maxX, &maxY);
    }
    QStringList faciesKeys;
    for (const QString &k : propKeys)
      if (k.contains(QString::fromUtf8("相")))
        faciesKeys.append(k);
    QString text = tr("要素个数：%1\n坐标范围：X %2–%3，Y %4–%5\n属性字段：%6\n相名字段：%7")
                       .arg(features.size())
                       .arg(QString::number(minX, 'f', 2), QString::number(maxX, 'f', 2),
                            QString::number(minY, 'f', 2), QString::number(maxY, 'f', 2))
                       .arg(propKeys.join(QStringLiteral(", ")))
                       .arg(faciesKeys.isEmpty() ? tr("无") : faciesKeys.join(QStringLiteral(", ")));
    auto *body = new QLabel(text, host);
    body->setStyleSheet(QStringLiteral("color: #24303E;"));
    body->setWordWrap(true);
    lay->addWidget(body);
    auto *warn = new QLabel(tr("未配准 — 不加入地图"), host);
    warn->setStyleSheet(QStringLiteral("color: #F29900;"));
    lay->addWidget(warn);
    lay->addStretch(1);
    return host;
  }

  // unknown / 其余类型：文件名 + 说明。
  lay->addWidget(stateLabel(tr("参考数据\n%1").arg(asset.displayName), host), 1);
  return host;
}
