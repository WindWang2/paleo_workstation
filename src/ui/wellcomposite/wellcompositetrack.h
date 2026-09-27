#pragma once

#include <QColor>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QPair>
#include <QPixmap>
#include <QRectF>
#include <QString>
#include <QVector>
#include <memory>

// ui/wellcomposite/ — ResFormStar 风格多井道综合柱状图数据结构与井道抽象
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
  DepthScale, // 标尺道
  Text,       // 文本道
  Formation,  // 地层道
  Lithology,  // 岩性道
  Core,       // 取芯道
  Image,      // 图片道
  Curve,      // 曲线道（支持1-4根曲线合并显示）
  Symbol      // 符号道
};

enum class CurveDisplayMode
{
  Continuous, // 连续物理曲线
  Discrete,   // 离散实测散点
  Histogram   // 阶梯/柱状直方图
};

// 单根曲线数据模型
struct CurveData
{
  QString name;
  QString unit;
  float minScale = 0.0f;
  float maxScale = 100.0f;
  bool isLogarithmic = false;
  QColor color = QColor(QStringLiteral("#2E7D32"));
  Qt::PenStyle penStyle = Qt::SolidLine;
  float penWidth = 1.0f;
  CurveDisplayMode mode = CurveDisplayMode::Continuous;
  QVector<float> depths; // 对应测深 (m)
  QVector<float> values; // 对应物理量读数

  bool isEmpty() const { return depths.isEmpty() || values.isEmpty(); }
  float valueAtDepth(float d) const;
};

// 文本道区间数据
struct TextInterval
{
  float topDepth = 0.0f;
  float bottomDepth = 0.0f;
  QString category; // e.g. "取样结论", "试油结论", "地层描述"
  QString text;
  QColor bgColor = QColor(255, 255, 255, 0); // 默认透明
};

// 地层道分层数据
struct FormationInterval
{
  float topDepth = 0.0f;
  float bottomDepth = 0.0f;
  QString name;
  QString code;
  QColor color = QColor(QStringLiteral("#FFE082"));
};

// 岩性道区间数据
struct LithologyInterval
{
  float topDepth = 0.0f;
  float bottomDepth = 0.0f;
  QString lithoName; // e.g. "灰色泥岩", "细砂岩", "生物灰岩"
  QString lithoCode;
  QColor baseColor = QColor(QStringLiteral("#FFF9C4"));
  QString patternType; // "sandstone", "mudstone", "limestone", "dolomite", etc.
};

// 取芯道筒次数据
struct CoreBarrel
{
  QString barrelNo;        // e.g. "C1", "1", "2"
  float topDepth = 0.0f;
  float bottomDepth = 0.0f;
  float cutLength = 0.0f;       // 进尺 (m)
  float recoveredLength = 0.0f; // 心长 (m)
  float recoveryRate = 0.0f;    // 收获率 (%)，如 92.5
  QString description;
};

// 图片道图像项
struct ImageDepthItem
{
  float topDepth = 0.0f;
  float bottomDepth = 0.0f;
  QString imagePath;
  QPixmap pixmap;
  QString caption;
};

// 符号道符号项
enum class SymbolKind
{
  Perforation,   // 射孔段
  OilShow,       // 油层
  GasShow,       // 气层
  WaterShow,     // 水层
  Dry,           // 干层
  OilWater,      // 油水同层
  PressureTest,  // 测压取样点
  PositiveCycle, // 正旋回 (向上变细)
  NegativeCycle  // 反旋回 (向上变粗)
};

struct SymbolItem
{
  float topDepth = 0.0f;
  float bottomDepth = 0.0f; // 若为单点，bottomDepth == topDepth
  SymbolKind kind = SymbolKind::Perforation;
  QString label;
};

// 标准地质岩性图案画刷生成器
class LithologyPatternFactory
{
public:
  static QBrush getBrush(const QString &lithoName, const QColor &baseBg = QColor());
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

  // 绘制置顶道头（headerRect 宽高确定，currentDepth 为当前十字准星深度）
  virtual void paintHeader(QPainter &painter, const QRectF &headerRect, double currentDepth) = 0;

  // 绘制道体（bodyRect 对应可见视口区域，topDepth/bottomDepth 为当前视口深度跨度）
  virtual void paintBody(QPainter &painter, const QRectF &bodyRect,
                         double topDepth, double bottomDepth, double pxPerMeter) = 0;

protected:
  QString m_title;
  bool m_visible = true;
};

// 1. 标尺道 (DepthScaleTrack)
class DepthScaleTrack : public WellTrack
{
public:
  explicit DepthScaleTrack(qreal width = 64.0);
  TrackType type() const override { return TrackType::DepthScale; }
  QString title() const override { return m_title.isEmpty() ? QStringLiteral("深度(m)") : m_title; }
  qreal width() const override { return m_width; }
  void setWidth(qreal w) override { m_width = w; }

  void setScaleRatio(const QString &ratioStr) { m_scaleRatio = ratioStr; }
  QString scaleRatio() const { return m_scaleRatio; }

  void paintHeader(QPainter &painter, const QRectF &headerRect, double currentDepth) override;
  void paintBody(QPainter &painter, const QRectF &bodyRect,
                 double topDepth, double bottomDepth, double pxPerMeter) override;

private:
  qreal m_width = 64.0;
  QString m_scaleRatio = QStringLiteral("1:500");
};

// 2. 文本道 (TextTrack)
class TextTrack : public WellTrack
{
public:
  explicit TextTrack(const QString &title = QStringLiteral("地质描述"), qreal width = 110.0);
  TrackType type() const override { return TrackType::Text; }
  QString title() const override { return m_title; }
  qreal width() const override { return m_width; }
  void setWidth(qreal w) override { m_width = w; }

  void setIntervals(const QVector<TextInterval> &intervals) { m_intervals = intervals; }
  void addInterval(const TextInterval &interval) { m_intervals.append(interval); }
  QVector<TextInterval> intervals() const { return m_intervals; }

  void paintHeader(QPainter &painter, const QRectF &headerRect, double currentDepth) override;
  void paintBody(QPainter &painter, const QRectF &bodyRect,
                 double topDepth, double bottomDepth, double pxPerMeter) override;

private:
  qreal m_width = 110.0;
  QVector<TextInterval> m_intervals;
};

// 3. 地层道 (FormationTrack)
class FormationTrack : public WellTrack
{
public:
  explicit FormationTrack(const QString &title = QStringLiteral("地层单位"), qreal width = 75.0);
  TrackType type() const override { return TrackType::Formation; }
  QString title() const override { return m_title; }
  qreal width() const override { return m_width; }
  void setWidth(qreal w) override { m_width = w; }

  void setIntervals(const QVector<FormationInterval> &intervals) { m_intervals = intervals; }
  void addInterval(const FormationInterval &interval) { m_intervals.append(interval); }
  QVector<FormationInterval> intervals() const { return m_intervals; }

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
  explicit LithologyTrack(const QString &title = QStringLiteral("岩性剖面"), qreal width = 75.0);
  TrackType type() const override { return TrackType::Lithology; }
  QString title() const override { return m_title; }
  qreal width() const override { return m_width; }
  void setWidth(qreal w) override { m_width = w; }

  void setIntervals(const QVector<LithologyInterval> &intervals) { m_intervals = intervals; }
  void addInterval(const LithologyInterval &interval) { m_intervals.append(interval); }
  QVector<LithologyInterval> intervals() const { return m_intervals; }

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
  explicit CoreTrack(const QString &title = QStringLiteral("取心数据"), qreal width = 65.0);
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
  explicit ImageTrack(const QString &title = QStringLiteral("岩芯/薄片照"), qreal width = 110.0);
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
  explicit CurveTrack(const QString &title = QStringLiteral("测井曲线"), qreal width = 170.0);
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

  void paintHeader(QPainter &painter, const QRectF &headerRect, double currentDepth) override;
  void paintBody(QPainter &painter, const QRectF &bodyRect,
                 double topDepth, double bottomDepth, double pxPerMeter) override;

private:
  qreal m_width = 170.0;
  QVector<CurveData> m_curves; // 最多 4 根曲线
};

// 8. 符号道 (SymbolTrack)
class SymbolTrack : public WellTrack
{
public:
  explicit SymbolTrack(const QString &title = QStringLiteral("符号道"), qreal width = 48.0);
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

} // namespace WellComposite
