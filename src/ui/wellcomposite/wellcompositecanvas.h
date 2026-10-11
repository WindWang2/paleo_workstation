// 层：视图
#pragma once

#include <QWidget>
#include <QScrollBar>
#include <QVector>
#include <memory>

#include "wellcompositetrack.h"
#include "wellcompositestore.h"
#include "depthtools.h"

// ui/wellcomposite/ — WellCompositeCanvas: ResFormStar 风格多井道综合柱状图交互画布
//
// 架构要点：
// 1. 道头置顶固定（Header Pinned）：纵向深度滚动时道头不动，横向多道平移时道头同步滚动；
// 2. 统一深度协调器（Depth Coordinator）：全道共享深度标尺与网格；
// 3. 手势交互：
//    - 滚轮缩放（鼠标锚点/中心两模式，D2.4；Shift 加速、Ctrl 微调，D2.10）；
//    - 缩放上下限（最小显示段 10m、最大整井，D2.8）；
//    - 左键/中键拖拽平移；Shift+左键拖出橡皮筋区间统计（D2.2）；
//    - 双击复位（深度标尺道上双击 = 加标注钉，D2.3）；
//    - 十字准星 + 深度气泡 + 道 tooltip（D1.9）；
// 4. 道头交互：拖拽换位（投影指示线，Esc 取消，D1.3）、分隔线拖拽调宽（D1.4）、
//    右键道菜单（隐藏/复制/导出 CSV/配置/跳深度/删除，D1.5）；
// 5. 标志层线渲染 + 吸附（D2.1）+ 间距 gap 高亮（D2.12）+ 编辑模式拖拽改顶深（D3.1）；
// 6. 键盘导航：Tab 道间移动焦点、Enter 开配置、Esc 逐级取消、方向键滚深度、+/- 缩放
//    （D7.1/D7.2 焦点环）。

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
  // D1.3 拖拽换位提交目标（from→to；to 按「插入到该序之前」语义）
  void moveTrack(int fromIndex, int toIndex);

  // --- 深度与比例尺控制 ---
  void setDepthRange(double minDepth, double maxDepth);
  double minDepth() const { return m_minDepth; }
  double maxDepth() const { return m_maxDepth; }

  // userDriven=false 时不施加 D2.1 吸附（程序化定位/书签跳转直达）
  void setScrollDepth(double depth, bool userDriven = false);
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

  // 道几何（可见道序列上的序号；body/header 坐标）
  int visibleTrackCount() const;
  int trackIndexAtX(qreal x) const;          // 可见序号；空白区返回 -1
  QRectF trackBodyRect(int visibleIndex) const;
  QList<int> visibleTrackIndices() const;

  // 十字准星与悬停
  double hoverDepth() const { return m_hoverDepth; }
  void setHoverDepth(double depth);

  // --- D2.1 吸附配置 ---
  void setSnapEnabled(bool on) { m_snapEnabled = on; }
  bool snapEnabled() const { return m_snapEnabled; }
  void setSnapThresholdPx(int px) { m_snapThresholdPx = qMax(2, px); }
  int snapThresholdPx() const { return m_snapThresholdPx; }
  void setSnapToGrid(bool on) { m_snapToGrid = on; }
  bool snapToGrid() const { return m_snapToGrid; }

  // --- D2.4 滚轮缩放锚点模式 ---
  void setWheelZoomAtMouse(bool atMouse) { m_wheelZoomAtMouse = atMouse; }
  bool wheelZoomAtMouse() const { return m_wheelZoomAtMouse; }

  // --- D2.8 缩放上下限 ---
  void setZoomSpanLimits(double minSpanMeters, double maxSpanMeters);
  double minVisibleSpan() const { return m_minVisibleSpan; }
  double maxVisibleSpan() const { return m_maxVisibleSpan; }

  // --- D2.3 深度标注钉（渲染 + 双击标尺创建意图） ---
  void setPins(const QList<DepthPin> &pins);
  QList<DepthPin> pins() const { return m_pins; }

  // --- 标志层线（渲染/吸附/读数/gap/编辑共用数据面） ---
  void setMarkerLines(const QVector<DepthTools::MarkerLine> &markers);
  QVector<DepthTools::MarkerLine> markerLines() const { return m_markers; }
  // D2.12 gap 高亮
  void setGapThresholdMeters(double meters) { m_gapThresholdM = qMax(0.0, meters); }
  double gapThresholdMeters() const { return m_gapThresholdM; }

  // --- D3.x 编辑模式（视觉区分 D3.14 + 标志层拖拽 D3.1） ---
  void setEditMode(bool on);
  bool editMode() const { return m_editMode; }

  // --- D6.3 深度单位显示（传播给深度标尺道） ---
  void setDepthUnitLabel(const QString &label);

  // --- D6.5 TWT 副刻度（传播给深度标尺道） ---
  void setTwtLabels(const QVector<QPair<double, QString>> &twtAtDepth);

  // --- D7.4 高对比模式（传播给深度标尺道） ---
  void setHighContrast(bool on);

  // --- D1.9 tooltip 文本（trackAt + depth） ---
  QString toolTipFor(int trackIndex, double depth) const;

  // D1.5 CSV 导出文本（带 BOM 由写文件方附加）
  QString trackCsvAt(int trackIndex) const;

  // --- D7.2 焦点道 ---
  int focusTrackIndex() const { return m_focusTrackIndex; }
  void setFocusTrackIndex(int index);

  // 内部重绘通知与视口变动广播（面板/图例同步也用）
  void updateAll();
  void notifyViewportChanged();
  void syncScrollBars();

  // 橡皮筋区间（程序化驱动口，测试/外部工具用）
  bool isRubberBandActive() const { return m_rubberBandActive; }
  QRect rubberBandRect() const { return m_rubberBandRect; }

  // 拖拽换位状态（测试观察）
  bool isHeaderDragActive() const { return m_headerDragIndex >= 0; }
  int headerDragInsertIndex() const { return m_headerDragInsertIndex; }
  bool headerDragCancelled() const { return m_headerDragCancelled; }

  // 分隔线命中（测试观察）：body/header 坐标 x 处 5px 内返回左侧道可见序号，否则 -1
  int splitterIndexAtX(qreal x) const;

signals:
  void zoomChanged(double factor);
  void depthHovered(double depth);
  void depthClicked(double depth);
  void scaleRatioChanged(const QString &ratio);
  void viewportChanged(double topDepth, double bottomDepth, double span);

  // D1.x 道操作意图（面板接线）
  void trackOrderChanged();
  void trackWidthChanged(int trackIndex);
  void trackVisibilityChanged(int trackIndex);
  void trackRemoved(int trackIndex);
  void trackConfigRequested(int trackIndex);
  void trackCsvRequested(int trackIndex);
  void trackDuplicationRequested(int trackIndex);

  // 跳深度意图（右键菜单/工具条；面板弹 Ctrl+G 对话框）
  void gotoDepthRequested();

  // D3.14 编辑模式开关广播
  void editModeChanged(bool on);

  // D2.x 深度交互意图
  void intervalSelected(double topDepth, double bottomDepth);
  void pinCreateRequested(double depth);
  void pinEditRequested(int pinIndex);

  // D3.x 编辑意图
  void markerMoved(const QString &markerName, double newDepth);
  void markerContextMenuRequested(const QString &markerName, double depth);

protected:
  void resizeEvent(QResizeEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;

private:
  void refreshScaleTracks();
  friend class WellCompositeHeader;
  friend class WellCompositeBody;

  // D2.8 视口跨度夹取；返回 true 表示发生了修正
  bool clampViewportToLimits(double *scrollDepth, double pxPerMeterTarget) const;
  // D2.1 对 scrollDepth 施加吸附（返回吸附后的值）
  double applySnap(double rawDepth) const;

  // 道头拖拽状态（D1.3）：m_headerDragIndex = 被拖道可见序号
  int m_headerDragIndex = -1;
  int m_headerDragInsertIndex = -1;
  bool m_headerDragCancelled = false;

  // 橡皮筋（D2.2）
  bool m_rubberBandActive = false;
  QRect m_rubberBandRect;

  // 标志层拖拽（D3.1）：被拖标志层索引（m_markers 内）。setMarkerLines 会按
  // 深度重排序——拖过相邻线后索引会换人，故同时按名字锁定被拖线（每帧重定位）。
  int m_markerDragIndex = -1;
  QString m_markerDragName;
  double m_markerDragGrabOffset = 0.0;

  QList<std::shared_ptr<WellTrack>> m_tracks;

  double m_minDepth = 0.0;
  double m_maxDepth = 3000.0;
  double m_scrollDepth = 0.0;
  double m_zoomFactor = 1.0; // 1.0 = 100%
  double m_basePxPerMeter = 0.5; // 基准像素/米
  double m_hoverDepth = -1.0;

  // D2.1 吸附
  bool m_snapEnabled = false;
  int m_snapThresholdPx = 8;
  bool m_snapToGrid = true;

  // D2.4 / D2.8
  bool m_wheelZoomAtMouse = true;
  double m_minVisibleSpan = 10.0;
  double m_maxVisibleSpan = 0.0; // 0 = 整井自适应

  // D2.3 / 标志层 / gap / 编辑
  QList<DepthPin> m_pins;
  QVector<DepthTools::MarkerLine> m_markers;
  double m_gapThresholdM = 0.0;
  bool m_editMode = false;

  // D7.2 焦点道
  int m_focusTrackIndex = -1;

  qreal m_headerHeight = 72.0;
  qreal m_hScrollOffset = 0.0;
  QString m_baseScaleRatio = QStringLiteral("1:500");
  QString m_scaleRatio = QStringLiteral("1:500");

  WellCompositeHeader *m_header = nullptr;
  WellCompositeBody *m_body = nullptr;
  QScrollBar *m_vScrollBar = nullptr;
  QScrollBar *m_hScrollBar = nullptr;
};

// ----------------------------------------------------------------------------
// 置顶道头组件 (Header Widget)：D1.3 拖拽换位 / D1.4 分隔线调宽 / 焦点环 D7.2
// ----------------------------------------------------------------------------
class WellCompositeHeader : public QWidget
{
  Q_OBJECT
public:
  explicit WellCompositeHeader(WellCompositeCanvas *canvas);

  // 测试观察：命中类型
  enum class HitKind { None, TrackTitle, Splitter };
  HitKind hitTest(const QPoint &pos, int *trackVisibleIndex) const;

protected:
  void paintEvent(QPaintEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;

private:
  WellCompositeCanvas *m_canvas = nullptr;
  bool m_draggingSplitter = false;
  int m_splitterTrackIndex = -1; // 分隔线左侧道（可见序号）
  qreal m_splitterStartWidth = 0.0;
};

// ----------------------------------------------------------------------------
// 道体组件 (Body Widget)：手势 + 右键菜单 + 橡皮筋 + 标志层编辑拖拽
// ----------------------------------------------------------------------------
class WellCompositeBody : public QWidget
{
  Q_OBJECT
public:
  explicit WellCompositeBody(WellCompositeCanvas *canvas);

  // D3.1 标志层命中（±4px；返回 m_markers 索引，出参给线深）
  int markerHitTest(qreal y, double *lineDepth = nullptr) const;

  // 测试观察：最近一次右键菜单目标道（-1 = 无）
  int lastContextMenuTrack() const { return m_lastContextMenuTrack; }

  // 程序化橡皮筋（测试）：三步驱动
  void beginRubberBand(const QPoint &pos);
  void updateRubberBand(const QPoint &pos);
  void endRubberBand();

protected:
  void paintEvent(QPaintEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void mouseDoubleClickEvent(QMouseEvent *event) override;
  void leaveEvent(QEvent *event) override;
  void contextMenuEvent(QContextMenuEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;

private:
  void openTrackContextMenu(const QPoint &pos, int trackIndex);

  WellCompositeCanvas *m_canvas = nullptr;
  bool m_isPanning = false;
  bool m_shiftRubber = false; // Shift 按下进入橡皮筋语义
  QPoint m_lastMousePos;
  QPoint m_pressPos;
  int m_lastContextMenuTrack = -1;
};

} // namespace WellComposite
