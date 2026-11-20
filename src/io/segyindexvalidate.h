// 层：数据
#pragma once
#include <QString>

struct SegyStoredIndex;

// io/ — SEG-Y 磁盘索引快照的共用边界/布局验证（P2/#1026）。
//
// load()/loadForResume()/restore() 三口同源：解码后的快照是外部磁盘数据
// （可能损坏/被篡改），在触碰任何 reader 内部状态之前必须先过本验证器。
// 验证只做 O(n) 纯算术（减法/checked 加法），不重读源文件——缓存恢复
// 不得伪装成样本重扫。
//
// 布局判据（v6 schema，见 docs/perf/INDEX_FORMAT.md §3）：
//  · 固定道长（layout 'F'）：每个好/坏道偏移与 firstTraceOffset 按整道长
//    同余，且完整道跨度（240 + samplesPerTrace·sampleBytes）在文件内；
//    好/坏偏移各自严格递增，合并仍严格递增（不重叠、不重复）。
//  · 变道长（layout 'V'）：traceNs 逐道记录有效 ns；首道恰在
//    firstTraceOffset，逐道连续（off[i+1] = off[i] + 240 + ns[i]·bytes），
//    不允许坏道（变道长下坏道边界不可恢复，扫描本就整索引报错），
//    不允许 checkpoint（resumeScan 按固定步长推进）。
//  · checkpoint（complete=false，仅固定道长）：firstTraceOffset ≤
//    scannedOffset ≤ fileSize（无 +4096 宽容）、与 firstTraceOffset 按道长
//    同余、≥ 已收录道的最大覆盖末尾。
//  · 完整索引：scannedOffset == fileSize 且已收录道覆盖到文件尾。
namespace SegyIndexAudit
{
// 返回空串 = 验证通过；否则返回可读原因（调用方写进 miss reason/自愈日志）。
// fileSize = 源文件当前大小（字节）。
QString validate(const SegyStoredIndex &idx, qint64 fileSize);
}
