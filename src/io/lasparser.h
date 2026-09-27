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

class LasParser
{
  public:
    // Reads `path`. On success returns true with curveNames + curves filled;
    // every curve's values vector has the same length (the row count) and
    // curves[0] is the depth index. On failure returns false and sets *error
    // when non-null. Wrap mode (WRAP YES) is not supported.
    static bool parse(const QString &path, QStringList &curveNames,
                      QList<LasCurve> &curves, QString *error = nullptr);

    // §3 绑定规则：测井先读 ~W 的 WELL（身份不取文件名）。只扫 ~V/~W 段。
    // D12：不再返回 UWI——井身份只走 name，uwi/aliases 字段已从实体模型剥离。
    static bool readWellInfo(const QString &path, QString &wellName,
                             QString *error = nullptr);
};
