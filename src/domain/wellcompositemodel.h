// 层：数据
#pragma once

#include <QColor>
#include <QPixmap>
#include <QPair>
#include <QString>
#include <QVector>

// domain/wellcompositemodel — ResFormStar 风格单井综合柱状图的纯数据模型
// （自 ui/wellcomposite/wellcompositetrack.h 下沉：io 解析器产出、ui 井道消费，
// 数据层只问答不管谁来问——道类与图案画刷仍留在 ui）。
//
// 涵盖的道数据类型：
//   曲线道 CurveData（连续/离散散点/直方图，1-4 根合并）
//   文本道 TextInterval / 地层道 FormationInterval
//   地层系统组 StratigraphyInterval / 沉积相 FaciesInterval
//   岩性道 LithologyInterval / 取芯道 CoreBarrel
//   图片道 ImageDepthItem / 符号道 SymbolItem

namespace WellComposite
{

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
  QString unitType; // 原始井道类型（组/段），预测输入不得把组冒充段。
};

// 地层系统组组合道区间数据 (系 | 统 | 组)
struct StratigraphyInterval
{
  float topDepth = 0.0f;
  float bottomDepth = 0.0f;
  QString system;     // 系，如 "新近系" / "古近系"
  QString series;     // 统，如 "中新统" / "渐新统" / "始新统" / "古新统"
  QString formation;  // 组，如 "韩江组" / "珠江组" / "珠海组" / "恩平组" / "文昌组"
  QColor systemColor = QColor(QStringLiteral("#FFF9C4"));
  QColor seriesColor = QColor(QStringLiteral("#FFE082"));
  QColor formationColor = QColor(QStringLiteral("#FFD54F"));
};

// 沉积相组合道区间数据 (相 | 亚 | 微，支持地质纹理填充)
struct FaciesInterval
{
  float topDepth = 0.0f;
  float bottomDepth = 0.0f;
  QString majorFacies; // 相，如 "三角洲相" / "浅海陆棚相" / "湖泊相"
  QString subFacies;   // 亚相，如 "三角洲前缘" / "三角洲平原" / "前三角洲"
  QString microFacies; // 微相，如 "水下分流河道" / "河口坝" / "席状砂" / "分流间湾"
  QString patternType; // 纹理类型，如 "distributary_channel", "mouth_bar", "sheet_sand", "interdistributary_bay" 等
  QColor majorColor = QColor(QStringLiteral("#FFF9C4"));
  QColor subColor = QColor(QStringLiteral("#FFE082"));
  QColor microColor = QColor(QStringLiteral("#FFE082"));
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

struct ComprehensiveWellData
{
  QString wellName;
  double x = 0.0;
  double y = 0.0;
  double minDepth = 0.0;
  double maxDepth = 0.0;

  QVector<CurveData> continuousCurves;
  QVector<CurveData> discreteCurves;
  QVector<LithologyInterval> lithologyIntervals;
  QVector<FormationInterval> formationIntervals;
  QVector<FormationInterval> sandIntervals;
  QVector<TextInterval> textIntervals;
  QVector<SymbolItem> symbolItems;
  QVector<CoreBarrel> coreBarrels;
  // 图片道（catalog core/lab_analysis 角色井附件，depthMd 锚）——paneldata
  // 装载时填；topDepth=bottomDepth=锚深（paintBody 自带最小显示高度）。
  QVector<ImageDepthItem> images;
  QVector<QPair<double, QString>> standardHorizons;
  QVector<StratigraphyInterval> stratigraphyIntervals;
  QVector<FaciesInterval> faciesIntervals;

  bool isEmpty() const;
};

} // namespace WellComposite
