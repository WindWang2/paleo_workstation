#include "segyreader.h"

#include <QFile>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
// Convert IBM 370 single-precision (format 1) to IEEE 754 float — 保留自旧实现。
float ibmToIeee(quint32 ibm)
{
  if ((ibm & 0x7fffffff) == 0)
    return 0.0f;

  const int sign = (ibm & 0x80000000) ? -1 : 1;
  const int exponent = static_cast<int>((ibm >> 24) & 0x7f);
  const quint32 mantissa = ibm & 0x00ffffff;

  // Value = sign * (mantissa / 16777216.0) * 16^(exponent - 64)
  //       = sign * mantissa * 2^(4 * (exponent - 64) - 24)
  const double val = std::ldexp(static_cast<double>(mantissa), 4 * (exponent - 64) - 24);
  if (val > static_cast<double>(std::numeric_limits<float>::max()))
    return (sign > 0) ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity();

  return static_cast<float>(sign * val);
}

qint32 beI32(const uchar *p)
{
  return qFromBigEndian<qint32>(p);
}

qint16 beI16(const uchar *p)
{
  return qFromBigEndian<qint16>(p);
}
} // namespace

bool SegyReader::open(const QString &path, QString *error)
{
  m_index.clear();
  m_byInline.clear();
  m_byXline.clear();
  m_samplesPerTrace = 0;
  m_sampleIntervalUs = 0.0f;
  m_geometry = SegyGeometry();
  m_path.clear();

  if (path.isEmpty())
  {
    if (error)
      *error = QStringLiteral("File path is empty");
    return false;
  }

  QFile file(path);
  if (!file.exists())
  {
    if (error)
      *error = QStringLiteral("File does not exist: %1").arg(path);
    return false;
  }

  if (!file.open(QIODevice::ReadOnly))
  {
    if (error)
      *error = QStringLiteral("Cannot open file: %1").arg(file.errorString());
    return false;
  }

  const qint64 fileSize = file.size();
  if (fileSize == 0)
  {
    if (error)
      *error = QStringLiteral("File is empty: %1").arg(path);
    return false;
  }

  if (fileSize < 3600)
  {
    if (error)
      *error = QStringLiteral("File size (%1 bytes) is smaller than required 3600-byte SEG-Y header").arg(fileSize);
    return false;
  }

  uchar bin[400];
  if (!file.seek(3200) || file.read(reinterpret_cast<char *>(bin), 400) != 400)
  {
    if (error)
      *error = QStringLiteral("Incomplete binary header: %1").arg(path);
    return false;
  }

  m_binLineNo = beI32(bin + 4);
  const qint16 binDt = beI16(bin + 16);
  const qint16 binNs = beI16(bin + 20);
  m_formatCode = beI16(bin + 24);
  const qint16 extHeaders = beI16(bin + 304);

  if (binNs <= 0)
  {
    if (error)
      *error = QStringLiteral("Invalid samples per trace in binary header (ns=%1)").arg(binNs);
    return false;
  }
  if (m_formatCode != 1 && m_formatCode != 5)
  {
    if (error)
      *error = QStringLiteral("Unsupported SEG-Y format code %1 (only 1=IBM and 5=IEEE are supported)").arg(m_formatCode);
    return false;
  }

  m_samplesPerTrace = binNs;
  m_sampleIntervalUs = static_cast<float>(binDt > 0 ? binDt : 0);

  qint64 offset = 3600 + static_cast<qint64>(extHeaders) * 3200;
  if (extHeaders < 0)
    offset = 3600;
  if (offset > fileSize)
  {
    if (error)
      *error = QStringLiteral("File truncated before trace data (offset %1 exceeds file size %2)").arg(offset).arg(fileSize);
    return false;
  }
  if (offset == fileSize)
  {
    if (error)
      *error = QStringLiteral("File contains 0 traces");
    return false;
  }
  m_firstTraceOffset = offset;

  // inline/crossline 道头位置探测：先按标准字节 189/193；该位置读出来在首段
  // 道里完全不变时，再试本工区体使用的 9/21（inline=field record、crossline=CDP）。
  const qint64 traceStride = 240 + static_cast<qint64>(m_samplesPerTrace) * 4;
  const int probeCount = qMin<qint64>(4096, (fileSize - offset) / traceStride);
  auto fieldVaries = [&](int fieldOff) -> bool {
    qint32 first = 0;
    bool haveFirst = false, varies = false;
    uchar h[240];
    for (int i = 0; i < probeCount && !varies; ++i)
    {
      if (!file.seek(offset + static_cast<qint64>(i) * traceStride) ||
          file.read(reinterpret_cast<char *>(h), 240) != 240)
        return false;
      const qint32 v = beI32(h + fieldOff);
      if (!haveFirst)
      {
        first = v;
        haveFirst = true;
      }
      else if (v != first)
        varies = true;
    }
    return varies;
  };
  int inlineOff = 188, xlineOff = 192; // 标准位（plan：crossline 默认字节 193）
  if (!fieldVaries(inlineOff))
  {
    // 9/21 也无变化时维持标准位（旧合成件 lineNo 全 0 的回退语义不变）。
    if (fieldVaries(8) || fieldVaries(20))
    {
      inlineOff = 8;
      xlineOff = 20;
    }
  }

  // 索引道：只读 240 字节道头；样本区用 seek 跳过——打开内存不随体增长。
  uchar trHdr[240];
  bool sawTrace = false;
  double delayMs = 0.0;
  while (offset + 240 <= fileSize)
  {
    if (!file.seek(offset) || file.read(reinterpret_cast<char *>(trHdr), 240) != 240)
    {
      if (error)
        *error = QStringLiteral("Truncated trace header at offset %1").arg(offset);
      m_index.clear();
      return false;
    }

    const qint16 traceNs = beI16(trHdr + 114);
    const qint16 traceDt = beI16(trHdr + 116);
    if (traceNs < 0)
    {
      if (error)
        *error = QStringLiteral("Corrupt trace ns (%1) at trace index %2").arg(traceNs).arg(m_index.size());
      m_index.clear();
      return false;
    }
    const int ns = traceNs > 0 ? traceNs : binNs;
    if (m_sampleIntervalUs <= 0.0f && traceDt > 0)
      m_sampleIntervalUs = static_cast<float>(traceDt);
    if (ns <= 0)
    {
      if (error)
        *error = QStringLiteral("Corrupt trace ns (%1) at trace index %2").arg(ns).arg(m_index.size());
      m_index.clear();
      return false;
    }
    // 样本区必须完整在文件内（截断的尾道是错误，不静默丢）。
    if (fileSize - (offset + 240) < static_cast<qint64>(ns) * 4)
    {
      if (error)
        *error = QStringLiteral("Truncated trace samples at offset %1 (needs %2 bytes, remaining %3)")
                     .arg(offset + 240)
                     .arg(static_cast<qint64>(ns) * 4)
                     .arg(fileSize - (offset + 240));
      m_index.clear();
      return false;
    }

    IndexEntry e;
    e.offset = offset;
    e.inlineNo = beI32(trHdr + inlineOff); // 探测出的 inline 字节位
    e.xlineNo = beI32(trHdr + xlineOff);  // crossline 字节位
    if (!sawTrace)
    {
      sawTrace = true;
      delayMs = static_cast<double>(beI16(trHdr + 108)); // 字节 109-110
    }

    // survey 几何冻结：范围 + 四角 (x,y)（CDP 坐标，字节 181-188）。
    const double cx = static_cast<double>(beI32(trHdr + 180));
    const double cy = static_cast<double>(beI32(trHdr + 184));
    if (m_index.isEmpty())
    {
      m_geometry.inlineMin = m_geometry.inlineMax = e.inlineNo;
      m_geometry.xlineMin = m_geometry.xlineMax = e.xlineNo;
      m_geometry.cornerX[0] = m_geometry.cornerX[1] = m_geometry.cornerX[2] = m_geometry.cornerX[3] = cx;
      m_geometry.cornerY[0] = m_geometry.cornerY[1] = m_geometry.cornerY[2] = m_geometry.cornerY[3] = cy;
    }
    else
    {
      if (e.inlineNo < m_geometry.inlineMin)
        m_geometry.inlineMin = e.inlineNo;
      if (e.inlineNo > m_geometry.inlineMax)
        m_geometry.inlineMax = e.inlineNo;
      if (e.xlineNo < m_geometry.xlineMin)
        m_geometry.xlineMin = e.xlineNo;
      if (e.xlineNo > m_geometry.xlineMax)
        m_geometry.xlineMax = e.xlineNo;
    }
    // 四角 slot：(inlMin,xlMin)=0 (inlMin,xlMax)=1 (inlMax,xlMax)=2 (inlMax,xlMin)=3。
    // 中点二分近似——对整齐测网（inline/xline 单调）即精确四角。
    const bool iLo = e.inlineNo * 2 <= m_geometry.inlineMin + m_geometry.inlineMax;
    const bool xLo = e.xlineNo * 2 <= m_geometry.xlineMin + m_geometry.xlineMax;
    const int slot = iLo ? (xLo ? 0 : 1) : (xLo ? 3 : 2);
    m_geometry.cornerX[slot] = cx;
    m_geometry.cornerY[slot] = cy;

    m_index.append(e);
    offset += 240 + static_cast<qint64>(ns) * 4;
  }
  if (m_index.isEmpty())
  {
    if (error)
      *error = QStringLiteral("File contains 0 traces");
    return false;
  }
  m_geometry.startTimeMs = delayMs;

  // 行/道索引（排序，供测线级读取）。
  for (int i = 0; i < m_index.size(); ++i)
    m_byInline[m_index.at(i).inlineNo].append(i);
  for (int i = 0; i < m_index.size(); ++i)
    m_byXline[m_index.at(i).xlineNo].append(i);
  const QVector<IndexEntry> &idx = m_index;
  for (auto it = m_byInline.begin(); it != m_byInline.end(); ++it)
  {
    std::sort(it.value().begin(), it.value().end(),
              [&idx](int a, int b) { return idx.at(a).xlineNo < idx.at(b).xlineNo; });
  }
  for (auto it = m_byXline.begin(); it != m_byXline.end(); ++it)
  {
    std::sort(it.value().begin(), it.value().end(),
              [&idx](int a, int b) { return idx.at(a).inlineNo < idx.at(b).inlineNo; });
  }

  m_path = path;
  return true;
}

bool SegyReader::decodeTrace(QFile &file, const IndexEntry &e, SegyTrace *out) const
{
  uchar trHdr[240];
  if (!file.seek(e.offset) || file.read(reinterpret_cast<char *>(trHdr), 240) != 240)
    return false;

  const qint16 traceNs = beI16(trHdr + 114);
  const int ns = traceNs > 0 ? traceNs : m_samplesPerTrace;

  QByteArray raw(static_cast<int>(ns) * 4, Qt::Uninitialized);
  if (file.read(raw.data(), raw.size()) != raw.size())
    return false;

  SegyTrace t;
  t.tracl = beI32(trHdr + 0);
  t.cdp = beI32(trHdr + 20);
  t.lineNo = e.inlineNo != 0 ? e.inlineNo : m_binLineNo;
  t.xlineNo = e.xlineNo;
  t.samples.resize(ns);
  const uchar *p = reinterpret_cast<const uchar *>(raw.constData());
  if (m_formatCode == 5)
  {
    for (int i = 0; i < ns; ++i)
    {
      const quint32 be = qFromBigEndian<quint32>(p + i * 4);
      float v;
      std::memcpy(&v, &be, sizeof(float));
      t.samples[i] = v;
    }
  }
  else
  {
    for (int i = 0; i < ns; ++i)
      t.samples[i] = ibmToIeee(qFromBigEndian<quint32>(p + i * 4));
  }
  *out = t;
  return true;
}

QVector<SegyTrace> SegyReader::readByIndexList(const QVector<int> &idxs) const
{
  QVector<SegyTrace> out;
  QFile file(m_path);
  if (!file.open(QIODevice::ReadOnly))
    return out;
  out.reserve(idxs.size());
  for (int i : idxs)
  {
    SegyTrace t;
    if (decodeTrace(file, m_index.at(i), &t))
      out.append(t);
  }
  return out;
}

QVector<SegyTrace> SegyReader::traces() const
{
  QVector<int> all;
  all.reserve(m_index.size());
  for (int i = 0; i < m_index.size(); ++i)
    all.append(i);
  return readByIndexList(all);
}

QVector<qint32> SegyReader::inlineNumbers() const
{
  QVector<qint32> out = m_byInline.keys();
  std::sort(out.begin(), out.end());
  return out;
}

QVector<qint32> SegyReader::crosslineNumbers() const
{
  QVector<qint32> out = m_byXline.keys();
  std::sort(out.begin(), out.end());
  return out;
}

bool SegyReader::readInline(qint32 inlineNo, QVector<SegyTrace> *out, QString *error) const
{
  if (!m_byInline.contains(inlineNo))
  {
    if (error)
      *error = QStringLiteral("inline %1 not present in %2").arg(inlineNo).arg(m_path);
    return false;
  }
  *out = readByIndexList(m_byInline.value(inlineNo));
  return !out->isEmpty();
}

bool SegyReader::readCrossline(qint32 xlineNo, QVector<SegyTrace> *out, QString *error) const
{
  if (!m_byXline.contains(xlineNo))
  {
    if (error)
      *error = QStringLiteral("crossline %1 not present in %2").arg(xlineNo).arg(m_path);
    return false;
  }
  *out = readByIndexList(m_byXline.value(xlineNo));
  return !out->isEmpty();
}
