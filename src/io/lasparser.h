// 层：数据
#pragma once
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

#include "lasdoc.h" // LasCurve / LasDoc（视图可用的纯类型门面）

// io/ — minimal LAS 2.x well-log parser (pure Qt, no QGIS dependency).
// Handles the CWLS sections ~V (version/wrap), ~W (well info — the NULL item
// drives NaN mapping), ~C (curve definitions in column order) and ~A (ASCII
// data rows). The first ~C curve is the DEPT index channel.

// 表头快照（header-only 解析的出参）：不触 ~A 数据行——大 LAS 的头部只有
// 几 KB，读它不构成「整文件解析」；消费方先拿曲线名铺 UI，数据行异步补。
struct LasHeaderInfo
{
  QStringList curveNames;   // ~C 列序（curves[0] 是 DEPT 索引道）
  QString wellName;         // ~W WELL（缺省空）
  double nullValue = -999.25; // ~W NULL（CWLS 缺省）
  bool sawAscii = false;    // 头部扫描途中遇到 ~A 段头（数据节存在）
};

class LasParser
{
  public:
    // Reads `path`. On success returns true with curveNames + curves filled;
    // every curve's values vector has the same length (the row count) and
    // curves[0] is the depth index. On failure returns false and sets *error
    // when non-null. Wrap mode (WRAP YES) is not supported.
    static bool parse(const QString &path, QStringList &curveNames,
                      QList<LasCurve> &curves, QString *error = nullptr);

    // header-only 快速入口（T1 LAS 解阻）：只读 ~V/~W/~C，见到 ~A 段头即停
    // ——不解析数据行，代价与文件头部大小成正比（与总行数无关）。语义与
    // parse() 同源：WRAP YES 拒绝、无 ~C 拒绝；~A 缺失不算失败（sawAscii
    // 如实报）。曲线名与 parse() 产出的 curveNames 逐项一致。
    static bool parseHeader(const QString &path, LasHeaderInfo &out,
                            QString *error = nullptr);

    // §3 绑定规则：测井先读 ~W 的 WELL（身份不取文件名）。只扫 ~V/~W 段。
    // D12：不再返回 UWI——井身份只走 name，uwi/aliases 字段已从实体模型剥离。
    static bool readWellInfo(const QString &path, QString &wellName,
                             QString *error = nullptr);
};
