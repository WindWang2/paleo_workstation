#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>

// io/ — project_area 井文本解析器（列契约来自 plan §3 / SMI 格式）。
// 一份多井文件 = 一个资产 + 每井一条关联；预览按当前井过滤，不拆文件。

// 井位 ExportWellHead.dat：井名 X Y KB TotalDepth BottomX BottomY WellType
struct WellHeadRecord
{
  QString name;
  double x = 0.0, y = 0.0, kb = 0.0, td = 0.0;
};
QVector<WellHeadRecord> parseWellHeadText(const QByteArray &text);

// 井分层 DC.dat：井名 层名 MD X Y Z TVD Time(ms)；'#' 跳过；
// -99999 的 Time/TVD/MD 视为空（对应 has* 为 false）。
struct WellTopRecord
{
  QString wellName;
  QString topName;
  double md = 0.0, x = 0.0, y = 0.0, z = 0.0, tvd = 0.0, timeMs = 0.0;
  bool hasMd = false, hasTvd = false, hasTime = false;
};
QVector<WellTopRecord> parseWellTopsText(const QByteArray &text);

// 时深 TD/*.dat：TIME(ms) TVDSS TVD MD [TVD'] [Well]；井名取 '# Well : A1'。
struct TdRow
{
  double timeMs = 0.0, tvdss = 0.0, tvd = 0.0, md = 0.0;
  bool hasTvd = false, hasMd = false; // -99999/缺列 → false
};
struct TimeDepthTable
{
  QString wellName;
  QVector<TdRow> rows;
};
TimeDepthTable parseTimeDepthText(const QByteArray &text);

// .xml 内容判定（§3：井口或测井，判不出作参考）。
enum class WellXmlKind { Unknown, WellHead, WellLog };
WellXmlKind sniffWellXml(const QByteArray &content);
