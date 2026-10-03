// 层：视图
#pragma once

#include <QColor>
#include <QFont>
#include <QPair>
#include <QImage>
#include <QPainter>
#include <QPair>
#include <QPixmap>
#include <QRectF>
#include <QString>
#include <QVector>
#include <memory>
#include <QCoreApplication>

#include "../../domain/wellcompositemodel.h"

// ui/wellcomposite/ — ResFormStar 风格多井道综合柱状图井道抽象
// （纯数据类型 CurveData/TextInterval/... 已下沉 domain/wellcompositemodel.h）
//
// 规范涵盖 8 类核心井道：
// 1. 标尺道 (DepthScaleTrack): 比例尺、MD/TVD 深度、自适应刻度网格
// 2. 文本道 (TextTrack): 深度区间、试油/取样结论、多行自适应排版
// 3. 地层道 (FormationTrack): 地质分层、顶底深度、地质填色、层名居中
// 4. 岩性道 (LithologyTrack): 标准地质岩性花纹填充（砂岩/泥岩/灰岩/白云岩等矢量图案）
// 5. 取芯道 (CoreTrack): 筒号、进尺/心长矩形框、收获率双色柱
// 6. 图片道 (ImageTrack): 壁心/微观铸体薄片/荧光照片深度等比贴合
// 7. 曲线道 (CurveTrack): 最多 4 根曲线同道合并显示（1-4根），支持连续/离散散点/直方图
// 8. 符号道 (SymbolTrack): 射孔段梳齿符号、产层流体性质符号、压力测试符号

namespace WellComposite
{

enum class TrackType
{
  DepthScale,           // 标尺道
  Text,                 // 文本道
  Formation,            // 地层道
  Lithology,            // 岩性道
  Core,                 // 取芯道
  Image,                // 图片道
  Curve,                // 曲线道（支持1-4根曲线合并显示）
  Symbol,               // 符号道
  StratigraphyCompound, // 地层系统组组合道 (系 | 统 | 组)
  FaciesCompound        // 沉积相组合道 (相 | 亚 | 微，带地质纹理)
};

// 标准地质岩性图案画刷生成器
class LithologyPatternFactory
{
public:
  static QBrush getBrush(const QString &lithoName, const QColor &baseBg = QColor());
  static QPixmap createPatternPixmap(const QString &patternType, const QColor &bg, const QColor &fg);
};

// 标准沉积相地质纹理画刷生成器（支持水下分流河道、河口坝、席状砂、分流间湾、三角洲前缘/平原/前三角洲、浅海陆棚、浊积砂体等纹理）
class FaciesPatternFactory
{
public:
  static QBrush getBrush(const QString &patternTypeOrName, const QColor &baseBg = QColor());
  static QPixmap createPatternPixmap(const QString &patternType, const QColor &bg, const QColor &fg);
};

// 井道抽象基类
class WellTrack
{
public:
  virtual ~WellTrack() = default;

  virtual TrackType type() const = 0;
  virtual QString title() const = 0;
  virtual void setTitle(const QString &title) { m_title = title; }

  virtual qreal width() const = 0;
  virtual void setWidth(qreal w) = 0;

  virtual bool isVisible() const { return m_visible; }
  virtual void setVisible(bool v) { m_visible = v; }

  // D4.10/D4.8 打印/导出开关：不参与打印导出的道在导出引擎中被跳过
  bool isPrintIncluded() const { return m_printIncluded; }
  void setPrintIncluded(bool on) { m_printIncluded = on; }

  // D1.11 道头三行区（标题|刻度|单位）——道宽自适应截断由 paintHeaderChrome 统一处理。
  // 返回空串表示该行不展示（如非曲线道无独立刻度行）。
  virtual QString headerScaleText() const { return QString(); }
  virtual QString headerUnitText() const { return QString(); }

  // D1.9 道悬停 tooltip 文本（曲线名/当前深度值/量程/单位）。
  virtual QString trackToolTip(double depth) const { Q_UNUSED(depth); return title(); }

  // 绘制置顶道头（headerRect 宽高确定，currentDepth 为当前十字准星深度）
  virtual void paintHeader(QPainter &painter, const QRectF &headerRect, double currentDepth) = 0;

  // 绘制道体（bodyRect 对应可见视口区域，topDepth/bottomDepth 为当前视口深度跨度）
  virtual void paintBody(QPainter &painter, const QRectF &bodyRect,
                         double topDepth, double bottomDepth, double pxPerMeter) = 0;

protected:
  // D1.11 标准道头三行区绘制：背景/边框/标题(截断)/刻度行/单位行。
  // scaleText/unitText 允许为空（空行收起，标题行纵向居中补位）。
  void paintHeaderChrome(QPainter &painter, const QRectF &headerRect,
                         const QString &scaleText, const QString &unitText) const;

  QString elideTitle(const QPainter &painter, const QString &text, qreal widthPx) const;

  QString m_title;
  bool m_visible = true;
  bool m_printIncluded = true;
};

// 1. 标尺道 (DepthScaleTrack)
class DepthScaleTrack : public WellTrack
{
public:
  explicit DepthScaleTrack(qreal width = 64.0);
  TrackType type() const override { return TrackType::DepthScale; }
  QString title() const override { return m_title.isEmpty() ? QCoreApplication::translate("WellCompositeTrack", "深度(m)") : m_title; }
  qreal width() const override { return m_width; }
  void setWidth(qreal w) override { m_width = w; }

  void setScaleRatio(const QString &ratioStr) { m_scaleRatio = ratioStr; }
  QString scaleRatio() const { return m_scaleRatio; }

  // D6.3 深度单位切换（显示层换算，数据不动）：空 = 米制原文
  void setDepthUnitLabel(const QString &unitLabel) { m_depthUnitLabel = unitLabel; }
  QString depthUnitLabel() const { return m_depthUnitLabel; }

  // D6.5 TWT 副刻度列：左半列在对应深度标注时深值（无表时清空）
  void setTwtLabels(const QVector<QPair<double, QString>> &twtAtDepth) { m_twtLabels = twtAtDepth; }
  QVector<QPair<double, QString>> twtLabels() const { return m_twtLabels; }

  // D7.4 高对比模式：次级刻度/文字提升至正文对比色
  void setHighContrast(bool on) { m_highContrast = on; }
  bool highContrast() const { return m_highContrast; }

  void paintHeader(QPainter &painter, const QRectF &headerRect, double currentDepth) override;
  void paintBody(QPainter &painter, const QRectF &bodyRect,
                 double topDepth, double bottomDepth, double pxPerMeter) override;

  QString trackToolTip(double depth) const override;

private:
  qreal m_width = 64.0;
  QString m_scaleRatio = QStringLiteral("1:500");
  QString m_depthUnitLabel; // 非空时道头/刻度单位行显示该单位（如 ft）
  QVector<QPair<double, QString>> m_twtLabels; // (depth, twtText)
  bool m_highContrast = false;
};

// 2. 文本道 (TextTrack)
class TextTrack : public WellTrack
{
public:
  explicit TextTrack(const QString &title = QCoreApplication::translate("WellCompositeTrack", "地质描述"), qreal width = 110.0);
  TrackType type() const override { return TrackType::Text; }
  QString title() const override { return m_title; }
  qreal width() const override { return m_width; }
  void setWidth(qreal w) override { m_width = w; }

  void setIntervals(const QVector<TextInterval> &intervals) { m_intervals = intervals; }
  void addInterval(const TextInterval &interval) { m_intervals.append(interval); }
  QVector<TextInterval> intervals() const { return m_intervals; }
  void setKeepTextVisible(bool keep) { m_keepTextVisible = keep; }

  void paintHeader(QPainter &painter, const QRectF &headerRect, double currentDepth) override;
  void paintBody(QPainter &painter, const QRectF &bodyRect,
                 double topDepth, double bottomDepth, double pxPerMeter) override;

private:
  qreal m_width = 110.0;
  QVector<TextInterval> m_intervals;
  bool m_keepTextVisible = false;
};

// 3. 地层道 (FormationTrack)
class FormationTrack : public WellTrack
{
public:
  explicit FormationTrack(const QString &title = QCoreApplication::translate("WellCompositeTrack", "地层单位"), qreal width = 75.0);
  TrackType type() const override { return TrackType::Formation; }
  QString title() const override { return m_title; }
  qreal width() const override { return m_width; }
  void setWidth(qreal w) override { m_width = w; }

  void setIntervals(const QVector<FormationInterval> &intervals) { m_intervals = intervals; }
  void addInterval(const FormationInterval &interval) { m_intervals.append(interval); }
  QVector<FormationInterval> intervals() const { return m_intervals; }

  // D3.1/D3.2 可编辑分层数据（编辑会话直接改写道内区间）
  FormationInterval *intervalAtDepth(float depth);
  int intervalIndexAtDepth(float depth) const;

  QString trackToolTip(double depth) const override;

  void paintHeader(QPainter &painter, const QRectF &headerRect, double currentDepth) override;
  void paintBody(QPainter &painter, const QRectF &bodyRect,
                 double topDepth, double bottomDepth, double pxPerMeter) override;

private:
  qreal m_width = 75.0;
  QVector<FormationInterval> m_intervals;
};

// 4. 岩性道 (LithologyTrack)
class LithologyTrack : public WellTrack
{
public:
  explicit LithologyTrack(const QString &title = QCoreApplication::translate("WellCompositeTrack", "岩性剖面"), qreal width = 75.0);
  TrackType type() const override { return TrackType::Lithology; }
  QString title() const override { return m_title; }
  qreal width() const override { return m_width; }
  void setWidth(qreal w) override { m_width = w; }

  void setIntervals(const QVector<LithologyInterval> &intervals) { m_intervals = intervals; }
  void addInterval(const LithologyInterval &interval) { m_intervals.append(interval); }
  QVector<LithologyInterval> intervals() const { return m_intervals; }

  // D3.4 岩性区间编辑：按深度定位/替换/追加/删除（编辑会话经此改写道内数据）
  int intervalIndexAtDepth(float depth) const;
  bool replaceIntervalAt(int idx, const LithologyInterval &interval);
  void appendInterval(const LithologyInterval &interval) { m_intervals.append(interval); }
  bool removeIntervalAt(int idx);

  QString trackToolTip(double depth) const override;

  void paintHeader(QPainter &painter, const QRectF &headerRect, double currentDepth) override;
  void paintBody(QPainter &painter, const QRectF &bodyRect,
                 double topDepth, double bottomDepth, double pxPerMeter) override;

private:
  qreal m_width = 75.0;
  QVector<LithologyInterval> m_intervals;
};

// 5. 取芯道 (CoreTrack)
class CoreTrack : public WellTrack
{
public:
  explicit CoreTrack(const QString &title = QCoreApplication::translate("WellCompositeTrack", "取心数据"), qreal width = 65.0);
  TrackType type() const override { return TrackType::Core; }
  QString title() const override { return m_title; }
  qreal width() const override { return m_width; }
  void setWidth(qreal w) override { m_width = w; }

  void setBarrels(const QVector<CoreBarrel> &barrels) { m_barrels = barrels; }
  void addBarrel(const CoreBarrel &barrel) { m_barrels.append(barrel); }
  QVector<CoreBarrel> barrels() const { return m_barrels; }

  void paintHeader(QPainter &painter, const QRectF &headerRect, double currentDepth) override;
  void paintBody(QPainter &painter, const QRectF &bodyRect,
                 double topDepth, double bottomDepth, double pxPerMeter) override;

private:
  qreal m_width = 65.0;
  QVector<CoreBarrel> m_barrels;
};

// 6. 图片道 (ImageTrack)
class ImageTrack : public WellTrack
{
public:
  explicit ImageTrack(const QString &title = QCoreApplication::translate("WellCompositeTrack", "岩芯/薄片照"), qreal width = 110.0);
  TrackType type() const override { return TrackType::Image; }
  QString title() const override { return m_title; }
  qreal width() const override { return m_width; }
  void setWidth(qreal w) override { m_width = w; }

  void addItem(const ImageDepthItem &item) { m_items.append(item); }
  void setItems(const QVector<ImageDepthItem> &items) { m_items = items; }
  QVector<ImageDepthItem> items() const { return m_items; }

  void paintHeader(QPainter &painter, const QRectF &headerRect, double currentDepth) override;
  void paintBody(QPainter &painter, const QRectF &bodyRect,
                 double topDepth, double bottomDepth, double pxPerMeter) override;

private:
  qreal m_width = 110.0;
  QVector<ImageDepthItem> m_items;
};

// 7. 曲线道 (CurveTrack) — 支持合并显示 1-4 根曲线
class CurveTrack : public WellTrack
{
public:
  explicit CurveTrack(const QString &title = QCoreApplication::translate("WellCompositeTrack", "测井曲线"), qreal width = 170.0);
  TrackType type() const override { return TrackType::Curve; }
  QString title() const override { return m_title; }
  qreal width() const override { return m_width; }
  void setWidth(qreal w) override { m_width = w; }

  // 曲线合并显示核心：支持 1 至 4 根曲线
  bool addCurve(const CurveData &curve);
  void setCurves(const QVector<CurveData> &curves);
  void clearCurves() { m_curves.clear(); }
  QVector<CurveData> curves() const { return m_curves; }
  int curveCount() const { return m_curves.size(); }
  // D1.7 多曲线组合编辑器：替换单根曲线（量程/单位/色独立可改）；索引越界返回 false
  bool setCurveAt(int idx, const CurveData &curve);
  bool removeCurveAt(int idx);

  // D1.7 重叠网格开关（默认开）
  bool showGrid() const { return m_showGrid; }
  void setShowGrid(bool on) { m_showGrid = on; }

  // D4.11 网格密度：0=无 1=2 等分 2=4 等分 3=10 等分（叠加次网格），随比例尺自适应
  int gridDensity() const { return m_gridDensity; }
  void setGridDensity(int density) { m_gridDensity = qBound(0, density, 3); }

  QString headerScaleText() const override;
  QString headerUnitText() const override;
  QString trackToolTip(double depth) const override;

  void paintHeader(QPainter &painter, const QRectF &headerRect, double currentDepth) override;
  void paintBody(QPainter &painter, const QRectF &bodyRect,
                 double topDepth, double bottomDepth, double pxPerMeter) override;

private:
  qreal m_width = 170.0;
  QVector<CurveData> m_curves; // 最多 4 根曲线
  bool m_showGrid = true;
  int m_gridDensity = 2;
};

// 8. 符号道 (SymbolTrack)
class SymbolTrack : public WellTrack
{
public:
  explicit SymbolTrack(const QString &title = QCoreApplication::translate("WellCompositeTrack", "符号道"), qreal width = 48.0);
  TrackType type() const override { return TrackType::Symbol; }
  QString title() const override { return m_title; }
  qreal width() const override { return m_width; }
  void setWidth(qreal w) override { m_width = w; }

  void setItems(const QVector<SymbolItem> &items) { m_items = items; }
  void addItem(const SymbolItem &item) { m_items.append(item); }
  QVector<SymbolItem> items() const { return m_items; }

  void paintHeader(QPainter &painter, const QRectF &headerRect, double currentDepth) override;
  void paintBody(QPainter &painter, const QRectF &bodyRect,
                 double topDepth, double bottomDepth, double pxPerMeter) override;

private:
  qreal m_width = 48.0;
  QVector<SymbolItem> m_items;
};

// 9. 地层系统组组合道 (StratigraphyCompoundTrack) — 2级道头「地层」下设「系 | 统 | 组」
class StratigraphyCompoundTrack : public WellTrack
{
public:
  explicit StratigraphyCompoundTrack(const QString &title = QCoreApplication::translate("WellCompositeTrack", "地层"), qreal width = 145.0);
  TrackType type() const override { return TrackType::StratigraphyCompound; }
  QString title() const override { return m_title; }
  qreal width() const override { return m_width; }
  void setWidth(qreal w) override { m_width = w; }

  void setIntervals(const QVector<StratigraphyInterval> &intervals) { m_intervals = intervals; }
  void addInterval(const StratigraphyInterval &interval) { m_intervals.append(interval); }
  QVector<StratigraphyInterval> intervals() const { return m_intervals; }

  qreal systemWidth() const { return m_systemWidth; }
  qreal seriesWidth() const { return m_seriesWidth; }
  qreal formationWidth() const { return m_width - m_systemWidth - m_seriesWidth; }
  void setSubColumnWidths(qreal sysW, qreal serW);

  // 按已识别层名查区域地层表生成「系 | 统 | 组」；未识别层名留空系/统，不臆造。
  // formations 为空时不产生任何区间。
  void autoDeriveStratigraphy(const QVector<FormationInterval> &formations, double minDepth, double maxDepth);

  void paintHeader(QPainter &painter, const QRectF &headerRect, double currentDepth) override;
  void paintBody(QPainter &painter, const QRectF &bodyRect,
                 double topDepth, double bottomDepth, double pxPerMeter) override;

private:
  qreal m_width = 145.0;
  qreal m_systemWidth = 38.0;
  qreal m_seriesWidth = 42.0;
  QVector<StratigraphyInterval> m_intervals;
};

// 10. 沉积相组合道 (FaciesCompoundTrack) — 2级道头「沉积相」下设「相 | 亚 | 微」，微相全地质纹理填充
class FaciesCompoundTrack : public WellTrack
{
public:
  explicit FaciesCompoundTrack(const QString &title = QCoreApplication::translate("WellCompositeTrack", "沉积相"), qreal width = 180.0);
  TrackType type() const override { return TrackType::FaciesCompound; }
  QString title() const override { return m_title; }
  qreal width() const override { return m_width; }
  void setWidth(qreal w) override { m_width = w; }

  void setIntervals(const QVector<FaciesInterval> &intervals) { m_intervals = intervals; }
  void addInterval(const FaciesInterval &interval) { m_intervals.append(interval); }
  QVector<FaciesInterval> intervals() const { return m_intervals; }

  // D3.5 相区间编辑：按深度定位（微相⊂亚相⊂相 三级联动校验见 intervaleditor）
  int intervalIndexAtDepth(float depth) const;
  bool replaceIntervalAt(int idx, const FaciesInterval &interval);

  QString trackToolTip(double depth) const override;

  qreal majorWidth() const { return m_majorWidth; }
  qreal subWidth() const { return m_subWidth; }
  qreal microWidth() const { return m_width - m_majorWidth - m_subWidth; }
  void setSubColumnWidths(qreal majW, qreal subW);

  void paintHeader(QPainter &painter, const QRectF &headerRect, double currentDepth) override;
  void paintBody(QPainter &painter, const QRectF &bodyRect,
                 double topDepth, double bottomDepth, double pxPerMeter) override;

private:
  qreal m_width = 180.0;
  qreal m_majorWidth = 48.0;
  qreal m_subWidth = 54.0;
  QVector<FaciesInterval> m_intervals;
};

} // namespace WellComposite
