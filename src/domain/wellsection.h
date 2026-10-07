// 层：数据
#pragma once
#include "domain/seismic/timedepthmodel.h"
#include <QImage>
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

// 相代码充填段：交会分类（crossplot 井层段）产物的按井深度段。
// classId 对 12 色Wheel 取色（数据符号色，视图侧解析）。
// 图片道锚（core/lab_analysis 井附件照片）：md 锚定 + 预解码位图
//（编排层任务线程装载；QtGui 于数据层无禁令——纯数据无 UI 语义）。
struct ImageAnchor {
  double md = 0.0;
  QString caption;
  QImage image;
};

struct FaciesSegment {
  double topMd = 0;
  double baseMd = 0;
  int classId = -1;
};

// 剖面上的一口井。tops 按 MD 升序（仅含有限 MD 的分层）。kb 为补心海拔
// （米，海平面以上为正；缺数据 = 0 → 海拔模式退化为井深模式）。
struct Well {
  QString id, name;
  double x = qQNaN(), y = qQNaN();
  double kb = 0.0;
  double totalDepth = qQNaN();
  QVector<Top> tops;
  QVector<Curve> curves;
  QVector<FaciesSegment> facies; // 交会分类井层段（catalog 派生资产）
  QVector<ImageAnchor> images;   // 图片道锚（岩心/薄片照片，md 升序）
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

// 连线改接（用户编辑产物）：井对无序（makeLinkOverride 归一键序），井序
// 重排不失效。overrides 只记显式改动的键；缺省（无条目）= 连接。
struct LinkOverride {
  QString leftWellId, rightWellId, topName;
  bool connected = false;
  bool operator==(const LinkOverride &o) const {
    return leftWellId == o.leftWellId && rightWellId == o.rightWellId &&
           topName == o.topName && connected == o.connected;
  }
};
LinkOverride makeLinkOverride(const QString &aId, const QString &bId,
                              const QString &topName, bool connected);
// 井对+顶名当前是否连接：显式条目优先，缺省 true。
bool linkConnected(const QVector<LinkOverride> &overrides, const QString &aId,
                   const QString &bId, const QString &topName);

// 井距模式：等距 = 均一缝宽；比例 = 相邻井地图距离加权分摊总缝宽
//（缺坐标井段用其余段中位距离，全缺退化等距）。
enum class SpacingMode { Equal, Proportional };
// n 井 → n−1 缝宽：等距全 totalGap/(n−1)；比例按距离分摊，逐缝夹取
// [minGap, maxGap]。井数 <2 → 空。
QVector<double> gapWidthsFor(const QVector<Well> &wells, SpacingMode mode,
                             double totalGap, double minGap, double maxGap);

// 各井在井口连线上的累计长分数（0..1，首井 0 末井 1）；任一井缺坐标 →
// 空（调用方按等距退化）。断层投绘横向映射的节点。
QVector<double> wellPathFractions(const QVector<Well> &wells);

// ---- 栅状图（fence）布点 ----
// 一条剖面 = 有序井 id 集（井口连线即剖面线）。
struct FenceSection {
  QString id;            // "1"、"2"…（store 节 id 前缀 fence-）
  QStringList wellIds;
};
struct FencePlan {
  enum class Status { Ok, MissingCoords, TooFewWells };
  QVector<FenceSection> sections;
  Status status = Status::Ok; // 结构化状态——用户文案由视图层 tr() 出
  bool ok() const { return status == Status::Ok && !sections.isEmpty(); }
};

// 自动布点（最小交叉启发式）：井位 PCA 主轴 (u,v)；按 v 等分
// targetSections 条带，条带内按 u 单调走线（剪草机式：奇偶条带方向
// 交替——相邻条带端点相接、走线互不交叉）；<2 井条带并入邻带。
// 任一井缺坐标 / 井数 <2 / target <1 → status 说明。
FencePlan planFence(const QVector<Well> &wells, int targetSections);

// 平面/图层树选井 → 剖面井序：井位 PCA 主轴投影升序（缺坐标井保持
// 原相对序排末）。wellsWithCoords 只需 id/x/y（tops/曲线不参与）。
QStringList orderWellsByPosition(const QStringList &ids,
                                 const QVector<Well> &wellsWithCoords);

// 断层投绘：along ∈ [0,1] 井路径累计长分数，depth 为深度 m（z 向下正）。
// 由断面 mesh ∩ 井径 curtain 求得（workflow 编排，渲染归视图）。
struct FaultTracePoint {
  double along = 0;
  double depth = 0;
};
struct FaultTrace {
  QString faultName;
  QVector<FaultTracePoint> points;
};

// 顶名按地层序归并：首井顶序为底，后续井把新名插在「该井最近的、已在
// 表中的较浅名」之后（没有则插最前）。确定性输出。
QStringList orderedTopNames(const QVector<Well> &wells);

// 拉平偏移 = 该井 flattenTop 的 MD；空名或缺该顶 → 0。
double flattenOffset(const Well &w, const QString &flattenTop);

// 基准面：显示深 = MD − datumOffset（井数据永不因模式改写）。
// Depth = 井口起算原样；Elevation = 补心海拔归零（各井按 kb 挂齐，
// kb 缺省 0 时与 Depth 等价）；Flatten = 指定标志层顶归零（其余层按
// 相对高程重排）。模式切换仅改视图偏移与轴标签——拉平不变量的落点。
enum class DatumMode { Depth = 0, Elevation = 1, Flatten = 2 };
struct Datum {
  DatumMode mode = DatumMode::Depth;
  QString flattenTop; // Flatten 模式的基准层名（空 → 视作 Depth）
  bool operator==(const Datum &o) const {
    return mode == o.mode && flattenTop == o.flattenTop;
  }
  bool operator!=(const Datum &o) const { return !(*this == o); }
};
double datumOffset(const Well &w, const Datum &d);
QString datumLabel(DatumMode mode); // 轴/表头用：「井深 m」/「海拔 m」/「拉平 m」

struct DepthWindow {
  double top = 0.0;
  double base = 100.0;
};
// 显示深度窗口（显示深 = MD − 基准面偏移）：各井 [首顶, 末顶] 的并集；
// 全井无顶 → 有限曲线深度范围的并集；仍无 → {0,100}。两端外扩
// max(5 m, 4% 跨度)，保证 top < base（最小 1 m）。
DepthWindow depthWindow(const QVector<Well> &wells, const QString &flattenTop);
DepthWindow depthWindow(const QVector<Well> &wells, const Datum &datum);

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

// 层位井深表（导出 CSV 用）：每井每顶一行 [井名, 顶名, MD]。
// 基准面模式只进首行标记（井名, 顶名两列后附 datumLabel 列），井深数值
// 不随模式变——模式切换前后 MD 列逐行相等是拉平不变量。
struct TopsTable {
  QStringList header;
  QVector<QStringList> rows;
  QString csv() const; // UTF-8；含逗号/引号/换行的格加引号转义
};
TopsTable topsTable(const QVector<Well> &wells, const Datum &datum);

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
