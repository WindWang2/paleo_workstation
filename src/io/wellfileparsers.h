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

// 井分层文本写侧（方向 32 编辑落库）：parseWellTopsText 的逆。
// 列序 井名 层名 MD X Y Z TVD Time(ms)；缺失值写 -99999.000；
// 数值用自适应小数位（3..9 位中最短的往返精确表示，超出退化 'g' 17）——
// fixture 精度（3 位）逐字节风格一致，更高精度导入件不丢位。
// parse(write(x)) 记录级恒等（Z 列无判空标志，仅当 X/Y 有效时写出）。
QByteArray writeWellTopsText(const QVector<WellTopRecord> &tops);

TimeDepthTable parseTimeDepthText(const QByteArray &text);

// 井斜站表文本：MD 井斜角 方位角；'# Well : <名>' 取井名（缺省空）。
// 三列任一无效（-99999/非数值/非有限）整行不进站表——站点缺角无法定位。
DeviationTable parseDeviationText(const QByteArray &text);

// .xml 内容判定（§3：井口或测井，判不出作参考）。
