// 层：数据
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include "../domain/deviationsurvey.h"

class DataCatalog;

// 一口井的已决 well_log 文件集，以及跨文件曲线并集。
// 文件本体仍在 LAS；这里只读 ~C 头（LasParser::parseHeader），不读数据体、不写 catalog
// ——例外：readCurveTvd 是按列读数据体的读值面（goal/well-trajectory 轮3），
// 溯源/别名协议与其余头扫描面不动。调用方在 catalog 所属线程使用。
// catalog 为空、未打开，或 wellId 为空 → 空结果。

struct WellLogFile
{
  QString assetId;
  QString versionId;
  QString path;            // resolvedVersionPath（受管/外链统一）
  bool isPrimary = false;
  int ordinal = 0;
  QStringList curveNames;  // parseHeader 的 ~C 列序，curves[0] 为深度索引
};

struct WellCurveRef
{
  QString mnemonic;         // 并集名：无冲突用原名；跨文件重名时非主文件为 <名>@<basename>
  QString sourceVersionId;
  QString path;
  int column = -1;          // ~C 列号（0 是深度索引，不会出现在并集里）
  bool canonical = false;   // 仅主文件上保留原名的那一列
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
};
