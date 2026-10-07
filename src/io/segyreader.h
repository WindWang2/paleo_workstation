// 层：数据
#pragma once
#include <QHash>
#include <QString>
#include <QVector>

#include "../domain/sectiontrace.h"   // SegyOptions/SegyTrace/SegyGeometry/SegySectionGrid（domain 纯数据）
#include "segyindexstore.h"           // StoredIndex/IndexStats（D2 索引持久化）

#include <atomic>
#include "../domain/arearules.h"

class QFile;
class QThreadPool;

// io/ — SEG-Y rev0/1 测线级读取器（plan §2/§7）。
// open() 只做道头索引：沿文件顺序逐道读 240 字节道头，冻结 survey 几何
// （角点、inline/crossline 范围、采样间隔、起始时间），建立 inline/crossline
// →文件偏移索引；样本不进内存（不再 readAll）。
// 单条 inline 或 crossline 用 readInline/readCrossline 按需解码。
// IBM 370 fp32（format 1）与 IEEE 754 fp32（format 5）解码保留。
// 纯数据类型（SegyOptions/SegyTrace/SegyGeometry/SegySectionGrid）在
// domain/sectiontrace.h——视图层直接消费它们，不进本头。
//
// wave/io-perf-cache D2：openCached() 在 open() 之上加磁盘索引层——身份
// 命中免全文件重扫；固定道长文件走 ≤4 线程并行扫描（D2.9）；损坏道跳过
// 记录（D2.7，仅固定道长布局）；扫描取消/文件追加可 checkpoint 续扫
// （D2.5/D2.8）。open() 语义保持逐字节不变（兼容路径）。
class SegyReader
{
  public:
    SegyReader() = default;

    // 索引式打开：只读道头，不读样本。opts 可传进度/取消钩子（D1 异步）。
    bool open(const QString &path, QString *error = nullptr,
              const SegyOptions *opts = nullptr);

    static bool open(const QString &path, SegyReader &reader, QString *error = nullptr)
    {
      return reader.open(path, error);
    }

    // D2 缓存式打开：磁盘索引命中 → 免扫；未命中 → open() 同款扫描（固定
    // 道长布局时 ≤4 线程并行）→ 原子发布索引。发布失败降级为无缓存（可用）。
    // progress 语义与 open() 相同（bytes 语义：offset/fileSize）；命中时发一次
    // (size,size) 让面板立刻收敛到 100%。
    bool openCached(const QString &path, const QString &indexCacheDir,
                    QString *error = nullptr, const SegyOptions *opts = nullptr);

    int traceCount() const { return static_cast<int>(m_index.size()); }
    int samplesPerTrace() const { return m_samplesPerTrace; }
    float sampleIntervalUs() const { return m_sampleIntervalUs; }
    SegyGeometry geometry() const { return m_geometry; }
    QString filePath() const { return m_path; }

    // 全量解码（兼容小文件路径；每次调用重新解码，调用方负责规模）。
    QVector<SegyTrace> traces(const SegyOptions *opts = nullptr,
                              SegyReadReport *report = nullptr) const;

    // ---- 测线级 ----
    QVector<qint32> inlineNumbers() const;    // 升序去重
    QVector<qint32> crosslineNumbers() const; // 升序去重
    bool readInline(qint32 inlineNo, QVector<SegyTrace> *out,
                    QString *error = nullptr, const SegyOptions *opts = nullptr,
                    SegyReadReport *report = nullptr) const;
    bool readCrossline(qint32 xlineNo, QVector<SegyTrace> *out,
                       QString *error = nullptr, const SegyOptions *opts = nullptr,
                    SegyReadReport *report = nullptr) const;

    // ---- D2 索引面 ----
    // 当前索引快照（open/openCached 成功后可取）。
    bool snapshot(SegyIndexStore::StoredIndex *out) const;
    // 从快照恢复（身份不校验——调用方持 load() 的结果；重建行/道哈希）。
    bool restore(const SegyIndexStore::StoredIndex &in, const QString &path);
    // D2.6 空洞报告（快照统计口径）。
    SegyIndexStore::IndexStats indexStats() const;
    // D2.7 被跳过的损坏道偏移（无则空）。
    QVector<qint64> badTraceOffsets() const { return m_badTraceOffsets; }
    // 最近一次扫描是否因取消而保留部分索引（checkpoint 可用）。
    bool lastScanPartial() const { return m_lastScanPartial; }
    // 取消时的扫描偏移量（字节）
    qint64 scannedOffset() const { return m_scannedOffset; }
    // B6（wave/deepen-perf）：顺序扫描是否观察到变道长布局（任一道
    // ns>0 且 ≠ 二进制头 ns）。变道长文件：checkpoint/resume 契约不适用
    //（续扫按固定步长推进），openCached 侧据此不落 checkpoint。
    bool variableTraceLayout() const { return m_sawVariableNs; }

  private:
    struct IndexEntry
    {
      qint32 inlineNo = 0;
      qint32 xlineNo = 0;
      qint64 offset = 0; // 道头起始偏移
    };

    bool decodeTrace(QFile &file, const IndexEntry &e, SegyTrace *out,
                     SegySampleIssue *issue) const;
    QVector<SegyTrace> readByIndexList(const QVector<int> &idxs,
                                     const SegyOptions *opts = nullptr,
                                     SegyReadReport *report = nullptr) const;

    // D2.9 并行扫描（固定道长布局专用）：成功时填 m_index/m_geometry/…。
    bool scanParallel(QFile &file, qint64 firstTraceOffset, qint64 traceSize,
                      qint64 traceCount, QString *error, const SegyOptions *opts);
    // D2.5/D2.8 断点续扫：从 scannedOffset 继续顺序扫（标准索引模式专用），
    // 追加进已恢复的部分索引。
    bool resumeScan(QFile &file, const SegyIndexStore::StoredIndex &partial,
                    QString *error, const SegyOptions *opts);
    // 从（部分）m_index 重建行/道哈希 + 范围几何（restore/resume 收尾）。
    void rebuildLineHashes();
    void freezeCorners(const QVector<double> &xs, const QVector<double> &ys);
    QVector<int> cornerTraceIndices() const;
    bool freezeCornersFromFile(QFile &file, bool fromCdpXY = false);
    void resetState();

    AreaRules::SegyIndexing m_indexingRules; // 本次索引取字口径快照
    QString m_path;
    int m_samplesPerTrace = 0;
    float m_sampleIntervalUs = 0.0f;
    qint16 m_formatCode = 0;
    qint32 m_binLineNo = 0;
    qint64 m_firstTraceOffset = 0;
    QVector<IndexEntry> m_index;
    QHash<qint32, QVector<int>> m_byInline; // inline -> m_index 下标（按 xline 升序）
    QHash<qint32, QVector<int>> m_byXline;  // xline  -> m_index 下标（按 inline 升序）
    SegyGeometry m_geometry;
    QVector<qint64> m_badTraceOffsets; // D2.7
    bool m_lastScanPartial = false;    // open()/openCached() 取消后可 checkpoint
    qint64 m_scannedOffset = 0;        // 取消时的扫描位置
    bool m_sawVariableNs = false;      // B6：变道长布局观察（checkpoint 契约门）
};
