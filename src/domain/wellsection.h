// 层：数据
#pragma once
#include "domain/seismic/timedepthmodel.h"
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtNumeric>
#include <optional>
#include <vector>

// 连井剖面（well correlation section）纯数据核：井/分层/曲线/时深容器 +
// 剖面几何推导（层段、对比连线、顶名归并、拉平偏移、深度窗口、砂泥段、
// 井间地震缝）。不读文件、不碰 catalog——编排层填数，视图层只读。
namespace wellsection {

// 一个分层顶（按 MD 记）。
struct Top {
  QString name;
  double md = qQNaN();
};

// 一条测井曲线：mnemonic 是请求/模板助记名（如 GR），sourceMnemonic 是
// 实际取到的 LAS 列名（如 NGR）。depths 为 MD 米（文件顺序），NaN = 缺段。
struct Curve {
  QString mnemonic;
  QString sourceMnemonic;
  QString unit;
  QVector<float> depths;
  QVector<float> values;
};

// MD 域时深关系：model 为严格表或常速模型，shiftMs 为校正时间平移。
// twtAt(md) = model.DepthToTwtMs(md) + shiftMs；不可换算 → NaN。
struct TimeDepth {
  seismic::TimeDepthModel model;
  double shiftMs = 0.0;
  QString status; // 「时深表」/「常速校正」等
  double twtAt(double md) const;
};

// 剖面上的一口井。tops 按 MD 升序（仅含有限 MD 的分层）。
struct Well {
  QString id, name;
  double x = qQNaN(), y = qQNaN();
  double totalDepth = qQNaN();
  QVector<Top> tops;
  QVector<Curve> curves;
  std::optional<TimeDepth> timeDepth;
  bool hasCoordinates() const; // 有限 x && y
  double topMd(const QString &name) const;            // 精确匹配；缺失 → NaN
  const Curve *curve(const QString &mnemonic) const;  // 大小写不敏感；缺失 → nullptr
};

// 层段：顶界 tops[i].md → 底界 tops[i+1].md，底段以 bottomMd 收。
// 顶界以上不出段；零厚度（顶底同深）跳过；末段仅当 bottomMd > 末顶 MD。
struct Zone {
  QString name;
  double topMd = 0;
  double baseMd = 0;
};
QVector<Zone> zones(const Well &w, double bottomMd);

// 对比连线：两井共有的顶名（精确匹配），按左井 MD 排序，每名一条。
struct Link {
  QString name;
  double leftMd = 0;
  double rightMd = 0;
};
QVector<Link> links(const Well &left, const Well &right);

// 顶名按地层序归并：首井顶序为底，后续井把新名插在「该井最近的、已在
// 表中的较浅名」之后（没有则插最前）。确定性输出。
QStringList orderedTopNames(const QVector<Well> &wells);

// 拉平偏移 = 该井 flattenTop 的 MD；空名或缺该顶 → 0。
double flattenOffset(const Well &w, const QString &flattenTop);

struct DepthWindow {
  double top = 0.0;
  double base = 100.0;
};
// 显示深度窗口（显示深 = MD − 拉平偏移）：各井 [首顶, 末顶] 的并集；
// 全井无顶 → 有限曲线深度范围的并集；仍无 → {0,100}。两端外扩
// max(5 m, 4% 跨度)，保证 top < base（最小 1 m）。
DepthWindow depthWindow(const QVector<Well> &wells, const QString &flattenTop);

// 地层厚度段：顶 = activeTop，底 = baseTop（须 > 顶，否则无底）。
struct Interval {
  double topMd = qQNaN();
  double baseMd = qQNaN();
  bool valid() const;   // 有限 topMd
  bool hasBase() const; // valid && 有限 baseMd && baseMd > topMd
};
Interval formationInterval(const Well &w, const QString &activeTop,
                           const QString &baseTop);

// GR 切砂：value < cutoff 为砂。按升序处理（降序输入先反转）；NaN 深度/
// 数值断段；异类相邻有限样点间以深度中点为界，断边以有限样点深度为界。
// 薄于 minThicknessM 的段并入前一段（段首则并入后一段），再合并相邻同类。
struct LithoInterval {
  double topMd = 0;
  double baseMd = 0;
  bool sand = false;
};
QVector<LithoInterval> inferSandShale(const Curve &gr, double cutoff,
                                      double minThicknessM = 0.5);

// 井间地震缝：reason 非空 = 不可绘（文字填缝）。values 行主序
// [sample*columns + column]，NaN 无效。column 0 = 左井端。
struct SeismicGap {
  QString reason;
  int columns = 0, samples = 0;
  double startMs = 0.0, stepMs = 0.0;
  std::vector<float> values;
  bool valid() const; // reason 空 && 尺寸 > 0 && values 数吻合 && stepMs > 0
  // columnFrac ∈ [0,1] → 最近列；时间向样点间线性；越界/无效 → NaN。
  float sampleAt(double columnFrac, double twtMs) const;
};

// 整条剖面的地震缝集：gaps[i] 位于 wells[i] 与 wells[i+1] 之间。
struct SeismicStrip {
  QVector<SeismicGap> gaps;
  float clip = 1.0f; // 对称显示截幅
  QString status;    // 总体状态/原因（"" = 正常）
  bool anyValid() const;
};

// 有效缝 |v| 的 percentile 截幅（有限值；跨步抽样至 ≤20 万点，
// nth_element 选位）；结果 ≤0 或无值 → 1.0。
float adaptiveClip(const QVector<SeismicGap> &gaps, double percentile = 0.99);

// 显示深 displayDepth 在井间位置 f（0=A，1=B）的 TWT：两端各自
// MD→TWT 后按 f 线性；任一缺时深/NaN → NaN。offA/offB 为拉平偏移。
double gapTwtMs(const Well &a, double offA, const Well &b, double offB,
                double f, double displayDepth);

} // namespace wellsection

Q_DECLARE_METATYPE(QVector<wellsection::Well>)
Q_DECLARE_METATYPE(wellsection::SeismicStrip)
