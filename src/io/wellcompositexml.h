// 层：数据
#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

#include "../domain/wellcompositemodel.h"

// io/ — wellcompositexml: 解析中国石油地质行业标准单井综合柱状图 XML (SpreadsheetML)
//
// 自动分流识别 9 大标准工作表：
// 1. 测井曲线-*井 -> 连续测井曲线（GR, AC, DEN, RT, etc.）
// 2. 离散曲线-*井 -> 离散实测散点（孔隙度 CPOR, 渗透率 CKAR, MDT流度, TOC）
// 3. 岩性道 -> 顶底深分段岩性（灰色泥岩, 细砂岩, 灰岩等）
// 4. 地层单位道 -> 地层组段分层（韩江组, 珠江组等）
// 5. 砂层组道 -> 细分砂层组分层（ZJ310, ZJ451等）
// 6. 文本道 -> 顶底深评价与结论文本（取样结论, 试油结论）
// 7. 符号道 -> 射孔段与产层符号
// 8. 取心数据道 -> 筒号, 进尺, 心长, 收获率
// 9. 标准层道 -> 地震界面/标志层 (T32, T35, T40)
// 10. 坐标 -> 井口 X/Y

namespace WellComposite
{

// 流式解析文件或二进制内容
bool parseComprehensiveWellXml(const QString &filePath, ComprehensiveWellData &outData, QString *errorMsg = nullptr);
bool parseComprehensiveWellXmlData(const QByteArray &content, ComprehensiveWellData &outData, QString *errorMsg = nullptr);

} // namespace WellComposite
