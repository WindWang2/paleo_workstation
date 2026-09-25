#pragma once
#include <QByteArray>
#include <QString>
#include <QVector>

// io/ — minimal SEG-Y rev0/1 parser (pure Qt, no QGIS dependency).
// Reads binary header (400 bytes, big-endian) and per-trace 240-byte trace
// headers + samples. Supports IBM 370 fp32 (format 1) and IEEE 754 fp32 (format 5).
struct SegyTrace
{
  qint32 cdp = 0;
  qint32 lineNo = 0;
  QVector<float> samples;
  qint64 tracl = 0;
};

class SegyReader
{
  public:
    SegyReader() = default;

    // Opens and parses the SEG-Y file at `path`.
    // Returns true on success, false on error (empty file, truncation, unsupported format).
    bool open(const QString &path, QString *error = nullptr);

    static bool open(const QString &path, SegyReader &reader, QString *error = nullptr)
    {
      return reader.open(path, error);
    }

    int traceCount() const { return static_cast<int>(m_traces.size()); }
    int samplesPerTrace() const { return m_samplesPerTrace; }
    float sampleIntervalUs() const { return m_sampleIntervalUs; }
    QVector<SegyTrace> traces() const { return m_traces; }

  private:
    int m_samplesPerTrace = 0;
    float m_sampleIntervalUs = 0.0f;
    QVector<SegyTrace> m_traces;
};
