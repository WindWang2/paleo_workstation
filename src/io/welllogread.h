// 层：数据
#pragma once
#include <QList>
#include <QString>
#include <QStringList>

#include "lasdoc.h" // LasHeaderInfo / LasCurve / LasDoc / LasIssue（井曲线文档契约）

class QByteArray;

// io/ — 井曲线格式分派门面（方向 44）：LAS / DLIS (RP66 v1) / LIS (LIS79)
// 统一进 LasParser 同构契约（LasHeaderInfo/LasCurve），消费面（welllogset/
// lascache/preview/导入井名提取）无感格式差异。
//
// 分派规则：内容嗅探优先（DlisParser::sniff / LisParser::sniff），
// 扩展名兜底（.las/.dlis/.lis，大小写不敏感）；两者都判不出 → 按 LAS 走
// （沿用旧行为：报 LAS 的解析错误比报「未知格式」对用户更有指向性——
// 错误面仍带原文路径）。.be 等未支持格式：分类器不给 well_log 假能力
// （projectclassifier 的 .xls 先例），导入面如实标未支持。
//
// 失败诚实面：所有失败逐条给因（截断/坏段/非法布局），issue 流与
// LasParser::parseDoc 同口径；本门面不吞错、不改写错误文本。

enum class WellLogFormat
{
  Las,
  Dlis,
  Lis,
};

class WellLogRead
{
  public:
    // 内容嗅探 + 扩展名兜底。打不开文件返回 Las（调用方会拿到打开错误）。
    static WellLogFormat detect(const QString &path);

    static QString formatTag(WellLogFormat f); // "las" / "dlis" / "lis"

    // 井身份提取（导入/plan 共用；~W WELL / ORIGIN.WELL-NAME / WELL 分量）。
    // 读不到井名不算失败（返回 true + 空名），与 LasParser::readWellInfo
    // 口径一致；打不开文件返回 false。
    static bool readWellInfo(const QString &path, QString &wellName,
                             QString *error = nullptr);

    // header-only 快照（welllogset 目录扫描）。语义与各格式 parseHeader 一致。
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
