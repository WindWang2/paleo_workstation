// 层：功能
// workflow/geostatatomcontract.h — I1 地统计原子的严格参数/网格解码与冻结计划。
//
// 三处共用同一契约（ATOMIC_CAPABILITIES §3.1：schema/服务验证/有效参数摘要
// 不各自漂移）：工具 schema（ai/chat/geostatictools）、宿主 capture 解码
// （geostatatomoperation）与本模块。解码只做纯值检查（无 I/O、无线程依赖）：
//   · 参数：未知键拒绝；number 必须有限；integer 必须整值；变差模型封闭
//     词表；NUGGET≥0、SILL>0、RANGE>0、ANISOTROPY≥1、AZIMUTH∈[0,360)；
//   · SEED：十进制字符串（或 ≤2^53 的 JSON 原生整值）→ quint64 精确往返，
//     不经浮点；
//   · 样本因子：factor_id 命中准入清单且与单因素词表逐项一致（单位/字段取
//     词表值；显式 field 只能与登记字段相同）；
//   · 网格：显式 extent + cell_size（样本投影 CRS 米制）。网格覆盖整个
//     extent：cols=ceil(spanX/cell)、rows=ceil(spanY/cell)，north-up，原点为
//     左上像素外角——跨度非像元整数倍时东/南边界最多外扩一个像元；
//   · 资源：在任何大数组/栅格创建前按 checked arithmetic 估算样本/网格/
//     R×cells/暂存磁盘上界并对照有名字的宿主上限。
// 默认值唯一来源是 AlgorithmSpec（validateAndFillParams），本模块只消费其
// effective 输出，不维护第二份默认。
#pragma once

#include <QJsonObject>
#include <QString>
#include <QVariantMap>

#include <cstdint>

namespace paleo::geostat_atom
{

// ---- 有名字、可测试的宿主资源上限（工程准入，不是科学质量阈值）----------
// 样本上限：邻域最近 K（≤64）求解对样本量不敏感，上限主要约束扫描/快照
// 成本（20000 样本 ≈ 1.4 MiB 索引 + ≤1.6 MiB 快照），与 UI 交互链同量级。
constexpr int kGeostatAtomMaxSamples = 20000;
// 网格像元上限：与 UI 工作流闸 kGeostatMaxWorkflowCells 同值（4M 格）。
constexpr qint64 kGeostatAtomMaxCells = 4'000'000;
// SGS 实现数组内存上界：R×cells×8B ≤ 512 MiB（内核自身 4 GiB 硬闸之内的
// 交互档收紧）。
constexpr qint64 kGeostatAtomMaxRealizationBytes = 512LL * 1024 * 1024;
// 私有样本快照文件上界（JSON 编码后字节数）。
constexpr qint64 kGeostatAtomMaxSnapshotBytes = 8LL * 1024 * 1024;
// 网格单边上限（PW-128 栅格维度护栏量级）。
constexpr int kGeostatAtomMaxGridSide = 100000;
// 峰值内存上界：样本 + 网格工作数组 + 实现数组 + 逐格簿记 ≤ 640 MiB
//（上述各上限的最坏组合 ≈ 610 MiB，本闸防它们被单独放宽）。
constexpr qint64 kGeostatAtomMaxPeakBytes = 640LL * 1024 * 1024;
// 暂存磁盘上界：两个 Float32 端口栅格 + 头余量 + 快照 ≤ 64 MiB
//（4M 格时栅格 32 MiB + 快照 ≤1.6 MiB）。
constexpr qint64 kGeostatAtomMaxOutputBytes = 64LL * 1024 * 1024;

enum class GeostatMethod
{
  Kriging,
  Sgs
};

GeostatMethod methodForCapability( const QString &capabilityId, bool *known = nullptr );
QString algorithmIdForMethod( GeostatMethod method );
QString capabilityIdForMethod( GeostatMethod method );

// 冻结网格（north-up，origin=左上像素外角；pixelHeight<0）。
struct GeostatGridPlan
{
  int cols = 0;
  int rows = 0;
  double originX = 0;
  double originY = 0;
  double pixelWidth = 0;
  double pixelHeight = 0;
  QJsonObject toJson() const;
  bool operator==( const GeostatGridPlan & ) const = default;
  // 形状有效（>0 维、pixelWidth>0、pixelHeight<0、有限原点、像元数在上限内）。
  bool valid() const;
  static GeostatGridPlan fromJson( const QJsonObject &json, bool *ok );
};

// 冻结变差模型（sill 为部分基台；值域在解码层把关）。
struct GeostatVariogramPlan
{
  QString model; // spherical | exponential | gaussian
  double nugget = 0;
  double sill = 1;
  double range = 1;
  double anisotropyRatio = 1;
  double azimuthDeg = 0;
  QJsonObject toJson() const;
  bool operator==( const GeostatVariogramPlan & ) const = default;
  bool valid() const;
  static GeostatVariogramPlan fromJson( const QJsonObject &json, bool *ok );
};

// 资源估算（分配前求出，随冻结计划持久化；validateGeostatFrozenPlan 按计划
// 字段重算比对——估算被改即视为计划不可信）。
struct GeostatResourceEstimate
{
  qint64 sampleBytes = 0;      // 样本数组 + 邻域索引
  qint64 gridBytes = 0;        // 估值/方差（或均值/离散）工作数组
  qint64 realizationBytes = 0; // SGS：R×cells×8B
  qint64 scratchBytes = 0;     // 邻域求解工作区 + 逐格簿记
  qint64 peakBytes = 0;        // sample + grid + realization + scratch
  qint64 snapshotBytes = 0;    // 私有样本快照（JSON 上界）
  qint64 outputBytes = 0;      // 暂存磁盘上界：两个 Float32 栅格 + 头余量
  bool operator==( const GeostatResourceEstimate & ) const = default;
  QJsonObject toJson() const;
  static bool fromJson( const QJsonObject &json, GeostatResourceEstimate *out );
};

// sampleCount：准入阶段（物化前）传 kGeostatAtomMaxSamples 作上界，物化后传
// 实际样本数。ok=false 表示维数/实现数非法或 checked arithmetic 溢出。
GeostatResourceEstimate estimateGeostatResources( GeostatMethod method,
                                                  const QVariantMap &effectiveParams, int cols,
                                                  int rows, int sampleCount, bool *ok );
// 估算对照上限：空 = 准入；否则 geostat_*_budget_exceeded。
QString geostatEstimateAdmission( const GeostatResourceEstimate &estimate );

// 冻结样本身份与来源（宿主解析结果，非模型自称）。
struct GeostatSamplePlan
{
  QString versionId;        // 受管 catalog 版本
  QString sourceSha256;     // 版本内容指纹（受管副本校验和）
  QString layerKey;         // manifest_layer_id / layer_id（可空）
  QString association;      // 样本↔单元关联依据：layer_key | version_unit
  QString factorId;         // 单因素词表因子
  QString field;            // 数值字段（词表登记字段）
  QString valueUnit;        // 词表单位
  QString crsAuthId;        // 样本 CRS authid（本地工程网格可空）
  QString crsWktSha256;     // 样本 CRS 规范 WKT 的摘要（全文在快照内）
  QString snapshotPath;     // 工程相对私有快照路径（内容寻址）
  QString snapshotSha256;   // 快照文件 SHA-256（= 快照目录名）
  int sampleCount = 0;
  int skippedRows = 0;
  QJsonObject toJson() const;
  bool operator==( const GeostatSamplePlan & ) const = default;
  bool valid() const;
  static GeostatSamplePlan fromJson( const QJsonObject &json, bool *ok );
};

// 快照的工程相对路径（纯函数；sha256 须为 64 位小写十六进制）。
QString geostatSnapshotRelativePath( const QString &snapshotSha256 );

// 完整冻结计划（FrozenOperation.frozenPlan 的 I1 载荷）。
struct GeostatFrozenPlan
{
  QString schema = QStringLiteral( "paleo.geostat-atom.v1" );
  GeostatMethod method = GeostatMethod::Kriging;
  QVariantMap effectiveParams; // validateAndFillParams 的有效参数
  GeostatVariogramPlan variogram;
  GeostatGridPlan grid;
  quint64 seed = 42;           // SGS
  QVariantMap unit;            // 冻结分析单元身份（scheme/revision/interval/sub）
  GeostatSamplePlan sample;
  GeostatResourceEstimate estimate;
  // plan.sourceVerification{path,versionId} 是 operation retention 已登记的
  // 引用形态（父目录成为 GC 根）：快照随操作记录保留；与 sample.* 同值，
  // fromJson 校验二者一致。
  bool operator==( const GeostatFrozenPlan & ) const = default;
  QJsonObject toJson() const;
  static GeostatFrozenPlan fromJson( const QJsonObject &json, bool *ok );
};

// capture（owner）阶段产出的解码结果：decode 只验证模型可见输入，
// 不做 catalog/DS 解析（那在宿主受信侧）。
struct GeostatDecodedRequest
{
  GeostatMethod method = GeostatMethod::Kriging;
  QVariantMap effectiveParams;
  GeostatVariogramPlan variogram;
  GeostatGridPlan grid;
  quint64 seed = 42;
  QString sampleRef;   // inputs.sample_ref（待宿主解析）
  QString factorId;    // inputs.factor_id（已对照词表）
  QString factorUnit;  // 词表单位
  QString factorField; // 词表登记字段
  QVariantMap unitSelection; // inputs.unit_selection（形状已验）
  QString sourceOperationId; // inputs.source_operation_id（compute-only 继承）
  GeostatResourceEstimate estimate; // 物化前上界（样本数取上限）
};

// 严格解码。parameters/inputs 来自工具 JSON（不可信）；publishIntent 为空按
// compute-only。返回空 = 成功；否则为机器错误码（geostat_param_* /
// geostat_input_* / geostat_grid_* / geostat_*_budget_exceeded /
// unsupported_intent / publish_requires_source_operation）。
QString decodeGeostatAtomRequest( GeostatMethod method, const QVariantMap &parameters,
                                  const QVariantMap &inputs, const QString &publishIntent,
                                  GeostatDecodedRequest *out );

// 冻结计划可重连性校验（restore 路径）：schema/方法/网格/变差/单元/样本/
// 估算齐全一致且在资源域内。返回空 = 可恢复。
QString validateGeostatFrozenPlan( const GeostatFrozenPlan &plan );

// 模型可见的有界计划事实（不含路径）：prepare 阶段没有 worker 摘要，物化
// 成本/来源/单位/参数只能从这里读回；供工具回包投影。
QJsonObject geostatPlanFacts( const GeostatFrozenPlan &plan );

// quint64 种子的精确解析：十进制字符串（≤20 位，≤UINT64_MAX）。接受 ≤2^53
// 的整值数值（JSON 原生整数在此范围内无损）；其余拒绝。
bool parseGeostatSeed( const QVariant &value, quint64 *seed );

} // namespace paleo::geostat_atom
