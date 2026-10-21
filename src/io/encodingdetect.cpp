// 层：数据
#include "encodingdetect.h"

#include <QByteArrayList>
#include <QStringConverter>
#include <QStringDecoder>

#include <algorithm>

namespace
{
  QString decodeGb(const QByteArray &raw)
  {
    QStringDecoder dec("GB18030", QStringConverter::Flag::Stateless);
    if (!dec.isValid())
      return QString();
    return dec.decode(raw);
  }

  bool hasHighBytes(const QByteArray &raw)
  {
    for (char c : raw)
      if (static_cast<uchar>(c) >= 0x80)
        return true;
    return false;
  }
} // namespace

namespace
{
  // detect() 的样本语义：样本可能是 UTF-8 文件被截断的前缀（尾部多字节序
  // 列不完整不是编码错误）——分类时剥掉尾部不完整序列再走严格校验。
  QByteArray trimIncompleteUtf8Tail(const QByteArray &raw)
  {
    if (raw.isEmpty())
      return raw;
    const auto *p = reinterpret_cast<const uchar *>(raw.constData());
    const qsizetype n = raw.size();
    const qsizetype last = n - 1;
    const uchar b = p[last];
    if (b < 0x80)
      return raw; // ASCII 结尾：无多字节序列
    if ((b & 0xC0) == 0x80)
    {
      // 续字节：回溯找首字节（最多 3 个续字节 + 1 首字节）。
      qsizetype back = 1;
      while (back < 4 && last - back >= 0 && (p[last - back] & 0xC0) == 0x80)
        ++back;
      if (last - back < 0)
        return raw;
      const uchar lead = p[last - back];
      int extra = 0;
      if ((lead & 0xE0) == 0xC0)
        extra = 1;
      else if ((lead & 0xF0) == 0xE0)
        extra = 2;
      else if ((lead & 0xF8) == 0xF0)
        extra = 3;
      else
        return raw; // 无合法首字节：交严格校验报错
      const qsizetype have = back; // lead 之后的续字节个数
      // #588：完整性判据是「续字节数 ≥ 该首字节所需数」，即 have < extra。
      // 旧码 have < extra + 1 把恰好完整的尾序列（如 UTF-8 三字节汉字的
      // 末两字节）也整段剥掉，白丢一个已解对的字符。
      if (have < extra)
        return raw.left(last - back); // 不完整：剥掉整个半截序列
      return raw;
    }
    // 首字节结尾：序列缺续字节。
    if ((b & 0xE0) == 0xC0 || (b & 0xF0) == 0xE0 || (b & 0xF8) == 0xF0)
    {
      int extra = 0;
      if ((b & 0xE0) == 0xC0)
        extra = 1;
      else if ((b & 0xF0) == 0xE0)
        extra = 2;
      else
        extra = 3;
      return n <= qsizetype(extra) ? QByteArray() : raw.left(n - 1);
    }
    return raw; // 非法字节：交严格校验报错
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
      return false; // #477：多字节序列尾部截断=非法 UTF-8（严格口径）；
                    // 样本前缀的截断容差由 detect() 剥尾后调用承担
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

  if (!hasHighBytes(sample))
    return TextEncoding::Utf8; // 纯 ASCII 是 UTF-8 子集

  // 样本可能是 UTF-8 文件被截断的前缀：剥掉尾部半截序列再判严格 UTF-8。
  const QByteArray trimmed = trimIncompleteUtf8Tail(sample);
  // 但「被截断」这个解释只在剥完仍留下多字节证据时才算成立：ASCII 前缀 +
  // 尾两字节同样可以是一个完整的 GB18030 汉字（GBK 里 lead∈[0xE0,0xEF] 的
  // 1024 个，如 唰=e0a7/唷=e0a1/刖=ebbe）——旧码无条件采纳截断解释，把这些
  // 汉字整段剥掉后按纯 ASCII 判成 UTF-8，末字符静默变成 U+FFFD。
  // （逐行解码是本仓主用法：行界在 \n，行尾半截序列只可能是坏文件，按 GB
  // 解出真字比吞成 U+FFFD 更贴近真实；反方向那一形态已在上面分支覆盖。）
  if (isStrictUtf8(trimmed) && (trimmed.size() == sample.size() || hasHighBytes(trimmed)))
    return TextEncoding::Utf8;

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
