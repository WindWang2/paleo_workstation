// 层：数据
#pragma once
#include <QJsonObject>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

// domain/ — 层序地层格架模型（方向 28「格架先行」）。
// 「格架先行」= 编图正确性前提：先有层序→体系域二级格架，再有标志层与
// 层位归属，最后才谈编图。本文件只描述模型与纯查询面——持久化走
// catalog/frameworkstore（资产 + 受管版本，禁止旁路存储），诊断走
// domain/frameworkdiagnostics，建议算法走 algorithms/frameworksuggester。
//
// 与既有语义的边界（不得破坏）：
//   · sequence_boundary 仍是 catalog 实体类型、「层序界面」语义不变；格架
//     只是在它之上加一层归属描述，不改写实体本身。
//   · mappingHorizons()（domain/mappinghorizons.h）仍是层位序的唯一权威；
//     格架单元的顶/底界面名必须是它的成员，非成员由诊断报
//     BoundaryOutsideSet，而不是悄悄改集合。
//
// 层级约定：level=Sequence（层序，一级，parentId 空）→ level=SystemsTract
//（体系域，二级，parentId 指向层序）。二级单元不继续嵌套。

namespace SequenceFramework
{

inline constexpr int kSchemaVersion = 1;

enum class UnitLevel
{
  Sequence = 1,     // 层序（一级）
  SystemsTract = 2  // 体系域（二级）
};

struct FrameworkUnit
{
  QString id;               // "sfu-1"（持久后稳定，重命名不改 id）
  QString parentId;         // 空 = 一级层序；否则指向父层序单元
  QString name;             // 显示名（如 "SQ1" / "TST"）
  UnitLevel level = UnitLevel::Sequence;
  int ordinal = 0;          // 同级内人工序（浅→深）；相等时按 name 稳定排序
  QString topBoundary;      // 顶界层序界面名（mappingHorizons() 成员）
  QString baseBoundary;     // 底界层序界面名；空 = 该层序最深的体系域
  double thickness = 0.0;   // 工区代表厚度（米）；<=0 = 未填（柱状图按等厚）
  QString colorKey;         // 色带键（保留给视图层，模型只透传）
  QString note;
};

struct MarkerBed
{
  QString id;               // "sfm-1"
  QString name;             // 标志层名
  QString unitId;           // 所属格架单元（可指一级或二级）
  QStringList layerNames;   // 引用的 well_stratification 分层名（引用完整性校验面）
  QString note;
};

struct Framework
{
  int schemaVersion = kSchemaVersion;
  QString name;             // 格架方案名（工区内唯一，缺省 "默认格架"）
  QVector<FrameworkUnit> units;
  QVector<MarkerBed> markers;
  bool isEmpty() const { return units.isEmpty() && markers.isEmpty(); }
};

// ---- 纯查询面（不改模型）----

// 一级单元（parentId 空），按 (ordinal, name) 稳定序。
QVector<FrameworkUnit> rootsOf( const Framework &fw );
// parentId 的直接子单元；parentId 空 = rootsOf。
QVector<FrameworkUnit> childrenOf( const Framework &fw, const QString &parentId );
const FrameworkUnit *unitById( const Framework &fw, const QString &id );
const MarkerBed *markerById( const Framework &fw, const QString &id );
// 单元路径名（"SQ1 / TST"），供诊断与视图定位；未知 id 回空串。
QString unitPath( const Framework &fw, const QString &id );
// 单元深度（层数）：一级=0，二级=1。未知 id 回 -1。
int depthOf( const Framework &fw, const QString &id );
// 叶子单元（无子单元）。层序一旦被体系域细分，编图归属就由体系域承担——
// 诊断与层位映射一律只看叶子，避免「层序与它自己的体系域互相歧义」。
bool isLeaf( const Framework &fw, const QString &id );
QVector<FrameworkUnit> leavesOf( const Framework &fw );

// 与 mappingHorizons() 的双向解析：
//   · 正向 —— 单元 → 它覆盖的层位区间（按 horizons 有序集合，浅→深）；
//     区间 = [topBoundary, baseBoundary)，底界归下一单元；baseBoundary 空
//     或不在集合 → 取到集合末尾。界面不在集合 → 空区间。
//   · 反向 —— 层位 → 覆盖它的单元 id 列表（正常恰 1 个；0 个 = 无归属，
//     >1 个 = 歧义，两者都由诊断面报告，这里如实返回）。
QStringList horizonsCoveredBy( const Framework &fw, const QString &unitId,
                               const QStringList &horizons );
QHash<QString, QStringList> horizonUnitMap( const Framework &fw, const QStringList &horizons );
QStringList unitsCoveringHorizon( const Framework &fw, const QString &horizon,
                                  const QStringList &horizons );

// id 分配：取现有同名前缀的最大序号 +1（删除后不复用旧号，避免历史版本
// 里的 id 指向错位）。前缀分别为 "sfu-" / "sfm-"。
QString nextUnitId( const Framework &fw );
QString nextMarkerId( const Framework &fw );

// 序列化：格架数据走 catalog 资产 + 受管版本，本文件只定义 JSON 形态。
// fromJson 对未知键前向兼容（忽略），对结构错如实报错不静默吞。
QJsonObject toJson( const Framework &fw );
Framework fromJson( const QJsonObject &obj, QString *error = nullptr );
QByteArray toJsonBytes( const Framework &fw );

} // namespace SequenceFramework
