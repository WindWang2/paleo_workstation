// 层：数据
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QList>

#include "../domain/deviationsurvey.h"
#include "../domain/wellrecords.h" // TimeDepthTable（方向44 TIME 基准对齐）

class DataCatalog;
struct CatalogVersion;
struct LasHeaderInfo;
struct LasCurve;

// 一口井的已决 well_log 文件集，以及跨文件曲线并集。
// 文件本体仍由原有资产/版本管理。目录优先使用受管版本的曲线元数据；
// 旧版本与外链通过 WellLogRead 读头（新 XML/文本回退需完整解析）。
// readCurveTvd 按列读取数据体，沿用来源/别名协议。调用方在 catalog 所属线程使用。
// catalog 为空、未打开，或 wellId 为空 → 空结果。
// well_log 来源由 WellLogRead 分派 LAS / DLIS / LIS / SpreadsheetML / 文本，
// indexBasis 透出各文件基准（"MD"/"TVD"/"TVDSS"/"TIME"/"" 未知）。

struct WellLogFile
{
  QString assetId;
  QString versionId;
  QString path;            // resolvedVersionPath（受管/外链统一）
  bool isPrimary = false;
  int ordinal = 0;
  QStringList curveNames;  // parseHeader 的 ~C 列序，curves[0] 为深度索引
  QString indexBasis;      // 深度基准（方向44）："MD"/"TVD"/"TVDSS"/"TIME"/""（未知）
  QString format;          // "las"/"dlis"/"lis"/"spreadsheetml"/"text"/"unknown"（WellLogRead 分派口径）
};

struct WellCurveRef
{
  QString mnemonic;         // 并集名：无冲突用原名；跨文件重名时非主文件为 <名>@<basename>
  QString sourceVersionId;
  QString path;
  int column = -1;          // ~C 列号（0 是深度索引，不会出现在并集里）
  bool canonical = false;   // 仅主文件上保留原名的那一列
  QString indexBasis;
  QString format;
};

// 一次扫描的告警面。wellLogFiles / wellCurveIndex 共用同一次判定。
struct WellLogWarnings
{
  int count = 0;
  QStringList messages;
};

class WellLogSet
{
public:
  // 新导入的受管文件保存已验证表头，目录/GUI 查询无需再扫描 XML 数据体。
  static bool storedHeader(const CatalogVersion &version, LasHeaderInfo *out);
  // 后台预热外链文件的头缓存（path+size+mtime 键控，进程级）：把
  // parseHeader 的整本 SpreadsheetML 解析从 GUI 首次激活挪到开工程后的
  // 工作线程。目录查询（wellCurveIndex 等）随后命中缓存免重扫。
  static void warmHeaderCache(const QStringList &paths);
  // 仅转换已声明的深度单位，不把 TIME/TVD/TVDSS 或新格式未知轴冒充 MD。
  static bool mdAxisInMetres(const WellCurveRef &ref, const QList<LasCurve> &curves,
                            QVector<double> *axis, QString *error = nullptr);
  // 已决 well_log，按 ordinal 升序，其次 versionNumber，再次 versionId。
  // 头解析失败或路径缺失的链接不进入结果，记入 warnings。
  static QVector<WellLogFile> wellLogFiles(const DataCatalog *catalog,
                                           const QString &projectDir,
                                           const QString &wellId,
                                           WellLogWarnings *warnings = nullptr);

  // 跨文件并集。不含深度索引列（column 0）。
  // 同名跨文件：主文件上有该列时，主文件列保留原名且 canonical=true，
  // 其余为 <mnemonic>@<completeBaseName>。主文件没有该列，或没有主关联时，
  // 重名列全部加别名，canonical 保持 false（不选举主文件）。
  // 别名仍撞车时再追加 #<versionId>。
  // 同文件同名：全部保留，第一条用原名，其后 mnemonic 改为 <名>#<列号>，并记 warning。
  // 只出现在非主文件、且没有重名的曲线保留原名，canonical=false。
  // 无已决链接 → 空，warnings 不因此增加。catalog 未打开记一条「catalog 未打开」。
  static QVector<WellCurveRef> wellCurveIndex(const DataCatalog *catalog,
                                              const QString &projectDir,
                                              const QString &wellId,
                                              WellLogWarnings *warnings = nullptr);

  // ---- MD/TVD 域读法（goal/well-trajectory 轮3）----
  // 读指定曲线列的数据体，深度列逐点经测斜轨迹映射 TVD（多文件井每条 ref
  // 自带 path/versionId——逐文件各自映射天然成立）。survey 为空指针或无效
  // → tvd 恒等 md（直井显式语义，调用方决定是否传表）。深度列单位 ft 自动
  // 折米；未知单位如实报错不猜。NaN 样值保留（缺失段不插值是消费面契约）。
  struct WellCurveTvdSamples
  {
    QString path;
    QString versionId;
    int column = -1;
    QVector<double> md;    // 米
    QVector<double> tvd;   // 米（survey 空时 ≡ md）
    QVector<double> values;
  };
  static bool readCurveTvd(const WellCurveRef &ref,
                           const paleo::WellDeviationSurvey *survey,
                           WellCurveTvdSamples *out, QString *error = nullptr);

  // ---- 深度基准对齐（方向 44）----
  // TIME 基准索引道 → 深度（米）：逐样点经时深表逆插值；indexUnit 是该道
  // 原始单位（只认 "s"/"ms"，未知或空单位拒绝不猜）；样点超表 → NaN
  // （下游重采样按范围外缺失处理，不外推）。表不可用（无样点/无序）→ false
  // + error 给因，indexValues 原样不动。petrophys 并集对齐与测试共用此面。
  static bool timeIndexToDepth(QVector<double> *indexValues,
                               const QString &indexUnit,
                               const TimeDepthTable &td, bool preferMd,
                               QString *error = nullptr);
};
