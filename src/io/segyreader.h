// 层：数据
#pragma once
#include <QHash>
#include <QString>
#include <QVector>

#include "../domain/sectiontrace.h"   // SegyOptions/SegyTrace/SegyGeometry/SegySectionGrid（domain 纯数据）
// D2 索引持久化类型只前向声明（完整定义见 segyindexstore.h；调用 snapshot/
// restore/indexStats 的 TU 自行 include），避免 io 重头经本头外溢。
struct SegyStoredIndex;
struct SegyIndexStats;

#include <atomic>
#include "../domain/arearules.h"

class QFile;
class QThreadPool;

// io/ — SEG-Y rev0/1 测线级读取器（plan §2/§7）。
// open() 只做道头索引：沿文件顺序逐道读 240 字节道头，冻结 survey 几何
// （角点、inline/crossline 范围、采样间隔、起始时间），建立 inline/crossline
// →文件偏移索引；样本不进内存（不再 readAll）。
// 单条 inline 或 crossline 用 readInline/readCrossline 按需解码。
// 样点格式 1/2/3/5/8，大端与小端（PW-070）。
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
    struct TraceCoordinate {
      double x = 0, y = 0;
      double resolution = 1; // 道头整数坐标经 scalar 缩放后的量化步长
    };
    SegyReader() = default;

    // PW-070/071/125：三条扫描路径（顺序/并行/续扫）+ 探针 + 解码共用的道 ns 判定。
    // ns 按 SEG-Y rev1/2 读作无符号 16 位。损坏判据显式：道头 ns 最高位置位
    //（≥32768，老读者眼里的「负 ns」）而二进制头 ns <32768 → Corrupt；二进制头
    // 自己就 ≥32768 的大道长文件不视为损坏。
    enum class TraceNsClass
    {
        UseBinary, // 道头 ns=0：沿用二进制头
        Normal,    // 道头 ns == 二进制头
        Variable,  // 合法但不同于二进制头（变道长）
        Corrupt    // 显式损坏——ns 字段按固定步长跳过
    };
    struct TraceNsInfo
    {
        TraceNsClass cls = TraceNsClass::Normal;
        int ns = 0; // 有效样点数（Corrupt 时 = binNs，供固定步长跳过）
    };
    static TraceNsInfo classifyTraceNs(quint16 rawTraceNs, int binNs);
    // 二进制头（400 字节）字节序判定：rev2 字节 3297-3300 常量 0x01020304 优先，
    // 否则按格式码在大/小端下哪个合法判定。默认大端。
    static bool detectLittleEndian(const unsigned char *bin400);
    // 支持的样点格式：1 IBM fp32、2 int32、3 int16、5 IEEE fp32、8 int8；其它返回 0。
    static int sampleBytesForFormat(int formatCode);
    bool littleEndian() const { return m_littleEndian; }

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

    // 免全扫几何探测（survey 边界恢复专用，不建索引）：规则网格（固定道长、
    // 道序 inline 单调、inline 内 xline 单调）下只读首 inline 各道头 + 末段
    // 三个道头，即可得到与全扫一致的 inline/xline 范围与四角坐标——196 GB
    // 体秒回，而 open/openCached 要全文件扫索引。角点未探到的槽为 NaN
    //（调用方按有限值过滤）。非常规布局（ordinal 方言 / 变道长 / 单调性
    // 校验不过）返回 false，调用方回退全扫路径。
    static bool probeGeometry(const QString &path, SegyGeometry *out,
                              QString *error = nullptr);

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

    // 与索引相同的坐标字段/方言及 scalar；只读道头，不解码振幅。
    bool readInlineCoordinates(qint32 inlineNo, QHash<qint32, TraceCoordinate> *out,
                               QString *error = nullptr, const SegyOptions *opts = nullptr) const;

    // ---- D2 索引面 ----
    // 当前索引快照（open/openCached 成功后可取）。
    bool snapshot(SegyStoredIndex *out) const;
    // 从快照恢复（身份不校验——调用方持 load() 的结果；重建行/道哈希）。
    bool restore(const SegyStoredIndex &in, const QString &path);
    // D2.6 空洞报告（快照统计口径）。
    SegyIndexStats indexStats() const;
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
    // xlineFromCdp/cornerFromCdpXY 为道头方言开关（与 open() 同判据，调用方
    // 探针后传入）——大文件并行路径不带方言会把该类体的号域扫成退化。
    bool scanParallel(QFile &file, qint64 firstTraceOffset, qint64 traceSize,
                      qint64 traceCount, QString *error, const SegyOptions *opts,
                      bool xlineFromCdp, bool cornerFromCdpXY);
    // D2.5/D2.8 断点续扫：从 scannedOffset 继续顺序扫（标准索引模式专用），
    // 追加进已恢复的部分索引。
    bool resumeScan(QFile &file, const SegyStoredIndex &partial,
                    QString *error, const SegyOptions *opts);
    // 从（部分）m_index 重建行/道哈希 + 范围几何（restore/resume 收尾）。
    void rebuildLineHashes();
    void freezeCorners(const QVector<double> &xs, const QVector<double> &ys);
    QVector<int> cornerTraceIndices() const;
    bool freezeCornersFromFile(QFile &file, bool fromCdpXY = false);
    void resetState();

    // #554：open() 按「头解析→探针→扫描→ordinal 收尾」机械拆段，各段可独立
    // 调用。失败路径的 m_index 清理、取消快照（m_lastScanPartial/
    // m_scannedOffset）与 52 处错误赋值语义与拆分前逐字一致。
    // 头解析：二进制头 + 扩展文本头 → 首道偏移（m_firstTraceOffset）与
    // ns/采样间隔/格式码等成员；binNs/binEnsTraces 供探针与 ordinal 收尾。
    bool parseSegyHeaders(QFile &file, const QString &path, qint64 fileSize,
                          int *binNs, int *binEnsTraces, QString *error);
    // 探针：道头方言判定（inline 字恒定 → ordinal 索引；crossline 位恒 0 且
    // CDP 变化 → 取 CDP；72/76 恒 0 且 180/184 非零 → 角点取 CDP X/Y），结果
    // 落 m_dialect。探针顺序与拆分前一致（短路语义保留）。
    void probeDialect(QFile &file, qint64 offset, qint64 fileSize, int binNs,
                      bool *ordinalIndex);
    // 扫描：顺序逐道读 240 字节道头建索引（方言经 m_dialect 取字）。
    // delayMs/firstLineWord/firstFieldRec/ordX/ordY 供 ordinal 收尾与几何。
    bool scanTracesSequential(QFile &file, qint64 offset, qint64 fileSize, int binNs,
                              bool ordinalIndex, double *delayMs, qint32 *firstLineWord,
                              qint32 *firstFieldRec, QVector<double> *ordX,
                              QVector<double> *ordY, QString *error,
                              const SegyOptions *opts);
    // ordinal 收尾：道号索引（inline = base + 道号/N）的道数/CDP 顺序/角点
    // 一致性校验与 inline 赋值、几何冻结。
    bool finalizeOrdinalIndexing(int binEnsTraces, qint32 firstLineWord,
                                 qint32 firstFieldRec, const QVector<double> &ordX,
                                 const QVector<double> &ordY, QString *error);

    // #290：方言探针结果——open()/scanParallel()/resumeScan() 三条扫描路径
    // 共用同一判据（segyreader.cpp 顶部 probe* 共享函数），避免并行/续扫
    // 索引与顺序索引口径分叉；经 StoredIndex::dialectFlags 随 checkpoint
    // 持久化，restore() 后 resumeScan 仍按同一口径续扫。
    struct DialectFlags
    {
        bool xlineFromCdp = false;    // crossline 位恒 0 且 CDP 变化 → 取 CDP
        bool cornerFromCdpXY = false; // 72/76 恒 0 且 180/184 非零 → 角点取 CDP X/Y
    };

    AreaRules::SegyIndexing m_indexingRules; // 本次索引取字口径快照
    DialectFlags m_dialect;                  // #290 方言探针结果（快照时一致）
    QString m_path;
    int m_samplesPerTrace = 0;
    float m_sampleIntervalUs = 0.0f;
    qint16 m_formatCode = 0;
    bool m_littleEndian = false; // PW-070：文件字节序（经 dialectFlags bit2 随索引持久化）
    int m_sampleBytes = 4;       // PW-070：每样点字节数（由格式码定）
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
