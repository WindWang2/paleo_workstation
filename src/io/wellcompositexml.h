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

// ============================================================================
// wave/wellcomposite-deep 扩展（D3.3/D3.11/D6.1/D6.5）
// ============================================================================

// D3.3 派生版本写回：把（编辑后的）井数据序列化为 SpreadsheetML XML。
// auditLines 逐条写入「编辑审计」工作表（D3.11 manifest 风格操作历史摘要）。
// 本函数只做序列化不落盘（调用方：壳/测试决定 DERIVED 版本落 catalog 或文件）。
QByteArray writeComprehensiveWellXml(const ComprehensiveWellData &data,
                                     const QStringList &auditLines = {});

// 便捷落盘（UTF-8 + XML 声明）
bool writeComprehensiveWellXmlFile(const ComprehensiveWellData &data, const QString &filePath,
                                   const QStringList &auditLines = {}, QString *errorMsg = nullptr);

// D6.1 井斜测量站（io 独立类型——domain 契约冻结，不扩 ComprehensiveWellData）
struct XmlDeviationStation
{
  double md = 0.0;
  double inclinationDeg = 0.0;
  double azimuthDeg = 0.0;
};

// D6.5 时深对（TVD m, TWT ms）
using XmlTimeDepthPair = QPair<double, double>;

// 从综合柱状图 XML 的「井斜数据」/「时深数据」工作表解析（无该表返回空 + false）
bool parseDeviationSurvey(const QString &filePath, QVector<XmlDeviationStation> &out, QString *errorMsg = nullptr);
bool parseTimeDepthTable(const QString &filePath, QVector<XmlTimeDepthPair> &out, QString *errorMsg = nullptr);

} // namespace WellComposite
