// 层：数据
#pragma once
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

#include "../domain/wellnumeric.h" // kLasDefaultNull（LasHeaderInfo 缺省 NULL）

// io/ — LAS 2.x 测井曲线的纯类型门面（从 lasparser.h 拆出；解析入口仍在
// lasparser.h，视图只消费这里的值类型 + services/previewdoc.h 门面）。
// 方向 59：LasHeaderInfo/LasIssue 一并迁入——las/dlis/lis/welllogread/
// lascache 共用的「井曲线文档契约」全部落位本头（纯类型、零解析入口），
// lasparser.h 只留解析入口，公共头不再互相扇出重头。

// ~C 段一条曲线定义 + ~A 数据行（NULL token → NaN）。curves[0] 是 DEPT
// 索引道。
struct LasCurve
{
  QString name;             // mnemonic, e.g. "DEPT"
  QString unit;             // e.g. "M"
  QString descr;            // description text after ':'
  QVector<double> values;   // one entry per ~A data row; NULL tokens -> NaN
};

// LAS 解析结果（PreviewDocService::lasAt 的返回型）。
struct LasDoc
{
  bool ok = false;
  QString error;
  QStringList curveNames;   // ~C 曲线名序（含 DEPT）
  QList<LasCurve> curves;
};

// 表头快照（header-only 解析的出参）：不触 ~A 数据行——大 LAS 的头部只有
// 几 KB，读它不构成「整文件解析」；消费方先拿曲线名铺 UI，数据行异步补。
// indexBasis 是深度基准口径（方向 44）："MD" | "TVD" | "TIME" | ""（未知，
// 不冒充）。LAS 规范不声明基准，按行业惯例记 "MD"；DLIS/LIS 按各自元数据
// 填（dlisparser/lisparser）。
struct LasHeaderInfo
{
  QStringList curveNames;   // ~C 列序（curves[0] 是 DEPT 索引道）
  QString wellName;         // ~W WELL（缺省空）
  double nullValue = paleo::wellnumeric::kLasDefaultNull; // ~W NULL（CWLS 缺省）
  bool sawAscii = false;    // 头部扫描途中遇到 ~A 段头（数据节存在）
  QString indexBasis;       // 深度基准："MD"/"TVD"/"TIME"/""（未知）
};

// D1.6 解析器错误分类：格式错/编码错/截断/单位缺失分级。Error 级必然伴随
// 解析失败；Warning 级「能解但值得提醒」；Info 是策略性提示（如空曲线）。
struct LasIssue
{
  enum class Severity { Info, Warning, Error };
  enum class Category { Format, Encoding, Truncated, MissingUnit, MissingCurve, Io, WrapMode, Oversize };
  Severity severity = Severity::Warning;
  Category category = Category::Format;
  int line = 0;         // 1-based；0 = 与行无关
  QString message;

  static QString severityText(Severity s)
  {
    switch (s)
    {
      case Severity::Info: return QStringLiteral("info");
      case Severity::Warning: return QStringLiteral("warning");
      case Severity::Error: return QStringLiteral("error");
    }
    return QStringLiteral("warning");
  }
  static QString categoryText(Category c)
  {
    switch (c)
    {
      case Category::Format: return QStringLiteral("format");
      case Category::Encoding: return QStringLiteral("encoding");
      case Category::Truncated: return QStringLiteral("truncated");
      case Category::MissingUnit: return QStringLiteral("missing-unit");
      case Category::MissingCurve: return QStringLiteral("missing-curve");
      case Category::Io: return QStringLiteral("io");
      case Category::WrapMode: return QStringLiteral("wrap-mode");
      case Category::Oversize: return QStringLiteral("oversize");
    }
    return QStringLiteral("format");
  }
};
