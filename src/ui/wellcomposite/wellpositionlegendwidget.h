#pragma once

#include <QDialog>
#include <QLabel>
#include <QPainter>
#include <QToolButton>
#include <QWidget>

#include "io/wellcompositexml.h"

// ui/wellcomposite/ — WellPositionLegendWidget:
// 位置显示图例与比例尺动态指示组件 (ResFormStar / QGIS 地质工作站标准规范)
//
// 核心能力：
// 1. 物理线段比例尺图例 (GraphicScaleBar)：真实屏幕厘米/毫米比例尺标尺，与当前缩放倍率和 pxPerMeter() 物理精准匹配；
// 2. 真实比例尺标注文字：动态联动当前 1:200, 1:250, 1:500, 1:1000, 1:2000 等标准比例尺与 (1cm ≈ X.X m) 实物换算；
// 3. 当前显示区域与视口范围标注：精准实时同步 [当前顶深 m ~ 底深 m]，视口跨度 (m) 与全井深度相对百分比；
// 4. 全井位置微缩示意图例 / 导航条 (WellOverviewMiniBar)：可视化全井柱状色带，高亮框选当前视口滑块，支持点击/拖拽即时漫游定位；
// 5. 光标测深与井位坐标标注：实时响应悬停深度；
// 6. 地质与道图例弹窗 (WellLegendDialog)：展示岩性花纹图例、地层色标、曲线样式与解释符号。

namespace WellComposite
{

class WellCompositeCanvas;

// ----------------------------------------------------------------------------
// 物理线段比例尺图例组件 (GraphicScaleBar)
// ----------------------------------------------------------------------------
class GraphicScaleBar : public QWidget
{
  Q_OBJECT

public:
  explicit GraphicScaleBar(QWidget *parent = nullptr);
  ~GraphicScaleBar() override = default;

  void setScaleMetrics(double pxPerMeter, const QString &scaleRatioStr);

  QSize sizeHint() const override { return QSize(120, 26); }
  QSize minimumSizeHint() const override { return QSize(80, 24); }

protected:
  void paintEvent(QPaintEvent *event) override;

private:
  double m_pxPerMeter = 7.559; // 默认 1:500 对应 ~7.559 px/m
  QString m_scaleRatio = QStringLiteral("1:500");
  double m_segmentMeters = 10.0;
  double m_pixelLength = 75.6;
};

// ----------------------------------------------------------------------------
// 全井位置微缩示意图例 / 导航条 (WellOverviewMiniBar)
// ----------------------------------------------------------------------------
class WellOverviewMiniBar : public QWidget
{
  Q_OBJECT

public:
  explicit WellOverviewMiniBar(QWidget *parent = nullptr);
  ~WellOverviewMiniBar() override = default;

  void setDepthRange(double minDepth, double maxDepth);
  void setVisibleRange(double topDepth, double bottomDepth);
  void setFormations(const QVector<FormationInterval> &formations);

  QSize sizeHint() const override { return QSize(180, 22); }
  QSize minimumSizeHint() const override { return QSize(100, 18); }

signals:
  void requestScrollDepth(double depth);

protected:
  void paintEvent(QPaintEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;

private:
  void handleMouseAt(const QPoint &pos);

  double m_minDepth = 0.0;
  double m_maxDepth = 3000.0;
  double m_topDepth = 0.0;
  double m_bottomDepth = 500.0;
  QVector<FormationInterval> m_formations;
  bool m_dragging = false;
};

// ----------------------------------------------------------------------------
// 地质与道图例弹窗 (WellLegendDialog)
// ----------------------------------------------------------------------------
class WellLegendDialog : public QDialog
{
  Q_OBJECT

public:
  explicit WellLegendDialog(const ComprehensiveWellData &wellData, QWidget *parent = nullptr);
  ~WellLegendDialog() override = default;

private:
  void setupUi(const ComprehensiveWellData &data);
};

// ----------------------------------------------------------------------------
// 位置显示图例综合控件 (WellPositionLegendWidget)
// ----------------------------------------------------------------------------
class WellPositionLegendWidget : public QWidget
{
  Q_OBJECT

public:
  explicit WellPositionLegendWidget(QWidget *parent = nullptr);
  ~WellPositionLegendWidget() override = default;

  void updateViewport(double topDepth, double bottomDepth, double span);
  void updateScale(double pxPerMeter, const QString &scaleRatio);
  void updateHoverDepth(double depth);
  void setWellData(const ComprehensiveWellData &data);
  void setDepthRange(double minDepth, double maxDepth);

  GraphicScaleBar *scaleBar() const { return m_scaleBar; }
  WellOverviewMiniBar *miniBar() const { return m_miniBar; }

signals:
  void requestScrollDepth(double depth);

private:
  void openLegendDialog();

  GraphicScaleBar *m_scaleBar = nullptr;
  QLabel *m_lblScaleRatio = nullptr;
  QLabel *m_lblViewportRange = nullptr;
  QLabel *m_lblWellRange = nullptr;
  QLabel *m_lblCursorDepth = nullptr;
  WellOverviewMiniBar *m_miniBar = nullptr;
  QToolButton *m_btnLegend = nullptr;

  ComprehensiveWellData m_wellData;
  double m_minDepth = 0.0;
  double m_maxDepth = 3000.0;
  double m_currentTopDepth = 0.0;
  double m_currentBottomDepth = 500.0;
  double m_currentSpan = 500.0;
  double m_pxPerMeter = 7.559;
  QString m_scaleRatioStr = QStringLiteral("1:500");
};

} // namespace WellComposite