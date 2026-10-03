// 层：数据
#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>

// domain/ — 井文本文件解析出的瞬态记录类型（从 io/wellfileparsers.h 拆出）。
// 纯值类型：视图层的预览表/剖面标定按这份 DTO 渲染，不需要碰解析器入口。

// 井位 ExportWellHead.dat：井名 X Y KB TotalDepth BottomX BottomY WellType
struct WellHeadRecord
{
  QString name;
  double x = 0.0, y = 0.0, kb = 0.0, td = 0.0;
  double bottomX = 0.0, bottomY = 0.0; // 井底坐标；-99999/缺列 → has* 为 false
  bool hasBottomX = false, hasBottomY = false;
  QString wellType;                    // 井型代码原样透传（缺列为空串）
};

// 井分层 DC.dat：井名 层名 MD X Y Z TVD Time(ms)；'#' 跳过；
// -99999 的 Time/TVD/MD 视为空（对应 has* 为 false）。
struct WellTopRecord
{
  QString wellName;
  QString topName;
  double md = 0.0, x = 0.0, y = 0.0, z = 0.0, tvd = 0.0, timeMs = 0.0;
  bool hasMd = false, hasX = false, hasY = false, hasTvd = false, hasTime = false;
};

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

// 井斜 dev/*.dat：MD 井斜角 方位角（三列全有效才成站）；'#' 跳过，
// 井名取 '# Well : A1'（缺行为空，导入侧回退文件名主名）。
struct DeviationStationRecord
{
  double md = 0.0;
  double inclinationDeg = 0.0;
  double azimuthDeg = 0.0;
};
struct DeviationTable
{
  QString wellName;
  QVector<DeviationStationRecord> stations;
};

// XML 内容嗅探：判 .xml 是井口/测井/井斜（SpreadsheetML/WITSML 启发）。
// 纯函数——domain/projectclassifier 消费（自 io/wellfileparsers 下沉）。
enum class WellXmlKind { Unknown, WellHead, WellLog, WellDeviation };
WellXmlKind sniffWellXml(const QByteArray &content);
