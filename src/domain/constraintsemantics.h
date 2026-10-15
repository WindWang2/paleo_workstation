// 层：数据
#pragma once

#include <QPointF>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QVector>

#include <optional>

// 单因素约束的统一语义解析 + 方法能力判定（方向 singlefactor-deep P1-A）。
//
// 唯一事实源：store 行（type / params_json / schema_version / 几何部件）
// → ConstraintSemantics（地质语义）→ ConstraintUseDecision（当前方法是否
// 真的消费、为何不消费）。算法侧 constraintparse.cpp 与 UI/工作流侧计划
// 都调用本文件，不各自数字符串；引擎回执（QC constraint_receipts）由引擎
// 按实际消费独立产出，测试断言二者一致。
//
// 无 QGIS、无 QtWidgets：几何以部件点列传入（调用方用 QgsGeometry 拆部件）。
namespace paleo::singlefactor
{

// ---- 语义 ---------------------------------------------------------------

// 上游 constraint_semantics.py：BLK_MODE 去空白小写；非阻断词表之外一律
// full_block（含未知词与缺省）。
QString normalizeBlockModeToken( const QString &mode );
bool isFullBlockModeToken( const QString &mode );

struct ConstraintSemantics
{
  // false：params_json 本身非法（解析失败/版本过新/语义缺失或未知/数值字段
  // 非数值）。本地方向/结构/Surfer 引擎对此整次拒绝（抛错），旧 IDW 逐条忽略。
  bool valid = true;
  QString error;
  bool fromParams = false;
  // params_json.semantic 或 type 列推导出的原始语义 token（重分类之前）。
  QString declaredSemantic;
  // 有效语义 token：hard_barrier | direction_guide | interpretive_boundary |
  // contour_stop | cartographic_detour。空 = 被忽略（见 ignoredReason）。
  // hard_barrier + blockMode=display_only 在默认选项下重分类为 contour_stop。
  QString semantic;
  bool enabled = true;
  QString blockMode = QStringLiteral( "full_block" ); // 原文（去首尾空白）
  // disabled | non_blocking | unknown_type | unknown_type:<type>
  QString ignoredReason;
  double ratio = 8;
  double influenceRadius = 0;
  double coreRadius = 0;
  double softStrength = 0.35;
  double softRadius = 0;
  double displayBuffer = 0;
  double cartographicBuffer = 0;
  QString unit;
};

struct ConstraintSemanticsOptions
{
  // 结构面引擎需要完整上游 barriers：停用行保留 enabled=false，hard_barrier
  // 的非阻断 blockMode 原文保留不重分类（见 constraintparse.h 同名选项）。
  bool keepDisabled = false;
  bool keepNonBlockingBarriers = false;
};

// type 列 → 语义 token（break_line→hard_barrier，direction_line→direction_guide，
// 新语义词原样）；不认识返回空。
QString semanticTokenFromTypeColumn( const QString &type );
bool isKnownSemanticToken( const QString &token );

// params_json 非空时以其为准（schemaVersion<=1，schema 列作缺省）；否则按 type 列。
ConstraintSemantics resolveConstraintSemantics( const QString &type, const QString &paramsJson,
                                                const QVariant &schemaColumn,
                                                const ConstraintSemanticsOptions &options = {},
                                                const QString &featureId = {} ); // 仅用于报错文案

// ---- 几何 ---------------------------------------------------------------

struct ConstraintPartCheck
{
  // 空 = 可用。short_line（<2 顶点）| zero_length（顶点全重合）|
  // self_intersecting（非相邻线段相交或相邻线段共线回折）。
  QString issue;
  bool duplicateVertices = false; // 连续重复顶点：引擎跳过零长段，仅告警
  double length = 0;
  QRectF bounds;
};

// 单个线部件的有效性。闭合线（首尾相接）不算自交。
ConstraintPartCheck checkConstraintPart( const QVector<QPointF> &part );

// 折线是否与矩形（含边界）有公共点：线段裁剪判定，不是包围盒近似。
bool polylineTouchesRect( const QVector<QPointF> &part, const QRectF &rect );

// ---- 方法能力 -----------------------------------------------------------

// 引擎对单条约束的实际作用。
// hard_barrier：数值硬隔断（网格连通或有限断层绕行）；direction_guide：
// 逐线局部各向异性；global_direction：旧 IDW 的全域长度加权平均方向；
// soft_boundary：数值软边界（跨界权重衰减）；hull_clip：旧 IDW 凸包裁剪。
struct ConstraintMethodContext
{
  QString methodId; // local_direction_idw | local_direction_kriging | surfer_idw | structural_idw | legacy | kriging | sgs | cokriging
  QString horizon;  // 非空时校验行 horizon
  std::optional<QRectF> domain; // 计算域（输出网格范围）；未知时不判域
  double legacyAnisoRatio = 2.0; // paleo_constraint_idw ANISO_RATIO（引擎缺省 2）
  bool structuralEnableBarriers = true;
  bool structuralEnableDirections = true;
  bool structuralInterpretiveStrengthPositive = true;
};

// 规范化方法 id：idw → legacy；未知返回空。
QString canonicalConstraintMethod( const QString &methodId );
// 该方法是否读取约束图层（地统 kriging/sgs/cokriging = false）。
bool methodReadsConstraints( const QString &methodId );
// 给 UI 的一行能力说明（中文），未知方法返回空。
QString methodConstraintCapabilityNote( const QString &methodId );
// 该方法遇到非法 params_json 是否整次拒绝（而不是逐条忽略）。
bool methodRejectsInvalidParams( const QString &methodId );
// 该方法引擎读约束时用的解析选项（structural_idw 保留停用行与非阻断原文）。
// decideConstraintPart 的 semantics 必须用本选项解析，计划才与引擎同口径。
ConstraintSemanticsOptions semanticsOptionsForMethod( const QString &methodId );

struct ConstraintUseDecision
{
  QString id;           // 多部件为 <id>#<k>，与引擎 stableId 一致
  QString featureId;    // 原要素 id
  QString storedType;
  QString semantic;     // 有效语义；忽略时为空
  QString blockMode;
  bool enabled = false;
  bool paramsValid = true;
  bool geometryValid = false;
  bool inHorizon = true;
  bool inDomain = true;   // domain 未知时为 true
  bool domainKnown = false;
  bool supported = false; // 当前方法支持该语义
  bool consumed = false;  // 当前方法实际数值消费
  bool cartographicOnly = false;
  bool blocking = false;  // 当前方法会因此整次拒绝运行
  QString role;           // consumed 时非空，见 ConstraintMethodContext 注释
  QString reason;         // !consumed 时非空（机器可读 token[:detail]）
  QStringList warnings;
  QVariantMap toVariant() const;
};

// 单部件判定：semantics + 部件几何 + 方法上下文 → 决策。
ConstraintUseDecision decideConstraintPart( const QString &featureId, const QString &rowHorizon,
                                            const QString &storedType, const ConstraintSemantics &semantics,
                                            const QVector<QPointF> &part, int partIndex, int partCount,
                                            const ConstraintMethodContext &context );

// 非线几何（点/面/空）：给出 not_line / empty_geometry 决策；旧 IDW 把未知
// type 的任何几何当凸包裁剪（hull_clip），本函数如实反映。
ConstraintUseDecision decideNonLineConstraint( const QString &featureId, const QString &rowHorizon,
                                               const QString &storedType, const ConstraintSemantics &semantics,
                                               bool emptyGeometry, const ConstraintMethodContext &context );

// 汇总：consumed id 列表、未消费 {id,reason} 列表、按角色计数。
QVariantMap summarizeConstraintDecisions( const QVector<ConstraintUseDecision> &decisions );

} // namespace paleo::singlefactor
