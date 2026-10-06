// 层：数据
#pragma once
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

#include "lasdoc.h" // LasCurve/LasDoc/LasHeaderInfo/LasIssue（井曲线文档契约，
                    // 方向 59 起纯类型门面全在 lasdoc.h；本头只留解析入口）

// io/ — minimal LAS 2.x well-log parser (pure Qt, no QGIS dependency).
// Handles the CWLS sections ~V (version/wrap), ~W (well info — the NULL item
// drives NaN mapping), ~C (curve definitions in column order) and ~A (ASCII
// data rows). The first ~C curve is the DEPT index channel.

// D1.6 解析器错误分类：格式错/编码错/截断/单位缺失分级（LasIssue，与
// LasHeaderInfo/LasCurve/LasDoc 同在 lasdoc.h——解析入口与契约类型分离，
// 见 lasdoc.h 头注释）。

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

    // ---- wave/io-perf-cache D1.4-D1.9：快路径 + 区间查询 + 错误分类 ----

    // 整文件快解析（字节级 ~A 解析；头段经编码嗅探统一解码）。产出与
    // parse() 同构的 LasDoc + 分级 issues。失败时 doc.ok=false、error 有文。
    // 头部段语义（WRAP YES 拒绝 / 无 ~C 拒绝 / 无 ~A 拒绝）与 parse() 一致。
    static LasDoc parseDoc(const QString &path, QList<LasIssue> *issues = nullptr);

    // D1.4 行区间查询：只解析 [rowFrom, rowTo) 的数据行（0-based，含头不
    // 含尾；越界自动夹取），不整文件读入内存——大 LAS 的局部浏览走这里。
    static bool parseRange(const QString &path, qint64 rowFrom, qint64 rowTo,
                           QStringList &curveNames, QList<LasCurve> &curves,
                           QString *error = nullptr, QList<LasIssue> *issues = nullptr);

    // D1.4 深度区间查询：DEPT 列落在 [fromDepth, toDepth] 内的行（DEPT 单
    // 调递增时越过 toDepth 即停——真·按需读段）。曲线名/单位与全量一致。
    static bool parseDepthRange(const QString &path, double fromDepth, double toDepth,
                                QStringList &curveNames, QList<LasCurve> &curves,
                                QString *error = nullptr, QList<LasIssue> *issues = nullptr);

    // #166：深度道单位 → 米的换算系数。米族（M/METER/METRE/METERS/METRES）= 1，
    // 英尺族（FT/F/FEET/FOOT）= 0.3048；空串/其它未知单位 = 0（调用方自定口径）。
    static double depthUnitToMeters(const QString &unit);

    // D1.8 大文件防护：>limit 的文件拒绝整读（parseDoc/parse 返回
    // Oversize Error），流式 parseRange/parseDepthRange 不受限。默认 500MB。
    static qint64 fileSizeLimit() { return s_fileSizeLimit; }
    static void setFileSizeLimit(qint64 bytes) { s_fileSizeLimit = bytes; }

    // 共享头扫描结果（parseDoc/parseRange/parseDepthRange 内部复用）。
    struct SectionMap
    {
        LasHeaderInfo header;
        qint64 asciiDataOffset = -1; // ~A 段头行之后首数据行字节偏移；无 ~A = -1
        bool sawWrapYes = false;
        bool ok = false;
        QString error;
    };
    static bool scanSections(const QByteArray &raw, SectionMap *out, QList<LasIssue> *issues);

  private:
    static qint64 s_fileSizeLimit;
    // 字节级 ~A 行解析核心：从 raw[asciiOffset..] 抽 [rowFrom,rowTo) 行。
    static QList<LasCurve> parseAsciiRows(const QByteArray &raw, qint64 asciiOffset,
                                          const QStringList &names, double nullValue,
                                          qint64 rowFrom, qint64 rowTo, qint64 *rowsTotal,
                                          QList<LasIssue> *issues);
};
