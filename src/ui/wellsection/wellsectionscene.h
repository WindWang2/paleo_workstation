// 层：视图
#pragma once
#include "domain/wellsection.h"
#include "wellsectionstyle.h"

#include <QGraphicsItem>
#include <QGraphicsView>
#include <QImage>
#include <QWidget>

// ui/wellsection — 剖面渲染核：面板持有一份渲染状态（过滤后的井集、
// 拉平偏移、深度窗口、模板、主题、高亮、地震缝、布局版本），列项与
// 井间项按状态只读重画。视图不做几何变换（identity）——缩放 = 布局
// 参数变化（px/m、井间距），文字始终按设备像素画，保持锐利。
// 版头不是场景项：它是视图上方的一个 QWidget，水平滚动跟随视图。
namespace wellsectionui {

// 渲染共享状态（panel 拥有；item/header 持指针只读）。
struct RenderState {
  QVector<wellsection::Well> wells;    // tops 已按模板过滤
  QVector<double> offsets;             // 每井拉平偏移（与 wells 等长）
  wellsection::DepthWindow window;     // 显示深度窗口
  wellsection::SectionTemplate tpl;
  wellsection::SectionTheme theme;
  wellsection::SeismicStrip strip;
  bool seismicOn = false;
  QString activeTop, baseTop;          // 高亮地层段（activeTop 空 = 无高亮）
  int selected = -1;                   // 选中井序号（-1 无）
  QStringList zoneOrder;               // orderedTopNames → zoneFill 取色序
  int margin = 16;
  double gapPx = 96.0;
  double pxPerMeter = 1.0;
  quint64 layoutVersion = 0;           // 几何/井集/模板变更递增（缓存键）
  quint64 curveVersion = 0;            // 曲线几何变更递增（px/m、偏移、数据、模板）——gapPx 不属其中
  quint64 stripVersion = 0;            // 地震缝数据变更递增（缓存键）

  double columnWidth() const { return tpl.columnWidth(); }
  double columnLeft(int i) const { return margin + i * (columnWidth() + gapPx); }
  double columnRight(int i) const { return columnLeft(i) + columnWidth(); }
  double minGap() const { return seismicOn ? 140.0 : 48.0; }
  double maxGap() const { return 600.0; }
  double yForDisplay(double displayDepth) const
  {
    return (displayDepth - window.top) * pxPerMeter;
  }
  double displayAtY(double y) const { return window.top + y / pxPerMeter; }
  double yForMd(int well, double md) const
  {
    return yForDisplay(md - offsets.value(well, 0.0));
  }
  // 命中测试：返回井序号或 -1（缝/边缘内）。
  int columnAtX(double x) const;
  double sceneWidth() const;
  double sceneHeight() const { return (window.base - window.top) * pxPerMeter; }
};

// 一口井的整列（纸面 + 道框 + 分层界线 + 各道内容）。expose 局部重画。
class ColumnItem : public QGraphicsItem
{
  public:
    ColumnItem(RenderState *st, int index);
    int index() const { return m_index; }
    // 布局参数变了（不是井集重建）：prepareGeometryChange + 重画。
    void relayout() { prepareGeometryChange(); update(); }
    QRectF boundingRect() const override;
    void paint(QPainter *p, const QStyleOptionGraphicsItem *option,
               QWidget *widget) override;

  private:
    void paintCurveTrack(QPainter *p, const QRectF &trackRect,
                         const QRectF &exposed,
                         const wellsection::TrackSpec &tr, int trackIdx);
    void paintDepthTrack(QPainter *p, const QRectF &trackRect,
                         const QRectF &exposed);
    void paintZoneTrack(QPainter *p, const QRectF &trackRect,
                        const QRectF &exposed);
    void paintLithologyTrack(QPainter *p, const QRectF &trackRect,
                             const QRectF &exposed,
                             const wellsection::TrackSpec &tr);

    RenderState *m_st;
    int m_index;
    // 曲线路径按曲线几何版本缓存（curveVersion 变 → 失效重算；井间距变化
    // 不重算——拉伸只动列位置不动路径）。
    mutable quint64 m_pathVersion = ~quint64(0);
    mutable QHash<quint64, QPainterPath> m_pathCache; // (trackIdx<<8|curveIdx) → path
    mutable QHash<quint64, QPainterPath> m_sandCache;
};

// 井间缝（两列之间）：地震 → 层段带 → 高亮带 → 分层连线；带 reason
// 的缝画灰字说明（tooltip 由面板在状态变更时挂，paint 不改自身状态）。
class GapItem : public QGraphicsItem
{
  public:
    GapItem(RenderState *st, int index);
    int index() const { return m_index; }
    void relayout() { prepareGeometryChange(); update(); }
    QRectF boundingRect() const override;
    void paint(QPainter *p, const QStyleOptionGraphicsItem *option,
               QWidget *widget) override;

  private:
    void paintSeismic(QPainter *p, const QRectF &exposed,
                      const wellsection::SeismicGap &gap);
    // 连线几何：主题 curvedLinks 时走 S 形贝塞尔，否则直线；层段带/
    // 高亮带/高亮边界共用同一路径（填充与线型不走两套几何）。
    QPainterPath linkPath(const wellsection::Link &lk) const;

    RenderState *m_st;
    int m_index;
    // 缝内地震图缓存：(exposed, layoutVersion, stripVersion, theme.id)。
    mutable QImage m_img;
    mutable QRect m_imgRect;
    mutable quint64 m_imgKeyLayout = ~quint64(0), m_imgKeyStrip = ~quint64(0);
    mutable QString m_imgKeyTheme;
};

// 吸顶版头（视图上方 QWidget，非场景项）：上行井名（点击选中、拖排、
// 右键移除），下行各道标题/刻度格。水平偏移跟随视图滚动条。
class HeaderWidget : public QWidget
{
  Q_OBJECT
  public:
    explicit HeaderWidget(RenderState *st, QWidget *parent = nullptr);
    void setScrollOffset(int x);
    // 模板/道宽变更后由面板调用：标题行 + 动态题注行高。
    void relayout();
    // 名行 + 题注行（题注行高 = 各道换行后最大行数 × lineSpacing + 8），
    // 不低于 kHeight。
    int headerHeight() const;
    QSize sizeHint() const override { return QSize(200, headerHeight()); }
    QSize minimumSizeHint() const override { return QSize(60, headerHeight()); }

    static constexpr int kHeight = 70; // 版头最小高
    // 与 renderImage 共用：在 (0,0,w×headerHeight) 内按状态画版头内容。
    void paintContents(QPainter *p, double xOffset) const;

  signals:
    void wellClicked(int index);
    void reorderRequested(int from, int to);
    void removeRequested(int index);

  protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;

  private:
    int columnAtX(double sceneX) const; // 版头内命中（含列宽范围）

    RenderState *m_st;
    int m_scrollX = 0;
    int m_pressCol = -1;
    QPoint m_pressPos;
    bool m_dragging = false;
    int m_insertAt = -1; // 拖排插入位（画主色标记线）
};

// 视图：identity 变换，缩放 = 改布局参数。Ctrl+滚轮 = 深度缩放（锚定
// 光标深度），Ctrl+Shift+滚轮 = 井间距，Shift+滚轮 = 横向，滚轮 = 纵向。
class View : public QGraphicsView
{
  Q_OBJECT
  public:
    View(RenderState *st, QGraphicsScene *scene, QWidget *parent = nullptr);

  signals:
    void depthZoomRequested(double factor, double anchorDepth, int viewportY);
    void gapZoomRequested(double factor);
    void viewportResized();                 // panel 在 autofit 时 refit
    void hoverChanged(const QString &text); // 空串 = 离开
    void columnClicked(int index);

  protected:
    void wheelEvent(QWheelEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;

  private:
    RenderState *m_st;
};

} // namespace wellsectionui
