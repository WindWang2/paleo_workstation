// 层：数据
#pragma once
#include <QByteArray>
#include <QCryptographicHash>
#include <QIODevice>
#include <QString>
#include <QVector>

#include <optional>

#include "../domain/wellrecords.h" // WellHeadRecord/WellTopRecord/TdRow/TimeDepthTable

// io/ — project_area 井文本解析器（列契约来自 plan §3 / SMI 格式）。
// 一份多井文件 = 一个资产 + 每井一条关联；预览按当前井过滤，不拆文件。
// 记录类型在 domain/wellrecords.h（视图读门面），这里只留解析入口。

QVector<WellHeadRecord> parseWellHeadText(const QByteArray &text);
QVector<WellHeadRecord> parseWellHeadText(const QByteArray &text, WellParseReport *report);

QVector<WellTopRecord> parseWellTopsText(const QByteArray &text);
QVector<WellTopRecord> parseWellTopsText(const QByteArray &text, WellParseReport *report);

// #561 流式逐行入口：与整读版同一解析核、同一报告/错误口径（编码嗅探对
// 全文件语义等价——先一趟 O(64KB) 内存嗅探，再逐行解码喂核；UTF-16 BOM
// 或顺序设备（fifo）退回整读兜底）。dev 需已打开只读；上限闸在调用方
//（井表导入按 LasParser::fileSizeLimit 拒大文件）。runningHash 非空时随读
// 喂哈希——对全部读过字节的哈希与「readAll 后整段哈希」等价，供调用方做
// 读取期间突变复验。nullopt = 读失败（ioError 给设备侧原因）。
std::optional<QVector<WellHeadRecord>>
parseWellHeadDevice(QIODevice *dev, QCryptographicHash *runningHash,
                    WellParseReport *report, QString *ioError = nullptr);
std::optional<QVector<WellTopRecord>>
parseWellTopsDevice(QIODevice *dev, QCryptographicHash *runningHash,
                    WellParseReport *report, QString *ioError = nullptr);
std::optional<TimeDepthTable>
parseTimeDepthDevice(QIODevice *dev, QCryptographicHash *runningHash,
                     WellParseReport *report, QString *ioError = nullptr);
std::optional<DeviationTable>
parseDeviationDevice(QIODevice *dev, QCryptographicHash *runningHash,
                     WellParseReport *report, QString *ioError = nullptr);

// 井分层文本写侧（方向 32 编辑落库）：parseWellTopsText 的逆。
// 列序 井名 层名 MD X Y Z TVD Time(ms)；缺失值写 -99999.000；
// 数值用自适应小数位（3..9 位中最短的往返精确表示，超出退化 'g' 17）——
// fixture 精度（3 位）逐字节风格一致，更高精度导入件不丢位。
// parse(write(x)) 记录级恒等（Z 列无判空标志，仅当 X/Y 有效时写出）。
QByteArray writeWellTopsText(const QVector<WellTopRecord> &tops);

TimeDepthTable parseTimeDepthText(const QByteArray &text);
TimeDepthTable parseTimeDepthText(const QByteArray &text, WellParseReport *report);

// 井斜站表文本：MD 井斜角 方位角；'# Well : <名>' 取井名（缺省空）。
// 三列任一无效（-99999/非数值/非有限）整行不进站表——站点缺角无法定位。
DeviationTable parseDeviationText(const QByteArray &text);
DeviationTable parseDeviationText(const QByteArray &text, WellParseReport *report);

// .xml 内容判定（§3：井口或测井，判不出作参考）。

// 计数与逐条列因串（供导入结果/台账展示）；无异常为空。
QString wellParseSummary(const WellParseReport &report);
