// 层：数据
#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>

#include "../domain/wellrecords.h" // WellHeadRecord/WellTopRecord/TdRow/TimeDepthTable

// io/ — project_area 井文本解析器（列契约来自 plan §3 / SMI 格式）。
// 一份多井文件 = 一个资产 + 每井一条关联；预览按当前井过滤，不拆文件。
// 记录类型在 domain/wellrecords.h（视图读门面），这里只留解析入口。

QVector<WellHeadRecord> parseWellHeadText(const QByteArray &text);

QVector<WellTopRecord> parseWellTopsText(const QByteArray &text);

TimeDepthTable parseTimeDepthText(const QByteArray &text);

// .xml 内容判定（§3：井口或测井，判不出作参考）。
