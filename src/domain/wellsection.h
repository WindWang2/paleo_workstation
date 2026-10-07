// 层：数据
#pragma once
#include "domain/deviationsurvey.h"
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

// 岩性段来源（方向 69）：解释 = 工程解释成果（catalog 资产，经 provider
// 通道按井消费）；推断 = GR 截断等算法推断（渲染期回落面）。题注与分色
// 按来源如实区分——推断不混充解释。
enum class LithoSource { Interpreted, Inferred };

// 岩性段：litho 为词面（解释段如「细砂岩」，视图按词面取工程图式花纹；
// 推断段为 provider 词面常量的砂/泥词），深度语义与其它深度字段一致——
// MD 记值，TVD 域经 Well::tvdOf 换算。provenance 为来源标注（题注级，
// 如「welllogfacies 测试微相 v1」）；空 = 无标注（题注回落「解释」）。
struct LithoSegment {
  double topMd = 0;
  double baseMd = 0;
  QString litho;
  LithoSource source = LithoSource::Interpreted;
  QString provenance;
};

// 解释岩性通道 provider 接口（方向 69）：按井问答岩性段，段自带
// source/provenance。workflow 的 catalog provider（解释资产按井链接消费）
// 与视图期回落 provider 各实现一本。
class WellLithologyProvider {
public:
  virtual ~WellLithologyProvider() = default;
  virtual QVector<LithoSegment> lithologyFor(const QString &wellId) const = 0;
  virtual QString sourceLabel() const = 0; // 来源标注词（如「GR」「catalog」）
};

// 深度显示域：MD = 井深原样；TVD = 真垂深（井斜换算）。井数据永不因
// 域改写——换算只发生在显示映射（拉平不变量的延伸）。
enum class DepthDomain { MD, TVD };

// TVD 域如实性三态（方向 69 语义对齐）：
//   Surveyed     — survey 有效（含纯垂井表）；
//   NoSurvey     — 无 survey 链接：几何按直井恒等（= 按 MD 绘制），但
//                  TVD 语义不可用——必须如实标注「无测斜」，不得静默冒充；
//   BrokenSurvey — 资产在但不可解析：TVD 域不出几何（NaN，不伪造）。
enum class TvdStatus { Surveyed, NoSurvey, BrokenSurvey };

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
  QVector<LithoSegment> litho;   // 解释岩性段（catalog 资产，可空 = GR 回落）
  QVector<ImageAnchor> images;   // 图片道锚（岩心/薄片照片，md 升序）
  std::optional<TimeDepth> timeDepth;
  // 井斜轨迹（catalog trajectory 角色）。无链接 = 直井几何（TVD≡MD，
  // 按 MD 绘制不变）；surveyError 非空 = 资产在但不可解析——TVD 域该井
  // 不出几何。可用性三态见 tvdStatus()（方向 69：无链接如实标无测斜）。
  std::optional<paleo::WellDeviationSurvey> survey;
  QString surveyError;
  bool hasCoordinates() const; // 有限 x && y
  double topMd(const QString &name) const;            // 精确匹配；缺失 → NaN
  const Curve *curve(const QString &mnemonic) const;  // 大小写不敏感；缺失 → nullptr
  // TVD 域可用性三态（survey 有效 / 无链接 / 坏表）。
  TvdStatus tvdStatus() const {
    if (!surveyError.isEmpty())
      return TvdStatus::BrokenSurvey;
    return survey ? TvdStatus::Surveyed : TvdStatus::NoSurvey;
  }
  // MD → TVD：直井恒等；井斜表坏 → NaN（调用方如实标，不得伪造）。
  double tvdOf(double md) const;
  // TVD → MD（井斜反解，契约 #126）；直井恒等；坏表 → NaN。
  double mdOf(double tvd) const;
  // TVD 域可出几何（无链接按直井恒等绘制；坏表 = false 不出几何）。
  bool tvdDisplayable() const { return tvdStatus() != TvdStatus::BrokenSurvey; }
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

// 井距模式：等距 = 均一缝宽；比例 = 相邻井真实地图距离加权分摊预算。
// 契约 v2（缺坐标井不参与比例轴）：任一端缺坐标的缝不进比例分摊——
// 固定按等距缝宽画；已知距离段分摊剩余预算（全缺/零距退化等距）。
// 未定位井名录由调用方从 wells 汇总（诚实面：谁没参与一目了然）。
enum class SpacingMode { Equal, Proportional };
// n 井 → n−1 缝宽：等距全 totalGap/(n−1)；比例按上述契约，逐缝夹取
// [minGap, maxGap]。井数 <2 → 空。
QVector<double> gapWidthsFor(const QVector<Well> &wells, SpacingMode mode,
                             double totalGap, double minGap, double maxGap);
// 比例模式下未定位（缺坐标）井名（井序）；等距模式恒空。
QStringList unpositionedWellNames(const QVector<Well> &wells,
                                  SpacingMode mode);

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

// 基准面：显示深 = 域深 − datumOffset（井数据永不因模式改写）。
// Depth = 井口起算原样；Elevation = 补心海拔归零（各井按 kb 挂齐，
// kb 缺省 0 时与 Depth 等价）；Flatten = 指定标志层顶归零（其余层按
// 相对高程重排）。模式切换仅改视图偏移与轴标签——拉平不变量的落点。
// 域参数：TVD 域的偏移在垂深空间取值（Flatten 取 tvdOf(顶)，Elevation
// 取 kb —— 海拔垂深 = kb − TVD）。
enum class DatumMode { Depth = 0, Elevation = 1, Flatten = 2 };
struct Datum {
  DatumMode mode = DatumMode::Depth;
  QString flattenTop; // Flatten 模式的基准层名（空 → 视作 Depth）
  bool operator==(const Datum &o) const {
    return mode == o.mode && flattenTop == o.flattenTop;
  }
  bool operator!=(const Datum &o) const { return !(*this == o); }
};
double datumOffset(const Well &w, const Datum &d,
                   DepthDomain domain = DepthDomain::MD);
QString datumLabel(DatumMode mode,
                   DepthDomain domain = DepthDomain::MD); // 轴/表头用

struct DepthWindow {
  double top = 0.0;
  double base = 100.0;
};
// 显示深度窗口（显示深 = 域深 − 基准面偏移）：各井 [首顶, 末顶] 的并集；
// 全井无顶 → 有限曲线深度范围的并集；仍无 → {0,100}。两端外扩
// max(5 m, 4% 跨度)，保证 top < base（最小 1 m）。TVD 域按 tvdOf 换算
// 后取并（坏表井 NaN 段不进窗——不下拽邻居）。
DepthWindow depthWindow(const QVector<Well> &wells, const QString &flattenTop);
DepthWindow depthWindow(const QVector<Well> &wells, const Datum &datum,
                        DepthDomain domain = DepthDomain::MD);

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

// GR 截断推断 provider（domain 内置回落面，方向 69）：包装 inferSandShale
// 产 Inferred 段——构造时持单井 GR 曲线，lithologyFor 的 wellId 忽略。
// 词面与 LithoInterval::sand 一一对应：sandWord()=「砂」/shaleWord()=「泥」
// （视图按词面还原砂/泥二分绘，不混入解释词表花纹）。渲染期回落专用——
// workflow 不预挂推断段（保持渲染期回落架构）。
class GrCutoffLithologyProvider : public WellLithologyProvider {
public:
  GrCutoffLithologyProvider(const Curve &gr, double cutoff,
                            double minThicknessM = 0.5,
                            const QString &sourceName = QLatin1String("GR"));
  QVector<LithoSegment> lithologyFor(const QString &wellId) const override;
  QString sourceLabel() const override { return m_sourceName; }
  static QString sandWord() { return QStringLiteral("砂"); }
  static QString shaleWord() { return QStringLiteral("泥"); }

private:
  QVector<LithoInterval> m_intervals;
  QString m_sourceName;
};

// 层位井深表（导出 CSV 用）：每井每顶一行 [井名, 顶名, MD]（TVD 域追加
// TVD 列——坏表井留空，不伪造）。基准面模式只进首行标记（井名, 顶名两列
// 后附 datumLabel 列），井深数值不随模式变——模式切换前后 MD 列逐行相等
// 是拉平不变量。
struct TopsTable {
  QStringList header;
  QVector<QStringList> rows;
  QString csv() const; // UTF-8；含逗号/引号/换行的格加引号转义
};
TopsTable topsTable(const QVector<Well> &wells, const Datum &datum,
                    DepthDomain domain = DepthDomain::MD);

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

// 显示深 displayDepth 在井间位置 f（0=A，1=B）的 TWT：两端各自换回 MD
// 后 MD→TWT 再按 f 线性；任一缺时深/坏表/NaN → NaN。offA/offB 为当前
// 域的基准面偏移；domain 决定 displayDepth→MD 的反解（TVD 走井斜）。
double gapTwtMs(const Well &a, double offA, const Well &b, double offB,
                double f, double displayDepth,
                DepthDomain domain = DepthDomain::MD);

} // namespace wellsection

Q_DECLARE_METATYPE(QVector<wellsection::Well>)
Q_DECLARE_METATYPE(wellsection::SeismicStrip)
Q_DECLARE_METATYPE(wellsection::DepthDomain)
Q_DECLARE_METATYPE(wellsection::SpacingMode)
