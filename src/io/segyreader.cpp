// 层：数据
#include "segyreader.h"

#include "../domain/arearules.h"

#include <QFile>
#include <QCoreApplication>
#include <QFileInfo>
#include <QThread>
#include <QThreadPool>
#include <QtEndian>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

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

bool isEndTextRecord(const QByteArray &record)
{
  const QByteArray marker = QByteArrayLiteral("((SEG: ENDTEXT))");
  if (record.toUpper().contains(marker))
    return true;
  // SEG-Y textual headers may be encoded in EBCDIC. Decode only the bytes
  // needed for the EndText marker; other bytes cannot accidentally match it.
  QByteArray ascii;
  ascii.reserve(record.size());
  for (unsigned char ch : record)
  {
    if (ch >= 0xc1 && ch <= 0xc9) ascii.append('A' + ch - 0xc1);
    else if (ch >= 0xd1 && ch <= 0xd9) ascii.append('J' + ch - 0xd1);
    else if (ch >= 0xe2 && ch <= 0xe9) ascii.append('S' + ch - 0xe2);
    else if (ch >= 0x81 && ch <= 0x89) ascii.append('A' + ch - 0x81);
    else if (ch >= 0x91 && ch <= 0x99) ascii.append('J' + ch - 0x91);
    else if (ch >= 0xa2 && ch <= 0xa9) ascii.append('S' + ch - 0xa2);
    else if (ch == 0x4d) ascii.append('(');
    else if (ch == 0x5d) ascii.append(')');
    else if (ch == 0x7a) ascii.append(':');
    else if (ch == 0x40) ascii.append(' ');
    else ascii.append('?');
  }
  return ascii.contains(marker);
}

// 四角 slot：(inlMin,xlMin)=0 (inlMin,xlMax)=1 (inlMax,xlMax)=2 (inlMax,xlMin)=3。
// 中点二分近似——对整齐测网（inline/xline 单调）即精确四角。
// #83：inline/xline 是不可信道头里的任意 qint32——`n * 2` 与 `min + max`
// 在 32 位下会有符号溢出（UB），一律提升到 64 位比较。
int cornerSlot(qint32 inlineNo, qint32 xlineNo, const SegyGeometry &g)
{
  const bool iLo = qint64(inlineNo) * 2 <= qint64(g.inlineMin) + qint64(g.inlineMax);
  const bool xLo = qint64(xlineNo) * 2 <= qint64(g.xlineMin) + qint64(g.xlineMax);
  return iLo ? (xLo ? 0 : 1) : (xLo ? 3 : 2);
}

// ── 道头方言探针：open() 顺序扫描与 openCached() 并行扫描共用同一判据 ──
// （demo 工区 docx 道头契约「道号位置 21 / X=181 / Y=185」）。抽查首段
// ≤4096 道；负 ns 在固定步长前缀下按坏道跳过（与主扫描同语义）。
// 两条路径判据若漂移，该方言体只在其中一条扫得全——号域退化即由此而来。
bool probeFieldVaries(QFile &file, qint64 firstTraceOffset, qint64 fileSize,
                      int binNs, int fieldOff)
{
  qint32 first = 0;
  bool haveFirst = false, varies = false;
  uchar h[240];
  qint64 probeOffset = firstTraceOffset;
  for (int i = 0; i < 4096 && !varies && probeOffset + 240 <= fileSize; ++i)
  {
    if (!file.seek(probeOffset) ||
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
    const qint16 traceNs = beI16(h + 114);
    if (traceNs < 0)
    {
      probeOffset += 240 + static_cast<qint64>(binNs) * 4;
      continue;
    }
    const int ns = traceNs > 0 ? traceNs : binNs;
    probeOffset += 240 + static_cast<qint64>(ns) * 4;
  }
  return varies;
}

bool probeFieldAllZero(QFile &file, qint64 firstTraceOffset, qint64 fileSize,
                       int binNs, int fieldOff)
{
  uchar h[240];
  qint64 probeOffset = firstTraceOffset;
  for (int i = 0; i < 4096 && probeOffset + 240 <= fileSize; ++i)
  {
    if (!file.seek(probeOffset) ||
        file.read(reinterpret_cast<char *>(h), 240) != 240)
      return false;
    if (beI32(h + fieldOff) != 0)
      return false;
    const qint16 traceNs = beI16(h + 114);
    if (traceNs < 0)
    {
      probeOffset += 240 + static_cast<qint64>(binNs) * 4;
      continue;
    }
    const int ns = traceNs > 0 ? traceNs : binNs;
    probeOffset += 240 + static_cast<qint64>(ns) * 4;
  }
  return true;
}

bool probePairHasNonZero(QFile &file, qint64 firstTraceOffset, qint64 fileSize,
                         int binNs, int offA, int offB)
{
  uchar h[240];
  qint64 probeOffset = firstTraceOffset;
  for (int i = 0; i < 4096 && probeOffset + 240 <= fileSize; ++i)
  {
    if (!file.seek(probeOffset) ||
        file.read(reinterpret_cast<char *>(h), 240) != 240)
      return false;
    if (beI32(h + offA) != 0 || beI32(h + offB) != 0)
      return true;
    const qint16 traceNs = beI16(h + 114);
    if (traceNs < 0)
    {
      probeOffset += 240 + static_cast<qint64>(binNs) * 4;
      continue;
    }
    const int ns = traceNs > 0 ? traceNs : binNs;
    probeOffset += 240 + static_cast<qint64>(ns) * 4;
  }
  return false;
}
} // namespace

bool SegyReader::open(const QString &path, QString *error,
                      const SegyOptions *opts)
{
  // #83：完整复位（含坏道列表、上次取消留下的 m_lastScanPartial/
  // m_scannedOffset）——同一 reader 重开时不得累加坏道、不得把成功扫描的
  // snapshot 写成 complete=false。
  resetState();
  m_indexingRules = AreaRules::active().segy;

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
  m_sawVariableNs = false;
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

  qint64 offset = 3600;
  if (extHeaders < -1)
  {
    if (error) *error = QStringLiteral("Invalid extended textual header count %1").arg(extHeaders);
    return false;
  }
  if (extHeaders == -1)
  {
    bool foundEnd = false;
    while (offset + 3200 <= fileSize)
    {
      if (!file.seek(offset)) break;
      const QByteArray record = file.read(3200);
      if (record.size() != 3200) break;
      offset += 3200;
      if (isEndTextRecord(record))
      {
        foundEnd = true;
        break;
      }
    }
    if (!foundEnd)
    {
      if (error) *error = QStringLiteral("Extended textual headers have no SEG EndText record");
      return false;
    }
  }
  else
    offset += static_cast<qint64>(extHeaders) * 3200;
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
  // 道头偏移经 AreaRules（segy 道号索引约定；第二工区方言经 project_area.json
  // 覆盖，见 docs/AREA_PARAMETERS.md）——open() 开始取一份快照，全程一致。
  const AreaRules::SegyIndexing sidx = m_indexingRules;
  // 方言探针（命名空间级共用实现，与 openCached() 并行路径同一判据）。
  auto fieldVaries = [&](int fieldOff) -> bool {
    return probeFieldVaries(file, offset, fileSize, binNs, fieldOff);
  };
  const bool ordinalIndex = !fieldVaries(sidx.inlineWordOffset);
  const int binEnsTraces = beI16(bin + 12); // 二进制头字节 13-14：每条 inline 道数

  // P?（demo 工区方言，工区 docx 道头契约「道号位置 21 / X=181 / Y=185」）：
  // ①标准 inline 位有效、crossline 位（字节 193）恒 0 且 CDP（字节 21）在变化
  //   → crossline 取 CDP；
  // ②角点坐标对（偏移 72/76，源点 X/Y）恒 0 而 CDP X/Y（偏移 180/184）非零
  //   → 角点取 181-188。两者都是探针判据（≤4096 道），既有布局（72/76 有值、
  //   193 有值）不触发，行为逐字节不变。
  auto fieldAllZero = [&](int fieldOff) -> bool {
    return probeFieldAllZero(file, offset, fileSize, binNs, fieldOff);
  };
  auto pairHasNonZero = [&](int offA, int offB) -> bool {
    return probePairHasNonZero(file, offset, fileSize, binNs, offA, offB);
  };
  const bool xlineFromCdp = !ordinalIndex &&
                            fieldAllZero(sidx.crosslineWordOffset) &&
                            fieldVaries(sidx.cdpXlineOffset);
  const bool cornerFromCdpXY = fieldAllZero(72) && pairHasNonZero(180, 184);

  // 索引道：只读 240 字节道头；样本区用 seek 跳过——打开内存不随体增长。
  uchar trHdr[240];
  bool sawTrace = false;
  double delayMs = 0.0;
  qint32 firstLineWord = 0; // ordinal 模式：偏移 188 的恒定值（inline 起点兜底）
  qint32 firstFieldRec = 0; // ordinal 模式：首道 field record（偏移 8）= inline 起点
  QVector<double> ordX, ordY; // ordinal 模式的坐标序列（一致性校验 + 角点）
  while (offset + 240 <= fileSize)
  {
    // D1 进度/取消：每 128 道 ≈ 数百 KB–数 MB 一次，任务面板据此画进度条。
    if (opts && (m_index.size() % 128) == 0)
    {
      if (opts->cancel && opts->cancel())
      {
        if (error)
          *error = QStringLiteral("cancelled");
        // D2.8：取消不清索引——m_lastScanPartial + m_scannedOffset 供
        // openCached() checkpoint 续扫（ordinal 模式由调用方识别后放弃）。
        m_lastScanPartial = true;
        m_scannedOffset = offset;
        return false;
      }
      if (opts->progress)
        opts->progress(offset, fileSize);
    }
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
      // B6（wave/deepen-perf）坏道跳过放宽：固定步长布局（此前所有道
      // ns==binNs 或 0）下，负 ns 与并行/resume 路径同语义——跳过并记录，
      // 不整体作废；变道长布局下坏道的后续边界不可恢复，保持整索引报错
      //（契约记录 docs/perf/INDEX_FORMAT.md §3）。
      if (m_sawVariableNs)
      {
        if (error)
          *error = QStringLiteral("Corrupt trace ns (%1) at trace index %2 in a "
                                  "variable-trace-length layout (alignment unrecoverable)")
                       .arg(traceNs)
                       .arg(m_index.size());
        m_index.clear();
        return false;
      }
      if (fileSize - (offset + 240) < static_cast<qint64>(binNs) * 4)
      {
        if (error) *error = QCoreApplication::translate("SegyReader",
            "坏道样点区截断（偏移 %1，需要 %2 字节，剩余 %3）")
            .arg(offset + 240).arg(static_cast<qint64>(binNs) * 4).arg(fileSize - offset - 240);
        m_index.clear();
        return false;
      }
      m_badTraceOffsets.append(offset); // 仅完整的固定道长坏道才可跳过
      offset += 240 + static_cast<qint64>(binNs) * 4;
      continue;
    }
    const int ns = traceNs > 0 ? traceNs : binNs;
    if (traceNs > 0 && traceNs != binNs)
      m_sawVariableNs = true; // B6：变道长布局观察——checkpoint 契约门用
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

    // survey 坐标（角点）：偏移 72/76（源点 X/Y，1 基字节 73/77）或——探针判
    // 该对恒 0 且 CDP X/Y（偏移 180/184，1 基字节 181-188）非零时——后者
    //（demo 工区方言，见 open() 头注释）。比例因子（偏移 70）为 0 或 1 都按
    // 原值取整数；正值乘、负值除。
    const int coordXOff = cornerFromCdpXY ? 180 : 72;
    const int coordYOff = cornerFromCdpXY ? 184 : 76;
    const qint16 scal = beI16(trHdr + 70);
    const double coordScale =
        (scal == 0 || scal == 1) ? 1.0
                                 : (scal > 0 ? static_cast<double>(scal)
                                             : 1.0 / -static_cast<double>(scal));
    const double cx = static_cast<double>(beI32(trHdr + coordXOff)) * coordScale;
    const double cy = static_cast<double>(beI32(trHdr + coordYOff)) * coordScale;

    if (ordinalIndex)
    {
      // 探测段内 inline 字（约定偏移）不变才进这条路；半路再变说明探测段不
      // 代表全文件，按 plan 报出期望值和读到的值后停止，不改读别的字节。
      const qint32 lineWord = beI32(trHdr + sidx.inlineWordOffset);
      if (m_index.isEmpty())
      {
        firstLineWord = lineWord;
        firstFieldRec = beI32(trHdr + sidx.fieldRecordOffset); // 道号索引的 inline 起点（本文件 1315）
      }
      else if (lineWord != firstLineWord)
      {
        if (error)
          *error = QStringLiteral("inline word at trace-header offset %4 must stay constant for "
                                  "ordinal indexing: expected %1, read %2 at trace %3")
                       .arg(firstLineWord)
                       .arg(lineWord)
                       .arg(m_index.size())
                       .arg(sidx.inlineWordOffset);
        m_index.clear();
        return false;
      }
      e.inlineNo = 0;                 // 占位：收尾统一按道号赋值
      e.xlineNo = beI32(trHdr + sidx.cdpXlineOffset); // crossline = 该道 CDP（plan §2）
      ordX.append(cx);
      ordY.append(cy);
    }
    else
    {
      e.inlineNo = beI32(trHdr + sidx.inlineWordOffset);    // 标准 inline 字节位（1-based 字节 189）
      // 标准 crossline 字节位（1-based 字节 193）；探针判其恒 0 且 CDP 变化时
      // 回退取 CDP（demo 工区「道号位置 21」方言）。
      e.xlineNo = beI32(trHdr + (xlineFromCdp ? sidx.cdpXlineOffset
                                             : sidx.crosslineWordOffset));

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
      const int slot = cornerSlot(e.inlineNo, e.xlineNo, m_geometry);
      m_geometry.cornerX[slot] = cx;
      m_geometry.cornerY[slot] = cy;
    }

    m_index.append(e);
    offset += 240 + static_cast<qint64>(ns) * 4;
  }
  if (offset != fileSize)
  {
    if (error) *error = QCoreApplication::translate("SegyReader",
        "道头截断（偏移 %1，剩余 %2 字节，需要 240 字节）").arg(offset).arg(fileSize - offset);
    m_index.clear();
    return false;
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
    // #83：base 来自不可信道头——base + lines - 1 越过 qint32 即有符号溢出（UB）。
    if (qint64(base) + qint64(lines) - 1 > std::numeric_limits<qint32>::max())
    {
      if (error)
        *error = QStringLiteral("inline numbering overflows qint32 (base %1, %2 lines)")
                     .arg(base)
                     .arg(lines);
      m_index.clear();
      return false;
    }
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
  if (!ordinalIndex && !freezeCornersFromFile(file, cornerFromCdpXY))
  {
    if (error)
      *error = QStringLiteral("无法读取测区四角的道头");
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

bool SegyReader::decodeTrace(QFile &file, const IndexEntry &e, SegyTrace *out,
                              SegySampleIssue *issue) const
{
  uchar trHdr[240];
  if (!file.seek(e.offset) || file.read(reinterpret_cast<char *>(trHdr), 240) != 240)
    return false;

  const qint16 traceNs = beI16(trHdr + 114);
  if (traceNs < 0) return false; // 打开后损坏的道头不得伪装成正常 fallback
  const int ns = traceNs > 0 ? traceNs : m_samplesPerTrace;

  QByteArray raw(static_cast<int>(ns) * 4, Qt::Uninitialized);
  if (file.read(raw.data(), raw.size()) != raw.size())
    return false;

  SegyTrace t;
  t.tracl = beI32(trHdr + 0);
  t.cdp = beI32(trHdr + 20);
  t.lineNo = e.inlineNo != 0 ? e.inlineNo : m_binLineNo;
  t.xlineNo = e.xlineNo;
  const qint16 traceDt = beI16(trHdr + 116);
  t.sampleIntervalUs = traceDt > 0 ? traceDt : m_sampleIntervalUs;
  t.startTimeMs = beI16(trHdr + 108);
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
  *issue = SegySampleIssue{};
  issue->traceOffset = e.offset;
  for (int i = 0; i < ns; ++i)
  {
    if (!std::isfinite(t.samples[i]))
    {
      t.samples[i] = std::numeric_limits<float>::quiet_NaN();
      ++issue->sampleCount;
      if (issue->sampleIndices.size() < 32) issue->sampleIndices.append(i);
    }
  }
  *out = t;
  return true;
}

QVector<SegyTrace> SegyReader::readByIndexList(const QVector<int> &idxs,
                                             const SegyOptions *opts,
                                             SegyReadReport *report) const
{
  QVector<SegyTrace> out;
  SegyReadReport quality;
  quality.requestedTraceCount = idxs.size();
  QFile file(m_path);
  if (!idxs.isEmpty() && !file.open(QIODevice::ReadOnly))
  {
    for (int i : idxs) quality.failedTraceOffsets.append(m_index.at(i).offset);
    quality.message = QCoreApplication::translate("SegyReader", "无法读取 SEG-Y %1：%2")
                          .arg(m_path, file.errorString());
  }
  else
  {
    out.reserve(idxs.size());
    int n = 0;
    for (int i : idxs)
    {
      if (opts && (n % 64) == 0)
      {
        if (opts->cancel && opts->cancel())
        {
          quality.cancelled = true;
          break;
        }
        if (opts->progress) opts->progress(n, idxs.size());
      }
      SegyTrace trace;
      SegySampleIssue issue;
      if (decodeTrace(file, m_index.at(i), &trace, &issue))
      {
        out.append(trace);
        if (issue.sampleCount > 0)
        {
          quality.sanitizedSampleCount += issue.sampleCount;
          quality.sanitizedTraces.append(issue);
        }
      }
      else quality.failedTraceOffsets.append(m_index.at(i).offset);
      ++n;
    }
  }
  quality.decodedTraceCount = out.size();
  QStringList messages;
  if (!quality.message.isEmpty()) messages.append(quality.message);
  if (quality.cancelled)
    messages.append(QCoreApplication::translate("SegyReader", "读取已取消（已解码 %1/%2 道）")
                        .arg(quality.decodedTraceCount).arg(quality.requestedTraceCount));
  if (quality.sanitizedSampleCount > 0)
    messages.append(QCoreApplication::translate("SegyReader",
        "已清洗 %1 个非有限样点为缺失值（%2 道；首道偏移 %3）")
        .arg(quality.sanitizedSampleCount).arg(quality.sanitizedTraces.size())
        .arg(quality.sanitizedTraces.first().traceOffset));
  if (!quality.failedTraceOffsets.isEmpty())
    messages.append(QCoreApplication::translate("SegyReader",
        "解码失败 %1/%2 道（首道偏移 %3），返回部分结果")
        .arg(quality.failedTraceOffsets.size()).arg(idxs.size())
        .arg(quality.failedTraceOffsets.first()));
  quality.message = messages.join(QStringLiteral("；"));
  if (report) *report = quality;
  // 保留既有解码丢道日志契约；新增 DTO 提供偏移与本地化详情。
  if (!quality.failedTraceOffsets.isEmpty())
    qWarning("SegyReader::readByIndexList(%s): dropped %d of %d traces (decode/read failure)",
             qPrintable(m_path), int(quality.failedTraceOffsets.size()), int(idxs.size()));
  if (quality.sanitizedSampleCount > 0 || quality.cancelled || !file.isOpen())
    if (!quality.message.isEmpty()) qWarning().noquote() << quality.message;
  return out;
}

QVector<SegyTrace> SegyReader::traces(const SegyOptions *opts, SegyReadReport *report) const
{
  QVector<int> all;
  all.reserve(m_index.size());
  for (int i = 0; i < m_index.size(); ++i) all.append(i);
  return readByIndexList(all, opts, report);
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

bool SegyReader::readInline(qint32 inlineNo, QVector<SegyTrace> *out,
                            QString *error, const SegyOptions *opts, SegyReadReport *report) const
{
  if (report) *report = SegyReadReport{};
  if (error) error->clear();
  if (!out)
  {
    if (error) *error = QCoreApplication::translate("SegyReader", "解码输出缓冲为空");
    return false;
  }
  out->clear();
  if (!m_byInline.contains(inlineNo))
  {
    if (error)
      *error = QStringLiteral("inline %1 not present in %2").arg(inlineNo).arg(m_path);
    return false;
  }
  const QVector<int> &idxs = m_byInline.value(inlineNo);
  SegyReadReport quality;
  *out = readByIndexList(idxs, opts, &quality);
  if (report) *report = quality;
  if (quality.cancelled)
  {
    if (error) *error = QCoreApplication::translate("SegyReader", "读取已取消");
    return false;
  }
  if (out->size() != idxs.size())
  {
    if (error)
      *error = QStringLiteral("failed to decode %1 of %2 traces for inline %3 in %4")
                   .arg(idxs.size() - out->size())
                   .arg(idxs.size())
                   .arg(inlineNo)
                   .arg(m_path);
    return false;
  }
  return true;
}

bool SegyReader::readCrossline(qint32 xlineNo, QVector<SegyTrace> *out,
                               QString *error, const SegyOptions *opts, SegyReadReport *report) const
{
  if (report) *report = SegyReadReport{};
  if (error) error->clear();
  if (!out)
  {
    if (error) *error = QCoreApplication::translate("SegyReader", "解码输出缓冲为空");
    return false;
  }
  out->clear();
  if (!m_byXline.contains(xlineNo))
  {
    if (error)
      *error = QStringLiteral("crossline %1 not present in %2").arg(xlineNo).arg(m_path);
    return false;
  }
  const QVector<int> &idxs = m_byXline.value(xlineNo);
  SegyReadReport quality;
  *out = readByIndexList(idxs, opts, &quality);
  if (report) *report = quality;
  if (quality.cancelled)
  {
    if (error) *error = QCoreApplication::translate("SegyReader", "读取已取消");
    return false;
  }
  if (out->size() != idxs.size())
  {
    if (error)
      *error = QStringLiteral("failed to decode %1 of %2 traces for crossline %3 in %4")
                   .arg(idxs.size() - out->size())
                   .arg(idxs.size())
                   .arg(xlineNo)
                   .arg(m_path);
    return false;
  }
  return true;
}

// ===========================================================================
// wave/io-perf-cache D2：磁盘索引层（openCached/snapshot/restore）+ 并行扫描
// + 断点续扫 + 空洞统计。open() 的顺序扫描语义保持不变（上方原实现）。
// ===========================================================================
void SegyReader::resetState()
{
  m_indexingRules = AreaRules::SegyIndexing{};
  m_index.clear();
  m_byInline.clear();
  m_byXline.clear();
  m_badTraceOffsets.clear();
  m_samplesPerTrace = 0;
  m_sampleIntervalUs = 0.0f;
  m_geometry = SegyGeometry();
  m_path.clear();
  m_firstTraceOffset = 0;
  m_formatCode = 0;
  m_binLineNo = 0;
  m_lastScanPartial = false;
  m_scannedOffset = 0;
  m_sawVariableNs = false;
}

void SegyReader::rebuildLineHashes()
{
  m_byInline.clear();
  m_byXline.clear();
  for (int i = 0; i < m_index.size(); ++i)
    m_byInline[m_index.at(i).inlineNo].append(i);
  for (int i = 0; i < m_index.size(); ++i)
    m_byXline[m_index.at(i).xlineNo].append(i);
  const QVector<IndexEntry> &idx = m_index;
  for (auto it = m_byInline.begin(); it != m_byInline.end(); ++it)
    std::sort(it.value().begin(), it.value().end(),
              [&idx](int a, int b) { return idx.at(a).xlineNo < idx.at(b).xlineNo; });
  for (auto it = m_byXline.begin(); it != m_byXline.end(); ++it)
    std::sort(it.value().begin(), it.value().end(),
              [&idx](int a, int b) { return idx.at(a).inlineNo < idx.at(b).inlineNo; });
}

bool SegyReader::snapshot(SegyIndexStore::StoredIndex *out) const
{
  if (m_index.isEmpty() || m_path.isEmpty())
    return false;
  const QFileInfo fi(m_path);
  out->ident = SegyIndexStore::identityOf(fi);
  out->samplesPerTrace = m_samplesPerTrace;
  out->sampleIntervalUs = qRound(m_sampleIntervalUs);
  out->formatCode = m_formatCode;
  out->binLineNo = m_binLineNo;
  out->firstTraceOffset = m_firstTraceOffset;
  out->geometry = m_geometry;
  out->headerWordOffsets = {m_indexingRules.inlineWordOffset, m_indexingRules.crosslineWordOffset,
                            m_indexingRules.fieldRecordOffset, m_indexingRules.cdpXlineOffset};
  out->inlineNos.resize(m_index.size());
  out->xlineNos.resize(m_index.size());
  out->offsets.resize(m_index.size());
  for (int i = 0; i < m_index.size(); ++i)
  {
    out->inlineNos[i] = m_index.at(i).inlineNo;
    out->xlineNos[i] = m_index.at(i).xlineNo;
    out->offsets[i] = m_index.at(i).offset;
  }
  out->badTraceOffsets = m_badTraceOffsets;
  out->complete = !m_lastScanPartial;
  out->scannedOffset = m_lastScanPartial ? m_scannedOffset : fi.size();
  out->prefixFingerprint =
      SegyIndexStore::prefixFingerprintOf(m_path, qMin<qint64>(64 * 1024, fi.size()));
  return true;
}

bool SegyReader::restore(const SegyIndexStore::StoredIndex &in, const QString &path)
{
  if (in.inlineNos.size() != in.xlineNos.size() ||
      in.inlineNos.size() != in.offsets.size() || in.inlineNos.isEmpty())
    return false;
  m_indexingRules = {in.headerWordOffsets[0], in.headerWordOffsets[1],
                     in.headerWordOffsets[2], in.headerWordOffsets[3]};
  m_samplesPerTrace = in.samplesPerTrace;
  m_sampleIntervalUs = static_cast<float>(in.sampleIntervalUs);
  m_formatCode = in.formatCode;
  m_binLineNo = in.binLineNo;
  m_firstTraceOffset = in.firstTraceOffset;
  m_geometry = in.geometry;
  m_badTraceOffsets = in.badTraceOffsets;
  m_index.resize(in.inlineNos.size());
  for (int i = 0; i < in.inlineNos.size(); ++i)
    m_index[i] = IndexEntry{in.inlineNos.at(i), in.xlineNos.at(i), in.offsets.at(i)};
  m_path = path;
  m_lastScanPartial = !in.complete;
  m_scannedOffset = in.scannedOffset;
  rebuildLineHashes();
  return true;
}

SegyIndexStore::IndexStats SegyReader::indexStats() const
{
  SegyIndexStore::StoredIndex snap;
  if (!snapshot(&snap))
    return SegyIndexStore::IndexStats();
  return SegyIndexStore::computeStats(snap);
}

bool SegyReader::scanParallel(QFile &file, qint64 firstTraceOffset, qint64 traceSize,
                              qint64 traceCount, QString *error, const SegyOptions *opts,
                              bool xlineFromCdp, bool cornerFromCdpXY)
{
  Q_UNUSED(file);
  const QString path = m_path;
  const AreaRules::SegyIndexing sidx = m_indexingRules;
  const int ns = m_samplesPerTrace;
  // 方言字节位（与顺序 open() 同判据，调用方探针后传入）：crossline 恒 0 且
  // CDP 变化 → crossline 取 CDP；源点坐标恒 0 且 CDP X/Y 非零 → 角点取 181-188。
  const int xlineOff = xlineFromCdp ? sidx.cdpXlineOffset : sidx.crosslineWordOffset;
  const int coordXOff = cornerFromCdpXY ? 180 : 72;
  const int coordYOff = cornerFromCdpXY ? 184 : 76;

  struct Shard
  {
      qint64 from = 0, to = 0;
      bool ok = false;
      bool cancelled = false;
      QString err;
      QVector<qint32> inlines, xlines;
      QVector<qint64> offsets, bad;
      QVector<double> xs, ys;
  };
  const int maxThreads = qBound(1, qMin(4, QThread::idealThreadCount()), 4);
  const int shardCount = static_cast<int>(qMin<qint64>(traceCount, maxThreads));
  std::vector<Shard> shards(static_cast<size_t>(shardCount));
  const qint64 per = traceCount / shardCount;
  const qint64 rem = traceCount % shardCount;
  qint64 start = 0;
  for (int s = 0; s < shardCount; ++s)
  {
    const qint64 count = per + (s < static_cast<int>(rem) ? 1 : 0);
    shards[static_cast<size_t>(s)].from = start;
    shards[static_cast<size_t>(s)].to = start + count;
    start += count;
  }

  std::atomic_bool cancelled{false};
  std::mutex progressMutex;
  QThreadPool pool;
  pool.setMaxThreadCount(maxThreads);
  for (int s = 0; s < shardCount; ++s)
  {
    Shard &sh = shards[static_cast<size_t>(s)];
    pool.start([&sh, &cancelled, &progressMutex, path, firstTraceOffset, traceSize, traceCount, &sidx, ns,
                xlineOff, coordXOff, coordYOff, opts]() {
      QFile local(path);
      if (!local.open(QIODevice::ReadOnly))
      {
        sh.err = QStringLiteral("cannot reopen %1").arg(path);
        return;
      }
      uchar h[240];
      for (qint64 i = sh.from; i < sh.to; ++i)
      {
        if ((i & 127) == 0 &&
            (cancelled.load() || (opts && opts->cancel && opts->cancel())))
        {
          cancelled.store(true);
          sh.cancelled = true;
          sh.err = QStringLiteral("cancelled");
          return;
        }
        const qint64 offset = firstTraceOffset + i * traceSize;
        if (!local.seek(offset) ||
            local.read(reinterpret_cast<char *>(h), 240) != 240)
        {
          sh.err = QStringLiteral("Truncated trace header at offset %1").arg(offset);
          return;
        }
        const qint16 traceNs = beI16(h + 114);
        if (traceNs < 0 || traceNs > ns)
        {
          sh.bad.append(offset); // D2.7：固定道长布局——跳过并记录，不整体作废
          continue;
        }
        const qint16 scal = beI16(h + 70);
        const double coordScale =
            (scal == 0 || scal == 1) ? 1.0
                                      : (scal > 0 ? static_cast<double>(scal)
                                                  : 1.0 / -static_cast<double>(scal));
        sh.inlines.append(beI32(h + sidx.inlineWordOffset));
        sh.xlines.append(beI32(h + xlineOff));
        sh.offsets.append(offset);
        sh.xs.append(static_cast<double>(beI32(h + coordXOff)) * coordScale);
        sh.ys.append(static_cast<double>(beI32(h + coordYOff)) * coordScale);
        if (opts && opts->progress && ((i - sh.from) % 128) == 0)
        {
          std::lock_guard<std::mutex> pLock(progressMutex);
          opts->progress(offset, firstTraceOffset + traceCount * traceSize);
        }
      }
      sh.ok = true;
    });
  }
  while (!pool.waitForDone(50))
  {
    if (opts && opts->cancel && opts->cancel())
      cancelled.store(true);
  }

  // 合并（分片连续有序 → 文件道序保持）。
  for (const Shard &sh : shards)
  {
    for (int i = 0; i < sh.inlines.size(); ++i)
      m_index.append(IndexEntry{sh.inlines.at(i), sh.xlines.at(i), sh.offsets.at(i)});
    m_badTraceOffsets += sh.bad;
    if (!sh.ok)
    {
      // 分片失败（取消/坏道头读失败）：保留「连续前缀」为部分索引。
      m_lastScanPartial = true;
      m_scannedOffset = firstTraceOffset + static_cast<qint64>(m_index.size() + m_badTraceOffsets.size()) * traceSize;
      if (error)
        *error = sh.err;
      return false;
    }
  }

  // 几何冻结（与顺序路径同一算法：运行 min/max + 中点二分四角槽位）。
  QVector<double> xs, ys;
  for (const Shard &sh : shards)
  {
    xs += sh.xs;
    ys += sh.ys;
  }
  if (!m_index.isEmpty())
  {
    m_geometry.inlineMin = m_geometry.inlineMax = m_index.first().inlineNo;
    m_geometry.xlineMin = m_geometry.xlineMax = m_index.first().xlineNo;
    m_geometry.cornerX[0] = m_geometry.cornerX[1] = m_geometry.cornerX[2] = m_geometry.cornerX[3] = xs.first();
    m_geometry.cornerY[0] = m_geometry.cornerY[1] = m_geometry.cornerY[2] = m_geometry.cornerY[3] = ys.first();
    for (int i = 0; i < m_index.size(); ++i)
    {
      if (m_index.at(i).inlineNo < m_geometry.inlineMin)
        m_geometry.inlineMin = m_index.at(i).inlineNo;
      if (m_index.at(i).inlineNo > m_geometry.inlineMax)
        m_geometry.inlineMax = m_index.at(i).inlineNo;
      if (m_index.at(i).xlineNo < m_geometry.xlineMin)
        m_geometry.xlineMin = m_index.at(i).xlineNo;
      if (m_index.at(i).xlineNo > m_geometry.xlineMax)
        m_geometry.xlineMax = m_index.at(i).xlineNo;
      const int slot = cornerSlot(m_index.at(i).inlineNo, m_index.at(i).xlineNo, m_geometry);
      m_geometry.cornerX[slot] = xs.at(i);
      m_geometry.cornerY[slot] = ys.at(i);
    }
  }
  freezeCorners(xs, ys);
  // 首道 delay（字 109-110）：重读一次首道头。
  {
    QFile local(path);
    if (local.open(QIODevice::ReadOnly) && local.seek(firstTraceOffset))
    {
      uchar h[240];
      if (local.read(reinterpret_cast<char *>(h), 240) == 240)
        m_geometry.startTimeMs = static_cast<double>(beI16(h + 108));
    }
  }
  m_lastScanPartial = false;
  m_scannedOffset = firstTraceOffset + traceCount * traceSize;
  rebuildLineHashes();
  return true;
}

bool SegyReader::resumeScan(QFile &file, const SegyIndexStore::StoredIndex &partial,
                            QString *error, const SegyOptions *opts)
{
  // 续扫前提：固定道长（ns 恒定）——增长量必须是整道。
  const qint64 traceSize = 240 + qint64(partial.samplesPerTrace) * 4;
  const qint64 fileSize = file.size();
  if ((fileSize - partial.scannedOffset) % traceSize != 0)
  {
    if (error)
      *error = QStringLiteral("appended bytes are not whole traces (%1 + %2 bytes)")
                   .arg(partial.scannedOffset)
                   .arg(fileSize - partial.scannedOffset);
    return false;
  }
  const AreaRules::SegyIndexing sidx = m_indexingRules;
  uchar trHdr[240];
  qint64 offset = partial.scannedOffset;
  while (offset + 240 <= fileSize)
  {
    if (opts && (m_index.size() % 128) == 0)
    {
      if (opts->cancel && opts->cancel())
      {
        if (error)
          *error = QStringLiteral("cancelled");
        m_lastScanPartial = true;
        m_scannedOffset = offset;
        return false;
      }
      if (opts->progress)
        opts->progress(offset, fileSize);
    }
    if (!file.seek(offset) || file.read(reinterpret_cast<char *>(trHdr), 240) != 240)
    {
      if (error)
        *error = QStringLiteral("Truncated trace header at offset %1").arg(offset);
      return false;
    }
    const qint16 traceNs = beI16(trHdr + 114);
    if (traceNs < 0 || traceNs > partial.samplesPerTrace)
    {
      m_badTraceOffsets.append(offset); // D2.7
      offset += traceSize;
      continue;
    }
    if (m_sampleIntervalUs <= 0.0f)
    {
      const qint16 traceDt = beI16(trHdr + 116);
      if (traceDt > 0)
        m_sampleIntervalUs = static_cast<float>(traceDt);
    }
    IndexEntry e;
    e.offset = offset;
    e.inlineNo = beI32(trHdr + sidx.inlineWordOffset);
    e.xlineNo = beI32(trHdr + sidx.crosslineWordOffset);
    // 几何延续（同一运行 min/max + 槽位规则）。
    const qint16 scal = beI16(trHdr + 70);
    const double coordScale =
        (scal == 0 || scal == 1) ? 1.0
                                 : (scal > 0 ? static_cast<double>(scal)
                                             : 1.0 / -static_cast<double>(scal));
    const double cx = static_cast<double>(beI32(trHdr + 72)) * coordScale;
    const double cy = static_cast<double>(beI32(trHdr + 76)) * coordScale;
    if (m_index.isEmpty())
    {
      m_geometry.inlineMin = m_geometry.inlineMax = e.inlineNo;
      m_geometry.xlineMin = m_geometry.xlineMax = e.xlineNo;
      for (int k = 0; k < 4; ++k)
      {
        m_geometry.cornerX[k] = cx;
        m_geometry.cornerY[k] = cy;
      }
    }
    else
    {
      m_geometry.inlineMin = qMin<qint32>(m_geometry.inlineMin, e.inlineNo);
      m_geometry.inlineMax = qMax<qint32>(m_geometry.inlineMax, e.inlineNo);
      m_geometry.xlineMin = qMin<qint32>(m_geometry.xlineMin, e.xlineNo);
      m_geometry.xlineMax = qMax<qint32>(m_geometry.xlineMax, e.xlineNo);
      const int slot = cornerSlot(e.inlineNo, e.xlineNo, m_geometry);
      m_geometry.cornerX[slot] = cx;
      m_geometry.cornerY[slot] = cy;
    }
    m_index.append(e);
    offset += traceSize;
  }
  if (!freezeCornersFromFile(file))
  {
    if (error)
      *error = QStringLiteral("无法读取测区四角的道头");
    return false;
  }
  m_lastScanPartial = false;
  m_scannedOffset = fileSize;
  rebuildLineHashes();
  return true;
}

bool SegyReader::openCached(const QString &path, const QString &indexCacheDir,
                            QString *error, const SegyOptions *opts)
{
  resetState();
  m_indexingRules = AreaRules::active().segy;
  const std::array<int, 4> requestedWords = {m_indexingRules.inlineWordOffset,
      m_indexingRules.crosslineWordOffset, m_indexingRules.fieldRecordOffset, m_indexingRules.cdpXlineOffset};
  SegyIndexStore::ensureLegacyGlobalCacheDir(); // D2.1：vendor 全局缓存目录预建
  const QFileInfo fi(path);
  if (!fi.exists())
  {
    if (error) *error = QStringLiteral("File does not exist: %1").arg(path);
    return false;
  }
  SegyIndexStore store(indexCacheDir);

  // 1) 精确身份命中（完整索引）→ 免扫直读。
  QString reason;
  if (auto stored = store.load(fi, /*partialOk=*/false, &reason))
  {
    if (stored->headerWordOffsets == requestedWords && restore(*stored, path))
    {
      if (opts && opts->progress)
        opts->progress(fi.size(), fi.size()); // 命中即收敛 100%
      return true;
    }
  }

  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
  {
    if (error) *error = QStringLiteral("Cannot open file: %1").arg(path);
    return false;
  }

  // 2) checkpoint / 追加增长 → 断点续扫（D2.5/D2.8）。
  if (auto partial = store.loadForResume(fi, &reason))
  {
    if (partial->headerWordOffsets == requestedWords && restore(*partial, path))
    {
      if (resumeScan(file, *partial, error, opts))
      {
        SegyIndexStore::StoredIndex done;
        if (snapshot(&done))
          store.save(done); // 完成态按新身份重存（失败降级无缓存）
        return true;
      }
      if (m_lastScanPartial)
      {
        // 续扫又被取消——checkpoint 再落一截。
        SegyIndexStore::StoredIndex cp;
        if (snapshot(&cp))
          store.save(cp);
      }
      return false;
    }
  }

  // 3) 全新扫描：固定道长 + 标准 inline 索引 → 并行（D2.9）；否则顺序 open()。
  {
    // 轻量头部解析（extHeaders >= 0 的常规布局；-1 EBCDIC 扩展头走顺序路径）。
    uchar bin[400];
    if (file.seek(3200) && file.read(reinterpret_cast<char *>(bin), 400) == 400)
    {
      const qint16 binDt = beI16(bin + 16);
      const qint16 binNs = beI16(bin + 20);
      const qint16 formatCode = beI16(bin + 24);
      const qint16 extHeaders = beI16(bin + 304);
      const qint64 firstTraceOffset = 3600 + static_cast<qint64>(extHeaders) * 3200;
      const qint64 traceSize = 240 + static_cast<qint64>(binNs) * 4;
      const qint64 tail = fi.size() - firstTraceOffset;
      bool eligible = extHeaders >= 0 && binNs > 0 && (formatCode == 1 || formatCode == 5) &&
                      tail > 0 && tail % traceSize == 0 && fi.size() > 1024 * 1024; // 1MB 以上才值得并行（分片开销换 IO 重叠）
      const AreaRules::SegyIndexing sidx = m_indexingRules;
      // inline 字必须在全文件范围内变化（ordinal 方言——偏移 188 恒定——其
      // 道号索引依赖全序上下文，走顺序路径）。抽查首/中/尾 + 前缀 8 道的 ns
      //（变道长判据；负 ns = 损坏道，不否定固定道长）。
      if (eligible)
      {
        const qint64 traceCountTotal = tail / traceSize;
        const qint64 probes[] = {0, traceCountTotal / 3, traceCountTotal * 2 / 3,
                                 traceCountTotal - 1};
        qint32 firstInline = 0;
        bool haveFirst = false;
        bool anyDifferent = false;
        for (qint64 t : probes)
        {
          uchar h[240];
          const qint64 off = firstTraceOffset + t * traceSize;
          if (!file.seek(off) || file.read(reinterpret_cast<char *>(h), 240) != 240)
          {
            eligible = false;
            break;
          }
          const qint32 v = beI32(h + sidx.inlineWordOffset);
          if (!haveFirst)
          {
            firstInline = v;
            haveFirst = true;
          }
          else if (v != firstInline)
          {
            anyDifferent = true;
          }
        }
        if (eligible && haveFirst && !anyDifferent)
          eligible = false; // 全程不变——ordinal 方言
        for (qint64 t = 0; eligible && t < 8 && t < traceCountTotal; ++t)
        {
          uchar h[240];
          const qint64 off = firstTraceOffset + t * traceSize;
          if (!file.seek(off) || file.read(reinterpret_cast<char *>(h), 240) != 240)
          {
            eligible = false;
            break;
          }
          const qint16 traceNs = beI16(h + 114);
          if (traceNs > 0 && traceNs != binNs)
            eligible = false; // 变道长布局——顺序路径
        }
      }
      if (eligible)
      {
        m_path = path;
        m_samplesPerTrace = binNs;
        m_sampleIntervalUs = static_cast<float>(binDt > 0 ? binDt : 0);
        m_formatCode = formatCode;
        m_firstTraceOffset = firstTraceOffset;
        const qint64 traceCount = tail / traceSize;
        // 方言探针（与顺序 open() 同判据）：并行分片按此选 crossline/坐标
        // 字节位。inline 恒定（ordinal 方言）已在 eligibility 中排除。
        const bool xlineFromCdp =
            probeFieldAllZero(file, firstTraceOffset, fi.size(), binNs,
                              sidx.crosslineWordOffset) &&
            probeFieldVaries(file, firstTraceOffset, fi.size(), binNs,
                             sidx.cdpXlineOffset);
        const bool cornerFromCdpXY =
            probeFieldAllZero(file, firstTraceOffset, fi.size(), binNs, 72) &&
            probePairHasNonZero(file, firstTraceOffset, fi.size(), binNs, 180, 184);
        if (scanParallel(file, firstTraceOffset, traceSize, traceCount, error, opts,
                         xlineFromCdp, cornerFromCdpXY))
        {
          SegyIndexStore::StoredIndex done;
          if (snapshot(&done))
            store.save(done);
          return true;
        }
        if (m_lastScanPartial)
        {
          SegyIndexStore::StoredIndex cp;
          if (snapshot(&cp))
            store.save(cp); // 取消 checkpoint（失败降级）
          return false;
        }
        // 非取消失败（坏道头读失败等）→ 落到顺序路径重试。
        resetState();
        file.seek(0);
      }
    }
  }

  // 4) 顺序 open()（既有语义），成功后发布索引。
  const bool ok = open(path, error, opts);
  if (ok)
  {
    SegyIndexStore::StoredIndex done;
    if (snapshot(&done))
      store.save(done);
    return true;
  }
  if (m_lastScanPartial)
  {
    // ordinal 方言（inlineNo 全 0 占位）没有可续扫的最终语义——不 checkpoint。
    bool ordinal = true;
    for (const IndexEntry &e : m_index)
      if (e.inlineNo != 0)
      {
        ordinal = false;
        break;
      }
    // B6：变道长布局不落 checkpoint——resumeScan 按固定步长推进，变道长的
    // 断点续扫会把错位的道头当好道收进索引（宁可重扫，不装作可续）。
    if (!ordinal && !m_index.isEmpty() && !m_sawVariableNs)
    {
      SegyIndexStore::StoredIndex cp;
      if (snapshot(&cp))
        store.save(cp);
    }
  }
  return false;
}

// 在最终号域内找四个极值角；道头顺序和扫描线程数不改变测区范围。
QVector<int> SegyReader::cornerTraceIndices() const
{
  QVector<int> indices(4, -1);
  const qint32 targetI[4] = {m_geometry.inlineMin, m_geometry.inlineMin,
                            m_geometry.inlineMax, m_geometry.inlineMax};
  const qint32 targetX[4] = {m_geometry.xlineMin, m_geometry.xlineMax,
                            m_geometry.xlineMax, m_geometry.xlineMin};
  for (int corner = 0; corner < 4; ++corner)
  {
    long double best = std::numeric_limits<long double>::infinity();
    for (int i = 0; i < m_index.size(); ++i)
    {
      const long double di = static_cast<long double>(m_index[i].inlineNo) - targetI[corner];
      const long double dx = static_cast<long double>(m_index[i].xlineNo) - targetX[corner];
      const long double distance = di * di + dx * dx;
      if (distance < best)
      {
        best = distance;
        indices[corner] = i;
      }
    }
  }
  return indices;
}

void SegyReader::freezeCorners(const QVector<double> &xs, const QVector<double> &ys)
{
  if (xs.size() != m_index.size() || ys.size() != m_index.size())
    return;
  const auto indices = cornerTraceIndices();
  for (int corner = 0; corner < indices.size(); ++corner)
    if (indices[corner] >= 0) {
      m_geometry.cornerX[corner] = xs[indices[corner]];
      m_geometry.cornerY[corner] = ys[indices[corner]];
    }
}

bool SegyReader::freezeCornersFromFile(QFile &file, bool fromCdpXY)
{
  // 标准顺序扫描不保留全体坐标数组，只重读四个极值道头（大体不增加线性内存）。
  const auto indices = cornerTraceIndices();
  uchar header[240];
  for (int corner = 0; corner < indices.size(); ++corner) {
    if (indices[corner] < 0 || !file.seek(m_index[indices[corner]].offset) ||
        file.read(reinterpret_cast<char *>(header), sizeof(header)) != sizeof(header))
      return false;
    const qint16 scale = beI16(header + 70);
    const double factor = scale == 0 ? 1.0 : scale > 0 ? double(scale) : 1.0 / -double(scale);
    m_geometry.cornerX[corner] = beI32(header + (fromCdpXY ? 180 : 72)) * factor;
    m_geometry.cornerY[corner] = beI32(header + (fromCdpXY ? 184 : 76)) * factor;
  }
  return true;
}
