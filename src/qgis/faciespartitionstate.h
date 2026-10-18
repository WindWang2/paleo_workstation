// 层：QGIS 封装
// 相面拓扑跨层状态契约（#535）+ 冻结覆盖范围的版本持久化（#540 子项 2）。
//
// #535：相面拓扑状态此前用「字符串键的 QGIS layer property」充当跨层通道——键名
// 以裸字面量散落在 QGIS 封装 / 功能 / 视图三层（faciespartitioneditor.cpp、
// mappingworkbench.cpp、editingtoolbar.cpp），任一处拼写不一致即静默改变拓扑
// 编辑或提交拒绝语义，且无编译期保护。本头收敛为具名键 + 访问器：三层只经
// 访问器读写，禁止裸字符串键跨层传递状态。
//
// #540 子项 2：冻结覆盖范围（GEOS 序列化的 WKB base64）原本钉进 catalog 版本
// extra，数据层契约耦合 QGIS/GEOS 序列化格式。改为与工作副本同目录的 sidecar
// 文件（随 artifacts/working/<draft>/ 一并回收），extra 里只留工程相对路径与
// 可读摘要；旧字段 partition_footprint_wkb 仅作读取兼容（loadFootprint 兜底），
// 既有项目的版本不得因此读不出来。
#pragma once
#include <QFileInfo>
#include <QString>
#include <QVariant>
#include <QVariantMap>

class QObject;
class QgsVectorLayer;

namespace FaciesPartitionState {

// ---- 具名键（原裸字面量；他处不得再写字符串） ------------------------------
inline constexpr const char *kRequireTopology = "paleo/requireFaciesTopology";
inline constexpr const char *kSchema = "paleo/faciesPartitionSchema";
inline constexpr const char *kFrozenFootprint = "paleo.faciesPartitionFootprint";
inline constexpr const char *kOperation = "paleo.partitionOperation";
inline constexpr const char *kEvidenceFailure = "paleo.evidenceEditFailure";
inline constexpr const char *kEvidenceFailures = "paleo.evidenceEditFailures";
inline constexpr const char *kCommitRejected = "paleo.partitionCommitRejected";
inline constexpr const char *kWorkbenchBinding = "paleo.workbenchBinding";

// ---- 版本 extra 键（#540 子项 2） -----------------------------------------
inline constexpr const char *kFootprintPathKey = "partition_footprint_path";
inline constexpr const char *kFootprintSummaryKey = "partition_footprint";
inline constexpr const char *kLegacyFootprintWkbKey = "partition_footprint_wkb";

// ---- 图层侧访问器 ----------------------------------------------------------
bool isPartition(const QgsVectorLayer *v);
void setRequireTopology(QgsVectorLayer *v, bool on);
QVariantList schema(const QgsVectorLayer *v);
void setSchema(QgsVectorLayer *v, const QVariantList &rows);
// 冻结覆盖范围（base64 WKB）。未冻结时 frozenFootprint() 为空串。
QString frozenFootprint(const QgsVectorLayer *v);
bool hasFrozenFootprint(const QgsVectorLayer *v);
void setFrozenFootprint(QgsVectorLayer *v, const QString &base64Wkb);
QString operation(const QgsVectorLayer *v);
void setOperation(QgsVectorLayer *v, const QString &kind);
QString evidenceFailure(const QgsVectorLayer *v);
void setEvidenceFailure(QgsVectorLayer *v, const QString &reason);
QVariantMap evidenceFailures(const QgsVectorLayer *v);
void setEvidenceFailures(QgsVectorLayer *v, const QVariantMap &failures);
bool commitRejected(const QgsVectorLayer *v);
void setCommitRejected(QgsVectorLayer *v, bool rejected);

// ---- 观察器侧访问器（MappingFeatureIdentity::Observer 上的工作台绑定）------
QObject *workbenchBinding(const QObject *observer);
void setWorkbenchBinding(QObject *observer, QObject *workbench);

// ---- 冻结覆盖范围的 sidecar 契约 ------------------------------------------
// working_path（extra 里登记的 "artifacts/working/<draft>/features.gpkg"）
// → 同目录 sidecar。绝对/相对路径都按原样拼接，调用方再经 QDir(projectDir)
// 解析。
inline QString footprintSidecarPath(const QString &workingPath)
{
  return QFileInfo(workingPath).path() + QStringLiteral("/partition-footprint.wkb");
}

// 写 sidecar（base64 WKB → 原始字节）并返回应登记进 version.extra 的条目：
// 路径 + 可读摘要（bbox / 面积 / 部件数 / sha256）。空 footprint（零要素相面）
// 不写文件、返回空 map；失败时 *error 留痕并返回空 map。
QVariantMap saveFootprint(const QString &projectDir, const QString &workingPath,
                          const QString &base64Wkb, QString *error = nullptr);

// 按 extra 读回 base64 WKB：先 sidecar，再旧字段 partition_footprint_wkb
// （#540 子项 2 读取兼容）。sidecar 丢失则回落旧字段；两者皆无返回空串，由
// install() 自行重算（与旧字段缺失时的既有行为一致）。
QString loadFootprint(const QString &projectDir, const QVariantMap &extra);

} // namespace FaciesPartitionState
