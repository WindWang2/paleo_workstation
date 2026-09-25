#include "segyreader.h"

#include <QFile>
#include <QtEndian>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
// Convert IBM 370 single-precision (format 1) to IEEE 754 float
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
} // namespace

bool SegyReader::open(const QString &path, QString *error)
{
  m_traces.clear();
  m_samplesPerTrace = 0;
  m_sampleIntervalUs = 0.0f;

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

  const QByteArray data = file.readAll();
  if (data.size() != fileSize)
  {
    if (error)
      *error = QStringLiteral("Incomplete read of file: %1").arg(path);
    return false;
  }

  const char *bin = data.constData() + 3200;
  const qint32 binLineNo = qFromBigEndian<qint32>(reinterpret_cast<const uchar *>(bin + 4));
  const qint16 binDt = qFromBigEndian<qint16>(reinterpret_cast<const uchar *>(bin + 16));
  const qint16 binNs = qFromBigEndian<qint16>(reinterpret_cast<const uchar *>(bin + 20));
  const qint16 formatCode = qFromBigEndian<qint16>(reinterpret_cast<const uchar *>(bin + 24));
  const qint16 extHeaders = qFromBigEndian<qint16>(reinterpret_cast<const uchar *>(bin + 304));

  if (binNs <= 0)
  {
    if (error)
      *error = QStringLiteral("Invalid samples per trace in binary header (ns=%1)").arg(binNs);
    return false;
  }

  if (formatCode != 1 && formatCode != 5)
  {
    if (error)
      *error = QStringLiteral("Unsupported SEG-Y format code %1 (only 1=IBM and 5=IEEE are supported)").arg(formatCode);
    return false;
  }

  qint64 offset = 3600;
  if (extHeaders > 0)
  {
    offset += static_cast<qint64>(extHeaders) * 3200;
  }

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

  int maxNs = 0;
  int intervalUs = binDt;

  while (offset < fileSize)
  {
    if (fileSize - offset < 240)
    {
      if (error)
        *error = QStringLiteral("Truncated trace header at offset %1 (remaining %2 bytes < 240)").arg(offset).arg(fileSize - offset);
      m_traces.clear();
      return false;
    }

    const char *trHdr = data.constData() + offset;
    const qint32 traclRaw = qFromBigEndian<qint32>(reinterpret_cast<const uchar *>(trHdr + 0));
    const qint32 cdp = qFromBigEndian<qint32>(reinterpret_cast<const uchar *>(trHdr + 20));
    const qint16 traceNs = qFromBigEndian<qint16>(reinterpret_cast<const uchar *>(trHdr + 114));
    const qint16 traceDt = qFromBigEndian<qint16>(reinterpret_cast<const uchar *>(trHdr + 116));
    const qint32 traceLine = qFromBigEndian<qint32>(reinterpret_cast<const uchar *>(trHdr + 188));

    if (traceNs < 0)
    {
      if (error)
        *error = QStringLiteral("Corrupt trace ns (%1) at trace index %2").arg(traceNs).arg(m_traces.size());
      m_traces.clear();
      return false;
    }

    const int ns = (traceNs > 0) ? traceNs : binNs;
    if (ns > maxNs)
      maxNs = ns;
    if (intervalUs <= 0 && traceDt > 0)
      intervalUs = traceDt;
    if (ns <= 0)
    {
      if (error)
        *error = QStringLiteral("Corrupt trace ns (%1) at trace index %2").arg(ns).arg(m_traces.size());
      m_traces.clear();
      return false;
    }

    const qint32 lineNo = (traceLine != 0) ? traceLine : binLineNo;
    const qint64 sampleBytes = static_cast<qint64>(ns) * 4;

    if (fileSize - (offset + 240) < sampleBytes)
    {
      if (error)
        *error = QStringLiteral("Truncated trace samples at offset %1 (needs %2 bytes, remaining %3)").arg(offset + 240).arg(sampleBytes).arg(fileSize - (offset + 240));
      m_traces.clear();
      return false;
    }

    SegyTrace trace;
    trace.tracl = traclRaw;
    trace.cdp = cdp;
    trace.lineNo = lineNo;
    trace.samples.resize(ns);

    const char *samplePtr = trHdr + 240;
    if (formatCode == 5)
    {
      for (int i = 0; i < ns; ++i)
      {
        const quint32 raw = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(samplePtr + i * 4));
        float val;
        std::memcpy(&val, &raw, sizeof(float));
        trace.samples[i] = val;
      }
    }
    else if (formatCode == 1)
    {
      for (int i = 0; i < ns; ++i)
      {
        const quint32 raw = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(samplePtr + i * 4));
        trace.samples[i] = ibmToIeee(raw);
      }
    }

    m_traces.append(std::move(trace));
    offset += 240 + sampleBytes;
  }

  if (m_traces.isEmpty())
  {
    if (error)
      *error = QStringLiteral("File contains 0 traces");
    return false;
  }

  m_samplesPerTrace = maxNs > 0 ? maxNs : binNs;
  m_sampleIntervalUs = static_cast<float>(intervalUs);
  return true;
}
