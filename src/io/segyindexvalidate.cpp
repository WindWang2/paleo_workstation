// 层：数据
#include "segyindexvalidate.h"
#include "segyindexstore.h"
#include "segyreader.h" // sampleBytesForFormat

#include <QtGlobal>

#include <limits>

namespace SegyIndexAudit
{
namespace
{
// 溢出安全加法（summand ≥ 0 前提下）：a + b > cap 即失败，绝不回绕。
bool checkedAdd(qint64 a, qint64 b, qint64 cap, qint64 *out)
{
  if (b < 0 || b > cap - a) // b<0：负跨度不得进加法（调用点均 ≥240）
    return false;
  *out = a + b;
  return true;
}
} // namespace

QString validate(const SegyStoredIndex &idx, qint64 fileSize)
{
  if (fileSize < 0)
    return QStringLiteral("negative file size");
  // 头块：未知样点格式直接失败——不允许退化为「每样 1 字节」掩盖
  //（P2/W2：qMax(1, sampleBytes) 的老口径删除）。
  if (idx.samplesPerTrace <= 0 || idx.samplesPerTrace > 65535)
    return QStringLiteral("samples-per-trace out of range (%1)").arg(idx.samplesPerTrace);
  const int sampleBytes = SegyReader::sampleBytesForFormat(idx.formatCode);
  if (sampleBytes == 0)
    return QStringLiteral("unknown sample format code (%1)").arg(idx.formatCode);
  for (int word : idx.headerWordOffsets)
    if (word < 0 || word > 236)
      return QStringLiteral("header word offset out of range (%1)").arg(word);
  if (idx.firstTraceOffset < 3600)
    return QStringLiteral("first trace offset inside volume headers (%1)").arg(idx.firstTraceOffset);
  if (idx.firstTraceOffset > fileSize)
    return QStringLiteral("first trace offset beyond EOF (%1 > %2)").arg(idx.firstTraceOffset).arg(fileSize);
  if (idx.inlineNos.size() != idx.xlineNos.size() ||
      idx.inlineNos.size() != idx.offsets.size() || idx.inlineNos.isEmpty())
    return QStringLiteral("parallel index arrays disagree or are empty");
  const bool variable = idx.layoutKind == QLatin1Char('V');
  if (!variable && idx.layoutKind != QLatin1Char('F'))
    return QStringLiteral("unknown layout marker");
  if (variable && !idx.complete)
    return QStringLiteral("variable layout must not be a checkpoint");
  if (variable && idx.traceNs.size() != idx.inlineNos.size())
    return QStringLiteral("variable layout lacks per-trace sample table");

  // 统一道序走查（好道 + 坏道按偏移合并）：扫描器从 firstTraceOffset 起逐道
  // 推进，每个道槽要么入索引（好道）要么记坏道跳过——任何布局下合并序列
  // 必须无洞、无重叠、无重复地从 firstTraceOffset 平铺到已扫末尾。
  // · 固定道长：好/坏道跨度都是 240 + samplesPerTrace·sampleBytes；
  // · 变道长：好道按 traceNs 逐道跨度；坏道仅可能出现在「尚未观察到变道长
  //   的前缀」（B6 契约），该前缀按固定道长跳过 → 跨度同固定口径。
  const qsizetype n = idx.inlineNos.size();
  const qsizetype badN = idx.badTraceOffsets.size();
  const qint64 cap = std::numeric_limits<qint64>::max();
  qint64 expected = idx.firstTraceOffset;
  qsizetype g = 0, b = 0;
  while (g < n || b < badN)
  {
    const bool takeGood =
        b >= badN || (g < n && idx.offsets.at(g) < idx.badTraceOffsets.at(int(b)));
    const qint64 off = takeGood ? idx.offsets.at(int(g)) : idx.badTraceOffsets.at(int(b));
    if (g < n && b < badN && idx.offsets.at(int(g)) == idx.badTraceOffsets.at(int(b)))
      return QStringLiteral("duplicate trace offset (%1) in good/bad sets").arg(off);
    if (off != expected)
      return QStringLiteral("trace offset %1 breaks file order/alignment (expected %2)")
          .arg(off)
          .arg(expected);
    const int ns = takeGood ? (variable ? int(idx.traceNs.at(int(g))) : idx.samplesPerTrace)
                            : idx.samplesPerTrace;
    if (ns <= 0 || ns > 65535)
      return QStringLiteral("per-trace sample count out of range (%1)").arg(ns);
    qint64 span = 0, next = 0;
    if (!checkedAdd(qint64(240), qint64(ns) * sampleBytes, cap, &span) ||
        !checkedAdd(expected, span, cap, &next))
      return QStringLiteral("trace span overflow at offset %1").arg(off);
    if (next > fileSize)
      return QStringLiteral("trace payload beyond EOF (offset %1 needs %2 bytes, file %3)")
          .arg(off)
          .arg(span)
          .arg(fileSize);
    expected = next;
    if (takeGood)
      ++g;
    else
      ++b;
  }

  // checkpoint / 完整性闸（#1026 G5：删除 size+4096 宽容——部分索引的
  // scannedOffset 必须是文件内的道边界，且与走查末尾严格一致）。
  if (idx.complete)
  {
    if (idx.scannedOffset != fileSize || expected != fileSize)
      return QStringLiteral("complete index does not end at EOF (scanned %1, covered %2, file %3)")
          .arg(idx.scannedOffset)
          .arg(expected)
          .arg(fileSize);
  }
  else
  {
    if (idx.scannedOffset < idx.firstTraceOffset || idx.scannedOffset > fileSize)
      return QStringLiteral("checkpoint offset outside file (%1, file %2)")
          .arg(idx.scannedOffset)
          .arg(fileSize);
    if (idx.scannedOffset != expected)
      return QStringLiteral("checkpoint offset %1 disagrees with covered traces (%2)")
          .arg(idx.scannedOffset)
          .arg(expected);
  }
  return QString();
}
} // namespace SegyIndexAudit
