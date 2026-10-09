// 层：数据
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include "outsourceworkbook.h" // WorkbookSheet / parseNumericCell

// io/ — 岩屑录井表读取面（方向 69 第二解释源）。导入侧把「岩屑」目录挂井
// role="cuttings"（dataimport_geodata.cpp 角色词表，roleregistry 注明
// 「岩性道数据源」），文件一井一份（xlsx / SpreadsheetML .xml / 文本表）。
//
// 纯表解析（parseCuttingsSheet）与文件分派（readCuttingsFile）分层：
// 前者全量可测（合成 WorkbookSheet 直调），后者只做扩展名分派与
// 首可解析工作表选择。
//
// 诚实面：缺必需列表头 → ok=false + error 点名缺哪列（不猜列序）；
// 行级坏数据（非数值/逆序/空词面）→ 跳过并进 issues（物理行号+原因），
// 不静默丢行；文件级失败如实带因返回。不抛异常。

// 层：数据
namespace paleo::io
{

struct CuttingsInterval
{
  double topMd = 0;
  double baseMd = 0;
  QString litho;
  QString description; // 描述列可选；无该列时为空
  int rowNumber = 0;   // 工作簿/文本表内物理行号（issues 归因用）
};

struct CuttingsTable
{
  bool ok = false;
  QString error;
  QString sheetName;
  QVector<CuttingsInterval> intervals; // 按 topMd 升序
  QStringList issues;                  // 被跳过行/工作表的逐条列因
};

// 必需列表头方言（去空白与括号单位后大小写不敏感匹配）：
//   顶深 ∈ {顶深, 顶界深度, 顶界, top}
//   底深 ∈ {底深, 底界深度, 底界, base, bot}
//   岩性 ∈ {岩性, 定名, 岩性定名, 岩性名称, 岩性段, litho, lithology}
//   描述（可选）∈ {描述, 岩性描述, 备注}
// 行级校验：深度数值有限（parseNumericCell）、baseMd>topMd、岩性词面非空。
CuttingsTable parseCuttingsSheet( const WorkbookSheet &sheet );

// 文本表（CSV/TSV）RFC4180 引号解析（"" 转义、跨行字段、字段内分隔符保护、错位行列数校验）
WorkbookSheet parseTextCuttings( const QString &content, const QString &name,
                                 QStringList *issues = nullptr );

// 按扩展名分派：.xlsx/.xml → readWorkbook，取首个可解析的 sheet（其余
// sheet 进 issues 说明跳过）；.csv/.txt/.tsv → 文本表（自动探测
// 逗号/制表/分号分隔，首行表头）转 WorkbookSheet 后走同一纯表解析。
CuttingsTable readCuttingsFile( const QString &path );

} // namespace paleo::io
