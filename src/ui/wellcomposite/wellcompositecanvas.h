// 层：视图
#pragma once

#include <QWidget>
#include <QScrollBar>
#include <QVector>
#include <memory>

#include "wellcompositetrack.h"

// ui/wellcomposite/ — WellCompositeCanvas: ResFormStar 风格多井道综合柱状图交互画布
//
// 架构要点：
// 1. 道头置顶固定（Header Pinned）：纵向深度滚动时道头不动，横向多道平移时道头同步滚动；
// 2. 统一深度协调器（Depth Coordinator）：全道共享深度标尺与网格；
// 3. 丰富手势交互：
//    - 鼠标滚轮（以光标处深度为锚点无级平滑缩放深度）；
//    - 鼠标左键/中键拖拽（平移漫游深度与水平井道）；
//    - 双击复位（1:1 适应全井段）；
//    - 十字准星（横跨全道虚线对齐，深度道浮显数值，曲线道图例浮显多曲线插值读数）。
// 4. 8 类井道插槽支持，曲线道严格支持 1 至 4 根曲线同道叠合渲染。

namespace WellComposite
{

class WellCompositeHeader;
class WellCompositeBody;

class WellCompositeCanvas : public QWidget
{
  Q_OBJECT

public:
  explicit WellCompositeCanvas(QWidget *parent = nullptr);
  ~WellCompositeCanvas() override = default;

  // --- 井道管理 ---
  void addTrack(const std::shared_ptr<WellTrack> &track);
  void insertTrack(int index, const std::shared_ptr<WellTrack> &track);
  void removeTrack(int index);
  void clearTracks();
  void setTracks(const QList<std::shared_ptr<WellTrack>> &tracks);
  QList<std::shared_ptr<WellTrack>> tracks() const { return m_tracks; }
  int trackCount() const { return m_tracks.size(); }

  // --- 深度与比例尺控制 ---
  void setDepthRange(double minDepth, double maxDepth);
  double minDepth() const { return m_minDepth; }
  double maxDepth() const { return m_maxDepth; }

  void setScrollDepth(double depth);
  double scrollDepth() const { return m_scrollDepth; }

  void setZoomFactor(double factor, double anchorDepth = -1.0);
  double zoomFactor() const { return m_zoomFactor; }
  void zoomIn();
  void zoomOut();
  void resetZoom();

  void setScaleRatio(const QString &ratioStr);
  QString scaleRatio() const { return m_scaleRatio; }
  QString calculateScaleRatioString() const;

  // 视口深度范围与跨度查询
  double visibleTopDepth() const { return m_scrollDepth; }
  double visibleDepthSpan() const;
  double visibleBottomDepth() const { return visibleTopDepth() + visibleDepthSpan(); }

  // 坐标转换
  double depthToY(double depth) const;
  double yToDepth(double y) const;
  double pxPerMeter() const;

  // 几何计算
  qreal totalTracksWidth() const;
  qreal headerHeight() const { return m_headerHeight; }
  qreal hScrollOffset() const { return m_hScrollOffset; }

  // 十字准星与悬停
  double hoverDepth() const { return m_hoverDepth; }
  void setHoverDepth(double depth);

  // 内部重绘通知与视口变动广播
  void updateAll();
  void notifyViewportChanged();

signals:
  void zoomChanged(double factor);
  void depthHovered(double depth);
  void depthClicked(double depth);
  void scaleRatioChanged(const QString &ratio);
  void viewportChanged(double topDepth, double bottomDepth, double span);

protected:
  void resizeEvent(QResizeEvent *event) override;

private:
  void syncScrollBars();

  QList<std::shared_ptr<WellTrack>> m_tracks;

  double m_minDepth = 0.0;
  double m_maxDepth = 3000.0;
  double m_scrollDepth = 0.0;
  double m_zoomFactor = 1.0; // 1.0 = 100%
  double m_basePxPerMeter = 0.5; // 基准像素/米
  double m_hoverDepth = -1.0;

  qreal m_headerHeight = 72.0;
  qreal m_hScrollOffset = 0.0;
  QString m_baseScaleRatio = QStringLiteral("1:500");
  QString m_scaleRatio = QStringLiteral("1:500");

  WellCompositeHeader *m_header = nullptr;
  WellCompositeBody *m_body = nullptr;
  QScrollBar *m_vScrollBar = nullptr;
  QScrollBar *m_hScrollBar = nullptr;

  friend class WellCompositeHeader;
  friend class WellCompositeBody;
};

// ----------------------------------------------------------------------------
// 置顶道头组件 (Header Widget)
// ----------------------------------------------------------------------------
class WellCompositeHeader : public QWidget
{
  Q_OBJECT
public:
  explicit WellCompositeHeader(WellCompositeCanvas *canvas);

protected:
  void paintEvent(QPaintEvent *event) override;

private:
  WellCompositeCanvas *m_canvas = nullptr;
};

// ----------------------------------------------------------------------------
// 道体组件 (Body Widget)
// ----------------------------------------------------------------------------
class WellCompositeBody : public QWidget
{
  Q_OBJECT
public:
  explicit WellCompositeBody(WellCompositeCanvas *canvas);

protected:
  void paintEvent(QPaintEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void mouseDoubleClickEvent(QMouseEvent *event) override;
  void leaveEvent(QEvent *event) override;

private:
  WellCompositeCanvas *m_canvas = nullptr;
  bool m_isPanning = false;
  QPoint m_lastMousePos;
  QPoint m_pressPos;
};

} // namespace WellComposite