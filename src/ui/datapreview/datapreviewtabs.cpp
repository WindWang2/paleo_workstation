// 层：视图
#include "datapreviewtabs.h"

#include "../paleotheme.h" // DESIGN.md token 出口（mono 数字面共用）
#include "../paleoicons.h" // 角落最大化/还原自绘图标

#include "../../catalog/datacatalog.h"
#include "../../domain/seismic/nicestep.h"
#include "../../domain/wellrecords.h"     // WellTopRecord/TimeDepthTable（domain 纯数据）
#include "../../domain/sectiontrace.h"    // SegyTrace/SegySectionGrid（domain 纯数据）
#include "../../io/lasdoc.h"              // LasCurve（白名单：数据模型）
#include "../../services/previewdoc.h"    // 唯一数据门面——解析/解码/SHA/PDF 编排全经它（W1）
#include "../seismic3d/seismic3dviewpanel.h"
#include "../seismicsection/seismicsectioncanvas.h"
#include "../wellcomposite/wellcompositepanel.h"

#include "../decorations/paleodecorations.h"
#include <qgsmapcanvas.h>
#include <qgslayertreemapcanvasbridge.h>
#include <qgsmaptoolpan.h>
#include <qgsproject.h>
#include <qgsrubberband.h>
#include <qgsgeometry.h>
#include <qgsvectorlayer.h>
#include <qgscategorizedsymbolrenderer.h>
#include <qgssymbol.h>
#include <qgsfillsymbol.h>
#include <qgsmarkersymbol.h>
#include <qgslinesymbol.h>
#include <qgspallabeling.h>
#include <qgsvectorlayerlabeling.h>
#include <qgstextbuffersettings.h>
#include <QButtonGroup>
#include <QTimer>

#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFileInfo>
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
#include <QMouseEvent>
#include <QPdfDocument>
#include <QPdfView>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QToolButton>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

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

  static QColor pickCurveColor(const QString &name, int index)
  {
    const QString upper = name.toUpper();
    if (upper == QLatin1String("GR") || upper.startsWith(QLatin1String("GR_")))
      return QColor(QStringLiteral("#2E7D32")); // Forest Green (Standard Gamma Ray)
    if (upper == QLatin1String("AC") || upper.startsWith(QLatin1String("AC_")) || upper == QLatin1String("DT"))
      return QColor(QStringLiteral("#0288D1")); // Cyan/Light Blue (Acoustic Sonic)
    if (upper == QLatin1String("DEN") || upper.startsWith(QLatin1String("DEN_")) || upper == QLatin1String("RHOB"))
      return QColor(QStringLiteral("#D32F2F")); // Red (Bulk Density)
    if (upper == QLatin1String("CNL") || upper == QLatin1String("NPHI"))
      return QColor(QStringLiteral("#E65100")); // Orange (Neutron Porosity)
    if (upper.startsWith(QLatin1String("RT")) || upper.startsWith(QLatin1String("RD")) || upper.startsWith(QLatin1String("LLD")))
      return QColor(QStringLiteral("#7B1FA2")); // Purple (Deep Resistivity)
    if (upper == QLatin1String("SP"))
      return QColor(QStringLiteral("#00796B")); // Teal (Spontaneous Potential)
    if (upper.startsWith(QLatin1String("CAL")))
      return QColor(QStringLiteral("#455A64")); // Slate (Caliper)

    static const QStringList kPalette{
      QStringLiteral("#1B73D0"),
      QStringLiteral("#E65100"),
      QStringLiteral("#7B1FA2"),
      QStringLiteral("#00838F"),
      QStringLiteral("#C2185B"),
      QStringLiteral("#5D6E80"),
      QStringLiteral("#F57C00"),
      QStringLiteral("#388E3C")
    };
    return QColor(kPalette.at(index % kPalette.size()));
  }

  static QColor faciesColor(const QString &name)
  {
    const QString lower = name.toLower();
    if (lower.contains(QStringLiteral("深湖")) || lower.contains(QStringLiteral("深盆")) || lower.contains(QStringLiteral("deep basin")))
      return QColor(QStringLiteral("#4DD0E1"));
    if (lower.contains(QStringLiteral("半深湖")) || lower.contains(QStringLiteral("semi-deep")))
      return QColor(QStringLiteral("#80DEEA"));
    if (lower.contains(QStringLiteral("滨浅湖")) || lower.contains(QStringLiteral("浅湖")) || lower.contains(QStringLiteral("shallow lake")) || lower.contains(QStringLiteral("lake")))
      return QColor(QStringLiteral("#81D4FA"));
    if (lower.contains(QStringLiteral("滩坝")) || lower.contains(QStringLiteral("滩砂")) || lower.contains(QStringLiteral("beach bar")))
      return QColor(QStringLiteral("#FFF59D"));
    if (lower.contains(QStringLiteral("水下分流河道")) || lower.contains(QStringLiteral("distributary channel")))
      return QColor(QStringLiteral("#FFD54F"));
    if (lower.contains(QStringLiteral("河口坝")) || lower.contains(QStringLiteral("mouth bar")))
      return QColor(QStringLiteral("#FFE082"));
    if (lower.contains(QStringLiteral("远砂坝")) || lower.contains(QStringLiteral("distal bar")))
      return QColor(QStringLiteral("#FFE57F"));
    if (lower.contains(QStringLiteral("分流间湾")) || lower.contains(QStringLiteral("interdistributary")))
      return QColor(QStringLiteral("#DCEDC8"));
    if (lower.contains(QStringLiteral("席状砂")) || lower.contains(QStringLiteral("sheet sand")))
      return QColor(QStringLiteral("#FFF176"));
    if (lower.contains(QStringLiteral("三角洲前缘")) || lower.contains(QStringLiteral("delta front")))
      return QColor(QStringLiteral("#FFE082"));
    if (lower.contains(QStringLiteral("三角洲平原")) || lower.contains(QStringLiteral("delta plain")))
      return QColor(QStringLiteral("#E6EE9C"));
    if (lower.contains(QStringLiteral("前三角洲")) || lower.contains(QStringLiteral("prodelta")))
      return QColor(QStringLiteral("#B2DFDB"));
    if (lower.contains(QStringLiteral("三角洲")) || lower.contains(QStringLiteral("delta")))
      return QColor(QStringLiteral("#FFE082"));
    if (lower.contains(QStringLiteral("冲积扇")) || lower.contains(QStringLiteral("alluvial")))
      return QColor(QStringLiteral("#FFAB91"));
    if (lower.contains(QStringLiteral("河流")) || lower.contains(QStringLiteral("fluvial")) || lower.contains(QStringLiteral("channel")))
      return QColor(QStringLiteral("#FFB74D"));
    if (lower.contains(QStringLiteral("碳酸盐")) || lower.contains(QStringLiteral("台地")) || lower.contains(QStringLiteral("carbonate")) || lower.contains(QStringLiteral("platform")))
      return QColor(QStringLiteral("#A5D6A7"));
    if (lower.contains(QStringLiteral("生物礁")) || lower.contains(QStringLiteral("礁滩")) || lower.contains(QStringLiteral("reef")))
      return QColor(QStringLiteral("#80CBC4"));
    if (lower.contains(QStringLiteral("陆棚")) || lower.contains(QStringLiteral("浅海")) || lower.contains(QStringLiteral("shelf")) || lower.contains(QStringLiteral("marine")))
      return QColor(QStringLiteral("#90CAF9"));
    if (lower.contains(QStringLiteral("潮坪")) || lower.contains(QStringLiteral("tidal")))
      return QColor(QStringLiteral("#D7CCC8"));
    if (lower.contains(QStringLiteral("浊积")) || lower.contains(QStringLiteral("重力流")) || lower.contains(QStringLiteral("turbidite")))
      return QColor(QStringLiteral("#FFCC80"));

    static const QVector<QColor> fallbackPalette = {
      QColor(QStringLiteral("#81D4FA")),
      QColor(QStringLiteral("#FFE082")),
      QColor(QStringLiteral("#A5D6A7")),
      QColor(QStringLiteral("#FFAB91")),
      QColor(QStringLiteral("#CE93D8")),
      QColor(QStringLiteral("#FFF59D")),
      QColor(QStringLiteral("#80CBC4")),
      QColor(QStringLiteral("#B0BEC5")),
      QColor(QStringLiteral("#FFCC80")),
      QColor(QStringLiteral("#B39DDB"))
    };
    const uint h = qHash(name);
    return fallbackPalette.at(h % fallbackPalette.size());
  }

  static std::unique_ptr<QgsSymbol> createFaciesSymbol(Qgis::GeometryType geomType, const QColor &color)
  {
    const QColor strokeColor = color.darker(150);
    if (geomType == Qgis::GeometryType::Point)
    {
      QVariantMap props;
      props[QStringLiteral("name")] = QStringLiteral("circle");
      props[QStringLiteral("color")] = color.name(QColor::HexArgb);
      props[QStringLiteral("outline_color")] = strokeColor.name();
      props[QStringLiteral("outline_width")] = QStringLiteral("0.8");
      props[QStringLiteral("size")] = QStringLiteral("5.5");
      return QgsMarkerSymbol::createSimple(props);
    }
    else if (geomType == Qgis::GeometryType::Line)
    {
      QVariantMap props;
      props[QStringLiteral("line_color")] = color.name();
      props[QStringLiteral("line_width")] = QStringLiteral("1.5");
      return QgsLineSymbol::createSimple(props);
    }
    else // Polygon
    {
      QVariantMap props;
      QColor fill = color;
      fill.setAlpha(200);
      props[QStringLiteral("color")] = QStringLiteral("%1,%2,%3,%4")
                                          .arg(fill.red()).arg(fill.green()).arg(fill.blue()).arg(fill.alpha());
      props[QStringLiteral("outline_color")] = strokeColor.name();
      props[QStringLiteral("outline_width")] = QStringLiteral("0.8");
      props[QStringLiteral("outline_style")] = QStringLiteral("solid");
      return QgsFillSymbol::createSimple(props);
    }
  }

  struct CurveData
  {
    QString name;
    QString unit;
    QColor color;
    QVector<QPointF> pts; // (x = value, y = depth)
    QPair<double, double> vRange{0, 1};
    bool visible = true;
    double hoverValue = std::numeric_limits<double>::quiet_NaN();
  };

  // 测井道曲线面板：支持多曲线叠合、深度缩放、拖拽平移、标尺与光标读数
  class CurvePanel : public QWidget
  {
  public:
    explicit CurvePanel(QWidget *parent = nullptr) : QWidget(parent)
    {
      setMinimumSize(280, 320);
      setMouseTracking(true);
      m_vScroll = new QScrollBar(Qt::Vertical, this);
      m_vScroll->setVisible(false);
      connect(m_vScroll, &QScrollBar::valueChanged, this, [this](int val) {
        if (m_updatingScroll || m_zoomFactor <= 1.0)
          return;
        const double totalSpan = m_dRange.second - m_dRange.first;
        const double visibleSpan = totalSpan / m_zoomFactor;
        const double maxScroll = m_dRange.second - visibleSpan;
        if (maxScroll <= m_dRange.first)
          return;
        const int maxVal = m_vScroll->maximum();
        const double frac = maxVal > 0 ? static_cast<double>(val) / maxVal : 0.0;
        m_scrollDepth = m_dRange.first + frac * (maxScroll - m_dRange.first);
        update();
      });
    }

    void setEmptyText(const QString &text)
    {
      m_emptyText = text;
      update();
    }

    void clearCurves()
    {
      m_curves.clear();
      m_dRange = {0, 1};
      m_scrollDepth = 0.0;
      m_zoomFactor = 1.0;
      updateScrollBar();
      update();
    }

    void addCurve(const QString &name, const QString &unit,
                  const QVector<double> &values, const QVector<double> &depths,
                  const QColor &color, bool visible = true)
    {
      CurveData cd;
      cd.name = name;
      cd.unit = unit;
      cd.color = color;
      cd.visible = visible;

      double vMin = std::numeric_limits<double>::max(), vMax = std::numeric_limits<double>::lowest();
      double dMin = m_curves.isEmpty() ? std::numeric_limits<double>::max() : m_dRange.first;
      double dMax = m_curves.isEmpty() ? std::numeric_limits<double>::lowest() : m_dRange.second;

      const int n = qMin(values.size(), depths.size());
      for (int i = 0; i < n; ++i)
      {
        const double v = values.at(i);
        const double d = depths.at(i);
        if (std::isnan(v) || std::isnan(d) || v <= -9999.0)
          continue;
        cd.pts.append(QPointF(v, d));
        vMin = qMin(vMin, v);
        vMax = qMax(vMax, v);
        dMin = qMin(dMin, d);
        dMax = qMax(dMax, d);
      }

      if (!cd.pts.isEmpty())
      {
        cd.vRange = {vMin == vMax ? vMin - 1.0 : vMin, vMax == vMin ? vMax + 1.0 : vMax};
        m_dRange = {dMin, dMax};
        m_scrollDepth = m_dRange.first;
      }
      m_curves.append(cd);
      updateScrollBar();
      update();
    }

    // 单道兼容接口（time_depth 或旧代码调用）
    void setCurve(const QString &name, const QString &unit,
                  const QVector<double> &values, const QVector<double> &depths)
    {
      clearCurves();
      addCurve(name, unit, values, depths, QColor(QStringLiteral("#1B73D0")), true);
    }

    void setCurveVisible(const QString &name, bool visible)
    {
      for (CurveData &c : m_curves)
      {
        if (c.name.compare(name, Qt::CaseInsensitive) == 0)
        {
          c.visible = visible;
          break;
        }
      }
      update();
    }

    bool isCurveVisible(const QString &name) const
    {
      for (const CurveData &c : m_curves)
        if (c.name.compare(name, Qt::CaseInsensitive) == 0)
          return c.visible;
      return false;
    }

    int pointCount() const
    {
      int count = 0;
      for (const CurveData &c : m_curves)
        count += c.pts.size();
      return count;
    }

    double zoomFactor() const { return m_zoomFactor; }

    void setZoom(double z, double anchorDepth = -1.0)
    {
      const double clampedZ = qBound(1.0, z, 50.0);
      if (qFuzzyCompare(clampedZ, m_zoomFactor) && anchorDepth < 0)
        return;

      const double totalSpan = m_dRange.second - m_dRange.first;
      if (totalSpan <= 0)
        return;

      const double oldSpan = totalSpan / m_zoomFactor;
      const double newSpan = totalSpan / clampedZ;

      if (anchorDepth < 0)
        anchorDepth = m_scrollDepth + oldSpan * 0.5;

      const double anchorFrac = oldSpan > 0 ? (anchorDepth - m_scrollDepth) / oldSpan : 0.5;
      m_scrollDepth = anchorDepth - anchorFrac * newSpan;
      m_zoomFactor = clampedZ;

      const double maxScroll = m_dRange.second - newSpan;
      m_scrollDepth = qBound(m_dRange.first, m_scrollDepth, qMax(m_dRange.first, maxScroll));

      updateScrollBar();
      update();
      if (onZoomChanged)
        onZoomChanged(m_zoomFactor);
    }

    void zoomIn() { setZoom(m_zoomFactor * 1.5); }
    void zoomOut() { setZoom(m_zoomFactor / 1.5); }
    void resetZoom() { setZoom(1.0); }

    std::function<void(double)> onZoomChanged;
    std::function<void(double depth, const QString &info)> onHoverChanged;

    double depthAtY(int y) const
    {
      const QRect pRect = plotRect();
      if (pRect.height() <= 0)
        return m_dRange.first;
      const double visibleSpan = (m_dRange.second - m_dRange.first) / m_zoomFactor;
      const double frac = static_cast<double>(y - pRect.top()) / pRect.height();
      return m_scrollDepth + frac * visibleSpan;
    }

    int yAtDepth(double d) const
    {
      const QRect pRect = plotRect();
      if (pRect.height() <= 0)
        return 0;
      const double visibleSpan = (m_dRange.second - m_dRange.first) / m_zoomFactor;
      if (visibleSpan <= 0)
        return pRect.top();
      const double frac = (d - m_scrollDepth) / visibleSpan;
      return pRect.top() + qRound(frac * pRect.height());
    }

    int calculateHeaderHeight() const
    {
      int visibleCount = 0;
      for (const CurveData &c : m_curves)
        if (c.visible) visibleCount++;
      if (visibleCount <= 2)
        return 28;
      if (visibleCount <= 4)
        return 46;
      return 64;
    }

    QRect plotRect() const
    {
      const int kRulerW = 50;
      const int headerH = calculateHeaderHeight();
      const int scrollW = (m_zoomFactor > 1.0) ? 14 : 0;
      return QRect(kRulerW, headerH, qMax(20, width() - kRulerW - scrollW - 6),
                   qMax(20, height() - headerH - 8));
    }

  protected:
    void updateScrollBar()
    {
      if (!m_vScroll)
        return;
      const QRect pRect = plotRect();
      m_vScroll->setGeometry(width() - 14, pRect.top(), 14, pRect.height());

      if (m_zoomFactor <= 1.0)
      {
        m_vScroll->setVisible(false);
        return;
      }
      m_vScroll->setVisible(true);

      const double totalSpan = m_dRange.second - m_dRange.first;
      const double visibleSpan = totalSpan / m_zoomFactor;
      const double maxScroll = m_dRange.second - visibleSpan;

      m_updatingScroll = true;
      const int kRange = 10000;
      const int pageStep = qMax(1, qRound(kRange / m_zoomFactor));
      m_vScroll->setRange(0, kRange - pageStep);
      m_vScroll->setPageStep(pageStep);
      const double frac = (maxScroll > m_dRange.first)
                              ? (m_scrollDepth - m_dRange.first) / (maxScroll - m_dRange.first)
                              : 0.0;
      m_vScroll->setValue(qRound(frac * (kRange - pageStep)));
      m_updatingScroll = false;
    }

    void updateHoverValues()
    {
      for (CurveData &c : m_curves)
      {
        if (!c.visible || c.pts.isEmpty() || std::isnan(m_hoverDepth))
        {
          c.hoverValue = std::numeric_limits<double>::quiet_NaN();
          continue;
        }

        auto it = std::lower_bound(c.pts.begin(), c.pts.end(), m_hoverDepth,
                                   [](const QPointF &pt, double d) { return pt.y() < d; });
        if (it == c.pts.end())
        {
          c.hoverValue = c.pts.last().x();
        }
        else if (it == c.pts.begin())
        {
          c.hoverValue = c.pts.first().x();
        }
        else
        {
          const QPointF &p0 = *(it - 1);
          const QPointF &p1 = *it;
          if (qAbs(p1.y() - p0.y()) > 1e-4)
          {
            const double t = (m_hoverDepth - p0.y()) / (p1.y() - p0.y());
            c.hoverValue = p0.x() + t * (p1.x() - p0.x());
          }
          else
          {
            c.hoverValue = p1.x();
          }
        }
      }
      if (onHoverChanged)
      {
        if (std::isnan(m_hoverDepth))
        {
          onHoverChanged(-1.0, QString());
        }
        else
        {
          QString info = QString::asprintf("MD: %.1f m", m_hoverDepth);
          for (const CurveData &c : m_curves)
          {
            if (c.visible && !std::isnan(c.hoverValue))
            {
              info += QStringLiteral(" | ") + c.name + QString::asprintf(": %.2f", c.hoverValue);
              if (!c.unit.isEmpty())
                info += QStringLiteral(" ") + c.unit;
            }
          }
          onHoverChanged(m_hoverDepth, info);
        }
      }
    }

    void mousePressEvent(QMouseEvent *e) override
    {
      const QRect pRect = plotRect();
      if (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton)
      {
        if (m_zoomFactor > 1.0 && pRect.contains(e->pos()))
        {
          m_dragging = true;
          m_dragStartY = e->pos().y();
          m_dragStartScrollDepth = m_scrollDepth;
          setCursor(Qt::ClosedHandCursor);
        }
      }
    }

    void mouseMoveEvent(QMouseEvent *e) override
    {
      const QRect pRect = plotRect();
      if (pRect.contains(e->pos()))
      {
        m_hoverDepth = depthAtY(e->pos().y());
        updateHoverValues();
      }
      else
      {
        m_hoverDepth = std::numeric_limits<double>::quiet_NaN();
      }

      if (m_dragging)
      {
        const double totalSpan = m_dRange.second - m_dRange.first;
        const double visibleSpan = totalSpan / m_zoomFactor;
        const double dy = e->pos().y() - m_dragStartY;
        const double dDepth = (dy / static_cast<double>(pRect.height())) * visibleSpan;
        const double maxScroll = m_dRange.second - visibleSpan;
        m_scrollDepth = qBound(m_dRange.first, m_dragStartScrollDepth - dDepth, qMax(m_dRange.first, maxScroll));
        updateScrollBar();
        update();
      }
      else
      {
        if (m_zoomFactor > 1.0 && pRect.contains(e->pos()))
          setCursor(Qt::OpenHandCursor);
        else
          unsetCursor();
        update();
      }
    }

    void mouseReleaseEvent(QMouseEvent *) override
    {
      if (m_dragging)
      {
        m_dragging = false;
        if (m_zoomFactor > 1.0 && plotRect().contains(mapFromGlobal(QCursor::pos())))
          setCursor(Qt::OpenHandCursor);
        else
          unsetCursor();
      }
    }

    void mouseDoubleClickEvent(QMouseEvent *e) override
    {
      if (plotRect().contains(e->pos()))
      {
        resetZoom();
      }
    }

    void wheelEvent(QWheelEvent *e) override
    {
      const QRect pRect = plotRect();
      if (!pRect.contains(e->position().toPoint()))
      {
        e->ignore();
        return;
      }

      if (e->modifiers() & Qt::ControlModifier)
      {
        const double f = e->angleDelta().y() > 0 ? 1.25 : 1.0 / 1.25;
        setZoom(m_zoomFactor * f, depthAtY(e->position().y()));
        e->accept();
        return;
      }

      if (m_zoomFactor > 1.0)
      {
        const double totalSpan = m_dRange.second - m_dRange.first;
        const double visibleSpan = totalSpan / m_zoomFactor;
        const double step = visibleSpan * 0.12 * (e->angleDelta().y() > 0 ? -1.0 : 1.0);
        const double maxScroll = m_dRange.second - visibleSpan;
        m_scrollDepth = qBound(m_dRange.first, m_scrollDepth + step, qMax(m_dRange.first, maxScroll));
        updateScrollBar();
        update();
        e->accept();
        return;
      }

      e->ignore();
    }

    void leaveEvent(QEvent *) override
    {
      m_hoverDepth = std::numeric_limits<double>::quiet_NaN();
      for (CurveData &c : m_curves)
        c.hoverValue = std::numeric_limits<double>::quiet_NaN();
      unsetCursor();
      update();
      if (onHoverChanged)
        onHoverChanged(-1.0, QString());
    }

    void resizeEvent(QResizeEvent *) override
    {
      updateScrollBar();
    }

    void drawHeader(QPainter &p, const QRect &pRect)
    {
      const int headerTop = 4;
      int curX = pRect.left() + 4;
      int curY = headerTop;
      const int rowHeight = 18;

      QFont fName = font();
      fName.setPointSize(8);
      fName.setBold(true);

      QFont fMono = PaleoTheme::monoFont();
      fMono.setPointSize(8);

      for (const CurveData &c : m_curves)
      {
        if (!c.visible)
          continue;

        // Sample line
        p.setPen(QPen(c.color, 2.5, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(curX, curY + rowHeight / 2, curX + 12, curY + rowHeight / 2);
        curX += 16;

        // Curve Name
        p.setFont(fName);
        p.setPen(c.color);
        const QString nameStr = c.name;
        p.drawText(curX, curY + rowHeight - 4, nameStr);
        curX += fontMetrics().horizontalAdvance(nameStr) + 4;

        // Scale range & unit: e.g. "0–150 API"
        p.setFont(fMono);
        p.setPen(QColor(QStringLiteral("#5D6E80")));
        QString scaleStr;
        if (!std::isnan(c.hoverValue))
        {
          scaleStr = QString::asprintf(": %.1f", c.hoverValue);
          if (!c.unit.isEmpty())
            scaleStr += QStringLiteral(" ") + c.unit;
          scaleStr += QString::asprintf(" (%.0f–%.0f)", c.vRange.first, c.vRange.second);
        }
        else
        {
          scaleStr = QString::asprintf("[%.0f–%.0f", c.vRange.first, c.vRange.second);
          if (!c.unit.isEmpty())
            scaleStr += QStringLiteral(" ") + c.unit;
          scaleStr += QStringLiteral("]");
        }

        p.drawText(curX, curY + rowHeight - 4, scaleStr);
        curX += QFontMetrics(fMono).horizontalAdvance(scaleStr) + 12;

        if (curX > pRect.right() - 80)
        {
          curX = pRect.left() + 4;
          curY += rowHeight;
        }
      }
    }

    void paintEvent(QPaintEvent *) override
    {
      QPainter p(this);
      p.setRenderHint(QPainter::Antialiasing, true);

      p.fillRect(rect(), Qt::white);

      const QRect pRect = plotRect();
      const int kRulerW = pRect.left();

      // Draw ruler background
      const QRect rulerRect(0, pRect.top(), kRulerW, pRect.height());
      p.fillRect(rulerRect, QColor(QStringLiteral("#F8FAFC")));
      p.setPen(QColor(QStringLiteral("#DFE5EC")));
      p.drawLine(kRulerW, pRect.top(), kRulerW, pRect.bottom());

      // Ruler title "MD (m)"
      QFont fCaption = font();
      fCaption.setPointSize(8);
      p.setFont(fCaption);
      p.setPen(QColor(QStringLiteral("#5D6E80")));
      p.drawText(QRect(2, 4, kRulerW - 4, pRect.top() - 4), Qt::AlignCenter | Qt::AlignVCenter,
                 QStringLiteral("MD (m)"));

      p.drawRect(pRect);

      int totalPoints = 0;
      int visibleCurves = 0;
      for (const CurveData &c : m_curves)
      {
        totalPoints += c.pts.size();
        if (c.visible)
          visibleCurves++;
      }

      if (totalPoints == 0)
      {
        p.setPen(QColor(QStringLiteral("#5D6E80")));
        p.drawText(pRect, Qt::AlignCenter, m_emptyText);
        return;
      }

      if (visibleCurves == 0)
      {
        p.setPen(QColor(QStringLiteral("#5D6E80")));
        p.drawText(pRect, Qt::AlignCenter, tr("未勾选任何曲线 — 在上方选择要显示的曲线"));
        return;
      }

      const double totalSpan = m_dRange.second - m_dRange.first;
      const double visibleSpan = (totalSpan > 0 && m_zoomFactor >= 1.0)
                                     ? totalSpan / m_zoomFactor
                                     : 1.0;
      const double dTop = m_scrollDepth;
      const double dBottom = m_scrollDepth + visibleSpan;

      const int targetTicks = qBound(4, pRect.height() / 45, 12);
      const double rawInterval = visibleSpan / targetTicks;
      double niceInterval = 100.0;
      const double intervals[] = {0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0};
      for (double iv : intervals)
      {
        niceInterval = iv;
        if (iv >= rawInterval)
          break;
      }

      const double firstTick = std::ceil(dTop / niceInterval) * niceInterval;
      QFont fMono = PaleoTheme::monoFont();
      fMono.setPointSize(8);

      p.setFont(fMono);
      for (double d = firstTick; d <= dBottom; d += niceInterval)
      {
        const int y = yAtDepth(d);
        if (y < pRect.top() || y > pRect.bottom())
          continue;

        p.setPen(QColor(QStringLiteral("#9AA7B4")));
        p.drawLine(kRulerW - 5, y, kRulerW, y);

        p.setPen(QPen(QColor(QStringLiteral("#F0F4F8")), 1, Qt::DashLine));
        p.drawLine(pRect.left(), y, pRect.right(), y);

        p.setPen(QColor(QStringLiteral("#5D6E80")));
        const QString dText = QString::number(d, 'f', (niceInterval < 1.0 ? 1 : 0));
        p.drawText(QRect(2, y - 8, kRulerW - 9, 16), Qt::AlignRight | Qt::AlignVCenter, dText);
      }

      p.setPen(QPen(QColor(QStringLiteral("#F0F4F8")), 1, Qt::DotLine));
      for (int i = 1; i <= 3; ++i)
      {
        const int vx = pRect.left() + (pRect.width() * i) / 4;
        p.drawLine(vx, pRect.top(), vx, pRect.bottom());
      }

      p.setClipRect(pRect);
      for (const CurveData &c : m_curves)
      {
        if (!c.visible || c.pts.isEmpty())
          continue;

        const double vSpan = c.vRange.second - c.vRange.first;
        if (vSpan <= 0)
          continue;

        const auto mapX = [&](double v) {
          const double f = (v - c.vRange.first) / vSpan;
          return pRect.left() + f * pRect.width();
        };

        p.setPen(QPen(c.color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));

        bool first = true;
        QPointF prev;
        for (const QPointF &pt : c.pts)
        {
          const double v = pt.x();
          const double d = pt.y();

          if (d < dTop - niceInterval || d > dBottom + niceInterval)
          {
            first = true;
            continue;
          }

          const QPointF mapped(mapX(v), yAtDepth(d));
          if (!first)
            p.drawLine(prev, mapped);
          prev = mapped;
          first = false;
        }
      }
      p.setClipping(false);

      drawHeader(p, pRect);

      if (!std::isnan(m_hoverDepth) && pRect.contains(mapFromGlobal(QCursor::pos())))
      {
        const int hy = yAtDepth(m_hoverDepth);
        if (hy >= pRect.top() && hy <= pRect.bottom())
        {
          p.setPen(QPen(QColor(QStringLiteral("#1B73D0")), 1, Qt::DashLine));
          p.drawLine(pRect.left(), hy, pRect.right(), hy);

          const QString hText = QString::number(m_hoverDepth, 'f', 1);
          p.setFont(fMono);
          const QRect badgeRect(2, hy - 8, kRulerW - 4, 16);
          p.fillRect(badgeRect, QColor(QStringLiteral("#24303E")));
          p.setPen(Qt::white);
          p.drawText(badgeRect, Qt::AlignCenter, hText);
        }
      }
    }

  private:
    QVector<CurveData> m_curves;
    QPair<double, double> m_dRange{0, 1};
    double m_zoomFactor = 1.0;
    double m_scrollDepth = 0.0;
    QScrollBar *m_vScroll = nullptr;
    bool m_updatingScroll = false;
    double m_hoverDepth = std::numeric_limits<double>::quiet_NaN();
    bool m_dragging = false;
    int m_dragStartY = 0;
    double m_dragStartScrollDepth = 0.0;
    QString m_emptyText = QObject::tr("无有效采样");
  };

  // 地震剖面：一条 inline/crossline 的红白蓝双极振幅渲染（§4/§7：只解码这一条）。
  // 时间轴与色标：左侧显示 TWT(ms) 时间刻度轴，右侧显示红白蓝振幅色标与极性标注。
  // D61 标定：井的 D61 分层经时深表换算成 ms 后，在剖面上画一条水平标记线。
  class SectionPanel : public QWidget
  {
  public:
    SectionPanel(QWidget *parent = nullptr) : QWidget(parent) { setMinimumSize(320, 260); }
    void setTraces(const QVector<SegyTrace> &traces, float dtUs, double t0Ms)
    {
      if (traces.isEmpty())
      {
        clearImage();
        return;
      }
      const int w = qMax(1, traces.size());
      const SegySectionGrid grid = SegySectionGrid::forTraces(traces, dtUs, t0Ms);
      const int h = grid.rows;
      m_img = QImage(w, h, QImage::Format_ARGB32_Premultiplied);
      m_img.fill(qRgb(255, 255, 255));
      float amp = 1e-6f;
      for (const SegyTrace &t : traces)
        for (float s : t.samples)
          amp = qMax(amp, qAbs(s));
      for (int x = 0; x < w; ++x)
      {
        const SegyTrace &t = traces.at(x);
        for (int y = 0; y < h; ++y)
        {
          float sample = 0.0f;
          if (!grid.sampleAt(t, y, dtUs, t0Ms, &sample)) continue;
          const float v = std::clamp((sample / amp) * 1.35f, -1.0f, 1.0f);
          const float mag = std::pow(std::abs(v), 0.85f);
          const float k = 1.0f - mag;
          QRgb color;
          if (v < 0.0f) {
              // Deep blue to white (Trough)
              color = qRgb(static_cast<int>(217 * k), static_cast<int>(230 * k), 255);
          } else {
              // White to deep red (Peak)
              color = qRgb(255, static_cast<int>(224 * k), static_cast<int>(214 * k));
          }
          m_img.setPixel(x, y, color);
        }
      }
      m_maxAmp = amp;
      m_t0Ms = grid.startMs;
      m_dtMs = grid.stepMs;
      m_caption = QObject::tr("%1 道 · %2 样点 · %3 ms 采样 · t0 = %4 ms")
                      .arg(traces.size())
                      .arg(grid.rows)
                      .arg(grid.stepMs, 0, 'f', 1)
                      .arg(grid.startMs, 0, 'f', 1);
      update();
    }
    bool hasImage() const { return !m_img.isNull(); }
    void clearImage()
    {
      m_img = QImage();
      m_caption.clear();
      m_error.clear();
      clearTieMarker();
      update();
    }
    void setError(const QString &text)
    {
      m_img = QImage();
      m_error = text;
      clearTieMarker();
      update();
    }
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
      p.setRenderHint(QPainter::Antialiasing, true);
      p.fillRect(rect(), Qt::white);
      if (m_img.isNull())
      {
        p.setPen(QColor(QStringLiteral("#5D6E80")));
        p.drawText(rect(), Qt::AlignCenter,
                   m_error.isEmpty() ? QObject::tr("尚未解码剖面") : m_error);
        return;
      }

      const int leftMargin = 58;
      const int rightMargin = 54;
      const int topMargin = 26;
      const int bottomMargin = 16;
      const QRect dst(leftMargin, topMargin,
                      std::max(10, width() - leftMargin - rightMargin),
                      std::max(10, height() - topMargin - bottomMargin));

      // 1. 左侧时间刻度轴 (TWT ms 时间剖面)
      const QRect leftRuler(0, topMargin, leftMargin, dst.height());
      p.fillRect(leftRuler, QColor(QStringLiteral("#F5F7FA")));
      p.setPen(QColor(QStringLiteral("#DFE5EC")));
      p.drawLine(leftMargin, topMargin, leftMargin, dst.bottom());

      QFont monoFont(QStringLiteral("JetBrains Mono"), 7);
      QFont bodyFont(QStringLiteral("Noto Sans SC"), 7);
      p.setFont(bodyFont);
      p.setPen(QColor(QStringLiteral("#5D6E80")));
      p.drawText(QRect(2, 4, leftMargin - 4, 18), Qt::AlignCenter, QStringLiteral("TWT (ms)"));

      if (m_dtMs > 0.0 && m_img.height() > 0)
      {
        const double endTimeMs = m_t0Ms + m_img.height() * m_dtMs;
        const auto ticks = seismic::NiceStep::GenerateTicks(m_t0Ms, endTimeMs, topMargin, dst.bottom(), 6, QStringLiteral("%.0f"));
        p.setFont(monoFont);
        for (const auto &tk : ticks)
        {
          if (tk.pixelPos < topMargin || tk.pixelPos > dst.bottom()) continue;
          p.setPen(QColor(QStringLiteral("#5D6E80")));
          p.drawLine(QPointF(leftMargin - 6.0, tk.pixelPos), QPointF(leftMargin, tk.pixelPos));
          p.setPen(QColor(QStringLiteral("#24303E")));
          p.drawText(QRectF(2, tk.pixelPos - 7.0, leftMargin - 10, 14), Qt::AlignRight | Qt::AlignVCenter, QString::number(qRound(tk.value)));
        }
      }

      // 2. 剖面核心地震图像
      p.drawImage(dst, m_img.scaled(dst.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation));

      // 3. D61 标定线（时间 ms → 剖面内水平线 + 井名标注）
      if (std::isfinite(m_tieMs) && m_dtMs > 0.0 && m_img.height() > 1)
      {
        const double row = (m_tieMs - m_t0Ms) / m_dtMs;
        const double yFrac = (row + 0.5) / m_img.height();
        if (yFrac >= 0.0 && yFrac <= 1.0)
        {
          const int y = dst.top() + qRound(yFrac * dst.height());
          p.setPen(QPen(QColor(QStringLiteral("#24303E")), 1.5));
          p.drawLine(dst.left(), y, dst.right(), y);
          p.setFont(bodyFont);
          p.drawText(QRect(dst.left() + 4, y - 16, dst.width() - 8, 14), Qt::AlignLeft,
                     m_tieLabel);
        }
      }

      // 4. 右侧振幅色标 (Color Bar)
      const QRect rightBarRect(dst.right(), 0, rightMargin, height());
      p.fillRect(rightBarRect, QColor(QStringLiteral("#F5F7FA")));
      p.setPen(QColor(QStringLiteral("#DFE5EC")));
      p.drawLine(dst.right(), 0, dst.right(), height());

      p.setFont(QFont(QStringLiteral("Noto Sans SC"), 7, QFont::Bold));
      p.setPen(QColor(QStringLiteral("#24303E")));
      p.drawText(QRect(dst.right(), 4, rightMargin, 16), Qt::AlignCenter, tr("色标"));

      const int barW = 10;
      const int barX = dst.right() + 6;
      const int barTop = topMargin + 8;
      const int barH = std::max(20, dst.height() - 24);

      QLinearGradient grad(barX, barTop, barX, barTop + barH);
      grad.setColorAt(0.0, QColor(220, 38, 38));   // Red Peak
      grad.setColorAt(0.5, QColor(255, 255, 255)); // White Zero
      grad.setColorAt(1.0, QColor(25, 118, 210));  // Blue Trough

      p.setBrush(grad);
      p.setPen(QPen(QColor(QStringLiteral("#DFE5EC")), 1.0));
      p.drawRoundedRect(QRectF(barX, barTop, barW, barH), 2.0, 2.0);

      // 刻度值
      p.setFont(monoFont);
      p.setPen(QColor(QStringLiteral("#24303E")));
      const QString maxStr = m_maxAmp >= 1000.0f
          ? QStringLiteral("+%1k").arg(m_maxAmp / 1000.0f, 0, 'f', 0)
          : QStringLiteral("+%1").arg(qRound(m_maxAmp));
      const QString minStr = m_maxAmp >= 1000.0f
          ? QStringLiteral("-%1k").arg(m_maxAmp / 1000.0f, 0, 'f', 0)
          : QStringLiteral("-%1").arg(qRound(m_maxAmp));

      p.drawLine(QPointF(barX + barW, barTop), QPointF(barX + barW + 3, barTop));
      p.drawText(QRectF(barX + barW + 4, barTop - 6, rightMargin - barW - 10, 12), Qt::AlignLeft | Qt::AlignVCenter, maxStr);

      const double midY = barTop + barH * 0.5;
      p.drawLine(QPointF(barX + barW, midY), QPointF(barX + barW + 3, midY));
      p.drawText(QRectF(barX + barW + 4, midY - 6, rightMargin - barW - 10, 12), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("0"));

      p.drawLine(QPointF(barX + barW, barTop + barH), QPointF(barX + barW + 3, barTop + barH));
      p.drawText(QRectF(barX + barW + 4, barTop + barH - 6, rightMargin - barW - 10, 12), Qt::AlignLeft | Qt::AlignVCenter, minStr);

      p.setFont(QFont(QStringLiteral("Noto Sans SC"), 7));
      p.setPen(QColor(220, 38, 38));
      p.drawText(QRectF(dst.right(), barTop - 12, rightMargin - 4, 10), Qt::AlignRight, tr("波峰+"));
      p.setPen(QColor(25, 118, 210));
      p.drawText(QRectF(dst.right(), barTop + barH + 2, rightMargin - 4, 10), Qt::AlignRight, tr("波谷-"));

      // 5. 顶部说明条
      p.setPen(QColor(QStringLiteral("#5D6E80")));
      p.setFont(bodyFont);
      p.drawText(QRect(leftMargin + 4, 4, dst.width() - 8, 18), Qt::AlignLeft | Qt::AlignVCenter, m_caption);
    }

  private:
    QImage m_img;
    QString m_caption;
    QString m_error;
    float m_maxAmp = 1.0f;
    double m_t0Ms = 0.0, m_dtMs = 0.0;
    double m_tieMs = qQNaN();
    QString m_tieLabel;
  };

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
  // D7 最大化 affordance：右上角 checkable 钮，切换时只发意图信号——实际
  // 分栏尺寸由 shell 决定。空态时 tabs 隐藏，按钮随之隐藏。
  auto *maxBtn = new QToolButton(m_tabs);
  maxBtn->setObjectName(QStringLiteral("previewMaxButton"));
  maxBtn->setCheckable(true);
  maxBtn->setText(tr("最大化预览"));
  maxBtn->setAccessibleName(tr("最大化预览"));
  maxBtn->setToolTip(tr("预览占满数据面（列表留一行）"));
  // QGIS 主题没有最大化/还原语义——PaleoIcons 自绘，随勾选态切换。
  maxBtn->setIcon(PaleoIcons::maximize());
  maxBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  connect(maxBtn, &QToolButton::toggled, this, [this, maxBtn](bool on) {
    maxBtn->setText(on ? tr("还原预览") : tr("最大化预览"));
    maxBtn->setIcon(on ? PaleoIcons::restore() : PaleoIcons::maximize());
    maxBtn->setToolTip(on ? tr("恢复列表/预览分栏") : tr("预览占满数据面（列表留一行）"));
    emit previewMaximizeToggled(on);
  });
  m_tabs->setCornerWidget(maxBtn, Qt::TopRightCorner);
  lay->addWidget(m_tabs);

  m_emptyLabel = stateLabel(tr("还没有打开的预览 — 在列表中选择一条数据"), this);
  m_emptyLabel->setObjectName(QStringLiteral("previewEmptyLabel"));
  lay->addWidget(m_emptyLabel);
  m_tabs->setVisible(false);
}

DataPreviewTabs::~DataPreviewTabs() = default;

void DataPreviewTabs::setImportService(DataImportService *svc)
{
  // 自建门面（测试/小环境）；壳共享实例经 setDocService。
  m_docOwned.reset(svc ? new PreviewDocService(svc) : nullptr);
  attachDoc(m_docOwned.get());
}

void DataPreviewTabs::setDocService(PreviewDocService *doc)
{
  m_docOwned.reset();
  attachDoc(doc);
}

void DataPreviewTabs::attachDoc(PreviewDocService *doc)
{
  if (m_doc)
    disconnect(m_doc, nullptr, this, nullptr);
  if (m_catalogForTitles)
    disconnect(m_catalogForTitles, nullptr, this, nullptr);
  m_catalogForTitles = nullptr;
  m_doc = doc;
  if (!m_doc)
    return;
  if (m_taskSvc)
    m_doc->setTaskService(m_taskSvc); // 接线顺序无关：后到的服务补进门面
  // 文档 PDF 转换完成/失败 → 重建该资产标签（「转换中」→ 预览或降级面）。
  connect(m_doc, &PreviewDocService::documentPdfReady, this,
          [this](const QString &assetId) { rebuildAssetTab(assetId); });
  connect(m_doc, &PreviewDocService::documentPdfFailed, this,
          [this](const QString &assetId, const QString &) { rebuildAssetTab(assetId); });
  // 测线解码结果（D1/T23）：陈旧结果已在服务内按世代号丢弃。
  connect(m_doc, &PreviewDocService::seismicSectionReady, this,
          &DataPreviewTabs::onSectionReady);
  connect(m_doc, &PreviewDocService::seismicSectionFailed, this,
          &DataPreviewTabs::onSectionFailed);
  connect(m_doc, &PreviewDocService::seismicSectionCancelled, this,
          [this](const QString &assetId) {
            onSectionFailed(assetId, tr("已取消"));
          });
  // B 包 staleness-lite：stale 标记可能来自其它标签的 sha 复验或上游版本
  // 取代——catalog 任一变更后重算已开标签的「过时」徽标（GUI 线程直连，
  // 不必重建标签）。换绑服务时先断旧 catalog（上面已断）。
  m_catalogForTitles = m_doc->catalog();
  if (m_catalogForTitles)
    connect(m_catalogForTitles, &DataCatalog::changed, this, [this]() {
      for (auto it = m_pageOfAsset.constBegin(); it != m_pageOfAsset.constEnd(); ++it)
        updateTabTitle(it.key());
    });
}

void DataPreviewTabs::setTaskService(PaleoTaskService *svc)
{
  m_taskSvc = svc;
  if (m_doc)
    m_doc->setTaskService(svc);
}

void DataPreviewTabs::setProject(QgsProject *project)
{
  m_project = project;
}

void DataPreviewTabs::openSurveyArea()
{
  const QString key = QStringLiteral("survey_area");
  if (QWidget *existing = m_pageOfAsset.value(key))
  {
    m_tabs->setCurrentIndex(m_tabs->indexOf(existing));
    if (auto *cv = existing->findChild<QgsMapCanvas *>())
    {
      cv->zoomToFullExtent();
      cv->refresh();
    }
    return;
  }

  QWidget *page = new QWidget(this);
  auto *pageLay = new QVBoxLayout(page);
  pageLay->setContentsMargins(0, 0, 0, 0);
  pageLay->setSpacing(0);

  QWidget *content = buildSurveyAreaContent(page);
  pageLay->addWidget(content ? content : stateLabel(tr("无法生成测区地图"), page), 1);

  const int idx = m_tabs->addTab(page, PaleoIcons::qgisTheme(QStringLiteral("mIconPolygonLayer.svg")), tr("测区全景地图"));
  m_pageOfAsset.insert(key, page);
  m_tabs->setVisible(true);
  m_emptyLabel->setVisible(false);
  m_tabs->setCurrentIndex(idx);
}

QWidget *DataPreviewTabs::buildSurveyAreaContent(QWidget *page)
{
  auto *w = new QWidget(page);
  auto *lay = new QVBoxLayout(w);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(0);

  // 顶部快捷控制条（遵照 DESIGN.md 设计规范）
  auto *topBar = new QWidget(w);
  auto *tbLay = new QHBoxLayout(topBar);
  tbLay->setContentsMargins(8, 4, 8, 4);
  tbLay->setSpacing(6);
  topBar->setStyleSheet(QStringLiteral("background: #EDF1F5; border-bottom: 1px solid #DFE5EC;"));

  const QString btnStyle = QStringLiteral(
      "QToolButton { background: #FFFFFF; border: 1px solid #DFE5EC; border-radius: 4px; "
      "padding: 4px 8px; font-size: 8.5pt; color: #24303E; }"
      "QToolButton:hover { background: #E2E8F0; border-color: #9AA7B4; }"
      "QToolButton:pressed { background: #DFE5EC; }"
      "QToolButton:checked { background: #E1EFFE; border-color: #1B73D0; color: #1B73D0; font-weight: 500; }");

  auto *lblTitle = new QLabel(tr("测区全景地图 (QGIS 画布)"), topBar);
  lblTitle->setStyleSheet(QStringLiteral("font-weight: 600; color: #1B73D0; font-size: 9pt;"));
  tbLay->addWidget(lblTitle);

  tbLay->addSpacing(8);

  auto *btnFull = new QToolButton(topBar);
  btnFull->setObjectName(QStringLiteral("btnSurveyFullExtent"));
  btnFull->setText(tr("全图"));
  btnFull->setToolTip(tr("缩放到测区全景范围"));
  btnFull->setStyleSheet(btnStyle);
  btnFull->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionZoomFullExtent.svg")));
  btnFull->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  tbLay->addWidget(btnFull);

  auto *btnIn = new QToolButton(topBar);
  btnIn->setObjectName(QStringLiteral("btnSurveyZoomIn"));
  btnIn->setText(tr("放大"));
  btnIn->setToolTip(tr("放大地图 (支持鼠标滚轮缩放)"));
  btnIn->setStyleSheet(btnStyle);
  btnIn->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionZoomIn.svg")));
  btnIn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  tbLay->addWidget(btnIn);

  auto *btnOut = new QToolButton(topBar);
  btnOut->setObjectName(QStringLiteral("btnSurveyZoomOut"));
  btnOut->setText(tr("缩小"));
  btnOut->setToolTip(tr("缩小地图 (支持鼠标滚轮缩放)"));
  btnOut->setStyleSheet(btnStyle);
  btnOut->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionZoomOut.svg")));
  btnOut->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  tbLay->addWidget(btnOut);

  auto *btnPan = new QToolButton(topBar);
  btnPan->setObjectName(QStringLiteral("btnSurveyPan"));
  btnPan->setText(tr("漫游"));
  btnPan->setToolTip(tr("按住鼠标左键拖拽平移地图"));
  btnPan->setStyleSheet(btnStyle);
  btnPan->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionPan.svg")));
  btnPan->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  tbLay->addWidget(btnPan);

  // 1. 工区概况图应该有比例尺，指南针，工区范围等显示
  auto *btnBoundary = new QToolButton(topBar);
  btnBoundary->setObjectName(QStringLiteral("btnToggleSurveyBoundary"));
  btnBoundary->setText(tr("工区范围"));
  btnBoundary->setToolTip(tr("显示/隐藏工区范围边界多边形"));
  btnBoundary->setCheckable(true);
  btnBoundary->setChecked(true);
  btnBoundary->setStyleSheet(btnStyle);
  btnBoundary->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mIconPolygonLayer.svg")));
  btnBoundary->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  tbLay->addWidget(btnBoundary);

  auto *btnScaleBar = new QToolButton(topBar);
  btnScaleBar->setObjectName(QStringLiteral("btnToggleScaleBar"));
  btnScaleBar->setText(tr("比例尺"));
  btnScaleBar->setToolTip(tr("开启/关闭左下角动态比例尺"));
  btnScaleBar->setCheckable(true);
  btnScaleBar->setChecked(true);
  btnScaleBar->setStyleSheet(btnStyle);
  btnScaleBar->setToolButtonStyle(Qt::ToolButtonTextOnly);
  tbLay->addWidget(btnScaleBar);

  auto *btnNorthArrow = new QToolButton(topBar);
  btnNorthArrow->setObjectName(QStringLiteral("btnToggleNorthArrow"));
  btnNorthArrow->setText(tr("指南针"));
  btnNorthArrow->setToolTip(tr("开启/关闭右上角指北针"));
  btnNorthArrow->setCheckable(true);
  btnNorthArrow->setChecked(true);
  btnNorthArrow->setStyleSheet(btnStyle);
  btnNorthArrow->setToolButtonStyle(Qt::ToolButtonTextOnly);
  tbLay->addWidget(btnNorthArrow);

  auto *btnGrid = new QToolButton(topBar);
  btnGrid->setObjectName(QStringLiteral("btnToggleGrid"));
  btnGrid->setText(tr("网格"));
  btnGrid->setToolTip(tr("开启/关闭坐标方格网"));
  btnGrid->setCheckable(true);
  btnGrid->setChecked(false);
  btnGrid->setStyleSheet(btnStyle);
  btnGrid->setToolButtonStyle(Qt::ToolButtonTextOnly);
  tbLay->addWidget(btnGrid);

  auto *btnSwitchMain = new QToolButton(topBar);
  btnSwitchMain->setObjectName(QStringLiteral("btnSwitchToMainCanvas"));
  btnSwitchMain->setText(tr("在主画布中查看"));
  btnSwitchMain->setToolTip(tr("切换到主工作区全屏 QGIS 地图画布"));
  btnSwitchMain->setStyleSheet(btnStyle);
  btnSwitchMain->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionMapSettings.svg")));
  btnSwitchMain->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  tbLay->addWidget(btnSwitchMain);

  tbLay->addStretch(1);

  // QGIS 地图画布
  auto *canvas = new QgsMapCanvas(w);
  canvas->setObjectName(QStringLiteral("surveyMapCanvas"));
  canvas->enableAntiAliasing(true);
  canvas->setCanvasColor(Qt::white);

  QgsProject *proj = m_project ? m_project.data() : QgsProject::instance();
  if (proj)
  {
    canvas->setProject(proj);
    canvas->setDestinationCrs(proj->crs());
    // 绑定项目图层树：所有井位、地震测线、边界、层位图层自动同步渲染
    new QgsLayerTreeMapCanvasBridge(proj->layerTreeRoot(), canvas, canvas);
  }

  // 挂载装饰管理器：比例尺 + 指南针 + 网格
  auto *decorMgr = new PaleoDecorationManager(canvas, canvas);
  decorMgr->setObjectName(QStringLiteral("surveyAreaDecorManager"));
  decorMgr->setScaleBarEnabled(true);
  decorMgr->setNorthArrowEnabled(true);

  // 构建工区边界 (QgsRubberBand)
  CatalogEntity survey;
  if (m_doc && m_doc->catalog())
  {
    const auto surveys = m_doc->catalog()->entities(QStringLiteral("seismic_survey"));
    if (!surveys.isEmpty())
      survey = surveys.first();
  }

  QgsGeometry surveyGeom;
  if (survey.corners.size() >= 3)
  {
    QgsPolylineXY ring;
    for (const auto &c : survey.corners)
      ring.append(QgsPointXY(c.first, c.second));
    if (!ring.isEmpty() && ring.first() != ring.last())
      ring.append(ring.first());
    surveyGeom = QgsGeometry::fromPolygonXY(QgsPolygonXY{ring});
  }
  else if (survey.inlineMax > survey.inlineMin && survey.xlineMax > survey.xlineMin)
  {
    surveyGeom = QgsGeometry::fromRect(QgsRectangle(survey.inlineMin, survey.xlineMin,
                                                    survey.inlineMax, survey.xlineMax));
  }
  else if (proj && !proj->mapLayers().isEmpty())
  {
    QgsRectangle ext;
    for (auto *layer : proj->mapLayers())
    {
      if (layer && !layer->extent().isEmpty())
        ext.combineExtentWith(layer->extent());
    }
    if (!ext.isEmpty())
      surveyGeom = QgsGeometry::fromRect(ext);
  }
  if (surveyGeom.isNull())
  {
    // 默认局部测区范围 (10 km × 10 km)
    surveyGeom = QgsGeometry::fromRect(QgsRectangle(0, 0, 10000, 10000));
  }

  auto *boundaryBand = new QgsRubberBand(canvas, Qgis::GeometryType::Polygon);
  boundaryBand->setParent(canvas);
  boundaryBand->setObjectName(QStringLiteral("surveyAreaRubberBand"));
  if (!surveyGeom.isNull() && surveyGeom.isGeosValid())
  {
    boundaryBand->setToGeometry(surveyGeom, nullptr);
  }
  boundaryBand->setColor(QColor(27, 115, 208, 16)); // #1B73D0 浅蓝半透明填充
  boundaryBand->setStrokeColor(QColor(QStringLiteral("#1B73D0"))); // 边界线
  boundaryBand->setWidth(2);
  boundaryBand->setLineStyle(Qt::DashLine);
  boundaryBand->show();

  // 工区范围与坐标系说明标签
  QString extentStr;
  if (!surveyGeom.isNull() && !surveyGeom.boundingBox().isEmpty())
  {
    const QgsRectangle box = surveyGeom.boundingBox();
    const double wKm = box.width() / 1000.0;
    const double hKm = box.height() / 1000.0;
    extentStr = tr("工区范围: %1 km × %2 km · 局部工程坐标系统 (米)")
                    .arg(QString::number(wKm, 'f', 1), QString::number(hKm, 'f', 1));
  }
  else
  {
    extentStr = tr("局部工程坐标系统 (米)");
  }
  auto *crsLabel = new QLabel(extentStr, topBar);
  crsLabel->setObjectName(QStringLiteral("surveyAreaExtentLabel"));
  crsLabel->setStyleSheet(QStringLiteral("color: #5D6E80; font-size: 8pt; font-family: 'JetBrains Mono', 'Noto Sans SC';"));
  tbLay->addWidget(crsLabel);

  lay->addWidget(topBar);
  lay->addWidget(canvas, 1);

  // 设置默认漫游工具
  auto *panTool = new QgsMapToolPan(canvas);
  canvas->setMapTool(panTool);

  auto zoomFull = [canvas, surveyGeom]() {
    if (!surveyGeom.isNull() && !surveyGeom.boundingBox().isEmpty())
    {
      QgsRectangle ext = surveyGeom.boundingBox();
      ext.grow(qMax(ext.width(), ext.height()) * 0.08);
      canvas->setExtent(ext);
      canvas->refresh();
    }
    else
    {
      canvas->zoomToFullExtent();
      canvas->refresh();
    }
  };

  connect(btnFull, &QToolButton::clicked, canvas, zoomFull);
  connect(btnIn, &QToolButton::clicked, canvas, &QgsMapCanvas::zoomIn);
  connect(btnOut, &QToolButton::clicked, canvas, &QgsMapCanvas::zoomOut);
  connect(btnPan, &QToolButton::clicked, canvas, [canvas, panTool]() {
    canvas->setMapTool(panTool);
  });
  connect(btnBoundary, &QToolButton::toggled, canvas, [boundaryBand, canvas](bool checked) {
    boundaryBand->setVisible(checked);
    canvas->refresh();
  });
  connect(btnScaleBar, &QToolButton::toggled, canvas, [decorMgr, canvas](bool checked) {
    decorMgr->setScaleBarEnabled(checked);
    canvas->refresh();
  });
  connect(btnNorthArrow, &QToolButton::toggled, canvas, [decorMgr, canvas](bool checked) {
    decorMgr->setNorthArrowEnabled(checked);
    canvas->refresh();
  });
  connect(btnGrid, &QToolButton::toggled, canvas, [decorMgr, canvas](bool checked) {
    decorMgr->setGridEnabled(checked);
    canvas->refresh();
  });
  connect(btnSwitchMain, &QToolButton::clicked, this, &DataPreviewTabs::requestShowOnMainCanvas);

  // 延迟自适应全图（等几何尺寸就绪）
  QTimer::singleShot(100, canvas, zoomFull);

  return w;
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
  // D1：标签关掉即释放该资产的索引缓存（持有文件句柄级状态）与世代号；
  // 进行中的解码任务请求取消——结果没人等了。
  if (m_doc)
    m_doc->releaseSection(assetId);
  m_pendingSection.remove(assetId);
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

bool DataPreviewTabs::relocateMissingSourceWith(const QString &assetId,
                                                const QString &versionId,
                                                const QString &pickedPath)
{
  // wave4：把死胡同接到 relocateVersionSource——内容一致才重接（服务层拒解
  // SHA 不一致的候选文件，不静默换源）。失败保留「找不到源文件」状态与按钮，
  // 错误就地可见，可换文件再试；成功清掉本会话的 SHA 已验缓存（新路径要在
  // 重建时重新过 §3 校验门）并重建标签加载真预览。
  if (!m_doc || assetId.isEmpty())
    return false;
  QString err;
  const QString newVer = m_doc->relocateVersionSource(versionId, pickedPath, &err);
  if (newVer.isEmpty())
  {
    QWidget *page = m_pageOfAsset.value(assetId);
    if (auto *lbl = page ? page->findChild<QLabel *>(QStringLiteral("stateText")) : nullptr)
      lbl->setText(tr("找不到源文件\n重新定位失败：%1").arg(err));
    return false;
  }
  if (m_doc)
    m_doc->resetSha(assetId);
  rebuildAssetTab(assetId);
  return true;
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
      m_doc ? m_doc->catalog()->assetById(assetId).displayName : assetId;
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
  if (!m_doc || assetId.isEmpty())
    return;
  // §4：well_head 标签的选中井在地图上高亮。多井标签只报该标签已选中的井
  // ——没有选中就不报，绝不拿第一条链接糊弄（m_wellEntityOfAsset 在
  // 唯一已决井/下拉框选择时写入）。
  const CatalogAsset asset = m_doc->catalog()->assetById(assetId);
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
      m_doc ? m_doc->catalog()->assetById(assetId).displayName : assetId;
  if (title.isEmpty())
    title = assetId;
  const QString suffix = m_titleSuffixOfAsset.value(assetId);
  if (!suffix.isEmpty())
    title += QStringLiteral(" · ") + suffix;
  // B 包 staleness-lite：资产当前版本被标 stale（上游 sha 失配/被取代）→
  // 标题带「过时」徽标——下游产物过期在数据页如实可见。
  if (m_doc && m_doc->catalog()->currentVersion(assetId)
                   .extra.value(QStringLiteral("stale"))
                   .toBool())
    title += QStringLiteral(" · ") + tr("过时");
  m_tabs->setTabText(idx, title);
}

void DataPreviewTabs::rebuildAssetTab(const QString &assetId)
{
  QWidget *page = m_pageOfAsset.value(assetId);
  if (!page || !m_doc)
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
  const QString name = m_doc->catalog()->assetById(assetId).displayName;
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
  if (!m_doc || assetId.isEmpty())
    return;
  if (QWidget *existing = m_pageOfAsset.value(assetId))
  {
    m_tabs->setCurrentIndex(m_tabs->indexOf(existing)); // 重选聚焦（§4）
    focusWellIfNeeded(assetId, existing);
    return;
  }

  const QString displayName = m_doc->catalog()->assetById(assetId).displayName;
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

void DataPreviewTabs::openAssetForWell(const QString &assetId, const QString &wellId)
{
  if (!m_doc || assetId.isEmpty())
    return;
  if (!wellId.isEmpty())
  {
    m_wellEntityOfAsset[assetId] = wellId;
    if (m_doc->catalog())
      m_titleSuffixOfAsset[assetId] = m_doc->catalog()->entityById(wellId).name;
  }
  openAsset(assetId);
  QWidget *page = m_pageOfAsset.value(assetId);
  if (page)
  {
    if (auto *combo = page->findChild<QComboBox *>(QStringLiteral("wellCombo")))
    {
      const int idx = combo->findData(wellId);
      if (idx >= 0 && combo->currentIndex() != idx)
        combo->setCurrentIndex(idx);
    }
    updateTabTitle(assetId);
    focusWellIfNeeded(assetId, page);
  }
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
  if (auto *modeTabs = page->findChild<QTabWidget *>(QStringLiteral("seismicSubTabs")))
    modeTabs->setCurrentIndex(0); // 聚焦到二维测线剖面页签
  Q_UNUSED(timeMs); // 目标时间的标注由剖面自身的 D61 标定线承担（§4/阶段B）
}

QWidget *DataPreviewTabs::buildContent(const QString &assetId, QWidget *page)
{
  Q_UNUSED(page);
  DataCatalog *cat = m_doc->catalog();
  const CatalogAsset asset = cat->assetById(assetId);
  if (asset.id.isEmpty())
    return nullptr;
  const CatalogVersion v = cat->currentVersion(assetId);
  CatalogVersion sourceVersion = v; // abs 实际对应的版本（文档标签锚回 RAW 原件）
  QString abs = m_doc->absolutePathForVersion(v);
  // 文档资产：RAW 原件是规范来源——currentVersion 可能已指向 DERIVED
  // PDF 转换件，缺失检查与「用系统程序打开」必须锚在原件上。
  if (asset.type == QLatin1String("document"))
    for (const CatalogVersion &cv : cat->versionsForAsset(assetId))
      if (cv.stage == QLatin1String("RAW"))
      {
        sourceVersion = cv;
        abs = m_doc->absolutePathForVersion(cv);
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

  // 外链/受管缺失态（§4：「找不到源文件」+路径）。外链版本（wave4）多给一个
  // 「重新定位文件…」出口——服务层流式 SHA-256 复验，内容一致才重接，不一致
  // 如实拒绝；受管文件缺失不是这条恢复路径能解的，不给按钮、只留文案。
  if (abs.isEmpty() || !QFile::exists(abs))
  {
    lay->addWidget(stateLabel(tr("找不到源文件\n%1").arg(abs.isEmpty() ? v.path : abs), host), 1);
    if (!sourceVersion.managed && m_doc)
    {
      auto *btn = new QPushButton(tr("重新定位文件…"), host);
      btn->setObjectName(QStringLiteral("relocateBtn"));
      const QString versionId = sourceVersion.id;
      connect(btn, &QPushButton::clicked, host, [this, assetId, versionId] {
        const QString picked = QFileDialog::getOpenFileName(
            this, tr("重新定位源文件"), QString(), QString());
        if (!picked.isEmpty())
          relocateMissingSourceWith(assetId, versionId, picked);
      });
      lay->addWidget(btn, 0, Qt::AlignHCenter);
    }
    return host;
  }

  // 外链完整性（§3）：入库时留过 SHA-256 的源文件被改过就不再解码——
  // 正文如实写「源文件与入库时的 SHA-256 不一致」。
  // D1：地震资产接了任务服务时把这道哈希移交异步解码任务——体量大不该堵
  // 住建标签；其它资产类型文件小，保留同步门（会话已验过的资产直接跳过）。
  // 托管/无指纹/本会话已验的短路、失配后的下游标过时都在门面里。
  const bool deferShaToTask =
      m_doc->taskService() && asset.type == QLatin1String("seismic");
  if (!deferShaToTask)
  {
    QString verr;
    if (!m_doc->verifyExternalSha(assetId, sourceVersion, &verr))
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
    if (!m_doc->lasAt(abs, &names, &curves, &perr))
    {
      lay->addWidget(failureState(assetId, perr, host), 1);
      return host;
    }
    auto *singlePage = new QWidget(host);
    auto *singleLay = new QVBoxLayout(singlePage);
    singleLay->setContentsMargins(0, 0, 0, 0);
    singleLay->setSpacing(6);

    auto *panel = new CurvePanel(singlePage);
    panel->setObjectName(QStringLiteral("curvePanel"));
    panel->setEmptyText(tr("这条曲线没有有效样点")); // §4：整条 -99999 → 不绘制

    // 1. 顶部控制栏（主选曲线 + 预设 + 缩放控制）
    auto *topBar = new QWidget(singlePage);
    auto *topLay = new QHBoxLayout(topBar);
    topLay->setContentsMargins(0, 0, 0, 0);
    topLay->setSpacing(6);

    auto *combo = new QComboBox(topBar);
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

    // 深度缩放按钮组
    auto *btnZoomOut = new QToolButton(topBar);
    btnZoomOut->setText(QStringLiteral("−"));
    btnZoomOut->setToolTip(tr("缩小深度 (Ctrl+滚轮向下)"));
    btnZoomOut->setStyleSheet(QStringLiteral("QToolButton { font-weight: bold; min-width: 24px; min-height: 22px; }"));

    auto *lblZoom = new QLabel(QStringLiteral("100%"), topBar);
    lblZoom->setFont(monoFont());
    lblZoom->setStyleSheet(QStringLiteral("color: #5D6E80; min-width: 44px;"));
    lblZoom->setAlignment(Qt::AlignCenter);

    auto *btnZoomIn = new QToolButton(topBar);
    btnZoomIn->setText(QStringLiteral("+"));
    btnZoomIn->setToolTip(tr("放大深度 (Ctrl+滚轮向上)"));
    btnZoomIn->setStyleSheet(QStringLiteral("QToolButton { font-weight: bold; min-width: 24px; min-height: 22px; }"));

    auto *btnZoomReset = new QToolButton(topBar);
    btnZoomReset->setText(tr("1:1 适应"));
    btnZoomReset->setToolTip(tr("重置为全井深 (双击图道重置)"));
    btnZoomReset->setStyleSheet(QStringLiteral("QToolButton { min-height: 22px; padding: 0 6px; }"));

    // 曲线快速预设按钮
    auto *btnSelectDefault = new QToolButton(topBar);
    btnSelectDefault->setText(tr("常规(GR/AC/DEN)"));
    btnSelectDefault->setToolTip(tr("显示三孔隙/常规测井曲线"));
    btnSelectDefault->setStyleSheet(QStringLiteral("QToolButton { min-height: 22px; padding: 0 6px; }"));

    auto *btnSelectAll = new QToolButton(topBar);
    btnSelectAll->setText(tr("全选"));
    btnSelectAll->setToolTip(tr("同时显示所有曲线"));
    btnSelectAll->setStyleSheet(QStringLiteral("QToolButton { min-height: 22px; padding: 0 6px; }"));

    auto *btnClear = new QToolButton(topBar);
    btnClear->setText(tr("仅主选"));
    btnClear->setToolTip(tr("仅显示当前下拉框选中的单根曲线"));
    btnClear->setStyleSheet(QStringLiteral("QToolButton { min-height: 22px; padding: 0 6px; }"));

    topLay->addWidget(caption8(tr("主选曲线:"), topBar));
    topLay->addWidget(combo);
    topLay->addSpacing(8);
    topLay->addWidget(btnSelectDefault);
    topLay->addWidget(btnSelectAll);
    topLay->addWidget(btnClear);
    topLay->addStretch(1);
    topLay->addWidget(caption8(tr("深度缩放:"), topBar));
    topLay->addWidget(btnZoomOut);
    topLay->addWidget(lblZoom);
    topLay->addWidget(btnZoomIn);
    topLay->addWidget(btnZoomReset);

    // 初始显示曲线集合（默认优先显示 GR/AC/DEN 常规三孔隙）
    QSet<QString> defaultShown;
    if (combo->findText(QStringLiteral("GR")) >= 0 && combo->itemData(combo->findText(QStringLiteral("GR"))).toInt() > 0)
      defaultShown.insert(QStringLiteral("GR"));
    if (combo->findText(QStringLiteral("AC")) >= 0 && combo->itemData(combo->findText(QStringLiteral("AC"))).toInt() > 0)
      defaultShown.insert(QStringLiteral("AC"));
    if (combo->findText(QStringLiteral("DEN")) >= 0 && combo->itemData(combo->findText(QStringLiteral("DEN"))).toInt() > 0)
      defaultShown.insert(QStringLiteral("DEN"));
    if (defaultShown.isEmpty() && names.size() > 1)
      defaultShown.insert(names.at(1));

    // 添加所有曲线数据至面板
    for (int i = 1; i < names.size(); ++i)
    {
      const QString &cname = names.at(i);
      const QColor col = pickCurveColor(cname, i - 1);
      panel->addCurve(cname, curves.at(i).unit, curves.at(i).values,
                      curves.at(0).values, col, defaultShown.contains(cname));
    }

    // 2. 曲线多选 Chips 栏（横向滚动条，支持单击自由切换各曲线可见性）
    auto *chipScroll = new QScrollArea(singlePage);
    chipScroll->setWidgetResizable(true);
    chipScroll->setFixedHeight(32);
    chipScroll->setFrameShape(QFrame::NoFrame);
    chipScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    chipScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    chipScroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; border: none; }"));

    auto *chipContainer = new QWidget(chipScroll);
    chipContainer->setStyleSheet(QStringLiteral("background: transparent;"));
    auto *chipLay = new QHBoxLayout(chipContainer);
    chipLay->setContentsMargins(0, 0, 0, 0);
    chipLay->setSpacing(6);
    chipLay->addWidget(caption8(tr("多曲线叠合:"), chipContainer));

    auto chipMap = std::make_shared<QHash<QString, QToolButton *>>();
    for (int i = 1; i < names.size(); ++i)
    {
      const QString &cname = names.at(i);
      const QColor col = pickCurveColor(cname, i - 1);
      auto *chip = new QToolButton(chipContainer);
      chip->setText(cname);
      chip->setCheckable(true);
      const bool isChecked = defaultShown.contains(cname);
      chip->setChecked(isChecked);
      chip->setToolTip(QStringLiteral("%1 (%2)").arg(cname, curves.at(i).unit));

      const QString styleOn = QStringLiteral(
          "QToolButton { background: #FFFFFF; border: 1.5px solid %1; border-radius: 9px; "
          "color: %1; font-weight: bold; padding: 1px 7px; font-size: 8pt; }"
          "QToolButton:hover { background: #EDF1F5; }").arg(col.name());
      const QString styleOff = QStringLiteral(
          "QToolButton { background: #FFFFFF; border: 1px solid #DFE5EC; border-radius: 9px; "
          "color: #5D6E80; padding: 1px 7px; font-size: 8pt; }"
          "QToolButton:hover { background: #EDF1F5; border-color: #9AA7B4; }");

      chip->setStyleSheet(isChecked ? styleOn : styleOff);

      connect(chip, &QToolButton::toggled, host, [panel, chip, cname, styleOn, styleOff](bool on) {
        panel->setCurveVisible(cname, on);
        chip->setStyleSheet(on ? styleOn : styleOff);
      });

      (*chipMap)[cname] = chip;
      chipLay->addWidget(chip);
    }
    chipLay->addStretch(1);
    chipScroll->setWidget(chipContainer);

    // 缩放接线
    connect(btnZoomIn, &QToolButton::clicked, host, [panel]() { panel->zoomIn(); });
    connect(btnZoomOut, &QToolButton::clicked, host, [panel]() { panel->zoomOut(); });
    connect(btnZoomReset, &QToolButton::clicked, host, [panel]() { panel->resetZoom(); });
    panel->onZoomChanged = [lblZoom](double z) {
      lblZoom->setText(QStringLiteral("%1%").arg(qRound(z * 100)));
    };

    // 主选下拉框变更时，自动确保该曲线被勾选显示
    connect(combo, &QComboBox::currentIndexChanged, host, [panel, combo, names, chipMap]() {
      const int ci = combo->currentData().toInt();
      if (ci <= 0 || ci >= names.size())
        return;
      const QString selName = names.at(ci);
      if (chipMap->contains(selName))
      {
        auto *btn = chipMap->value(selName);
        if (!btn->isChecked())
          btn->setChecked(true);
      }
    });

    // 预设按钮事件
    connect(btnSelectAll, &QToolButton::clicked, host, [chipMap]() {
      for (auto *b : *chipMap)
        if (!b->isChecked()) b->setChecked(true);
    });

    connect(btnSelectDefault, &QToolButton::clicked, host, [chipMap, defaultShown]() {
      for (auto it = chipMap->begin(); it != chipMap->end(); ++it)
      {
        const bool on = defaultShown.contains(it.key());
        if (it.value()->isChecked() != on)
          it.value()->setChecked(on);
      }
    });

    connect(btnClear, &QToolButton::clicked, host, [combo, names, chipMap]() {
      const int ci = combo->currentData().toInt();
      const QString activeName = (ci > 0 && ci < names.size()) ? names.at(ci) : QString();
      for (auto it = chipMap->begin(); it != chipMap->end(); ++it)
      {
        const bool on = (it.key() == activeName);
        if (it.value()->isChecked() != on)
          it.value()->setChecked(on);
      }
    });

    singleLay->addWidget(topBar);
    singleLay->addWidget(chipScroll);
    singleLay->addWidget(panel, 1);

    // ResFormStar 多井道综合柱状图总装
    auto *compPanel = new WellComposite::WellCompositePanel(host);
    compPanel->setObjectName(QStringLiteral("wellCompositePanel"));

    QVector<WellComposite::CurveData> compCurves;
    if (!curves.isEmpty())
    {
      const auto &depList = curves.at(0).values;
      QVector<float> depVec;
      depVec.reserve(depList.size());
      for (double d : depList)
        depVec.append(static_cast<float>(d));

      for (int i = 1; i < names.size(); ++i)
      {
        const auto &src = curves.at(i);
        WellComposite::CurveData cd;
        cd.name = names.at(i);
        cd.unit = src.unit;
        cd.color = pickCurveColor(cd.name, i - 1);
        cd.depths = depVec;
        cd.values.reserve(src.values.size());
        float valMin = 1e9f, valMax = -1e9f;
        for (double v : src.values)
        {
          if (v <= -999.0 || v >= 99999.0)
          {
            cd.values.append(-9999.0f);
            continue;
          }
          float fv = static_cast<float>(v);
          cd.values.append(fv);
          if (fv < valMin) valMin = fv;
          if (fv > valMax) valMax = fv;
        }
        if (valMin < valMax)
        {
          cd.minScale = valMin;
          cd.maxScale = valMax;
        }
        else
        {
          cd.minScale = 0.0f;
          cd.maxScale = 100.0f;
        }
        compCurves.append(cd);
      }
    }

    // 查询该井是否有关联分层数据 (DC.dat)
    QVector<WellComposite::FormationInterval> formationIntervals;
    if (!linkedWell.isEmpty())
    {
      static const QVector<QColor> kFormColors = {
          QColor(QStringLiteral("#FFE082")), QColor(QStringLiteral("#FFF59D")),
          QColor(QStringLiteral("#C8E6C9")), QColor(QStringLiteral("#A5D6A7")),
          QColor(QStringLiteral("#80CBC4")), QColor(QStringLiteral("#80DEEA")),
          QColor(QStringLiteral("#90CAF9")), QColor(QStringLiteral("#B39DDB"))};

      const auto wLinks = cat->linksForEntity(linkedWell);
      for (const auto &lk : wLinks)
      {
        if (lk.role == QLatin1String("tops"))
        {
          CatalogAsset topsAsset = cat->assetById(lk.assetId);
          CatalogVersion topsVer = cat->currentVersion(lk.assetId);
          QString topsPath = m_doc->absolutePathForVersion(topsVer);
          {
            QVector<WellTopRecord> tops;
            if (!topsPath.isEmpty() && m_doc->wellTopsAt(topsPath, &tops))
            {
              const QString normWell = DataCatalog::normalizeWellName(wells.isEmpty() ? QString() : wells.front().second);
              QVector<WellTopRecord> wellTops;
              for (const auto &tr : tops)
              {
                if (normWell.isEmpty() || DataCatalog::normalizeWellName(tr.wellName) == normWell)
                  wellTops.append(tr);
              }
              std::sort(wellTops.begin(), wellTops.end(), [](const WellTopRecord &a, const WellTopRecord &b) {
                return a.md < b.md;
              });
              for (int ti = 0; ti < wellTops.size(); ++ti)
              {
                WellComposite::FormationInterval fi;
                fi.name = wellTops.at(ti).topName;
                fi.topDepth = static_cast<float>(wellTops.at(ti).md);
                fi.bottomDepth = static_cast<float>((ti + 1 < wellTops.size()) ? wellTops.at(ti + 1).md : (wellTops.at(ti).md + 50.0));
                fi.color = kFormColors.at(ti % kFormColors.size());
                formationIntervals.append(fi);
              }
            }
          }
          break;
        }
      }
    }

    const QString wellTitle = wells.isEmpty() ? asset.displayName : wells.front().second;
    compPanel->loadLasCurves(wellTitle, compCurves, formationIntervals);

    // 视图模式切换条与堆叠容器
    auto *viewSwitchBar = new QWidget(host);
    auto *switchLay = new QHBoxLayout(viewSwitchBar);
    switchLay->setContentsMargins(0, 0, 0, 0);
    switchLay->setSpacing(8);

    auto *btnResForm = new QToolButton(viewSwitchBar);
    btnResForm->setObjectName(QStringLiteral("btnResFormView"));
    btnResForm->setText(tr("ResFormStar 综合多井道柱状图 (推荐)"));
    btnResForm->setCheckable(true);
    btnResForm->setChecked(true);
    btnResForm->setStyleSheet(QStringLiteral(
        "QToolButton { background: #1B73D0; color: #FFFFFF; font-weight: bold; "
        "border-radius: 4px; padding: 3px 10px; font-size: 8.5pt; }"
        "QToolButton:hover { background: #15589E; }"));

    auto *btnSingle = new QToolButton(viewSwitchBar);
    btnSingle->setObjectName(QStringLiteral("btnSingleView"));
    btnSingle->setText(tr("单道叠合检视"));
    btnSingle->setCheckable(true);
    btnSingle->setChecked(false);
    btnSingle->setStyleSheet(QStringLiteral(
        "QToolButton { background: #FFFFFF; border: 1px solid #DFE5EC; "
        "border-radius: 4px; padding: 3px 10px; font-size: 8.5pt; color: #24303E; }"
        "QToolButton:hover { background: #EDF1F5; }"));

    auto *viewStack = new QStackedWidget(host);
    viewStack->setObjectName(QStringLiteral("logViewStack"));
    viewStack->addWidget(compPanel);   // 0: ResForm 多井道柱状图（默认）
    viewStack->addWidget(singlePage);  // 1: 单道快速检视

    connect(btnResForm, &QToolButton::clicked, host, [btnResForm, btnSingle, viewStack] {
      btnResForm->setChecked(true);
      btnSingle->setChecked(false);
      btnResForm->setStyleSheet(QStringLiteral(
          "QToolButton { background: #1B73D0; color: #FFFFFF; font-weight: bold; "
          "border-radius: 4px; padding: 3px 10px; font-size: 8.5pt; }"));
      btnSingle->setStyleSheet(QStringLiteral(
          "QToolButton { background: #FFFFFF; border: 1px solid #DFE5EC; "
          "border-radius: 4px; padding: 3px 10px; font-size: 8.5pt; color: #24303E; }"));
      viewStack->setCurrentIndex(0);
    });

    connect(btnSingle, &QToolButton::clicked, host, [btnResForm, btnSingle, viewStack] {
      btnSingle->setChecked(true);
      btnResForm->setChecked(false);
      btnSingle->setStyleSheet(QStringLiteral(
          "QToolButton { background: #1B73D0; color: #FFFFFF; font-weight: bold; "
          "border-radius: 4px; padding: 3px 10px; font-size: 8.5pt; }"));
      btnResForm->setStyleSheet(QStringLiteral(
          "QToolButton { background: #FFFFFF; border: 1px solid #DFE5EC; "
          "border-radius: 4px; padding: 3px 10px; font-size: 8.5pt; color: #24303E; }"));
      viewStack->setCurrentIndex(1);
    });

    switchLay->addWidget(caption8(tr("呈现模式:"), viewSwitchBar));
    switchLay->addWidget(btnResForm);
    switchLay->addWidget(btnSingle);
    switchLay->addStretch(1);

    lay->addWidget(viewSwitchBar);
    lay->addWidget(viewStack, 1);
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

    // ---- 标定井：catalog 序第一口有目标层位分层的井 + 主 time_depth 表插值
    // + 初始测线内插——派生量全在数据门面一次算好（§3/阶段 B 口径不变）。----
    const PreviewDocService::TieMarker tie = m_doc->seismicTieMarker(assetId);
    const bool haveTieTop = tie.haveTop;
    const QString tieWellName = tie.wellName;
    const int initialInline = tie.initialInline;

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
      if (tie.ok)
        tieCaption->setText(
            tr("%1 %2 · %3 ms").arg(tieWellName, tie.horizon).arg(tie.timeMs, 0, 'f', 1));
      else
        tieCaption->setText(
            tr("%1 %2 · %3").arg(tieWellName, tie.horizon, tie.statusText));
    }
    const auto decode = [this, assetId, abs, v, mode, no, panel, tieCaption]() {
      panel->clearImage(); // 换测线先清掉上一张剖面（§4）
      const QString idxTip = tr("正在建立道索引");
      mode->setEnabled(false);
      no->setEnabled(false);
      mode->setToolTip(idxTip);
      no->setToolTip(idxTip);
      const bool isInline = mode->currentData().toString() == QLatin1String("inline");
      const int lineNo = no->value();

      // D1/T23：读者缓存/世代号/协作取消/SHA 复验/下游标过时全在门面——
      // 这里只挂起控件组（结果信号回来按 assetId 找回控件贴图）。
      SectionPending pend;
      pend.panel = panel;
      pend.mode = mode;
      pend.spin = no;
      pend.tieCaption = tieCaption;
      pend.tieText = tieCaption->text();
      const PreviewDocService::TieMarker tieNow = m_doc->seismicTieMarker(assetId);
      pend.hasTie = tieNow.haveTop && tieNow.ok;
      pend.tieMs = tieNow.timeMs;
      m_pendingSection[assetId] = pend;
      m_doc->requestSection(assetId, v.id, abs, v.managed, v.sha256,
                            isInline, lineNo);
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
    auto *modeTabs = new QTabWidget(host);
    modeTabs->setObjectName(QStringLiteral("seismicSubTabs"));
    modeTabs->setStyleSheet(QStringLiteral(
        "QTabWidget::pane { border: 1px solid #DFE5EC; background: #FFFFFF; }"
        "QTabBar::tab { background: #EDF1F5; color: #5D6E80; padding: 4px 12px; border: 1px solid #DFE5EC; border-bottom: none; }"
        "QTabBar::tab:selected { background: #FFFFFF; color: #1B73D0; font-weight: 500; }"));

    // 1. 二维测线 (2D)
    auto *w2d = new QWidget(modeTabs);
    w2d->setObjectName(QStringLiteral("seismic2DContainer"));
    auto *lay2d = new QVBoxLayout(w2d);
    lay2d->setContentsMargins(6, 6, 6, 6);
    lay2d->setSpacing(4);
    lay2d->addWidget(caption8(tr("选择一条测线解码"), w2d));
    lay2d->addWidget(bar);
    lay2d->addWidget(tieCaption);
    lay2d->addWidget(panel, 1);
    modeTabs->addTab(w2d, tr("二维测线 (2D)"));

    // 2. 三维立体 (3D)
    auto *panel3d = new seismic::Seismic3DViewPanel(modeTabs);
    panel3d->setObjectName(QStringLiteral("seismic3DPanel"));
    modeTabs->addTab(panel3d, tr("三维立体 (3D)"));

    // 3. 水平时间切片剖面 (Time Slice)
    auto *wTime = new QWidget(modeTabs);
    wTime->setObjectName(QStringLiteral("seismicTimeSliceContainer"));
    auto *layTime = new QVBoxLayout(wTime);
    layTime->setContentsMargins(6, 6, 6, 6);
    layTime->setSpacing(4);

    auto *timeBar = new QWidget(wTime);
    auto *timeBarLay = new QHBoxLayout(timeBar);
    timeBarLay->setContentsMargins(0, 0, 0, 0);
    timeBarLay->setSpacing(8);

    auto *lblTimeTitle = caption8(tr("水平时间切片 (TWT)"), timeBar);
    timeBarLay->addWidget(lblTimeTitle);

    auto *lblTimeIndex = new QLabel(tr("时间采样:"), timeBar);
    lblTimeIndex->setStyleSheet(QStringLiteral("color: #5D6E80; font-size: 8.5pt;"));
    timeBarLay->addWidget(lblTimeIndex);

    auto *sliderTime = new QSlider(Qt::Horizontal, timeBar);
    sliderTime->setObjectName(QStringLiteral("timeSliceSlider"));
    sliderTime->setFixedWidth(160);
    timeBarLay->addWidget(sliderTime);

    auto *spinTime = new QSpinBox(timeBar);
    spinTime->setObjectName(QStringLiteral("timeSliceSpin"));
    spinTime->setFont(QFont(QStringLiteral("JetBrains Mono"), 8));
    spinTime->setFixedWidth(64);
    timeBarLay->addWidget(spinTime);

    auto *lblTimeMs = new QLabel(QStringLiteral("0.0 ms"), timeBar);
    lblTimeMs->setObjectName(QStringLiteral("timeSliceMsLabel"));
    lblTimeMs->setFont(QFont(QStringLiteral("JetBrains Mono"), 8));
    lblTimeMs->setStyleSheet(QStringLiteral("color: #1B73D0; font-weight: bold;"));
    lblTimeMs->setFixedWidth(90);
    timeBarLay->addWidget(lblTimeMs);

    auto *btnFitTime = new QToolButton(timeBar);
    btnFitTime->setText(tr("适应窗口"));
    btnFitTime->setStyleSheet(QStringLiteral(
        "QToolButton { background: transparent; border: 1px solid #DFE5EC; border-radius: 4px; padding: 2px 8px; font-size: 8.5pt; color: #24303E; }"
        "QToolButton:hover { background: #EDF1F5; border-color: #1B73D0; }"));
    timeBarLay->addWidget(btnFitTime);

    timeBarLay->addStretch(1);
    layTime->addWidget(timeBar);

    auto *timeCanvas = new seismic::SeismicSectionCanvas(wTime);
    timeCanvas->setObjectName(QStringLiteral("timeSliceCanvas"));
    timeCanvas->setColorMap(seismic::SectionColorMapType::RedWhiteBlue);
    timeCanvas->setGain(1.2f);
    timeCanvas->setContrast(1.3f);
    layTime->addWidget(timeCanvas, 1);

    connect(btnFitTime, &QToolButton::clicked, timeCanvas, &seismic::SeismicSectionCanvas::fitToWindow);

    modeTabs->addTab(wTime, tr("水平时间切片 (Time Slice)"));

    auto sharedVol = std::make_shared<std::shared_ptr<seismic::SgyVolume>>();

    const auto loadVolumeIfNeeded = [this, sharedVol, panel3d, sliderTime, spinTime, abs]() {
      if (*sharedVol == nullptr && !abs.isEmpty() && QFile::exists(abs))
      {
        if (m_doc && m_doc->seismicTaskService())
          panel3d->setTaskService(m_doc->seismicTaskService());
        auto vol = std::make_shared<seismic::SgyVolume>();
        std::string volErr;
        if (vol->Load(abs.toStdString(), volErr))
        {
          *sharedVol = vol;
          panel3d->setVolume(vol);
          if (panel3d->viewport())
          {
            panel3d->viewport()->setPresetView(seismic::SeismicCameraController::PresetView::Isometric);
            panel3d->viewport()->fitToBounds();
          }

          sliderTime->blockSignals(true);
          spinTime->blockSignals(true);
          sliderTime->setRange(0, vol->SampleMax());
          spinTime->setRange(0, vol->SampleMax());
          const int mid = vol->SampleMax() / 2;
          sliderTime->setValue(mid);
          spinTime->setValue(mid);
          sliderTime->blockSignals(false);
          spinTime->blockSignals(false);
        }
      }
    };

    const auto updateTimeSlice = [sharedVol, timeCanvas, lblTimeMs](int sampleIndex) {
      if (!*sharedVol || !(*sharedVol)->IsLoaded())
        return;
      const auto vol = *sharedVol;
      const double ms = sampleIndex * (vol->SampleIntervalUs() / 1000.0);
      lblTimeMs->setText(QStringLiteral("%1 ms").arg(ms, 0, 'f', 1));

      seismic::SgySliceImage img;
      std::string err;
      if (vol->ExtractSlice(seismic::SgySliceType::Time, sampleIndex, img, err))
      {
        timeCanvas->setTimeSliceData(img, ms, vol->InlineMin(), vol->InlineMax(), vol->XlineMin(), vol->XlineMax());
      }
    };

    connect(sliderTime, &QSlider::valueChanged, host, [spinTime, updateTimeSlice](int val) {
      spinTime->blockSignals(true);
      spinTime->setValue(val);
      spinTime->blockSignals(false);
      updateTimeSlice(val);
    });

    connect(spinTime, QOverload<int>::of(&QSpinBox::valueChanged), host, [sliderTime, updateTimeSlice](int val) {
      sliderTime->blockSignals(true);
      sliderTime->setValue(val);
      sliderTime->blockSignals(false);
      updateTimeSlice(val);
    });

    connect(modeTabs, &QTabWidget::currentChanged, host, [loadVolumeIfNeeded, panel3d, timeCanvas, updateTimeSlice, sliderTime](int idx) {
      if (idx == 1)
      {
        loadVolumeIfNeeded();
        if (panel3d->viewport())
        {
          panel3d->viewport()->fitToBounds();
          panel3d->viewport()->update();
        }
      }
      else if (idx == 2)
      {
        loadVolumeIfNeeded();
        updateTimeSlice(sliderTime->value());
        QTimer::singleShot(20, timeCanvas, [timeCanvas]() {
          timeCanvas->fitToWindow();
        });
      }
    });

    lay->addWidget(modeTabs, 1);
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
    else if (m_doc)
    {
      m_doc->ensureDocumentPdf(assetId);
      switch (m_doc->documentPdfState(assetId))
      {
        case PreviewDocService::DocPdfState::Ready:
          pdfAbs = m_doc->documentPdfPath(assetId);
          break;
        case PreviewDocService::DocPdfState::Failed:
          lay->addWidget(
              stateLabel(tr("无 PDF 预览：%1").arg(m_doc->documentPdfError(assetId)),
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

  if (asset.type == QLatin1String("geojson") ||
      (asset.type == QLatin1String("boundary") && asset.displayName.endsWith(QLatin1String(".geojson"), Qt::CaseInsensitive)))
  {
    QJsonDocument doc;
    QString gerr;
    if (!m_doc->geoJsonDocumentAt(abs, &doc, &gerr))
    {
      lay->addWidget(failureState(assetId, gerr, host), 1);
      return host;
    }
    const QJsonArray features = doc.object().value(QStringLiteral("features")).toArray();
    QStringList propKeys;
    for (const QJsonValue &fv : features)
    {
      const QJsonObject props = fv.toObject()
                                    .value(QStringLiteral("properties"))
                                    .toObject();
      for (auto it = props.begin(); it != props.end(); ++it)
        if (!propKeys.contains(it.key()))
          propKeys.append(it.key());
    }

    // 寻找沉积相分类字段候选 (相、亚相、微相、facies 等)
    QStringList faciesCandidates;
    for (const QString &k : propKeys)
    {
      if (k == QLatin1String("相") || k == QLatin1String("微相") || k == QLatin1String("亚相") ||
          k.contains(QStringLiteral("相")) ||
          k.compare(QLatin1String("facies"), Qt::CaseInsensitive) == 0 ||
          k.compare(QLatin1String("sub_facies"), Qt::CaseInsensitive) == 0 ||
          k.compare(QLatin1String("micro_facies"), Qt::CaseInsensitive) == 0)
      {
        faciesCandidates.append(k);
      }
    }
    if (faciesCandidates.isEmpty())
    {
      for (const QString &k : propKeys)
      {
        if (k.compare(QLatin1String("name"), Qt::CaseInsensitive) == 0 ||
            k.compare(QLatin1String("type"), Qt::CaseInsensitive) == 0 ||
            k.compare(QLatin1String("zone"), Qt::CaseInsensitive) == 0)
        {
          faciesCandidates.append(k);
        }
      }
    }
    if (faciesCandidates.isEmpty() && !propKeys.isEmpty())
      faciesCandidates.append(propKeys.first());

    QString activeFaciesField;
    if (faciesCandidates.contains(QStringLiteral("相")))
      activeFaciesField = QStringLiteral("相");
    else if (!faciesCandidates.isEmpty())
      activeFaciesField = faciesCandidates.first();

    // 尝试创建 QGIS 矢量图层用于相图画布渲染
    auto *vlayer = new QgsVectorLayer(abs, asset.displayName, QStringLiteral("ogr"));
    vlayer->setParent(host);

    auto applyFaciesRenderer = [vlayer](const QString &fieldName) {
      if (!vlayer || !vlayer->isValid() || fieldName.isEmpty())
        return;

      const int fieldIdx = vlayer->fields().lookupField(fieldName);
      if (fieldIdx < 0)
        return;

      QSet<QString> uniqueVals;
      QgsFeatureIterator it = vlayer->getFeatures();
      QgsFeature feat;
      while (it.nextFeature(feat))
      {
        const QString v = feat.attribute(fieldIdx).toString().trimmed();
        if (!v.isEmpty())
          uniqueVals.insert(v);
      }

      QgsCategoryList categories;
      for (const QString &val : uniqueVals)
      {
        const QColor col = faciesColor(val);
        std::unique_ptr<QgsSymbol> sym = createFaciesSymbol(vlayer->geometryType(), col);
        categories.append(QgsRendererCategory(val, sym.release(), val));
      }
      std::unique_ptr<QgsSymbol> defSym = createFaciesSymbol(vlayer->geometryType(), QColor(QStringLiteral("#CFD8DC")));
      categories.append(QgsRendererCategory(QVariant(), defSym.release(), QObject::tr("其他")));

      vlayer->setRenderer(new QgsCategorizedSymbolRenderer(fieldName, categories));

      // 文本标注 (白色光晕 + 9pt 中黑)
      QgsPalLayerSettings palSettings;
      palSettings.fieldName = fieldName;
      palSettings.isExpression = false;
      QgsTextFormat txtFmt;
      QFont font(QStringLiteral("Noto Sans SC"), 9, QFont::Medium);
      txtFmt.setFont(font);
      txtFmt.setSize(9.0);
      txtFmt.setSizeUnit(Qgis::RenderUnit::Points);
      txtFmt.setColor(QColor(QStringLiteral("#24303E")));
      QgsTextBufferSettings buf;
      buf.setEnabled(true);
      buf.setSize(1.5);
      buf.setColor(Qt::white);
      txtFmt.setBuffer(buf);
      palSettings.setFormat(txtFmt);

      vlayer->setLabeling(new QgsVectorLayerSimpleLabeling(palSettings));
      vlayer->setLabelsEnabled(true);
      vlayer->triggerRepaint();
    };

    // 顶部操作与空间提示工具栏
    auto *topBar = new QWidget(host);
    auto *topLay = new QHBoxLayout(topBar);
    topLay->setContentsMargins(8, 4, 8, 4);
    topLay->setSpacing(6);
    topBar->setStyleSheet(QStringLiteral("background: #EDF1F5; border-bottom: 1px solid #DFE5EC;"));

    const QString btnStyle = QStringLiteral(
        "QToolButton { background: #FFFFFF; border: 1px solid #DFE5EC; border-radius: 4px; "
        "padding: 4px 8px; font-size: 8.5pt; color: #24303E; }"
        "QToolButton:hover { background: #E2E8F0; border-color: #9AA7B4; }"
        "QToolButton:pressed { background: #DFE5EC; }"
        "QToolButton:checked { background: #E1EFFE; border-color: #1B73D0; color: #1B73D0; font-weight: 500; }");

    // 视图切换器: 相图地图 / 属性列表
    auto *btnViewMap = new QToolButton(topBar);
    btnViewMap->setObjectName(QStringLiteral("btnViewFaciesMap"));
    btnViewMap->setText(tr("相图地图"));
    btnViewMap->setCheckable(true);
    btnViewMap->setChecked(true);
    btnViewMap->setStyleSheet(btnStyle);
    btnViewMap->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mIconPolygonLayer.svg")));
    btnViewMap->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    topLay->addWidget(btnViewMap);

    auto *btnViewTable = new QToolButton(topBar);
    btnViewTable->setObjectName(QStringLiteral("btnViewFaciesTable"));
    btnViewTable->setText(tr("属性列表"));
    btnViewTable->setCheckable(true);
    btnViewTable->setChecked(false);
    btnViewTable->setStyleSheet(btnStyle);
    btnViewTable->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionOpenTable.svg")));
    btnViewTable->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    topLay->addWidget(btnViewTable);

    auto *viewGroup = new QButtonGroup(topBar);
    viewGroup->addButton(btnViewMap);
    viewGroup->addButton(btnViewTable);

    topLay->addSpacing(6);

    // 地图浏览工具
    auto *btnFull = new QToolButton(topBar);
    btnFull->setObjectName(QStringLiteral("btnFaciesFullExtent"));
    btnFull->setText(tr("全图"));
    btnFull->setToolTip(tr("缩放到相图完整范围"));
    btnFull->setStyleSheet(btnStyle);
    btnFull->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionZoomFullExtent.svg")));
    btnFull->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    topLay->addWidget(btnFull);

    auto *btnIn = new QToolButton(topBar);
    btnIn->setObjectName(QStringLiteral("btnFaciesZoomIn"));
    btnIn->setText(tr("放大"));
    btnIn->setStyleSheet(btnStyle);
    btnIn->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionZoomIn.svg")));
    btnIn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    topLay->addWidget(btnIn);

    auto *btnOut = new QToolButton(topBar);
    btnOut->setObjectName(QStringLiteral("btnFaciesZoomOut"));
    btnOut->setText(tr("缩小"));
    btnOut->setStyleSheet(btnStyle);
    btnOut->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionZoomOut.svg")));
    btnOut->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    topLay->addWidget(btnOut);

    auto *btnPan = new QToolButton(topBar);
    btnPan->setObjectName(QStringLiteral("btnFaciesPan"));
    btnPan->setText(tr("漫游"));
    btnPan->setStyleSheet(btnStyle);
    btnPan->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionPan.svg")));
    btnPan->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    topLay->addWidget(btnPan);

    // 字段选择下拉框（若有多个相分类字段）
    QComboBox *fieldCombo = nullptr;
    QLabel *fieldLbl = nullptr;
    if (faciesCandidates.size() > 1)
    {
      fieldLbl = new QLabel(tr("渲染字段:"), topBar);
      fieldLbl->setStyleSheet(QStringLiteral("color: #5D6E80; font-size: 8.5pt;"));
      topLay->addWidget(fieldLbl);

      fieldCombo = new QComboBox(topBar);
      fieldCombo->setObjectName(QStringLiteral("faciesFieldCombo"));
      fieldCombo->addItems(faciesCandidates);
      if (!activeFaciesField.isEmpty())
        fieldCombo->setCurrentText(activeFaciesField);
      fieldCombo->setStyleSheet(QStringLiteral(
          "QComboBox { background: #FFFFFF; border: 1px solid #DFE5EC; border-radius: 4px; padding: 2px 6px; font-size: 8.5pt; color: #24303E; }"
          "QComboBox:hover { border-color: #9AA7B4; }"));
      topLay->addWidget(fieldCombo);
    }

    topLay->addSpacing(8);

    // D11 临时配准入口：手工仿射把这份 GeoJSON 拉到工程测网。产物是
    // DERIVED 版本 + 「临时配准」水印图层，不改原 RAW。
    auto *regBtn = new QPushButton(tr("临时配准（手工仿射）…"), host);
    regBtn->setObjectName(QStringLiteral("provisionalRegisterButton"));
    regBtn->setAccessibleName(tr("临时配准"));
    regBtn->setToolTip(tr("手工输入仿射参数，把 GeoJSON 变换到工程局部测网"));
    regBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: #FFFFFF; border: 1px solid #DFE5EC; border-radius: 4px; padding: 4px 8px; font-size: 8.5pt; color: #24303E; }"
        "QPushButton:hover { background: #E2E8F0; border-color: #9AA7B4; }"));
    topLay->addWidget(regBtn);

    topLay->addStretch(1);

    auto *warnLbl = warnLabel(tr("经纬度，与本测网不是同一空间"), host);
    warnLbl->setStyleSheet(QStringLiteral("color: #D32F2F; font-size: 11px; font-weight: 500;"));
    topLay->addWidget(warnLbl);
    lay->addWidget(topBar);

    connect(regBtn, &QPushButton::clicked, this, [this, assetId, abs]() {
      double srcB[4];
      QString berr;
      if (!m_doc->geoJsonBounds(abs, srcB, &berr))
      {
        warnLabel(tr("读不出坐标范围：%1").arg(berr), m_tabs);
        return;
      }

      QDialog dlg(this);
      dlg.setWindowTitle(tr("临时配准（手工仿射）"));
      auto *form = new QFormLayout(&dlg);
      auto *srcLbl = new QLabel(
          tr("源坐标范围：X %1–%2 · Y %3–%4")
              .arg(QString::number(srcB[0], 'f', 2), QString::number(srcB[2], 'f', 2),
                   QString::number(srcB[1], 'f', 2), QString::number(srcB[3], 'f', 2)),
          &dlg);
      srcLbl->setWordWrap(true);
      form->addRow(srcLbl);
      auto *gridHint = new QLabel(
          tr("目标：工程局部测网（约 X 0–12800 · Y 0–16400，单位米）"), &dlg);
      gridHint->setWordWrap(true);
      gridHint->setStyleSheet(QStringLiteral("color: #5D6E80;"));
      form->addRow(gridHint);

      auto *tx = new QDoubleSpinBox(&dlg);
      auto *ty = new QDoubleSpinBox(&dlg);
      auto *sx = new QDoubleSpinBox(&dlg);
      auto *sy = new QDoubleSpinBox(&dlg);
      auto *rot = new QDoubleSpinBox(&dlg);
      for (auto *s : {tx, ty})
      {
        s->setRange(-1e9, 1e9);
        s->setDecimals(2);
        s->setSingleStep(1000.0);
      }
      for (auto *s : {sx, sy})
      {
        s->setRange(1e-6, 1e6);
        s->setDecimals(6);
        s->setValue(1.0);
      }
      rot->setRange(-360.0, 360.0);
      rot->setDecimals(2);
      form->addRow(tr("平移 X（米）"), tx);
      form->addRow(tr("平移 Y（米）"), ty);
      form->addRow(tr("缩放 X"), sx);
      form->addRow(tr("缩放 Y"), sy);
      form->addRow(tr("旋转（度）"), rot);

      auto *dstLbl = new QLabel(&dlg);
      dstLbl->setObjectName(QStringLiteral("affineDstBounds"));
      dstLbl->setWordWrap(true);
      dstLbl->setStyleSheet(QStringLiteral("color: #5D6E80;"));
      form->addRow(dstLbl);
      const auto refreshDst = [this, srcB, tx, ty, sx, sy, rot, dstLbl]() {
        const QVariantMap p{{QStringLiteral("tx"), tx->value()},
                            {QStringLiteral("ty"), ty->value()},
                            {QStringLiteral("sx"), sx->value()},
                            {QStringLiteral("sy"), sy->value()},
                            {QStringLiteral("rotDeg"), rot->value()}};
        double lo[2], hi[2];
        m_doc->affinePreviewBounds(srcB, p, lo, hi);
        dstLbl->setText(tr("变换后范围：X %1–%2 · Y %3–%4")
                            .arg(QString::number(lo[0], 'f', 1),
                                 QString::number(hi[0], 'f', 1),
                                 QString::number(lo[1], 'f', 1),
                                 QString::number(hi[1], 'f', 1)));
      };
      for (auto *s : {tx, ty, sx, sy, rot})
        connect(s, qOverload<double>(&QDoubleSpinBox::valueChanged), dstLbl, refreshDst);
      refreshDst();

      auto *buttons = new QDialogButtonBox(
          QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
      buttons->button(QDialogButtonBox::Ok)->setText(tr("登记为临时配准"));
      form->addRow(buttons);
      connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
      connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
      if (dlg.exec() != QDialog::Accepted)
        return;

      emit provisionalRegistrationRequested(
          assetId, {{QStringLiteral("tx"), tx->value()},
                    {QStringLiteral("ty"), ty->value()},
                    {QStringLiteral("sx"), sx->value()},
                    {QStringLiteral("sy"), sy->value()},
                    {QStringLiteral("rotDeg"), rot->value()}});
    });

    auto *viewStack = new QStackedWidget(host);
    viewStack->setObjectName(QStringLiteral("faciesViewStack"));

    // 1. QGIS 相图地图画布页
    auto *canvasPage = new QWidget(viewStack);
    auto *cvLay = new QVBoxLayout(canvasPage);
    cvLay->setContentsMargins(0, 0, 0, 0);
    cvLay->setSpacing(0);

    auto *canvas = new QgsMapCanvas(canvasPage);
    canvas->setObjectName(QStringLiteral("faciesMapCanvas"));
    canvas->enableAntiAliasing(true);
    canvas->setCanvasColor(Qt::white);

    if (vlayer && vlayer->isValid())
    {
      applyFaciesRenderer(activeFaciesField);
      canvas->setDestinationCrs(vlayer->crs());
      canvas->setLayers({vlayer});

      auto *decorMgr = new PaleoDecorationManager(canvas, canvas);
      decorMgr->setObjectName(QStringLiteral("faciesDecorManager"));
      decorMgr->setScaleBarEnabled(true);
      decorMgr->setNorthArrowEnabled(true);

      auto *panTool = new QgsMapToolPan(canvas);
      canvas->setMapTool(panTool);

      auto zoomFaciesFull = [canvas, vlayer]() {
        if (vlayer && !vlayer->extent().isEmpty())
        {
          QgsRectangle ext = vlayer->extent();
          ext.grow(qMax(ext.width(), ext.height()) * 0.1);
          canvas->setExtent(ext);
          canvas->refresh();
        }
        else
        {
          canvas->zoomToFullExtent();
          canvas->refresh();
        }
      };

      connect(btnFull, &QToolButton::clicked, canvas, zoomFaciesFull);
      connect(btnIn, &QToolButton::clicked, canvas, &QgsMapCanvas::zoomIn);
      connect(btnOut, &QToolButton::clicked, canvas, &QgsMapCanvas::zoomOut);
      connect(btnPan, &QToolButton::clicked, canvas, [canvas, panTool]() {
        canvas->setMapTool(panTool);
      });

      if (fieldCombo)
      {
        connect(fieldCombo, &QComboBox::currentTextChanged, canvas, [applyFaciesRenderer, canvas](const QString &fld) {
          applyFaciesRenderer(fld);
          canvas->refresh();
        });
      }

      QTimer::singleShot(100, canvas, zoomFaciesFull);
    }
    else
    {
      btnViewMap->setEnabled(false);
      btnViewTable->setChecked(true);
    }

    cvLay->addWidget(canvas, 1);
    viewStack->addWidget(canvasPage);

    // 2. 要素属性表格预览
    auto *table = new QTableWidget(viewStack);
    table->setObjectName(QStringLiteral("geoJsonFeatureTable"));
    table->setAlternatingRowColors(true);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setStyleSheet(QStringLiteral(
        "QTableWidget { background-color: #FFFFFF; gridline-color: #DFE5EC; border: 1px solid #DFE5EC; font-size: 12px; }"
        "QHeaderView::section { background-color: #F8FAFC; color: #5D6E80; border: none; border-bottom: 1px solid #DFE5EC; border-right: 1px solid #DFE5EC; padding: 4px 8px; font-weight: 500; font-size: 11px; }"));

    QStringList headers;
    headers << tr("序号") << tr("几何类型");
    headers.append(propKeys);
    table->setColumnCount(headers.size());
    table->setHorizontalHeaderLabels(headers);

    const int maxRows = qMin(features.size(), 1000);
    table->setRowCount(maxRows);
    for (int r = 0; r < maxRows; ++r)
    {
      const QJsonObject feat = features.at(r).toObject();
      const QString geomType = feat.value(QStringLiteral("geometry")).toObject().value(QStringLiteral("type")).toString();
      const QJsonObject props = feat.value(QStringLiteral("properties")).toObject();

      auto *idItem = new QTableWidgetItem(QString::number(r + 1));
      idItem->setTextAlignment(Qt::AlignCenter);
      idItem->setFlags(idItem->flags() & ~Qt::ItemIsEditable);
      table->setItem(r, 0, idItem);

      auto *geomItem = new QTableWidgetItem(geomType.isEmpty() ? QStringLiteral("—") : geomType);
      geomItem->setTextAlignment(Qt::AlignCenter);
      geomItem->setFlags(geomItem->flags() & ~Qt::ItemIsEditable);
      table->setItem(r, 1, geomItem);

      for (int c = 0; c < propKeys.size(); ++c)
      {
        const QString key = propKeys.at(c);
        const QJsonValue val = props.value(key);
        QString valStr;
        if (val.isDouble())
          valStr = QString::number(val.toDouble());
        else if (val.isString())
          valStr = val.toString();
        else if (val.isBool())
          valStr = val.toBool() ? QStringLiteral("true") : QStringLiteral("false");
        else if (val.isNull())
          valStr = QStringLiteral("null");
        else
          valStr = QString::fromUtf8(QJsonDocument(val.toArray()).toJson(QJsonDocument::Compact));

        auto *valItem = new QTableWidgetItem(valStr);
        valItem->setFlags(valItem->flags() & ~Qt::ItemIsEditable);
        table->setItem(r, c + 2, valItem);
      }
    }
    table->horizontalHeader()->setStretchLastSection(true);
    table->resizeColumnsToContents();
    viewStack->addWidget(table);

    const auto updateViewMode = [viewStack, btnFull, btnIn, btnOut, btnPan, fieldLbl, fieldCombo](int idx) {
      viewStack->setCurrentIndex(idx);
      const bool isMap = (idx == 0);
      btnFull->setVisible(isMap);
      btnIn->setVisible(isMap);
      btnOut->setVisible(isMap);
      btnPan->setVisible(isMap);
      if (fieldLbl) fieldLbl->setVisible(isMap);
      if (fieldCombo) fieldCombo->setVisible(isMap);
    };

    connect(btnViewMap, &QToolButton::clicked, host, [updateViewMode]() { updateViewMode(0); });
    connect(btnViewTable, &QToolButton::clicked, host, [updateViewMode]() { updateViewMode(1); });

    lay->addWidget(viewStack, 1);
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

    if (abs.endsWith(QLatin1String(".xml"), Qt::CaseInsensitive))
    {
      auto *compositePanel = new WellComposite::WellCompositePanel(host);
      compositePanel->setObjectName(QStringLiteral("wellCompositePanel"));
      if (compositePanel->loadComprehensiveXml(abs))
      {
        lay->addWidget(compositePanel, 1);
        return host;
      }
      delete compositePanel;
    }
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
    QVector<WellTopRecord> tops;
    QString werr;
    if (!m_doc->wellTopsAt(absPath, &tops, &werr))
      return failureState(asset.id, werr, parent);
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
    TimeDepthTable td;
    QString terr;
    if (!m_doc->timeDepthAt(absPath, &td, &terr))
      return failureState(asset.id, terr, parent);
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
    QVector<WellHeadRecord> rows;
    QString herr;
    if (!m_doc->wellHeadsAt(absPath, &rows, &herr))
      return failureState(asset.id, herr, parent);
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
            : m_doc->catalog()->entityById(wellEntityId).coordinateStatus;
    // T27：坐标状态行中文化 + text-muted（计划 §4：这些状态仍用 #5D6E80）。
    addRow(tr("坐标状态"), coordinateStatusText(status), false, true);
    hl->addWidget(info);
    hl->addWidget(caption8(tr("选中时地图同时高亮该井"), holder));
    hl->addStretch(1);
    return holder;
  }

  return stateLabel(tr("先选择一口井"), parent); // 兜底（不可达）
}

// ---- 测线解码结果应用（PreviewDocService 信号 → 挂起控件组）----
// 陈旧结果与 SHA 标过时都在服务内做完；这里只把最新一代贴上控件。
void DataPreviewTabs::onSectionReady(const QString &assetId,
                                     const PreviewDocService::SectionDoc &doc)
{
  const SectionPending pend = m_pendingSection.value(assetId);
  if (!pend.panel)
    return;
  // SectionPanel 是本 cpp 内聚的预览控件——挂起时存的是它。
  auto *sp = static_cast<SectionPanel *>(pend.panel.data());
  sp->setTraces(doc.traces, doc.sampleIntervalUs, doc.startTimeMs);
  if (pend.hasTie)
    sp->setTieMarker(pend.tieText, pend.tieMs);
  // 标题后缀：「文件名 · IL1315」/「文件名 · XL4165」（§4）。
  m_titleSuffixOfAsset[assetId] =
      (doc.isInline ? QStringLiteral("IL") : QStringLiteral("XL")) +
      QString::number(doc.lineNo);
  updateTabTitle(assetId);
  if (pend.mode)
  {
    pend.mode->setEnabled(true);
    pend.mode->setToolTip(QString());
  }
  if (pend.spin)
  {
    pend.spin->setEnabled(true);
    pend.spin->setToolTip(QString());
  }
}

void DataPreviewTabs::onSectionFailed(const QString &assetId,
                                      const QString &reason)
{
  const SectionPending pend = m_pendingSection.value(assetId);
  if (auto *sp = static_cast<SectionPanel *>(
          pend.panel ? pend.panel.data() : nullptr))
    sp->setError(reason.isEmpty() ? tr("无法解码测线") : reason); // §4：如实写，不装灰图
  if (pend.mode)
  {
    pend.mode->setEnabled(true);
    pend.mode->setToolTip(QString());
  }
  if (pend.spin)
  {
    pend.spin->setEnabled(true);
    pend.spin->setToolTip(QString());
  }
}
