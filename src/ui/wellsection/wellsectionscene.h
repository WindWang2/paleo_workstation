// 层：视图
#pragma once
#include "domain/wellsection.h"
#include "wellsectionstyle.h"

#include <QGraphicsItem>
#include <QGraphicsView>
#include <QImage>
#include <QWidget>
#include <cmath>
#include <functional>

// ui/wellsection — 剖面渲染核：面板持有一份渲染状态（过滤后的井集、
// 拉平偏移、深度窗口、模板、主题、高亮、地震缝、布局版本），列项与
// 井间项按状态只读重画。视图不做几何变换（identity）——缩放 = 布局
// 参数变化（px/m、井间距），文字始终按设备像素画，保持锐利。
// 版头不是场景项：它是视图上方的一个 QWidget，水平滚动跟随视图。
namespace wellsectionui {

// TVD 域名行角标文本：无测斜/坏表井如实标注「TVD 不可用」（MD 域或
// survey 正常井 → 空串）。版头绘制与面板测试钩子共用同一口径。
QString tvdBadgeText(const wellsection::Well &w,
                     wellsection::DepthDomain domain);
// 深度道题注文本（随基准面模式与深度域：垂深/m、海拔垂深/m…）——
// paintContents 与字符串断言通道共用，导出图随版头自然携带口径词。
QString depthTrackCaption(const wellsection::TrackSpec &tr,
                          wellsection::DepthDomain domain,
                          const wellsection::Datum &datum);
// hover 井柱读数文案（方向 69 R2 收口：单源供 View 的 hoverChanged 与
// 面板测试钩子共用）：井名 + MD 读数 + TVD 域口径（正常井出 TVD 数值；
// 无测斜注明「按井深绘制」——恒等值冒充垂深会与角标抵触；坏表井域反解
// 不出 md（NaN）→ 如实说明无读数）+ 层段名。
QString hoverReadoutText(const wellsection::Well &w,
                         wellsection::DepthDomain domain, double md,
                         const QString &zoneName);

// 岩性道题注文本（方向 69 来源标注）：解释段带资产来源（如
// 「解释·welllogfacies 测试微相 v1」；无 provenance 回落「解释」）；
// 无解释资产 → 「推断·<曲线> 截断」（GR 二分回落，如实不混充解释）。
// paintContents/headerHeight 与面板测试钩子共用同一口径。
QString lithoTrackCaptionText(const wellsection::Well &w,
                              const QString &sourceMnemonic);

// 渲染共享状态（panel 拥有；item/header 持指针只读）。
struct RenderState {
  QVector<wellsection::Well> wells;    // tops 已按模板过滤
  QVector<double> offsets;             // 每井基准面偏移（当前域空间，与 wells 等长）
  wellsection::DepthDomain domain = wellsection::DepthDomain::MD; // 深度显示域
  wellsection::DepthWindow window;     // 显示深度窗口
  wellsection::Datum datum;            // 基准面（深度道轴标签随模式切换）
  wellsection::SectionTemplate tpl;
  wellsection::SectionTheme theme;
  wellsection::SeismicStrip strip;
  bool seismicOn = false;
  QString activeTop, baseTop;          // 高亮地层段（activeTop 空 = 无高亮）
  int selected = -1;                   // 选中井序号（-1 无）
  QStringList zoneOrder;               // orderedTopNames → zoneFill 取色序
  QVector<wellsection::LinkOverride> linkOverrides; // 连线断开/重连（用户编辑）
  QVector<wellsection::FaultTrace> faultTraces; // 断层投绘（数据来自 workflow）
  QVector<double> pathFractions;      // 井路径累计长分数（断层横向映射节点）
  bool faultsOn = false;
  int margin = 16;
  double gapPx = 96.0;                 // 等距缝宽 / 比例模式的平均缝宽
  QVector<double> gapWidths;           // 比例模式逐缝宽；空 = 全 gapPx（等距）
  double pxPerMeter = 1.0;
  quint64 layoutVersion = 0;           // 几何/井集/模板变更递增（缓存键）
  quint64 curveVersion = 0;            // 曲线几何变更递增（px/m、偏移、数据、模板）——gapPx 不属其中
  quint64 stripVersion = 0;            // 地震缝数据变更递增（缓存键）
  quint64 imageVersion = 0;            // 图片道数据变更递增（QPixmap 缓存键）

  double columnWidth() const { return tpl.columnWidth(); }
  double gapWidth(int i) const {
    return gapWidths.isEmpty() ? gapPx : gapWidths.value(i, gapPx);
  }
  double columnLeft(int i) const {
    double x = margin;
    for (int k = 0; k < i; ++k)
      x += columnWidth() + gapWidth(k);
    return x;
  }
  double columnRight(int i) const { return columnLeft(i) + columnWidth(); }
  double minGap() const { return seismicOn ? 140.0 : 48.0; }
  double maxGap() const { return 600.0; }
  double yForDisplay(double displayDepth) const
  {
    return (displayDepth - window.top) * pxPerMeter;
  }
  double displayAtY(double y) const { return window.top + y / pxPerMeter; }
  // 深度域映射（TVD/MD，井斜在 Well 内）+ 基准面偏移 → 显示深。
  // 井序越界/坏表井 → NaN（painter 自行跳过，不伪造几何）。
  double displayOfMd(int well, double md) const
  {
    if (well < 0 || well >= wells.size())
      return qQNaN();
    const double d = domain == wellsection::DepthDomain::TVD
                         ? wells.at(well).tvdOf(md)
                         : md;
    if (!std::isfinite(d))
      return qQNaN();
    return d - offsets.value(well, 0.0);
  }
  // 显示深 → MD（域反解：TVD 经井斜 tvdToMd；直井/坏表井语义同 Well）。
  double mdOfDisplay(int well, double display) const
  {
    if (well < 0 || well >= wells.size())
      return qQNaN();
    const double d = display + offsets.value(well, 0.0);
    const double md = domain == wellsection::DepthDomain::TVD
                          ? wells.at(well).mdOf(d)
                          : d;
    return std::isfinite(md) ? md : qQNaN();
  }
  double yForMd(int well, double md) const
  {
    return yForDisplay(displayOfMd(well, md));
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
    void paintFaciesTrack(QPainter *p, const QRectF &trackRect,
                          const QRectF &exposed);
    void paintImageTrack(QPainter *p, const QRectF &trackRect, const QRectF &exposed);

    RenderState *m_st;
    int m_index;
    // 曲线路径按曲线几何版本缓存（curveVersion 变 → 失效重算；井间距变化
    // 不重算——拉伸只动列位置不动路径）。
    mutable quint64 m_pathVersion = ~quint64(0);
    mutable QHash<quint64, QPainterPath> m_pathCache; // (trackIdx<<8|curveIdx) → path
    // 图片道位图缓存：((wellIdx<<16)|anchorIdx)<<8 | 低 8 位 imageVersion 截断
    // ——QImage(任务线程装载) → QPixmap(GUI 线程) 只转一次，换数据即失效。
    mutable QHash<quint64, QPixmap> m_pixmapCache;
    mutable QHash<quint64, QPainterPath> m_sandCache;
};

// 井间缝（两列之间）：地震 → 层段带 → 高亮带 → 分层连线；带 reason
// 的缝画灰字说明（tooltip 由面板在状态变更时挂，paint 不改自身状态）。
// 连线可拾取：hover 高亮 + 提示（linkHovered），右键请求断开/重连菜单
//（linkMenuRequested——item 非 QObject，回调由面板注入）。
class GapItem : public QGraphicsItem
{
  public:
    GapItem(RenderState *st, int index);
    int index() const { return m_index; }
    void relayout() { prepareGeometryChange(); update(); }
    QRectF boundingRect() const override;
    void paint(QPainter *p, const QStyleOptionGraphicsItem *option,
               QWidget *widget) override;
    // (gap 序号, 顶名, 当前是否连接)；面板弹菜单/执行改接。
    void setLinkMenuCallback(std::function<void(int, const QString &, bool)> cb) {
      m_linkMenu = std::move(cb);
    }
    // 空串 = 离开；text 为面板要显示的 hover 提示。
    void setLinkHoverCallback(std::function<void(const QString &)> cb) {
      m_linkHover = std::move(cb);
    }

  protected:
    void hoverMoveEvent(QGraphicsSceneHoverEvent *e) override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent *e) override;
    void mousePressEvent(QGraphicsSceneMouseEvent *e) override;

  private:
    void paintSeismic(QPainter *p, const QRectF &exposed,
                      const wellsection::SeismicGap &gap);
    // 连线几何：主题 curvedLinks 时走 S 形贝塞尔，否则直线；层段带/
    // 高亮带/高亮边界共用同一路径（填充与线型不走两套几何）。
    QPainterPath linkPath(const wellsection::Link &lk) const;
    // 命中最近连线（阈值内）；空名 = 未命中。坐标为 item 局部。
    QString hitLink(const QPointF &p) const;

    RenderState *m_st;
    int m_index;
    QString m_hoverLink; // hover 中的连线顶名
    std::function<void(int, const QString &, bool)> m_linkMenu;
    std::function<void(const QString &)> m_linkHover;
    // 缝内地震图缓存：(exposed, layoutVersion, stripVersion, theme.id)。
    mutable QImage m_img;
    mutable QRect m_imgRect;
    mutable quint64 m_imgKeyLayout = ~quint64(0), m_imgKeyStrip = ~quint64(0);
    mutable QString m_imgKeyTheme;
};

// 断层投绘覆盖项（全幅）：trace 的 along（井路径长分数）经井节点映射到
// 列中心 x，深度经缝内插基准面偏移映射到 y（与地震缝同一插值口径）。
class FaultOverlayItem : public QGraphicsItem
{
  public:
    explicit FaultOverlayItem(RenderState *st);
    void relayout() { prepareGeometryChange(); update(); }
    QRectF boundingRect() const override;
    void paint(QPainter *p, const QStyleOptionGraphicsItem *option,
               QWidget *widget) override;

  private:
    // 井路径分数 → 场景 x（节点 = 列中心；分数缺失/越界 → NaN）。
    double xForAlong(double along) const;
    // 场景 x 处的基准面偏移（缝两端线性内插）。
    double offsetAtX(double x) const;

    RenderState *m_st;
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
    // 深度道题注文本（随基准面模式与深度域）——导出/截图的口径标签
    // 字符串级断言通道（比像素断言稳；无深度道 → 空串）。
    QString depthCaptionText() const;

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
