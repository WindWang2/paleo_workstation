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

  // inline/crossline 道头固定为标准位（plan §2）：偏移 188/192（SEG-Y 1-based
  // 字节 189/193）。本工区文件偏移 188 恒为 0——该字在首段不变时按道号索引：
  //   inline = base + 道号/N（N = 一条 inline 的道数），crossline = 该道 CDP
  //   （偏移 20）。不回退去读偏移 8/20；道数、CDP 顺序、角点对不上就停止，
  //   并排报出期望值和读到的值。
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
  const bool ordinalIndex = !fieldVaries(188);
  const int binEnsTraces = beI16(bin + 12); // 二进制头字节 13-14：每条 inline 道数

  // 索引道：只读 240 字节道头；样本区用 seek 跳过——打开内存不随体增长。
  uchar trHdr[240];
  bool sawTrace = false;
  double delayMs = 0.0;
  qint32 firstLineWord = 0; // ordinal 模式：偏移 188 的恒定值（inline 起点兜底）
  qint32 firstFieldRec = 0; // ordinal 模式：首道 field record（偏移 8）= inline 起点
  QVector<double> ordX, ordY; // ordinal 模式的坐标序列（一致性校验 + 角点）
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
    if (!sawTrace)
    {
      sawTrace = true;
      delayMs = static_cast<double>(beI16(trHdr + 108)); // 字节 109-110
    }

    // survey 坐标（角点）：道头偏移 180/184 的整数米（plan §2）。比例因子
    // （偏移 70）为 0 或 1 都按原值取整数；正值乘、负值除。
    const qint16 scal = beI16(trHdr + 70);
    const double coordScale =
        (scal == 0 || scal == 1) ? 1.0
                                 : (scal > 0 ? static_cast<double>(scal)
                                             : 1.0 / -static_cast<double>(scal));
    const double cx = static_cast<double>(beI32(trHdr + 180)) * coordScale;
    const double cy = static_cast<double>(beI32(trHdr + 184)) * coordScale;

    if (ordinalIndex)
    {
      // 探测段内偏移 188 不变才进这条路；半路再变说明探测段不代表全文件，
      // 按 plan 报出期望值和读到的值后停止，不改读别的字节。
      const qint32 lineWord = beI32(trHdr + 188);
      if (m_index.isEmpty())
      {
        firstLineWord = lineWord;
        firstFieldRec = beI32(trHdr + 8); // 道号索引的 inline 起点（本文件 1315）
      }
      else if (lineWord != firstLineWord)
      {
        if (error)
          *error = QStringLiteral("inline word at trace-header offset 188 must stay constant for "
                                  "ordinal indexing: expected %1, read %2 at trace %3")
                       .arg(firstLineWord)
                       .arg(lineWord)
                       .arg(m_index.size());
        m_index.clear();
        return false;
      }
      e.inlineNo = 0;                 // 占位：收尾统一按道号赋值
      e.xlineNo = beI32(trHdr + 20); // crossline = 该道 CDP（plan §2）
      ordX.append(cx);
      ordY.append(cy);
    }
    else
    {
      e.inlineNo = beI32(trHdr + 188); // 标准 inline 字节位（1-based 字节 189）
      e.xlineNo = beI32(trHdr + 192); // 标准 crossline 字节位（1-based 字节 193）

      // survey 几何冻结：范围 + 四角 (x,y)。
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
    }

    m_index.append(e);
    offset += 240 + static_cast<qint64>(ns) * 4;
  }
  if (m_index.isEmpty())
  {
    if (error)
      *error = QStringLiteral("File contains 0 traces");
    return false;
  }

  if (ordinalIndex)
  {
    // 偏移 188 全程不变 → 按道号索引（plan §2）：inline = base + 道号/N，
    // crossline = 该道 CDP。道数、CDP 顺序、角点对不上就报数停止。
    const int n = m_index.size();

    // N = 一条 inline 的道数：CDP 序列首次回落处；全程不回落 → 全文件一条线。
    int perLine = n;
    for (int i = 1; i < n; ++i)
    {
      if (m_index.at(i).xlineNo < m_index.at(i - 1).xlineNo)
      {
        perLine = i;
        break;
      }
    }
    // 二进制头字节 13-14 声明的每条 inline 道数若与 CDP 回落不符 → 对不上。
    if (binEnsTraces > 0 && binEnsTraces != perLine)
    {
      if (error)
        *error = QStringLiteral("traces-per-inline mismatch: binary header declares %1, CDP order gives %2")
                     .arg(binEnsTraces)
                     .arg(perLine);
      m_index.clear();
      return false;
    }
    // 道数：总道数必须是每条 inline 道数的整数倍（残线属截断）。
    if (n % perLine != 0)
    {
      if (error)
        *error = QStringLiteral("trace count mismatch: expected a multiple of %1 traces per inline, read %2 traces")
                     .arg(perLine)
                     .arg(n);
      m_index.clear();
      return false;
    }
    // CDP 顺序：后续每条线必须重复第一条线的 CDP 序列。
    for (int i = perLine; i < n; ++i)
    {
      if (m_index.at(i).xlineNo != m_index.at(i - perLine).xlineNo)
      {
        if (error)
          *error = QStringLiteral("CDP order mismatch at trace %1: expected %2 (first line at same position), read %3")
                       .arg(i)
                       .arg(m_index.at(i - perLine).xlineNo)
                       .arg(m_index.at(i).xlineNo);
        m_index.clear();
        return false;
      }
    }
    // 角点一致性：线内 x 不减、随线号 y 不减（整齐测网的结构校验）。
    for (int i = 1; i < n; ++i)
    {
      if (i % perLine != 0 && ordX.at(i) < ordX.at(i - 1))
      {
        if (error)
          *error = QStringLiteral("corner coordinate mismatch at trace %1: expected x >= %2, read %3")
                       .arg(i)
                       .arg(ordX.at(i - 1))
                       .arg(ordX.at(i));
        m_index.clear();
        return false;
      }
      if (i >= perLine && ordY.at(i) < ordY.at(i - perLine))
      {
        if (error)
          *error = QStringLiteral("corner coordinate mismatch at trace %1: expected y >= %2, read %3")
                       .arg(i)
                       .arg(ordY.at(i - perLine))
                       .arg(ordY.at(i));
        m_index.clear();
        return false;
      }
    }

    // inline = base + 道号/N：base 取首道 field record（本文件 = 1315），
    // field record 为空时退回偏移 188 的恒定值。
    const qint32 base = firstFieldRec != 0 ? firstFieldRec : firstLineWord;
    const int lines = n / perLine;
    for (int i = 0; i < n; ++i)
      m_index[i].inlineNo = base + i / perLine;

    m_geometry.inlineMin = base;
    m_geometry.inlineMax = base + lines - 1;
    m_geometry.xlineMin = m_index.first().xlineNo;
    m_geometry.xlineMax = m_index.first().xlineNo;
    for (int i = 0; i < n; ++i)
    {
      if (m_index.at(i).xlineNo < m_geometry.xlineMin)
        m_geometry.xlineMin = m_index.at(i).xlineNo;
      if (m_index.at(i).xlineNo > m_geometry.xlineMax)
        m_geometry.xlineMax = m_index.at(i).xlineNo;
    }
    // 四角 = 四条极端道的坐标（整齐网格上即精确角点）：先在线内找
    // xlMin/xlMax 的位置，再取首末两条线的对应道。
    // slot：(inlMin,xlMin)=0 (inlMin,xlMax)=1 (inlMax,xlMax)=2 (inlMax,xlMin)=3。
    int jMin = 0, jMax = 0;
    for (int j = 1; j < perLine; ++j)
    {
      if (m_index.at(j).xlineNo < m_index.at(jMin).xlineNo)
        jMin = j;
      if (m_index.at(j).xlineNo > m_index.at(jMax).xlineNo)
        jMax = j;
    }
    const int lastLine = n - perLine;
    m_geometry.cornerX[0] = ordX.at(jMin);             m_geometry.cornerY[0] = ordY.at(jMin);
    m_geometry.cornerX[1] = ordX.at(jMax);             m_geometry.cornerY[1] = ordY.at(jMax);
    m_geometry.cornerX[2] = ordX.at(lastLine + jMax);  m_geometry.cornerY[2] = ordY.at(lastLine + jMax);
    m_geometry.cornerX[3] = ordX.at(lastLine + jMin);  m_geometry.cornerY[3] = ordY.at(lastLine + jMin);
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
