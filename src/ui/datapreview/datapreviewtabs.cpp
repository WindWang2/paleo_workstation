#include "datapreviewtabs.h"

#include "../paleotheme.h" // DESIGN.md token 出口（mono 数字面共用）

#include "../../catalog/datacatalog.h"
#include "../../io/dataimportservice.h"
#include "../../io/lasparser.h"
#include "../../io/segyreader.h"
#include "../../io/timedeptool.h"
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
#include <QStandardItemModel>
#include <QTabWidget>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <cmath>
#include <limits>

// ---------------------------------------------------------------------------
// §4 状态文案与九类资产面板。DESIGN.md dock 面板 tokens：
// surface #FFFFFF、border #DFE5EC、text #24303E、text-muted #5D6E80、8pt captions；
// 数值列 JetBrains Mono 9pt 右对齐；语义色 #F29900(警告)/#E53935(失败)。
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

  QLabel *warnLabel(const QString &text, QWidget *parent)
  {
    auto *l = new QLabel(text, parent);
    l->setStyleSheet(QStringLiteral("color: #F29900;")); // DESIGN.md warning
    l->setWordWrap(true);
    return l;
  }

  // DESIGN.md mono：数值/坐标/深度一律 JetBrains Mono 9pt tnum。
  QFont monoFont()
  {
    return PaleoTheme::monoFont(); // wave3/ux-consistency：共用注册/vendor 路径
  }

  QLabel *valueLabel(const QString &text, QWidget *parent, bool mono = false)
  {
    auto *v = new QLabel(text, parent);
    v->setStyleSheet(QStringLiteral("color: #24303E;"));
    if (mono)
    {
      v->setFont(monoFont());
      v->setAlignment(Qt::AlignRight | Qt::AlignVCenter); // 数字列右对齐（§4）
    }
    return v;
  }

  void setNumericItem(QTableWidgetItem *it)
  {
    it->setFont(monoFont());
    it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
  }

  // 单道曲线（§4：现有单道绘制风格，不搬连井面板）。y=深度向下，NaN 断线。
  class CurvePanel : public QWidget
  {
  public:
    CurvePanel(QWidget *parent = nullptr) : QWidget(parent) { setMinimumSize(260, 320); }
    // 空态文案按资产类型给（§4：time_depth → 「无时深表」，LAS → 「这条曲线没有有效样点」）。
    void setEmptyText(const QString &text)
    {
      m_emptyText = text;
      update();
    }
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
        p.drawText(plot, Qt::AlignCenter, m_emptyText);
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
    QString m_emptyText = QObject::tr("无有效采样");
    QVector<QPointF> m_pts;
    QPair<double, double> m_vRange{0, 1}, m_dRange{0, 1};
  };

  // 地震剖面：一条 inline/crossline 的变密度灰度渲染（§4/§7：只解码这一条）。
  // D61 标定：井的 D61 分层经时深表换算成 ms 后，在剖面上画一条水平标记线。
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
      m_t0Ms = t0Ms;
      m_dtMs = dtUs / 1000.0;
      m_caption = QObject::tr("%1 道 · %2 样点 · %3 ms 采样 · t0 = %4 ms")
                      .arg(traces.size())
                      .arg(traces.isEmpty() ? 0 : traces.front().samples.size())
                      .arg(dtUs / 1000.0f, 0, 'f', 1)
                      .arg(t0Ms, 0, 'f', 1);
      update();
    }
    bool hasImage() const { return !m_img.isNull(); }
    // 换测线先清掉上一张剖面（§4）。
    void clearImage()
    {
      m_img = QImage();
      m_caption.clear();
      m_error.clear();
      clearTieMarker();
      update();
    }
    // 解码入口的失败面（如外链 SHA-256 不一致）：清图并写出原因，不装成剖面。
    void setError(const QString &text)
    {
      m_img = QImage();
      m_error = text;
      clearTieMarker();
      update();
    }
    // 「井名 D61 · ms」标定线：挂在剖面时间轴上（没有数值就绝不画）。
    void setTieMarker(const QString &label, double ms)
    {
      m_tieLabel = label;
      m_tieMs = ms;
      update();
    }
    void clearTieMarker()
    {
      m_tieMs = qQNaN();
      m_tieLabel.clear();
    }

  protected:
    void paintEvent(QPaintEvent *) override
    {
      QPainter p(this);
      p.fillRect(rect(), Qt::white);
      if (m_img.isNull())
      {
        p.setPen(QColor(QStringLiteral("#5D6E80")));
        p.drawText(rect(), Qt::AlignCenter,
                   m_error.isEmpty() ? QObject::tr("尚未解码剖面") : m_error);
        return;
      }
      const QRect dst = rect().adjusted(6, 22, -6, -20);
      p.drawImage(dst, m_img.scaled(dst.size(), Qt::IgnoreAspectRatio, Qt::FastTransformation));
      // D61 标定线（§4/阶段 B）：时间 ms → 样点行 → 剖面内水平线 + 井名标注。
      if (std::isfinite(m_tieMs) && m_dtMs > 0.0 && m_img.height() > 1)
      {
        const double row = (m_tieMs - m_t0Ms) / m_dtMs;
        const double yFrac = (row + 0.5) / m_img.height();
        if (yFrac >= 0.0 && yFrac <= 1.0)
        {
          const int y = dst.top() + qRound(yFrac * dst.height());
          p.setPen(QPen(QColor(QStringLiteral("#24303E")), 1.5));
          p.drawLine(dst.left(), y, dst.right(), y);
          QFont f = p.font();
          f.setPointSize(8);
          p.setFont(f);
          p.drawText(QRect(dst.left() + 4, y - 16, dst.width() - 8, 14), Qt::AlignLeft,
                     m_tieLabel);
        }
      }
      p.setPen(QColor(QStringLiteral("#5D6E80")));
      QFont f = p.font();
      f.setPointSize(8);
      p.setFont(f);
      p.drawText(rect().adjusted(6, 2, -6, -2), Qt::AlignLeft, m_caption);
    }

  private:
    QImage m_img;
    QString m_caption;
    QString m_error;
    double m_t0Ms = 0.0, m_dtMs = 0.0;
    double m_tieMs = qQNaN();
    QString m_tieLabel;
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

// T27 中文化：coordinate_status 枚举 → §4 计划文案。untransformed 用与
// 状态栏/PDF 页脚同一句「工程坐标 · 米 · 未投影」；invalid/missing 用
// 「坐标无效」「没有坐标」，仍 text-muted（#5D6E80）。
QString DataPreviewTabs::coordinateStatusText(const QString &status)
{
  if (status == QLatin1String("ok"))
    return tr("坐标有效");
  if (status == QLatin1String("untransformed"))
    return tr("工程坐标 · 米 · 未投影");
  if (status == QLatin1String("invalid"))
    return tr("坐标无效");
  return tr("没有坐标"); // missing / 空 / 未知
}

void DataPreviewTabs::setHorizonOnMap(const QString &layerId, bool on)
{
  // T29 双向同步：所有绑到该 layerId 的「在地图上显示」按钮跟随图层可见性。
  for (QPushButton *btn : findChildren<QPushButton *>(QStringLiteral("showOnMapBtn")))
    if (btn->property("layerId").toString() == layerId)
    {
      btn->setProperty("onMap", on);
      btn->setText(on ? tr("已在地图上") : tr("在地图上显示"));
    }
}

DataPreviewTabs::DataPreviewTabs(QWidget *parent)
  : QWidget(parent)
{
  auto *lay = new QVBoxLayout(this);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(4);

  m_tabs = new QTabWidget(this);
  m_tabs->setObjectName(QStringLiteral("dataPreviewTabs"));
  m_tabs->setTabsClosable(true);
  m_tabs->setUsesScrollButtons(true); // T32：标签超宽滚动，不挤压
  m_tabs->setAccessibleName(tr("预览"));
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
  m_wellEntityOfAsset.remove(assetId);
  m_titleSuffixOfAsset.remove(assetId);
  page->setParent(nullptr); // 摘出子树再推迟删除，关闭后 findChild 不再命中
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

QLabel *DataPreviewTabs::loadingLabel(const QString &fileName, QWidget *parent)
{
  // §4 读取中态：「正在读取」+文件名。读取仍是同步的——标签先就位并立即
  // 重绘，文件读完后隐藏（钩子存在，但不引入线程）。
  auto *l = stateLabel(tr("正在读取\n%1").arg(fileName), parent);
  l->setObjectName(QStringLiteral("loadingText"));
  return l;
}

QWidget *DataPreviewTabs::failureState(const QString &assetId, const QString &reason,
                                       QWidget *parent)
{
  // §4 失败态：「读取失败」+原因+文件名+「重试」。重试 = 重建该标签。
  const QString name =
      m_svc ? m_svc->catalog()->assetById(assetId).displayName : assetId;
  auto *box = new QWidget(parent);
  auto *l = new QVBoxLayout(box);
  l->setContentsMargins(0, 0, 0, 0);
  l->setSpacing(4);
  l->addStretch(1);
  l->addWidget(stateLabel(tr("读取失败\n%1\n%2").arg(reason, name), box));
  auto *btn = new QPushButton(tr("重试"), box);
  btn->setObjectName(QStringLiteral("retryBtn"));
  connect(btn, &QPushButton::clicked, box,
          [this, assetId] { rebuildAssetTab(assetId); });
  l->addWidget(btn, 0, Qt::AlignHCenter);
  l->addStretch(1);
  return box;
}

void DataPreviewTabs::focusWellIfNeeded(const QString &assetId, QWidget *page)
{
  Q_UNUSED(page);
  if (!m_svc || assetId.isEmpty())
    return;
  // §4：well_head 标签的选中井在地图上高亮。多井标签只报该标签已选中的井
  // ——没有选中就不报，绝不拿第一条链接糊弄（m_wellEntityOfAsset 在
  // 唯一已决井/下拉框选择时写入）。
  const CatalogAsset asset = m_svc->catalog()->assetById(assetId);
  if (asset.type != QLatin1String("well_head"))
    return;
  const QString wellId = m_wellEntityOfAsset.value(assetId);
  if (!wellId.isEmpty())
    emit wellSelected(wellId);
}

void DataPreviewTabs::updateTabTitle(const QString &assetId)
{
  QWidget *page = m_pageOfAsset.value(assetId);
  if (!page)
    return;
  const int idx = m_tabs->indexOf(page);
  if (idx < 0)
    return;
  // §4：标题是「文件名 · 井名」/「文件名 · 测线」；无过滤时只有文件名。
  QString title =
      m_svc ? m_svc->catalog()->assetById(assetId).displayName : assetId;
  if (title.isEmpty())
    title = assetId;
  const QString suffix = m_titleSuffixOfAsset.value(assetId);
  if (!suffix.isEmpty())
    title += QStringLiteral(" · ") + suffix;
  m_tabs->setTabText(idx, title);
}

void DataPreviewTabs::rebuildAssetTab(const QString &assetId)
{
  QWidget *page = m_pageOfAsset.value(assetId);
  if (!page || !m_svc)
    return;
  auto *pageLay = qobject_cast<QVBoxLayout *>(page->layout());
  if (!pageLay)
    return;
  while (QLayoutItem *it = pageLay->takeAt(0))
  {
    if (QWidget *w = it->widget())
    {
      // 信号发送者（如「重试」钮）可能就在被清的子树里——不能就地 delete，
      // 但先摘出父子树，deleteLater 后 findChild 不再碰到陈旧控件。
      w->setParent(nullptr);
      w->deleteLater();
    }
    delete it;
  }
  const QString name = m_svc->catalog()->assetById(assetId).displayName;
  QLabel *loading = loadingLabel(name.isEmpty() ? assetId : name, page);
  pageLay->addWidget(loading, 1);
  loading->repaint(); // 「正在读取」先可见，随后同步读
  QWidget *content = buildContent(assetId, page);
  loading->setVisible(false); // 保留在树里，便于测试/诊断读取中态
  pageLay->addWidget(content ? content : stateLabel(tr("无法生成预览"), page), 1);
  updateTabTitle(assetId);
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

  const QString displayName = m_svc->catalog()->assetById(assetId).displayName;
  QWidget *page = new QWidget(this);
  auto *pageLay = new QVBoxLayout(page);
  pageLay->setContentsMargins(8, 8, 8, 8);

  QLabel *loading = loadingLabel(displayName.isEmpty() ? assetId : displayName, page);
  pageLay->addWidget(loading, 1);

  const int idx = m_tabs->addTab(page, displayName.isEmpty() ? assetId : displayName);
  m_pageOfAsset.insert(assetId, page);
  m_tabs->setVisible(true);
  m_emptyLabel->setVisible(false);
  m_tabs->setCurrentIndex(idx);
  loading->repaint(); // 「正在读取」+文件名在同步读取前先可见（§4）

  QWidget *content = buildContent(assetId, page);
  loading->setVisible(false); // 读取完成；隐藏但保留节点便于测试断言该状态
  pageLay->addWidget(content ? content : stateLabel(tr("无法生成预览"), page), 1);
  updateTabTitle(assetId);
  focusWellIfNeeded(assetId, page);
}

void DataPreviewTabs::openSeismicLine(const QString &assetId, const QString &kind,
                                      int line, double timeMs)
{
  openAsset(assetId); // §4 重选语义：已有标签聚焦，否则新开
  QWidget *page = m_pageOfAsset.value(assetId);
  if (!page)
    return;
  auto *mode = page->findChild<QComboBox *>(QStringLiteral("lineMode"));
  auto *no = page->findChild<QSpinBox *>(QStringLiteral("lineSpin"));
  if (!mode || !no)
    return; // 非地震标签（或地震正文未建出来）——不造假测线控件
  const int want = mode->findData(
      kind == QLatin1String("crossline") ? QStringLiteral("crossline")
                                         : QStringLiteral("inline"));
  if (want >= 0 && mode->currentIndex() != want)
    mode->setCurrentIndex(want); // currentIndexChanged → 该控件链路上的 decode
  if (no->value() != line)
    no->setValue(line); // valueChanged → decode 目标测线
  Q_UNUSED(timeMs); // 目标时间的标注由剖面自身的 D61 标定线承担（§4/阶段B）
}

QWidget *DataPreviewTabs::buildContent(const QString &assetId, QWidget *page)
{
  Q_UNUSED(page);
  DataCatalog *cat = m_svc->catalog();
  const CatalogAsset asset = cat->assetById(assetId);
  if (asset.id.isEmpty())
    return nullptr;
  const CatalogVersion v = cat->currentVersion(assetId);
  CatalogVersion sourceVersion = v; // abs 实际对应的版本（文档标签锚回 RAW 原件）
  QString abs = m_svc->absolutePathForVersion(v);
  // 文档资产：RAW 原件是规范来源——currentVersion 可能已指向 DERIVED
  // PDF 转换件，缺失检查与「用系统程序打开」必须锚在原件上。
  if (asset.type == QLatin1String("document"))
    for (const CatalogVersion &cv : cat->versionsForAsset(assetId))
      if (cv.stage == QLatin1String("RAW"))
      {
        sourceVersion = cv;
        abs = m_svc->absolutePathForVersion(cv);
        break;
      }

  const auto links = cat->linksForAsset(assetId);
  // 已决井链接 → 多井标签的「井」下拉框数据源（未决链接不进列表，§4）。
  QVector<QPair<QString, QString>> wells; // (entityId, 井名)
  QString linkedBoundary;
  bool hasResolvedNonAux = false;
  bool hasAuxLink = false;
  for (const EntityAssetLink &l : links)
  {
    if (l.unresolved || l.entityId.isEmpty())
      continue;
    if (l.entityType == QLatin1String("well"))
    {
      const CatalogEntity w = cat->entityById(l.entityId);
      wells.append({l.entityId, w.name.isEmpty() ? l.entityId : w.name});
      hasResolvedNonAux = true;
      continue;
    }
    if (l.entityType == QLatin1String("sequence_boundary") && linkedBoundary.isEmpty())
      linkedBoundary = l.entityId;
    if (l.entityType == QLatin1String("auxiliary"))
      hasAuxLink = true;
    else
      hasResolvedNonAux = true;
  }
  // 固定辅助参考（§4 阶段 D）：XML 被内容判成井类但按规则钉在辅助实体上
  // （如 参考资料/ 下的 HZ28-6-1）——链接全部是 auxiliary 时一律走参考面板，
  // 绝不拿 well_head/well_log 类型去解析。
  const bool auxOnly = hasAuxLink && !hasResolvedNonAux;

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

  // 外链完整性（§3）：入库时留过 SHA-256 的源文件被改过就不再解码——
  // 正文如实写「源文件与入库时的 SHA-256 不一致」。
  if (!sourceVersion.managed && !sourceVersion.sha256.isEmpty())
  {
    QString verr;
    if (!cat->verifyExternalVersionSha(sourceVersion, &verr))
    {
      lay->addWidget(stateLabel(verr, host), 1);
      return host;
    }
  }

  if (asset.type == QLatin1String("well_log") && !auxOnly)
  {
    // 单井曲线：按已决链接过滤（LAS 本就是单井文件），标题带井名。
    QString linkedWell;
    if (!wells.isEmpty())
      linkedWell = wells.front().first;
    if (!linkedWell.isEmpty())
    {
      m_wellEntityOfAsset[assetId] = linkedWell;
      m_titleSuffixOfAsset[assetId] = wells.front().second;
    }
    QStringList names;
    QList<LasCurve> curves;
    QString perr;
    if (!LasParser::parse(abs, names, curves, &perr))
    {
      lay->addWidget(failureState(assetId, perr, host), 1);
      return host;
    }
    auto *panel = new CurvePanel(host);
    panel->setEmptyText(tr("这条曲线没有有效样点")); // §4：整条 -99999 → 不绘制
    auto *combo = new QComboBox(host);
    combo->setObjectName(QStringLiteral("curveCombo"));
    combo->setAccessibleName(tr("曲线"));
    for (int i = 1; i < names.size(); ++i) // curves[0] 是深度道
      combo->addItem(names.at(i), i); // userData = curves 下标（禁用项不受序号偏移影响）
    // §4：约定的 GR/AC/DEN 缺了就给禁用项，tooltip 写「这条曲线不在文件里」。
    static const QStringList kExpected{QStringLiteral("GR"), QStringLiteral("AC"),
                                       QStringLiteral("DEN")};
    for (const QString &cn : kExpected)
      if (combo->findText(cn) < 0)
      {
        const int j = combo->count();
        combo->addItem(cn, -1);
        combo->setItemData(j, tr("这条曲线不在文件里"), Qt::ToolTipRole);
        auto *model = qobject_cast<QStandardItemModel *>(combo->model());
        if (model && model->item(j))
          model->item(j)->setEnabled(false);
      }
    const int def = combo->findText(QStringLiteral("GR"));
    if (def >= 0 && combo->itemData(def).toInt() > 0) // 禁用项不当作默认曲线
      combo->setCurrentIndex(def);
    const auto apply = [panel, combo, names, curves]() {
      const int ci = combo->currentData().toInt();
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

  const bool wellFilterable = asset.type == QLatin1String("well_head") ||
                              asset.type == QLatin1String("well_stratification") ||
                              asset.type == QLatin1String("time_depth");
  if (wellFilterable && !auxOnly)
  {
    if (wells.size() == 1)
    {
      // 恰好一口已决井：直接按它过滤（§4 autoplan：唯一解析时过滤即它）。
      const QString wellId = wells.front().first;
      m_wellEntityOfAsset[assetId] = wellId;
      m_titleSuffixOfAsset[assetId] = wells.front().second;
      lay->addWidget(buildWellBody(asset, abs, wellId, wells.front().second, host), 1);
      return host;
    }
    // T31 死胡同文案：多井 tab 无井可挂（下拉会是空的）时不留空白页——
    // 工程没井指向导入；资产未决指向数据页「挂到这口井」入口。
    if (wells.isEmpty())
    {
      auto *deadEnd = stateLabel(
          cat->entities(QStringLiteral("well")).isEmpty()
              ? tr("工程里还没有井 — 先导入工区文件夹（井位表会建立井）")
              : tr("这个资产还没有挂到任何井 — 在数据页资产表的「未决」行，"
                   "用「挂到这口井」把它挂上"),
          host);
      deadEnd->setObjectName(QStringLiteral("deadEndText"));
      lay->addWidget(deadEnd, 1);
      return host;
    }

    // 多井文件（井口表、DC.dat、多井 TD）或未决资产：每标签自带「井」下拉框，
    // 只列已决链接的井；默认未选 → 正文「先选择一口井」。
    auto *bar = new QWidget(host);
    auto *barLay = new QHBoxLayout(bar);
    barLay->setContentsMargins(0, 0, 0, 0);
    barLay->addWidget(caption8(tr("井"), bar));
    auto *combo = new QComboBox(bar);
    combo->setObjectName(QStringLiteral("wellCombo"));
    combo->setAccessibleName(tr("井"));
    for (const auto &w : wells)
      combo->addItem(w.second, w.first);
    combo->setCurrentIndex(-1); // 默认未选（§4）
    barLay->addWidget(combo);
    barLay->addStretch(1);
    lay->addWidget(bar);

    auto *bodyHost = new QWidget(host);
    auto *bodyLay = new QVBoxLayout(bodyHost);
    bodyLay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(bodyHost, 1);

    const auto applyWell = [this, assetId, asset, abs, cat, combo, bodyLay, bodyHost,
                            host](const QString &wellId) {
      // 本标签自己的选择：不动其他标签（§4）。
      if (wellId.isEmpty())
        m_wellEntityOfAsset.remove(assetId);
      else
        m_wellEntityOfAsset[assetId] = wellId;
      const QString wname =
          wellId.isEmpty() ? QString() : cat->entityById(wellId).name;
      m_titleSuffixOfAsset[assetId] = wname;
      updateTabTitle(assetId);
      while (QLayoutItem *it = bodyLay->takeAt(0))
      {
        if (QWidget *w = it->widget())
          delete w; // 直接删：发送者（下拉框）不在正文子树里，陈旧控件立刻出树
        delete it;
      }
      if (wellId.isEmpty())
        bodyLay->addWidget(stateLabel(tr("先选择一口井"), bodyHost), 1);
      else
      {
        bodyLay->addWidget(buildWellBody(asset, abs, wellId, wname, bodyHost), 1);
        if (asset.type == QLatin1String("well_head"))
          emit wellSelected(wellId); // §4：选中时地图同时高亮该井
      }
    };
    connect(combo, &QComboBox::currentIndexChanged, host,
            [applyWell, combo](int idx) {
              applyWell(idx >= 0 ? combo->itemData(idx).toString() : QString());
            });
    // 重建时恢复本标签之前选中的井；否则保持未选。
    const QString prev = m_wellEntityOfAsset.value(assetId);
    const int prevIdx = prev.isEmpty() ? -1 : combo->findData(prev);
    if (prevIdx >= 0)
      combo->setCurrentIndex(prevIdx); // 触发 applyWell → 正文按该井渲染
    else
      applyWell(QString());
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
    const QString gridTxt =
        derived.id.isEmpty()
            ? tr("派生栅格：未生成")
            : tr("网格 %1×%2 · Z %3 %4–%5 · 拒绝 %6 · 碰撞 %7")
                  .arg(derived.extra.value(QStringLiteral("grid_rows")).toInt())
                  .arg(derived.extra.value(QStringLiteral("grid_cols")).toInt())
                  .arg(derived.extra.value(QStringLiteral("z_units")).toString(),
                       QString::number(derived.extra.value(QStringLiteral("z_min")).toDouble(), 'f', 1),
                       QString::number(derived.extra.value(QStringLiteral("z_max")).toDouble(), 'f', 1))
                  .arg(derived.extra.value(QStringLiteral("rejected")).toInt())
                  .arg(derived.extra.value(QStringLiteral("collisions")).toInt());
    lay->addWidget(caption8(tr("层位 %1").arg(sb.name.isEmpty() ? asset.displayName : sb.name), host));
    auto *grid = new QLabel(gridTxt, host);
    grid->setStyleSheet(QStringLiteral("color: #24303E;"));
    grid->setWordWrap(true);
    lay->addWidget(grid);
    if (!pendingNote.isEmpty())
    {
      auto *p = warnLabel(pendingNote, host);
      lay->addWidget(p);
    }
    // 「在地图上显示」（§4/T29）：无派生栅格时禁用并给出原因 tooltip；点击
    // 发意图（shell 实例化+缩放+闪烁后回调 setHorizonOnMap 置「已在地图上」；
    // 图层树里关掉可见性时同样回调置回）。
    auto *btn = new QPushButton(tr("在地图上显示"), host);
    btn->setObjectName(QStringLiteral("showOnMapBtn"));
    btn->setAccessibleName(tr("在地图上显示层位 %1").arg(sb.name.isEmpty()
                                                              ? asset.displayName
                                                              : sb.name));
    if (derived.id.isEmpty() || sb.name.isEmpty())
    {
      btn->setEnabled(false);
      btn->setToolTip(tr("还没有这个层位的栅格"));
    }
    else
    {
      const QString layerId = QStringLiteral("horizon.%1").arg(sb.name);
      btn->setProperty("layerId", layerId); // T29：双向同步按 layerId 寻址
      connect(btn, &QPushButton::clicked, this, [this, layerId]() {
        emit showHorizonOnMapRequested(layerId);
      });
    }
    lay->addWidget(btn, 0, Qt::AlignLeft);
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

    // ---- 标定井：catalog 序第一口有 D61 分层的井（§3/阶段 B）。----
    // 分层点有坐标用分层点，没有退回井口；TD 表取该井已决 time_depth 关联。
    QString tieWellId, tieWellName;
    WellTopRecord tieTop;
    bool haveTieTop = false;
    double tieX = 0.0, tieY = 0.0;
    bool tieHasCoords = false;
    {
      // 先按资产把各井的 D61 行收集起来（多井文件解析一次就好）。
      QHash<QString, WellTopRecord> d61ByNorm;
      for (const CatalogAsset &a : cat->assets())
      {
        if (a.type != QLatin1String("well_stratification"))
          continue;
        const CatalogVersion tv = cat->currentVersion(a.id);
        const QString p = tv.id.isEmpty() ? QString() : m_svc->absolutePathForVersion(tv);
        if (p.isEmpty() || !QFile::exists(p))
          continue;
        QFile f(p);
        if (!f.open(QIODevice::ReadOnly))
          continue;
        for (const WellTopRecord &t : parseWellTopsText(f.readAll()))
        {
          if (t.topName != QLatin1String("D61"))
            continue;
          const QString norm = DataCatalog::normalizeWellName(t.wellName);
          if (!norm.isEmpty() && !d61ByNorm.contains(norm))
            d61ByNorm.insert(norm, t);
        }
      }
      for (const CatalogEntity &w : cat->entities(QStringLiteral("well")))
      {
        const auto it = d61ByNorm.constFind(DataCatalog::normalizeWellName(w.name));
        if (it == d61ByNorm.constEnd())
          continue;
        tieWellId = w.id;
        tieWellName = w.name;
        tieTop = *it;
        haveTieTop = true;
        if (it->hasX && it->hasY)
        {
          tieX = it->x;
          tieY = it->y;
          tieHasCoords = true;
        }
        else if (w.hasSurface)
        {
          tieX = w.surfaceX;
          tieY = w.surfaceY;
          tieHasCoords = true;
        }
        break;
      }
    }
    TimeDepthTable tieTd;
    if (haveTieTop)
      for (const EntityAssetLink &l : cat->linksForEntity(tieWellId))
      {
        if (l.role != QLatin1String("time_depth") || l.unresolved || !l.isPrimary)
          continue;
        const CatalogVersion tv = cat->currentVersion(l.assetId);
        const QString p = tv.id.isEmpty() ? QString() : m_svc->absolutePathForVersion(tv);
        QFile f(p);
        if (!p.isEmpty() && f.open(QIODevice::ReadOnly))
          tieTd = parseTimeDepthText(f.readAll());
        break; // 主关联只有一条
      }

    // 标定线：D61 分层深度经 TD 表换成 ms；失败原因如实写，绝不造时间。
    // 分层点 TVD 空时改用 MD（§3）；两者皆空 → 留默认 NoTable「无时深表」。
    TimeDepthTool::TdResult tie;
    if (haveTieTop && (tieTop.hasTvd || tieTop.hasMd))
    {
      const bool useMd = !tieTop.hasTvd;
      const double depth = tieTop.hasTvd ? tieTop.tvd : tieTop.md;
      tie = TimeDepthTool::interpolateTimeMs(tieTd, depth, useMd);
    }

    // 初始测线：标定井所在 inline（survey 角点线性内插；判不出回 min，§4/§7）。
    int initialInline = -1;
    if (tieHasCoords && survey.corners.size() == 4 && survey.inlineMax > survey.inlineMin)
    {
      // corners 序：(inlMin,xlMin) (inlMin,xlMax) (inlMax,xlMax) (inlMax,xlMin)。
      const double yAtMin =
          (survey.corners.at(0).second + survey.corners.at(1).second) * 0.5;
      const double yAtMax =
          (survey.corners.at(2).second + survey.corners.at(3).second) * 0.5;
      if (yAtMax != yAtMin)
      {
        const double f = (tieY - yAtMin) / (yAtMax - yAtMin);
        const int inl = qRound(survey.inlineMin + f * (survey.inlineMax - survey.inlineMin));
        if (inl >= survey.inlineMin && inl <= survey.inlineMax)
          initialInline = inl;
      }
    }

    auto *bar = new QWidget(host);
    auto *barLay = new QHBoxLayout(bar);
    barLay->setContentsMargins(0, 0, 0, 0);
    auto *mode = new QComboBox(bar);
    mode->setObjectName(QStringLiteral("lineMode"));
    mode->setAccessibleName(tr("测线"));
    mode->addItem(tr("纵测线"), QStringLiteral("inline"));
    mode->addItem(tr("横测线"), QStringLiteral("crossline"));
    auto *no = new QSpinBox(bar);
    no->setObjectName(QStringLiteral("lineSpin"));
    no->setAccessibleName(tr("测线号"));
    no->setRange(static_cast<int>(survey.inlineMin),
                 static_cast<int>(qMax(survey.inlineMax, survey.inlineMin)));
    if (survey.inlineMin == 0 && survey.inlineMax == 0) // 无 survey 元数据时放开范围
      no->setRange(0, 1000000);
    if (initialInline >= 0)
      no->setValue(initialInline);
    auto *panel = new SectionPanel(host);
    auto *tieCaption = caption8(QString(), host);
    tieCaption->setObjectName(QStringLiteral("tieLabel"));
    if (haveTieTop)
    {
      // 标定写「A1 D61」和时间，或「无时深表」「超出时深表」「时深表无序」之一。
      if (tie.ok())
        tieCaption->setText(
            tr("%1 D61 · %2 ms").arg(tieWellName).arg(tie.timeMs, 0, 'f', 1));
      else
        tieCaption->setText(
            tr("%1 D61 · %2").arg(tieWellName, TimeDepthTool::reasonText(tie.status)));
    }
    const auto decode = [this, assetId, abs, survey, mode, no, panel, tieCaption, v,
                         haveTieTop, tie, tieWellName]() {
      panel->clearImage(); // 换测线先清掉上一张剖面（§4）
      // 「正在建立道索引」：索引/解码仍同步进行——套上禁用态+tooltip+立绘，
      // 状态钩子就位但不引入线程。
      const QString idxTip = tr("正在建立道索引");
      mode->setEnabled(false);
      no->setEnabled(false);
      mode->setToolTip(idxTip);
      no->setToolTip(idxTip);
      const auto restore = [mode, no]() {
        mode->setEnabled(true);
        no->setEnabled(true);
        mode->setToolTip(QString());
        no->setToolTip(QString());
      };
      // §3：外链源在入库时留过 SHA-256——每次解码前照它再验一遍（文件可能在
      // 标签打开后被改动）。不一致就只写原因，不解码。
      if (!v.managed && !v.sha256.isEmpty() && m_svc)
      {
        QString verr;
        if (!m_svc->catalog()->verifyExternalVersionSha(v, &verr))
        {
          panel->setError(verr);
          restore();
          return;
        }
      }
      SegyReader r;
      QString err;
      if (!r.open(abs, &err))
      {
        // 解码失败如实写原因（§4），不装成灰 1×1。
        panel->setError(err.isEmpty() ? tr("无法打开文件") : err);
        restore();
        return;
      }
      QVector<SegyTrace> line;
      const bool isInline = mode->currentData().toString() == QLatin1String("inline");
      const bool ok = isInline ? r.readInline(no->value(), &line, &err)
                               : r.readCrossline(no->value(), &line, &err);
      if (!ok)
      {
        panel->setError(err.isEmpty() ? tr("无法解码测线") : err);
        restore();
        return;
      }
      panel->setTraces(line, r.sampleIntervalUs(), r.geometry().startTimeMs);
      if (haveTieTop && tie.ok())
        panel->setTieMarker(tieCaption->text(), tie.timeMs);
      // 标题后缀：「文件名 · IL1315」/「文件名 · XL4165」（§4）。
      m_titleSuffixOfAsset[assetId] =
          (isInline ? QStringLiteral("IL") : QStringLiteral("XL")) +
          QString::number(no->value());
      updateTabTitle(assetId);
      restore();
    };
    connect(mode, &QComboBox::currentIndexChanged, host, [mode, no, survey, decode]() {
      const bool isInline = mode->currentData().toString() == QLatin1String("inline");
      no->setRange(isInline ? static_cast<int>(survey.inlineMin) : static_cast<int>(survey.xlineMin),
                   isInline ? static_cast<int>(qMax(survey.inlineMax, survey.inlineMin))
                            : static_cast<int>(qMax(survey.xlineMax, survey.xlineMin)));
      decode();
    });
    connect(no, &QSpinBox::valueChanged, host, decode);
    decode();
    barLay->addWidget(mode);
    barLay->addWidget(no);
    if (initialInline >= 0)
      barLay->addWidget(caption8(tr("%1 所在测线").arg(tieWellName), bar)); // §4 旁注
    barLay->addStretch(1);
    lay->addWidget(caption8(tr("选择一条测线解码"), host));
    lay->addWidget(bar);
    lay->addWidget(tieCaption);
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
      lay->addWidget(failureState(assetId, tr("无法解析图片"), host), 1);
      return host;
    }
    imgLabel->setPixmap(pm.scaledToWidth(560, Qt::SmoothTransformation)); // 按面板宽缩放
    scroll->setWidget(imgLabel);
    lay->addWidget(scroll, 1);
    lay->addWidget(warnLabel(tr("未配准，不加入地图"), host)); // §4
    return host;
  }

  if (asset.type == QLatin1String("document"))
  {
    lay->addWidget(caption8(tr("文件"), host));
    lay->addWidget(valueLabel(asset.displayName, host));
    lay->addWidget(caption8(tr("类型"), host));
    lay->addWidget(valueLabel(asset.format.toUpper(), host));
    auto *openErr = warnLabel(QString(), host);
    openErr->setObjectName(QStringLiteral("openErrorText"));
    openErr->setVisible(false);
    auto *btn = new QPushButton(tr("用系统程序打开"), host);
    connect(btn, &QPushButton::clicked, host, [abs, openErr]() {
      if (!QDesktopServices::openUrl(QUrl::fromLocalFile(abs)))
      {
        openErr->setText(QObject::tr("系统没有打开这个文件\n%1").arg(abs));
        openErr->setVisible(true);
      }
    });
    lay->addWidget(btn, 0, Qt::AlignLeft);
    lay->addWidget(openErr);

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
    lay->addWidget(warnLabel(tr("未配准，不加入地图"), host)); // §4
    return host;
  }

  if (asset.type == QLatin1String("geojson"))
  {
    QFile f(abs);
    if (!f.open(QIODevice::ReadOnly))
    {
      lay->addWidget(failureState(assetId, f.errorString(), host), 1);
      return host;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject())
    {
      lay->addWidget(failureState(assetId, tr("GeoJSON 解析失败"), host), 1);
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
    lay->addWidget(warnLabel(tr("经纬度，与本测网不是同一空间"), host)); // §4
    lay->addStretch(1);
    return host;
  }

  // ---- 辅助/参考与未知类型（§4 阶段 D）：文件名 + 类型 + 系统打开 +
  // 「未配准，不加入地图」；HZ28-6-1 XML 额外写「不对应 A1–A20」。----
  {
    QString auxName;
    for (const EntityAssetLink &l : links)
      if (l.entityType == QLatin1String("auxiliary") && !l.entityId.isEmpty())
      {
        auxName = cat->entityById(l.entityId).name;
        break;
      }
    lay->addWidget(caption8(tr("文件"), host));
    lay->addWidget(valueLabel(asset.displayName, host));
    lay->addWidget(caption8(tr("类型"), host));
    lay->addWidget(valueLabel(asset.format.isEmpty() ? asset.type : asset.format.toUpper(),
                              host));
    auto *openErr = warnLabel(QString(), host);
    openErr->setObjectName(QStringLiteral("openErrorText"));
    openErr->setVisible(false);
    auto *btn = new QPushButton(tr("用系统程序打开"), host);
    connect(btn, &QPushButton::clicked, host, [abs, openErr]() {
      if (!QDesktopServices::openUrl(QUrl::fromLocalFile(abs)))
      {
        openErr->setText(QObject::tr("系统没有打开这个文件\n%1").arg(abs));
        openErr->setVisible(true);
      }
    });
    lay->addWidget(btn, 0, Qt::AlignLeft);
    lay->addWidget(openErr);
    lay->addWidget(warnLabel(tr("未配准，不加入地图"), host));
    // 参考资料/ 下 HZ28-6-1 的 XML：不按内容挂井、不并进 A1–A20（§3 固定规则）。
    if (asset.displayName.contains(QStringLiteral("HZ28-6-1")) ||
        auxName.contains(QStringLiteral("HZ28-6-1")))
      lay->addWidget(warnLabel(tr("不对应 A1–A20"), host));
    lay->addStretch(1);
    return host;
  }
}

QWidget *DataPreviewTabs::buildWellBody(const CatalogAsset &asset, const QString &absPath,
                                        const QString &wellEntityId,
                                        const QString &wellName, QWidget *parent)
{
  const QString normWell = DataCatalog::normalizeWellName(wellName);
  const auto matchWell = [&normWell](const QString &rowName) {
    return normWell.isEmpty() ||
           DataCatalog::normalizeWellName(rowName) == normWell;
  };

  if (asset.type == QLatin1String("well_stratification"))
  {
    QFile f(absPath);
    if (!f.open(QIODevice::ReadOnly))
      return failureState(asset.id, f.errorString(), parent);
    const QVector<WellTopRecord> tops = parseWellTopsText(f.readAll());
    auto *holder = new QWidget(parent);
    auto *hl = new QVBoxLayout(holder);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(6);
    // §4：层名、MD、TVD、X、Y；Time 列为空就显示空，不填 -99999，也不填假时间。
    auto *table = new QTableWidget(0, 6, holder);
    table->setObjectName(QStringLiteral("topsTable"));
    table->setHorizontalHeaderLabels(
        {tr("层名"), tr("MD"), tr("TVD"), tr("X"), tr("Y"), tr("Time(ms)")});
    table->verticalHeader()->setVisible(false);
    for (const WellTopRecord &t : tops)
    {
      if (!matchWell(t.wellName))
        continue; // 多井文件按当前井过滤，不拆文件（§3）
      const int r = table->rowCount();
      table->insertRow(r);
      table->setItem(r, 0, new QTableWidgetItem(t.topName));
      auto *md = new QTableWidgetItem(t.hasMd ? QString::number(t.md, 'f', 1) : QString());
      auto *tvd = new QTableWidgetItem(t.hasTvd ? QString::number(t.tvd, 'f', 1) : QString());
      auto *x = new QTableWidgetItem(t.hasX ? QString::number(t.x, 'f', 2) : QString());
      auto *y = new QTableWidgetItem(t.hasY ? QString::number(t.y, 'f', 2) : QString());
      auto *tm = new QTableWidgetItem(t.hasTime ? QString::number(t.timeMs, 'f', 1) : QString());
      for (QTableWidgetItem *it : {md, tvd, x, y, tm})
        setNumericItem(it); // JetBrains Mono 9pt 右对齐（§4/DESIGN.md）
      table->setItem(r, 1, md);
      table->setItem(r, 2, tvd);
      table->setItem(r, 3, x);
      table->setItem(r, 4, y);
      table->setItem(r, 5, tm); // Time 空（-99999）就显示空，不填假时间
    }
    table->horizontalHeader()->setStretchLastSection(true);
    hl->addWidget(caption8(wellName.isEmpty() ? tr("分层表")
                                              : tr("%1 的分层表").arg(wellName),
                           holder));
    hl->addWidget(table, 1);
    return holder;
  }

  if (asset.type == QLatin1String("time_depth"))
  {
    QFile f(absPath);
    if (!f.open(QIODevice::ReadOnly))
      return failureState(asset.id, f.errorString(), parent);
    const TimeDepthTable td = parseTimeDepthText(f.readAll());
    QVector<double> tvds, times;
    for (const TdRow &r : td.rows)
    {
      if (!r.hasTvd)
        continue;
      tvds.append(r.tvd);
      times.append(r.timeMs);
    }
    // §4：time_depth 没有可用样点时写「无时深表」，不画假线。
    if (tvds.isEmpty())
      return stateLabel(tr("无时深表"), parent);
    auto *panel = new CurvePanel(parent);
    panel->setEmptyText(tr("无时深表")); // 双保险：NaN 过滤后仍空的兜底文案
    panel->setCurve(tr("TIME–TVD"), QStringLiteral("ms"), times, tvds);
    return panel;
  }

  if (asset.type == QLatin1String("well_head"))
  {
    QFile f(absPath);
    if (!f.open(QIODevice::ReadOnly))
      return failureState(asset.id, f.errorString(), parent);
    const QVector<WellHeadRecord> rows = parseWellHeadText(f.readAll());
    auto *holder = new QWidget(parent);
    auto *hl = new QVBoxLayout(holder);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(6);
    auto *info = new QWidget(holder);
    auto *grid = new QVBoxLayout(info);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(4);
    const auto addRow = [&](const QString &k, const QString &val, bool mono = false,
                            bool muted = false) {
      auto *row = new QWidget(info);
      auto *rl = new QHBoxLayout(row);
      rl->setContentsMargins(0, 0, 0, 0);
      rl->addWidget(caption8(k, row));
      auto *v = valueLabel(val, row, mono);
      if (muted)
        v->setStyleSheet(QStringLiteral("color: #5D6E80;")); // text-muted
      rl->addWidget(v, 1);
      grid->addWidget(row);
    };
    // 预览按当前井过滤（多井井位文件；井名规范化后比较）
    const WellHeadRecord *rec = nullptr;
    for (const WellHeadRecord &r : rows)
      if (matchWell(r.name))
        rec = &r;
    if (!rec && !wellName.isEmpty())
    {
      hl->addWidget(stateLabel(tr("井 %1 不在该井位文件中").arg(wellName), holder), 1);
      return holder;
    }
    if (rec)
    {
      // §4：井名、X、Y、KB、TD、BottomX、BottomY、WellType、coordinate_status；
      // 数字 JetBrains Mono 9pt 右对齐。
      addRow(tr("井名"), rec->name);
      addRow(tr("X"), QString::number(rec->x, 'f', 2), true);
      addRow(tr("Y"), QString::number(rec->y, 'f', 2), true);
      addRow(tr("KB"), QString::number(rec->kb, 'f', 2), true);
      addRow(tr("TD"), QString::number(rec->td, 'f', 2), true);
      addRow(tr("BottomX"),
             rec->hasBottomX ? QString::number(rec->bottomX, 'f', 2) : QString(), true);
      addRow(tr("BottomY"),
             rec->hasBottomY ? QString::number(rec->bottomY, 'f', 2) : QString(), true);
      addRow(tr("WellType"), rec->wellType);
    }
    const QString status =
        wellEntityId.isEmpty()
            ? QString()
            : m_svc->catalog()->entityById(wellEntityId).coordinateStatus;
    // T27：坐标状态行中文化 + text-muted（计划 §4：这些状态仍用 #5D6E80）。
    addRow(tr("坐标状态"), coordinateStatusText(status), false, true);
    hl->addWidget(info);
    hl->addWidget(caption8(tr("选中时地图同时高亮该井"), holder));
    hl->addStretch(1);
    return holder;
  }

  return stateLabel(tr("先选择一口井"), parent); // 兜底（不可达）
}
