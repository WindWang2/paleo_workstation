// 层：数据
#include "streaming.h"

#include "encodingdetect.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
  constexpr int kChunk = 256 * 1024;

  // 分块行读取器：跨块的行拼接；\n、\r\n、\r 三种行界（对齐 QTextStream）。
  class ChunkedLineReader
  {
    public:
      explicit ChunkedLineReader(QFile *f) : m_file(f) {}

      // 读到下一行（不含行界符）。文件尾 → false。
      bool nextLine(QByteArray *line)
      {
        line->clear();
        for (;;)
        {
          if (m_pos >= m_buf.size())
          {
            m_buf = m_file->read(kChunk);
            m_pos = 0;
            if (m_buf.isEmpty())
            {
              if (line->isEmpty())
                return false;
              return true; // 末行无换行符
            }
          }
          const char *p = m_buf.constData() + m_pos;
          const qsizetype avail = m_buf.size() - m_pos;
          const void *nl = memchr(p, '\n', static_cast<size_t>(avail));
          const void *cr = memchr(p, '\r', static_cast<size_t>(avail));
          const bool hasNl = nl != nullptr, hasCr = cr != nullptr;
          if (!hasNl && !hasCr)
          {
            line->append(p, static_cast<int>(avail));
            m_pos = m_buf.size();
            continue;
          }
          const char *sep = hasCr && (!hasNl || cr < nl) ? static_cast<const char *>(cr)
                                                          : static_cast<const char *>(nl);
          const qsizetype take = sep - p;
          line->append(p, static_cast<int>(take));
          m_pos += take + 1;
          if (*sep == '\r' && m_pos >= m_buf.size())
          {
            // \r 在块尾：下一块开头若是 \n 才吞掉。
            m_buf = m_file->peek(1);
            if (!m_buf.isEmpty() && m_buf.at(0) == '\n')
            {
              m_buf = m_file->read(1);
              m_pos = 0;
              m_buf.clear();
            }
            else
            {
              m_buf.clear();
              m_pos = 0;
            }
          }
          else if (*sep == '\r' && m_buf.at(m_pos) == '\n')
          {
            ++m_pos; // \r\n
          }
          return true;
        }
      }

    private:
      QFile *m_file = nullptr;
      QByteArray m_buf;
      qsizetype m_pos = 0;
  };
} // namespace

namespace Streaming
{

qint64 forEachLine(const QString &path,
                   const std::function<bool(qint64, const QString &)> &onLine,
                   const std::function<bool()> &cancel,
                   const std::function<void(qint64, qint64)> &progress,
                   QString *error)
{
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
  {
    if (error)
      *error = QStringLiteral("cannot open %1: %2").arg(path, f.errorString());
    return -1;
  }
  const qint64 total = f.size();
  ChunkedLineReader reader(&f);
  QByteArray raw;
  qint64 index = 0;
  while (reader.nextLine(&raw))
  {
    if (cancel && cancel())
      return index;
    if (!onLine(index, EncodingDetect::decodeText(raw)))
      return index + 1; // 消费方主动停——已产出的行仍有效
    ++index;
    if (progress && (index & 0x3FF) == 0)
      progress(f.pos(), total);
  }
  if (progress)
    progress(total, total);
  return index;
}

qint64 forEachRecord(const QString &path, char sep,
                     const std::function<bool(qint64, const QStringList &)> &onRow,
                     const std::function<bool()> &cancel, QString *error)
{
  qint64 rows = 0;
  const qint64 n = forEachLine(
      path,
      [&rows, &onRow, sep](qint64, const QString &line) {
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
          return true; // 注释/空行跳过（井文本口径）
        const QStringList cols = sep == 0
                                     ? line.split(QRegularExpression(QStringLiteral("\\s+")),
                                                  Qt::SkipEmptyParts)
                                     : line.split(QLatin1Char(sep));
        ++rows;
        return onRow(rows - 1, cols);
      },
      cancel, nullptr, error);
  return n < 0 ? -1 : rows;
}

// ---------------------------------------------------------------------------
// GeoJSON 增量扫描
// ---------------------------------------------------------------------------
namespace
{
  class GeoJsonScanner
  {
    public:
      // chunk 里逐字节推进。inCoordinates → 当前在 "coordinates" 值区内。
      void feed(const QByteArray &chunk)
      {
        const qsizetype n = chunk.size();
        while (m_i < n)
        {
          const char c = chunk.at(m_i);
          switch (m_state)
          {
            case State::Outside:
              if (c == '"')
              {
                m_state = State::InString;
                m_pendingString.clear(); // 上一串值不清会污染键名（键从头累积）
                m_strStart = true;
              }
              break;
            case State::InString:
              if (c == '\\')
                m_state = State::InStringEscape;
              else if (c == '"')
              {
                m_state = State::AfterKeyString;
                m_lastString = m_pendingString;
                m_pendingString.clear();
                m_strStart = true; // 下一串可能是值
              }
              else if (m_strStart || m_pendingString.size() < 16)
              {
                m_pendingString.append(c);
                m_strStart = false;
              }
              break;
            case State::InStringEscape:
              m_state = State::InString;
              break;
            case State::AfterKeyString:
              if (c == ':')
              {
                m_state = State::AfterColon;
                m_lastWasCoordinatesKey = (m_lastString == QStringLiteral("coordinates"));
                m_lastWasPropertiesKey = (m_lastString == QStringLiteral("properties"));
                m_lastWasTypeKey = (m_lastString == QStringLiteral("type"));
              }
              else if (c == '"' || c == '{' || c == '[')
              {
                m_state = State::Outside;
                m_i--; // 重处理当前字节
              }
              else if (!QChar(QLatin1Char(c)).isSpace())
                m_state = State::Outside;
              break;
            case State::AfterColon:
              if (QChar(QLatin1Char(c)).isSpace())
                break;
              if (c == '"')
              {
                if (m_lastWasTypeKey)
                {
                  m_state = State::InTypeQuoted; // "type":"Feature" 的带引号值
                  m_pendingString.clear();
                  m_strStart = true;
                }
                else
                {
                  m_state = State::InValueString;
                  m_pendingString.clear();
                  m_strStart = true;
                }
              }
              else
              {
                if (m_lastWasCoordinatesKey)
                {
                  m_state = State::InCoordinates;
                  m_depth = 0;
                  m_runCount = 0;
                  m_i--; // 重处理（可能是 '[' 或数字首字符）
                }
                else if (m_lastWasPropertiesKey && c == '{')
                {
                  m_state = State::InProperties;
                  m_propDepth = 0;
                }
                else if (m_lastWasTypeKey)
                {
                  m_state = State::InTypeValue;
                  m_typeValue.clear();
                }
                else
                  m_state = State::Outside;
              }
              break;
            case State::InValueString:
              if (c == '\\')
                m_state = State::InValueStringEscape;
              else if (c == '"')
                m_state = State::Outside;
              break;
            case State::InValueStringEscape:
              m_state = State::InValueString;
              break;
            case State::InTypeQuoted:
              if (c == '\\')
                m_state = State::InTypeQuotedEscape;
              else if (c == '"')
              {
                if (m_pendingString == QStringLiteral("Feature"))
                  ++m_featureCount;
                m_state = State::Outside;
              }
              else if (m_pendingString.size() < 24)
                m_pendingString.append(c);
              break;
            case State::InTypeQuotedEscape:
              m_state = State::InTypeQuoted;
              break;
            case State::InTypeValue:
            {
              // 读到下一个 ',' 或 '}' 前的原文（"Feature"/"FeatureCollection"…）。
              if (c == ',' || c == '}' || c == ']')
              {
                if (m_typeValue.trimmed() == QStringLiteral("\"Feature\""))
                  ++m_featureCount;
                m_state = State::Outside;
                m_i--; // 让 Outside 分支处理结构字节
              }
              else if (m_typeValue.size() < 24)
                m_typeValue.append(c);
              break;
            }
            case State::InProperties:
              if (c == '{')
                ++m_propDepth;
              else if (c == '}')
              {
                if (m_propDepth == 0)
                  m_state = State::Outside;
                else
                  --m_propDepth;
              }
              else if (c == '"')
              {
                // properties 直接子级的键：depth==0 且紧随 '{' 或 ','。
                if (m_propDepth == 0 && m_lastPropSep)
                  m_state = State::InPropKey;
                else
                  m_state = State::InPropValueString;
                m_pendingString.clear();
                m_strStart = true;
                m_lastPropSep = false;
              }
              else if (c == ',' || c == ':')
                m_lastPropSep = (c == ',');
              break;
            case State::InPropKey:
              if (c == '\\')
                m_state = State::InPropKeyEscape;
              else if (c == '"')
              {
                m_state = State::InProperties;
                if (m_lastString.size() < 48 && m_pendingString.size() < 24)
                  m_propKeys.insert(m_pendingString);
              }
              else if (m_pendingString.size() < 24)
                m_pendingString.append(c);
              break;
            case State::InPropKeyEscape:
              m_state = State::InPropKey;
              break;
            case State::InPropValueString:
              if (c == '\\')
                m_state = State::InPropValueEscape;
              else if (c == '"')
                m_state = State::InProperties;
              break;
            case State::InPropValueEscape:
              m_state = State::InPropValueString;
              break;
            case State::InCoordinates:
              handleCoordinateByte(c);
              break;
          }
          ++m_i;
        }
        m_i = 0; // 下一 chunk
      }

      bool haveBounds() const { return m_haveAny; }
      double minX() const { return m_minX; }
      double minY() const { return m_minY; }
      double maxX() const { return m_maxX; }
      double maxY() const { return m_maxY; }
      qint64 featureCount() const { return m_featureCount; }
      QSet<QString> propertyKeys() const { return m_propKeys; }

    private:
      enum class State
      {
        Outside,
        InString,
        InStringEscape,
        AfterKeyString,
        AfterColon,
        InValueString,
        InValueStringEscape,
        InTypeValue,
        InTypeQuoted,
        InTypeQuotedEscape,
        InProperties,
        InPropKey,
        InPropKeyEscape,
        InPropValueString,
        InPropValueEscape,
        InCoordinates,
      };

      void handleCoordinateByte(char c)
      {
        if (c == '[')
        {
          ++m_depth;
          m_numAtDepth = 0;
          m_numBuf.clear();
          m_inNumber = false;
        }
        else if (c == ']')
        {
          if (m_inNumber)
            flushNumber();
          if (m_depth > 0)
            --m_depth;
          if (m_depth == 0)
            m_state = State::Outside; // coordinates 值区结束
          m_numAtDepth = 0;
        }
        else if (c == ',' )
        {
          if (m_inNumber)
            flushNumber();
        }
        else if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' ||
                 c == 'e' || c == 'E')
        {
          m_numBuf.append(c);
          m_inNumber = true;
        }
        else if (m_inNumber)
        {
          flushNumber(); // 数字后跟空白等
        }
      }

      void flushNumber()
      {
        m_inNumber = false;
        if (m_numBuf.isEmpty())
          return;
        const double v = m_numBuf.toDouble();
        m_numBuf.clear();
        if (m_numAtDepth == 0)
        {
          m_pendingX = v;
          m_numAtDepth = 1;
        }
        else if (m_numAtDepth == 1)
        {
          m_numAtDepth = 2;
          if (!m_haveAny || v < m_minY)
            m_minY = v;
          if (!m_haveAny || m_pendingX < m_minX)
            m_minX = m_pendingX;
          if (!m_haveAny || v > m_maxY)
            m_maxY = v;
          if (!m_haveAny || m_pendingX > m_maxX)
            m_maxX = m_pendingX;
          m_haveAny = true;
        }
        // 第 3+ 个数（Z/M 维）忽略；后续 position 的第 1/2 个在 ']' 时已复位。
      }

      State m_state = State::Outside;
      bool m_strStart = true;
      QString m_pendingString;
      QString m_lastString;
      bool m_lastWasCoordinatesKey = false;
      bool m_lastWasPropertiesKey = false;
      bool m_lastWasTypeKey = false;
      QString m_typeValue;
      bool m_lastPropSep = true;
      QSet<QString> m_propKeys;

      int m_depth = 0;
      int m_runCount = 0;
      int m_propDepth = 0;
      int m_numAtDepth = 0;
      QByteArray m_numBuf;
      bool m_inNumber = false;
      double m_pendingX = 0.0;
      bool m_haveAny = false;
      double m_minX = 0.0, m_minY = 0.0, m_maxX = 0.0, m_maxY = 0.0;
      qint64 m_featureCount = 0;

      qsizetype m_i = 0;
  };
} // namespace

bool geoJsonBoundsStreaming(const QString &path, double outBounds[4],
                            const std::function<bool()> &cancel, QString *error)
{
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
  {
    if (error)
      *error = QStringLiteral("cannot open %1: %2").arg(path, f.errorString());
    return false;
  }
  GeoJsonScanner scanner;
  while (!f.atEnd())
  {
    if (cancel && cancel())
    {
      if (error)
        *error = QStringLiteral("cancelled");
      return false;
    }
    QByteArray chunk = f.read(kChunk);
    if (chunk.isEmpty())
      break;
    if (chunk.size() >= 3 && static_cast<uchar>(chunk[0]) == 0xEF &&
        static_cast<uchar>(chunk[1]) == 0xBB && static_cast<uchar>(chunk[2]) == 0xBF &&
        f.pos() == static_cast<qint64>(chunk.size()))
      chunk.remove(0, 3); // 首 chunk BOM
    scanner.feed(chunk);
  }
  if (!scanner.haveBounds())
  {
    if (error)
      *error = QStringLiteral("no coordinates found in %1").arg(path);
    return false;
  }
  outBounds[0] = scanner.minX();
  outBounds[1] = scanner.minY();
  outBounds[2] = scanner.maxX();
  outBounds[3] = scanner.maxY();
  return true;
}

qint64 geoJsonFeatureCountStreaming(const QString &path, QStringList *outKeys, QString *error)
{
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
  {
    if (error)
      *error = QStringLiteral("cannot open %1: %2").arg(path, f.errorString());
    return -1;
  }
  GeoJsonScanner scanner;
  while (!f.atEnd())
  {
    QByteArray chunk = f.read(kChunk);
    if (chunk.isEmpty())
      break;
    if (chunk.size() >= 3 && static_cast<uchar>(chunk[0]) == 0xEF &&
        static_cast<uchar>(chunk[1]) == 0xBB && static_cast<uchar>(chunk[2]) == 0xBF &&
        f.pos() == static_cast<qint64>(chunk.size()))
      chunk.remove(0, 3);
    scanner.feed(chunk);
  }
  if (outKeys)
  {
    const QSet<QString> keys = scanner.propertyKeys();
    QStringList sorted = keys.values();
    std::sort(sorted.begin(), sorted.end());
    *outKeys = sorted;
  }
  return scanner.featureCount();
}

} // namespace Streaming
