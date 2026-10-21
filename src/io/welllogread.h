// 层：数据
#pragma once
#include <QList>
#include <QString>
#include <QStringList>

#include "lasdoc.h" // LasHeaderInfo / LasCurve / LasDoc / LasIssue（井曲线文档契约）

class QByteArray;

// io/ — 井曲线格式分派门面：LAS / DLIS / LIS / SpreadsheetML / text.
// 统一进 LasParser 同构契约（LasHeaderInfo/LasCurve），消费面（welllogset/
// lascache/preview/导入井名提取）无感格式差异。
//
// 分派规则：内容嗅探优先（DlisParser::sniff / LisParser::sniff），
// 扩展名只为已支持格式兜底；未知内容明确返回 Unknown，不猜 XML 结构。
//
// 失败诚实面：所有失败逐条给因（截断/坏段/非法布局），issue 流与
// LasParser::parseDoc 同口径；本门面不吞错、不改写错误文本。

enum class WellLogFormat
{
  Las,
  Dlis,
  Lis,
  SpreadsheetXml,
  Text,
  Unknown,
};

class WellLogRead
{
  public:
    // 内容嗅探 + 扩展名兜底。打不开文件返回 Unknown。
    static WellLogFormat detect(const QString &path);

    static QString formatTag(WellLogFormat f); // stable lower-case format identifier

    // 井身份提取（导入/plan 共用；~W WELL / ORIGIN.WELL-NAME / WELL 分量）。
    // 读不到井名不算失败（返回 true + 空名），与 LasParser::readWellInfo
    // 口径一致；打不开文件返回 false。
    static bool readWellInfo(const QString &path, QString &wellName,
                             QString *error = nullptr);

    // Unified metadata + values parse. XML/text are decoded once. LAS keeps
    // its inexpensive header scan plus the existing optimized data parser.
    // All outputs remain empty on failure.
    static bool parse(const QString &path, LasHeaderInfo &header,
                      QList<LasCurve> &curves, QString *error = nullptr,
                      QList<LasIssue> *issues = nullptr);
    // 已持有 detect 结果的调用方用这版——parse 内部不再重复嗅探（每文件
    // 每轮省 2-3 次文件打开 + 64KB 头读，C-8）。
    static bool parse(const QString &path, WellLogFormat format,
                      LasHeaderInfo &header, QList<LasCurve> &curves,
                      QString *error = nullptr, QList<LasIssue> *issues = nullptr);

    // 表头快照（welllogset 目录扫描）。LAS/DLIS/LIS 保持 header-only；
    // XML/文本验证整份文件后返回快照，避免坏行以成功表头进入正式目录。
    static bool parseHeader(const QString &path, LasHeaderInfo &out,
                            QString *error = nullptr,
                            QList<LasIssue> *issues = nullptr);

    // 整文件数据读（welllogset readCurveTvd 等）。
    static bool parseCurves(const QString &path, QStringList &curveNames,
                            QList<LasCurve> &curves, QString *error = nullptr,
                            QList<LasIssue> *issues = nullptr);

    // lascache 载荷解析（LasDoc 形态）。
    static LasDoc parseDoc(const QString &path, QList<LasIssue> *issues = nullptr);
};
