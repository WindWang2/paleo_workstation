// 层：数据
#include "encodingdetect.h"

#include <QByteArrayList>
#include <QStringConverter>
#include <QStringDecoder>

#include <algorithm>

namespace
{
  bool looksLikeGbLead(quint8 b)
  {
    return b >= 0x81; // GB18030 双/四字节首字节均 >= 0x81
  }

  QString decodeGb(const QByteArray &raw)
  {
    QStringDecoder dec("GB18030", QStringConverter::Flag::Stateless);
    if (!dec.isValid())
      return QString();
    return dec.decode(raw);
  }
} // namespace

bool EncodingDetect::isStrictUtf8(const QByteArray &raw)
{
  const uchar *p = reinterpret_cast<const uchar *>(raw.constData());
  const qsizetype n = raw.size();
  qsizetype i = 0;
  while (i < n)
  {
    const uchar b = p[i];
    if (b < 0x80)
    {
      ++i;
      continue;
    }
    int extra = 0;
    quint32 minCP = 0, maxCP = 0;
    if ((b & 0xE0) == 0xC0) { extra = 1; minCP = 0x80; maxCP = 0x7FF; }
    else if ((b & 0xF0) == 0xE0) { extra = 2; minCP = 0x800; maxCP = 0xFFFF; }
    else if ((b & 0xF8) == 0xF0) { extra = 3; minCP = 0x10000; maxCP = 0x10FFFF; }
    else return false; // 0x80-0xBF 作首字节 / 0xF8+ 均非法
    if (i + extra >= n)
      return true; // 样本在多字节序列中间截断——不算非法（整文件校验时不会发生）
    quint32 cp = b & (0x3F >> extra);
    for (int k = 1; k <= extra; ++k)
    {
      const uchar c = p[i + k];
      if ((c & 0xC0) != 0x80)
        return false;
      cp = (cp << 6) | (c & 0x3F);
    }
    if (cp < minCP || cp > maxCP || (cp >= 0xD800 && cp <= 0xDFFF))
      return false; // 过长编码 / 代理区
    i += extra + 1;
  }
  return true;
}

TextEncoding EncodingDetect::detect(const QByteArray &sample)
{
  if (sample.size() >= 3 &&
      static_cast<uchar>(sample[0]) == 0xEF && static_cast<uchar>(sample[1]) == 0xBB &&
      static_cast<uchar>(sample[2]) == 0xBF)
    return TextEncoding::Utf8;
  if (sample.size() >= 2 &&
      ((static_cast<uchar>(sample[0]) == 0xFF && static_cast<uchar>(sample[1]) == 0xFE) ||
       (static_cast<uchar>(sample[0]) == 0xFE && static_cast<uchar>(sample[1]) == 0xFF)))
    return TextEncoding::Utf8; // UTF-16 → stripBom 转 UTF-8

  if (isStrictUtf8(sample))
    return TextEncoding::Utf8;

  // 非 UTF-8：出现 GB 首字节就按 GB18030 试解码；解出的串没有 U+FFFD 才认。
  bool hasHighByte = false;
  for (char c : sample)
    if (static_cast<uchar>(c) >= 0x80) { hasHighByte = true; break; }
  if (!hasHighByte)
    return TextEncoding::Utf8; // 纯 ASCII 是 UTF-8 子集
  Q_UNUSED(looksLikeGbLead);
  {
    // GB 文本常以 ASCII 段开头（"~VERSION…"）——只要含高位字节就按 GB 试解。
    const QString s = decodeGb(sample);
    if (!s.isEmpty() && !s.contains(QChar(0xFFFD)))
      return TextEncoding::GB18030;
  }
  return TextEncoding::Latin1;
}

double EncodingDetect::confidence(const QByteArray &sample)
{
  const TextEncoding enc = detect(sample);
  if (enc == TextEncoding::Utf8)
    return 1.0;
  if (enc == TextEncoding::GB18030)
  {
    const QString s = decodeGb(sample);
    const int bad = s.count(QChar(0xFFFD));
    return sample.isEmpty() ? 0.0 : qMax(0.0, 1.0 - double(bad) / double(sample.size()));
  }
  return 0.25; // Latin1 保底——永远「能解」但信心低
}

QByteArray EncodingDetect::stripBom(const QByteArray &raw)
{
  if (raw.size() >= 3 &&
      static_cast<uchar>(raw[0]) == 0xEF && static_cast<uchar>(raw[1]) == 0xBB &&
      static_cast<uchar>(raw[2]) == 0xBF)
    return raw.mid(3);
  if (raw.size() >= 2 &&
      static_cast<uchar>(raw[0]) == 0xFF && static_cast<uchar>(raw[1]) == 0xFE)
  {
    // UTF-16LE：转 UTF-8 输出。
    const qsizetype units = (raw.size() - 2) / 2;
    return QString::fromUtf16(reinterpret_cast<const char16_t *>(raw.constData() + 2), units).toUtf8();
  }
  if (raw.size() >= 2 &&
      static_cast<uchar>(raw[0]) == 0xFE && static_cast<uchar>(raw[1]) == 0xFF)
  {
    // UTF-16BE：换字节序再转。
    QByteArray swapped = raw.mid(2);
    char *p = swapped.data();
    for (qsizetype i = 0; i + 1 < swapped.size(); i += 2)
      std::swap(p[i], p[i + 1]);
    return QString::fromUtf16(reinterpret_cast<const char16_t *>(swapped.constData()),
                              swapped.size() / 2)
        .toUtf8();
  }
  return raw;
}

QString EncodingDetect::decodeText(const QByteArray &raw)
{
  const QByteArray noBom = stripBom(raw);
  const TextEncoding enc = detect(noBom);
  switch (enc)
  {
    case TextEncoding::GB18030:
    {
      const QString s = decodeGb(noBom);
      if (!s.isEmpty())
        return s;
      break;
    }
    case TextEncoding::Latin1:
      return QString::fromLatin1(noBom);
    case TextEncoding::Utf8:
      break;
  }
  return QString::fromUtf8(noBom);
}
