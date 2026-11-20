// 层：数据
#pragma once

#include <QString>
#include <QXmlStreamReader>

// P1-W3（#1023）：宿主侧 XML 读取的统一前导 token 策略。
//
// 项目解析策略：第三方 XML 一律拒绝 DTD/实体声明——QXmlStreamReader 会按
// DTD 展开内部实体（数十 KB 声明可展开成 GB 级文本）；合法的
// SpreadsheetML/OOXML 部件不带 DTD。此前只有 readSpreadsheetMl 主循环按令
//牌级拒绝，其余入口各写各的：流式入口先用 readNextStartElement() 越过前导
// token（DTD 被静默消耗）、OOXML 辅助部件与井斜/时深收集器完全不查——同
// 一份带 DTD 的文件在不同入口命运不同。本头把策略集中成三件小事，供每个
// 完整 XML 读取入口在消费业务文本/属性前调用：
//   - rejectDtdToken：令牌级检查（放进读取循环，任意位置可命中）；
//   - advanceToRootElement：前导 token 推进到根 StartElement（代替裸
//     readNextStartElement，DTD/解析错误如实拒绝）；
//   - parseErrorMessage：reader.hasError() 时的统一文案（含源与行列，
//     不回显原始内容）。
//
// 诚实边界：拒绝 DTD 是项目解析策略，不是对所有拒绝服务风险的充分防护，
// 也不声称已验证实体炸弹/SSRF/超大内存占用等场景；Qt 自身的解析限制保持
// 原样（无证据需要显式收紧实体数/实体展开上限）。
// 层：数据
namespace paleo::io::xmlsafe
{

// token 是 DTD → true 并把拒绝理由（含 source 定位）写进 *error；否则
// false 且不碰 *error。source 是「文件名:部件名」样式的定位串。
bool rejectDtdToken( QXmlStreamReader::TokenType token, const QString &source, QString *error );

// 把 reader 从初始态推进到根 StartElement：StartDocument/注释/空白/处理
// 指令正常跳过；DTD → 拒绝；解析错误 → 透传（*error 含行列）。返回 false
// 即失败（*error 已写）；true 时 reader 停在根 StartElement 上，调用方自行
// 校验元素名/命名空间。XML 声明、BOM、命名空间与普通转义字符均不受影响。
bool advanceToRootElement( QXmlStreamReader &reader, const QString &source, QString *error );

// reader.hasError() 时的统一错误文案：「<source> XML 解析错误（行 L:列
// C）：<原因>」。source 为空时省略定位前缀。
QString parseErrorMessage( const QXmlStreamReader &reader, const QString &source );

} // namespace paleo::io::xmlsafe
