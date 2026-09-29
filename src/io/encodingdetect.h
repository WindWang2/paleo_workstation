// 层：数据
#pragma once
#include <QByteArray>
#include <QString>

// io/ — 文本编码统一收口（wave/io-perf-cache D7.2）。
// 井文本（井口/分层/时深/DC.dat）在工区里以 UTF-8 与 GB18030 两种面目出现；
// 此前各解析器只走 QString::fromUtf8 —— GB 文件静默变 �。这里给出统一的
// 嗅探 + 解码入口，全部解析器共用一套口径。

enum class TextEncoding
{
  Utf8,    // BOM 或严格 UTF-8 校验通过
  GB18030, // 非 UTF-8 且 GB18030 解码成功（含中文井名文件）
  Latin1,  // 都不是——按 Latin1 保字节（数值列不受影响）
};

namespace EncodingDetect
{
  // 嗅探一段样本（建议 ≥4KB，BOM 优先）。
  TextEncoding detect(const QByteArray &sample);
  // 信心值 0~1（Utf8 严格校验=1.0；GB 按可解码比例）。
  double confidence(const QByteArray &sample);

  // 解码整段/文件首块：剥 BOM（UTF-8/UTF-16）→ 按嗅探结果解码。
  // Latin1 情形走 QString::fromLatin1（保底不丢字节）。
  QString decodeText(const QByteArray &raw);

  // 剥 UTF-8 BOM（EF BB BF）与 UTF-16 BOM（返回已剥的 UTF-8 字节；UTF-16
  // 输入转 UTF-8）。无 BOM 原样返回。
  QByteArray stripBom(const QByteArray &raw);

  // 工具：整段是否严格 UTF-8（无 BOM 也算）。
  bool isStrictUtf8(const QByteArray &raw);
} // namespace EncodingDetect
