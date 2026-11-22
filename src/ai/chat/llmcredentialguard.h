// 层：功能
#pragma once
// G1（模型通信）：对外内容的凭据边界——诊断文本与模型目录的结构化防回显。
//
// 三条纪律（与 llmclient/llmkeystore 的「密钥只在内存/钥匙串」一脉相承）：
//   · 任意非空长度的凭据都按等值回显判泄露——不设 ≥8 门槛（短 key 同样是
//     真凭据，短 key 回显进模型上下文/历史同样是泄露）；
//   · ≥8 字节时另做包含式回显检测（沿用旧字节包含防线的语义，作为纵深，
//     只对结构化值生效，避免短 key 在正常文本里的误伤）；
//   · 文本脱敏对任意非空 key 生效：命中即替换为占位符——宁可文本受损也不
//     让真凭据进 Agent/历史/日志。
#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace paleo::chat {

// 单个结构化字符串值是否泄露凭据：等值（任意非空长度）或包含（key ≥8 时）。
bool valueDisclosesCredential(const QString &value, const QByteArray &apiKey);

// 递归扫描 JSON 值（对象/数组/标量）的字符串值——按结构化值比对，不做
// 序列化字节包含（key 含引号/反斜杠/非 ASCII 时 JSON 转义可绕过字节包含）。
bool jsonDisclosesCredential(const QJsonValue &value, const QByteArray &apiKey);

// 诊断文本脱敏：把 key 的每次出现替换为占位符（key 为空原样返回）。
QString redactCredential(QString text, const QByteArray &apiKey);

} // namespace paleo::chat
