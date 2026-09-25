#pragma once
#include <QHash>
#include <QString>
#include <QVector>

class QFile;

// io/ — SEG-Y rev0/1 测线级读取器（plan §2/§7）。
// open() 只做道头索引：沿文件顺序逐道读 240 字节道头，冻结 survey 几何
// （角点、inline/crossline 范围、采样间隔、起始时间），建立 inline/crossline
// →文件偏移索引；样本不进内存（不再 readAll）。
// 单条 inline 或 crossline 用 readInline/readCrossline 按需解码。
// IBM 370 fp32（format 1）与 IEEE 754 fp32（format 5）解码保留。
struct SegyTrace
{
  qint32 cdp = 0;
  qint32 lineNo = 0;   // inline（道头字节 189；0 时回退二进制头 line number）
  qint32 xlineNo = 0;  // crossline（道头字节 193，默认）
  QVector<float> samples;
  qint64 tracl = 0;
};

struct SegyGeometry
{
  qint32 inlineMin = 0, inlineMax = 0;
  qint32 xlineMin = 0, xlineMax = 0;
  // survey 角点 (x,y)：取自 CDP 道头坐标（字节 181-188），
  // 顺序 (inlMin,xlMin) (inlMin,xlMax) (inlMax,xlMax) (inlMax,xlMin)。
  double cornerX[4] = {0, 0, 0, 0};
  double cornerY[4] = {0, 0, 0, 0};
  double startTimeMs = 0; // 道头延迟记录时间（字节 109-110，ms）
};

class SegyReader
{
  public:
    SegyReader() = default;

    // 索引式打开：只读道头，不读样本。
    bool open(const QString &path, QString *error = nullptr);

    static bool open(const QString &path, SegyReader &reader, QString *error = nullptr)
    {
      return reader.open(path, error);
    }

    int traceCount() const { return static_cast<int>(m_index.size()); }
    int samplesPerTrace() const { return m_samplesPerTrace; }
    float sampleIntervalUs() const { return m_sampleIntervalUs; }
    SegyGeometry geometry() const { return m_geometry; }
    QString filePath() const { return m_path; }

    // 全量解码（兼容小文件路径；每次调用重新解码，调用方负责规模）。
    QVector<SegyTrace> traces() const;

    // ---- 测线级 ----
    QVector<qint32> inlineNumbers() const;    // 升序去重
    QVector<qint32> crosslineNumbers() const; // 升序去重
    bool readInline(qint32 inlineNo, QVector<SegyTrace> *out, QString *error = nullptr) const;
    bool readCrossline(qint32 xlineNo, QVector<SegyTrace> *out, QString *error = nullptr) const;

  private:
    struct IndexEntry
    {
      qint32 inlineNo = 0;
      qint32 xlineNo = 0;
      qint64 offset = 0; // 道头起始偏移
    };

    bool decodeTrace(QFile &file, const IndexEntry &e, SegyTrace *out) const;
    QVector<SegyTrace> readByIndexList(const QVector<int> &idxs) const;

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
};
