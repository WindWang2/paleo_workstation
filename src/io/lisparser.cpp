// 层：数据
#include "lisparser.h"
#include "parserissues_internal.h"

#include "../domain/wellnumeric.h"
#include "lasparser.h" // LasParser::fileSizeLimit（大文件防护共用口径）
#include <QFile>
#include <QFileInfo>

#include <cmath>
#include <cstring>
#include <limits>

// LIS79 字节布局（全大端；lisparser.h 头注释 + ledger 逐条对账）：
//   TIF（磁带映像）：每条物理记录包 [12B 磁带标头（小端 u32 ×3：
//        type/prev/next）][PR 字节]；type 0=数据记录、1=EOF 带标（无载荷）；
//        prev/next 是前后标头的文件绝对偏移（hint，只用于校验与推进）。
//        载荷长 = next − cur − 12。检测 = 首标头 type∈{0,1} 且 prev<next
//        （dlisio tapemark 语义；布局经 dlisio 公开夹具 layout_tif_00 逐字节核实）。
//   PRH：[len u16 BE][attrs u16 BE]，len 含头与尾；attrs 位：
//        bit13|12 校验和（尾 2B）/ bit10 文件号（尾 2B）/ bit9 记录号（尾 2B）
//        / bit1 前驱 / bit0 后继 / bit14 记录类型位 / bit6 奇偶错 / bit5 校验错。
//   PR 尾（按位出现，自尾剥）：[文件号 2B][记录号 2B][校验和 2B]。
//   LRH（LR 首个 PR 的 PRH 之后 2B）：[type u8][attrs u8]（attrs 未定义）。
//   LR = 首段 [PRH][LRH][data] + 后续段 [PRH][data] 拼接。
//   信息记录分量块：[type_nb u8][reprc u8][size u8][category u8]
//        [mnemonic 4B][units 4B][value size B]。
//   DFSR 条目块：[type u8][size u8][reprc u8][value size B]，type 0 终止。
//   Spec Block（40B）：[mnemonic 4][service_id 6][service_order_nr 8]
//        [units 4][子类型相关 4][filenr i16][reserved_size i16][pad 2]
//        [子类型相关 1][samples u8][reprc u8][子类型尾 5]。

namespace
{
using paleo::io_detail::addIssue;

  // LIS79 reprc（Appendix B 编号）
  constexpr int LisReprcI8 = 56;
  constexpr int LisReprcString = 65;
  constexpr int LisReprcByte = 66;
  constexpr int LisReprcF32 = 68;
  constexpr int LisReprcF32fix = 70;
  constexpr int LisReprcMask = 77;
  constexpr int LisReprcI32 = 73;
  constexpr int LisReprcF16 = 49;
  constexpr int LisReprcF32low = 50;
  constexpr int LisReprcI16 = 79;

  // 记录类型（LIS79 图 3.9；dlisio valid_rectype 同口径）
  constexpr uchar RecNormalData = 0;
  constexpr uchar RecAlternateData = 1;
  constexpr uchar RecJobIdent = 32;
  constexpr uchar RecWellsiteData = 34;
  constexpr uchar RecToolStringInfo = 39;
  constexpr uchar RecEncTableDump = 42;
  constexpr uchar RecTableDump = 47;
  constexpr uchar RecDataFormatSpec = 64;
  constexpr uchar RecDataDescriptor = 65;
  constexpr uchar RecFileHeader = 128;
  constexpr uchar RecFileTrailer = 129;
  constexpr uchar RecTapeHeader = 130;
  constexpr uchar RecTapeTrailer = 131;
  constexpr uchar RecReelHeader = 132;
  constexpr uchar RecReelTrailer = 133;

  bool validRecordType(uchar t)
  {
    switch (t)
    {
      case 0: case 1: case 32: case 34: case 39: case 42: case 47:
      case 64: case 65: case 85: case 86: case 95: case 96: case 97:
      case 100: case 101: case 102: case 128: case 129: case 130:
      case 131: case 132: case 133: case 137: case 138: case 139:
      case 141: case 224: case 225: case 227: case 232: case 234:
        return true;
      default:
        return false;
    }
  }

  int lisFixedRepSize(int repc)
  {
    switch (repc)
    {
      case LisReprcI8: case LisReprcByte: return 1;
      case LisReprcI16: case LisReprcF16: return 2;
      case LisReprcI32: case LisReprcF32: case LisReprcF32low:
      case LisReprcF32fix: return 4;
      default: return -1; // 65/77 变长，其余未定义
    }
  }

  bool lisNumericRep(int repc) { return lisFixedRepSize(repc) > 0; }

  // 符号位独立、数值位 2 补码（LIS 浮点的符号-幅值式存储）
  double twosComplement(bool sign, quint64 number, int len)
  {
    if (!sign)
      return double(number);
    const quint64 mask = (len >= 64) ? ~quint64(0) : ((quint64(1) << len) - 1);
    return double((~number & mask) + 1);
  }

  // ---- LIS 浮点（Appendix B.1/B.2/B.5/B.6） ----
  double decodeLisF16(quint16 v)
  {
    const bool sign = v & 0x8000;
    const int exp = int(v & 0x000F);
    quint16 frac = (v & 0x7FF0) >> 4;
    if (sign)
      frac = quint16(((~frac) & 0x07FF) + 1);
    const double fraction = double(frac) * std::pow(2.0, -11);
    return (sign ? -1.0 : 1.0) * fraction * std::pow(2.0, double(exp));
  }

  double decodeLisF32(quint32 v)
  {
    const bool sign = v & 0x80000000;
    quint32 expBits = (v & 0x7F800000) >> 23;
    if (sign)
      expBits = ~expBits & 0xFF; // 负指数 1 补码（B.5）
    const quint32 frac2 = quint32(twosComplement(sign, v & 0x007FFFFF, 23));
    const double fraction = double(frac2) * std::pow(2.0, -23);
    return (sign ? -1.0 : 1.0) * fraction *
           std::pow(2.0, double(expBits) - 128.0);
  }

  double decodeLisF32low(quint32 v)
  {
    const bool fracSign = v & 0x00008000;
    const bool expSign = v & 0x80000000;
    const double exponent = (expSign ? -1.0 : 1.0) *
        double(quint32(twosComplement(expSign, (v & 0x7FFF0000) >> 16, 15)));
    const double frac2 = double(quint32(twosComplement(fracSign, v & 0x00007FFF, 15)));
    return (fracSign ? -1.0 : 1.0) * frac2 * std::pow(2.0, exponent - 15.0);
  }

  double decodeLisF32fix(quint32 v)
  {
    const bool sign = v & 0x80000000;
    const quint32 data = quint32(twosComplement(sign, v & 0x7FFFFFFF, 31));
    const double integer = double(data >> 16);
    const double real = double(data & 0xFFFF) * std::pow(2.0, -16);
    return (sign ? -1.0 : 1.0) * (integer + real);
  }

  struct LisCur
  {
    const uchar *p = nullptr;
    const uchar *end = nullptr;
    bool bad = false;

    int left() const { return int(end - p); }
    bool need(int n) const { return left() >= n; }

    uchar u8()
    {
      if (!need(1)) { bad = true; return 0; }
      return *p++;
    }
    quint16 u16()
    {
      if (!need(2)) { bad = true; return 0; }
      const quint16 v = quint16(p[0]) << 8 | quint16(p[1]);
      p += 2;
      return v;
    }
    quint32 u32()
    {
      if (!need(4)) { bad = true; return 0; }
      const quint32 v = quint32(p[0]) << 24 | quint32(p[1]) << 16 |
                        quint32(p[2]) << 8 | quint32(p[3]);
      p += 4;
      return v;
    }
    QString fixed(int n)
    {
      if (!need(n)) { bad = true; return QString(); }
      const QString s = QString::fromLatin1(reinterpret_cast<const char *>(p), n);
      p += n;
      return s;
    }
    void skip(int n)
    {
      if (!need(n)) { bad = true; return; }
      p += n;
    }
  };

  double readLisNumeric(LisCur &c, int repc)
  {
    switch (repc)
    {
      case LisReprcI8: return qint8(c.u8());
      case LisReprcByte: return c.u8();
      case LisReprcI16: return qint16(c.u16());
      case LisReprcI32: return qint32(c.u32());
      case LisReprcF16: return decodeLisF16(c.u16());
      case LisReprcF32: return decodeLisF32(c.u32());
      case LisReprcF32low: return decodeLisF32low(c.u32());
      case LisReprcF32fix: return decodeLisF32fix(c.u32());
      default: c.bad = true; return 0.0;
    }
  }

  double leU32(const uchar *p)
  {
    return double(quint32(p[0]) | quint32(p[1]) << 8 | quint32(p[2]) << 16 |
                  quint32(p[3]) << 24);
  }

  // 首标头轻量判定（sniff 高频路径）：type∈{0,1} 且 prev<next，不建段索引
  bool looksLikeTifHead(QFile &f)
  {
    if (f.size() < 12 || !f.seek(0))
      return false;
    uchar hdr[12];
    if (f.read(reinterpret_cast<char *>(hdr), 12) != 12)
      return false;
    const quint32 type = quint32(leU32(hdr));
    const quint32 prev = quint32(leU32(hdr + 4));
    const quint32 next = quint32(leU32(hdr + 8));
    return (type == 0 || type == 1) && prev < next;
  }

  // ---- TIF 视图：把磁带标头链折成连续逻辑流 ----
  class TifView
  {
  public:
    // 检测 + 建索引。返回 false = 不是 TIF（调用方回落裸流）。
    // 置位 errorOut 时 = 结构坏（是 TIF 但链断——如实报，不猜）。
    static bool detect(QFile &f, TifView *out, bool *malformed, QString *error)
    {
      *malformed = false;
      if (f.size() < 12)
        return false;
      if (!f.seek(0))
      {
        if (error)
          *error = QStringLiteral("seek 失败");
        return false;
      }
      uchar hdr[12];
      if (f.read(reinterpret_cast<char *>(hdr), 12) != 12)
        return false;
      const quint32 type = quint32(leU32(hdr));
      const quint32 prev = quint32(leU32(hdr + 4));
      const quint32 next = quint32(leU32(hdr + 8));
      // dlisio tapemark 语义：type∈{0,1} 且 prev<next
      if ((type != 0 && type != 1) || prev >= next)
        return false;

      TifView v;
      v.m_file = &f;
      v.m_size = f.size();
      qint64 cur = 0;
      bool sawType0 = false;
      while (cur + 12 <= v.m_size)
      {
        uchar th[12];
        if (!f.seek(cur) || f.read(reinterpret_cast<char *>(th), 12) != 12)
        {
          *malformed = true;
          if (error)
            *error = QStringLiteral("磁带标头读取失败于 %1").arg(cur);
          return true;
        }
        const quint32 t = quint32(leU32(th));
        const quint32 nx = quint32(leU32(th + 8));
        if ((t != 0 && t != 1) || nx <= cur + 12 || nx > v.m_size)
        {
          if (t != 0 && t != 1)
          {
            *malformed = true;
            if (error)
              *error = QStringLiteral("磁带标头 type %1 非法（于 %2）").arg(t).arg(cur);
            return true;
          }
          // 链尾容差：next 无效时载荷取到文件尾
          if (t == 0)
            v.m_segments.append(qMakePair(cur + 12, v.m_size - cur - 12));
          break;
        }
        if (t == 0)
        {
          v.m_segments.append(qMakePair(cur + 12, qint64(nx) - cur - 12));
          sawType0 = true;
        }
        cur = qint64(nx);
      }
      if (!sawType0)
        return false; // 只有带标：不像数据文件，回落裸流判定
      v.m_logicalSize = 0;
      for (const auto &seg : v.m_segments)
        v.m_logicalSize += seg.second;
      *out = v;
      return true;
    }

    qint64 size() const { return m_logicalSize; }

    // 逻辑偏移读（可跨段）。段按逻辑偏移有序 → 二分定位（杂数跳过的
    // 2 字节探针高频调用，线性扫描会把大 TIF 文件读成 O(n²)）。
    bool read(qint64 at, uchar *buf, int n) const
    {
      if (m_prefix.isEmpty())
        rebuildPrefix();
      int done = 0;
      while (done < n)
      {
        // 二分：首个前缀和 > at+done 的段
        int lo = 0, hi = m_prefix.size() - 1, seg = -1;
        const qint64 want = at + done;
        while (lo <= hi)
        {
          const int mid = (lo + hi) / 2;
          if (m_prefix.at(mid) > want)
          {
            seg = mid;
            hi = mid - 1;
          }
          else
            lo = mid + 1;
        }
        if (seg < 0 || seg >= m_segments.size())
          return false;
        const qint64 acc = seg == 0 ? 0 : m_prefix.at(seg - 1);
        const qint64 lo2 = want - acc;
        const qint64 phys = m_segments.at(seg).first + lo2;
        const qint64 avail = m_segments.at(seg).second - lo2;
        const int want2 = int(qMin<qint64>(n - done, avail));
        if (!m_file->seek(phys))
          return false;
        const int got = int(m_file->read(reinterpret_cast<char *>(buf) + done, want2));
        if (got != want2)
          return false;
        done += want2;
      }
      return true;
    }

  private:
    QFile *m_file = nullptr;
    qint64 m_size = 0;
    qint64 m_logicalSize = 0;
    QVector<QPair<qint64, qint64>> m_segments; // (物理偏移, 长度)
    mutable QVector<qint64> m_prefix;          // 段长前缀和（懒建，二分用）

    void rebuildPrefix() const
    {
      m_prefix.clear();
      m_prefix.reserve(m_segments.size());
      qint64 acc = 0;
      for (const auto &seg : m_segments)
      {
        acc += seg.second;
        m_prefix.append(acc);
      }
    }
  };

  // ---- 逻辑流读取器：杂数跳过 + PR/LR 拼接 ----
  struct LogicalRecord
  {
    uchar type = 0;
    QByteArray data; // 不含 LRH
  };

  class LogicalReader
  {
  public:
    LogicalReader(QFile &f, const TifView *tif)
        : m_f(f), m_tif(tif), m_size(tif ? tif->size() : f.size()) {}

    qint64 size() const { return m_size; }
    qint64 pos() const { return m_pos; }

    enum class Status { Ok, Eof, Truncated, Invalid };

    // 读下一逻辑记录（多 PR 拼接；截断/非法如实报）
    Status next(LogicalRecord *rec, QString *error, QList<LasIssue> *issues)
    {
      QByteArray body;
      bool first = true;
      while (true)
      {
        // 杂数跳过：双字节前瞻（0x00/0x20 均匀区）
        uchar probe[2] = { 1, 1 };
        while (m_pos + 4 <= m_size)
        {
          if (!readAt(m_pos, probe, 2))
            return fail(error, QStringLiteral("读取失败于 %1").arg(m_pos),
                        Status::Invalid);
          const bool padA = probe[0] == 0x00 || probe[0] == 0x20;
          const bool padB = probe[1] == 0x00 || probe[1] == 0x20;
          if (padA && padB)
            m_pos += 2;
          else
            break;
        }
        if (m_pos >= m_size)
          return first ? Status::Eof
                       : truncated(error, issues,
                                   QStringLiteral("逻辑记录缺尾段（%1 处截断）")
                                       .arg(m_pos));
        if (m_size - m_pos < 4)
        {
          // 尾随 1..3 字节填充（偶数对齐余量）：全填充 = 干净 EOF
          uchar t[3] = { 1, 1, 1 };
          const int room = int(m_size - m_pos);
          bool allPad = room > 0 && readAt(m_pos, t, room);
          for (int i = 0; i < room && allPad; ++i)
            allPad = (t[i] == 0x00 || t[i] == 0x20);
          if (allPad)
            return first ? Status::Eof
                         : truncated(error, issues,
                                     QStringLiteral("逻辑记录缺尾段（%1 处截断）")
                                         .arg(m_pos));
          return truncated(error, issues,
                           QStringLiteral("PR 头截断（%1 字节）").arg(room));
        }

        uchar hdr[4];
        if (!readAt(m_pos, hdr, 4))
          return fail(error, QStringLiteral("PR 头读取失败于 %1").arg(m_pos),
                      Status::Invalid);
        const quint16 len = quint16(hdr[0]) << 8 | quint16(hdr[1]);
        const quint16 attrs = quint16(hdr[2]) << 8 | quint16(hdr[3]);
        if (len < 4)
          return fail(error, QStringLiteral("PR 长度 %1 非法（于 %2）")
                                   .arg(len)
                                   .arg(m_pos),
                      Status::Invalid);
        if (m_pos + len > m_size)
          return truncated(error, issues,
                           QStringLiteral("PR（%1 字节）越过文件尾（于 %2）")
                               .arg(len)
                               .arg(m_pos));
        int minLen = (attrs & 0x0002) ? 4 : 6;
        if (attrs & 0x0400) minLen += 2;
        if (attrs & 0x0200) minLen += 2;
        if (attrs & 0x3000) minLen += 2;
        if (len < minLen)
          return fail(error, QStringLiteral("PR 长度 %1 小于最小合法 %2（于 %3）")
                                   .arg(len)
                                   .arg(minLen)
                                   .arg(m_pos),
                      Status::Invalid);
        int trailer = 0;
        if (attrs & 0x0400) trailer += 2;
        if (attrs & 0x0200) trailer += 2;
        if (attrs & 0x3000) trailer += 2;
        const int dataLen = int(len) - 4 - trailer;
        if (dataLen < 0)
          return fail(error, QStringLiteral("PR 尾越过记录（len %1）").arg(len),
                      Status::Invalid);

        QByteArray chunk(dataLen, Qt::Uninitialized);
        if (dataLen > 0 && !readAt(m_pos + 4,
                                   reinterpret_cast<uchar *>(chunk.data()), dataLen))
          return fail(error, QStringLiteral("PR 数据读取失败于 %1").arg(m_pos),
                      Status::Invalid);
        if (first)
        {
          if (dataLen < 2)
            return fail(error, QStringLiteral("首段容不下 LRH（len %1）").arg(len),
                        Status::Invalid);
          rec->type = uchar(chunk.at(0));
          chunk.remove(0, 2);
          first = false;
        }
        body.append(chunk);
        m_pos += len;
        if (!(attrs & 0x0001)) // 无后继段
          break;
      }
      rec->data = body;
      return Status::Ok;
    }

  private:
    QFile &m_f;
    const TifView *m_tif = nullptr;
    qint64 m_size = 0;
    qint64 m_pos = 0;

    bool readAt(qint64 at, uchar *buf, int n)
    {
      if (at + n > m_size)
        return false;
      if (m_tif)
        return m_tif->read(at, buf, n);
      if (!m_f.seek(at))
        return false;
      return m_f.read(reinterpret_cast<char *>(buf), n) == n;
    }
    Status fail(QString *error, const QString &msg, Status st)
    {
      if (error)
        *error = msg;
      return st;
    }
    Status truncated(QString *error, QList<LasIssue> *issues, const QString &msg)
    {
      if (error)
        *error = msg;
      addIssue(issues, LasIssue::Severity::Error, LasIssue::Category::Truncated, msg);
      return Status::Truncated;
    }
  };

  // ---- DFSR（logset 定义） ----
  struct SpecBlock
  {
    QString mnemonic;
    QString units;
    qint32 reservedSize = 0;
    int samples = 1;
    int reprc = -1;
    bool tvdCorrected = false; // subtype-1 过程指示器 TVD 位
    bool suppressed = false;
  };

  struct LogSet
  {
    QList<SpecBlock> specs;
    int depthMode = 0;      // 0=帧内索引道；1=每记录深度+帧距
    int depthReprc = -1;    // 模式 1 深度表示码（条目 15）
    QString depthUnits;     // 模式 1 深度单位（条目 14）
    double spacing = 0;     // 帧距（条目 8）
    int direction = 1;      // 1=UP（递减）255=DOWN（递增）（条目 4）
    double absentValue = paleo::wellnumeric::kLasDefaultNull;
    bool sawData = false;
    bool whitelisted = false;
    QString whitelistReason;
    QVector<double> flat;   // 每帧 [非抑制道值×] 顺序平铺
    QVector<double> depths; // 每帧深度（模式 0=索引道；模式 1=推算）
    int columns() const
    {
      int n = 0;
      for (const SpecBlock &sp : specs)
        if (!sp.suppressed)
          ++n;
      return n;
    }
  };

  QString logsetWhitelistReason(const LogSet &ls)
  {
    for (const SpecBlock &sp : ls.specs)
    {
      if (sp.suppressed)
        continue;
      if (!lisNumericRep(sp.reprc))
        return QStringLiteral("通道 %1 表示码 %2 非数值——白名单：无标量列")
                   .arg(sp.mnemonic)
                   .arg(sp.reprc);
      if (sp.samples != 1)
        return QStringLiteral("通道 %1 采样率 %2——白名单：快道子帧布局 V1 未实现")
                   .arg(sp.mnemonic)
                   .arg(sp.samples);
    }
    return QString();
  }

  int specFrameBytes(const SpecBlock &sp)
  {
    if (sp.suppressed || sp.reservedSize != 0)
      return qAbs(sp.reservedSize);
    const int fixed = lisFixedRepSize(sp.reprc);
    return fixed > 0 ? sp.samples * fixed : -1;
  }

  struct WalkOut
  {
    QString wellName;
    QList<LogSet> logsets;
    int primaryLogset = -1;
    bool stoppedAtTrailer = false;
  };

  enum class LisWalkResult { Ok, Truncated, Invalid };

  LisWalkResult walkRecords(QFile &f, bool stopAtFirstData, WalkOut &out,
                         QList<LasIssue> *issues, QString *error)
  {
    TifView tif;
    bool malformed = false;
    const bool isTif = TifView::detect(f, &tif, &malformed, error);
    if (malformed)
      return LisWalkResult::Invalid;
    LogicalReader reader(f, isTif ? &tif : nullptr);

    int activeLogset = -1;
    bool primaryChosen = false;
    bool wellNameDone = !out.wellName.isEmpty();

    while (true)
    {
      LogicalRecord rec;
      QString rerr;
      const LogicalReader::Status st = reader.next(&rec, &rerr, issues);
      if (st == LogicalReader::Status::Eof)
        break;
      if (st == LogicalReader::Status::Truncated)
      {
        if (error)
          *error = QStringLiteral("文件截断：%1").arg(rerr);
        return LisWalkResult::Truncated;
      }
      if (st == LogicalReader::Status::Invalid)
      {
        if (error)
          *error = rerr;
        return LisWalkResult::Invalid;
      }
      if (!validRecordType(rec.type))
      {
        if (error)
          *error = QStringLiteral("记录类型 %1 不在 LIS79 表内（于 %2）")
                       .arg(int(rec.type))
                       .arg(reader.pos());
        return LisWalkResult::Invalid;
      }

      switch (rec.type)
      {
        case RecTapeHeader: case RecTapeTrailer:
        case RecReelHeader: case RecReelTrailer:
        case RecFileHeader: case RecDataDescriptor:
          break; // 结构性记录：不含井名/曲线（井名在 wellsite 信息记录）
        case RecFileTrailer:
          out.stoppedAtTrailer = true;
          break;
        case RecWellsiteData:
        case RecJobIdent:
        case RecToolStringInfo:
        {
          if (wellNameDone)
            break;
          LisCur c{ reinterpret_cast<const uchar *>(rec.data.constData()),
                 reinterpret_cast<const uchar *>(rec.data.constData()) +
                     rec.data.size(),
                 false };
          while (c.left() >= 12 && !c.bad)
          {
            const int typeNb = int(c.u8());
            const int reprc = int(c.u8());
            const int size = int(c.u8());
            c.u8(); // category（LIS79 未定义）
            const QString mnemonic = c.fixed(4).trimmed();
            c.fixed(4); // units
            QString strValue;
            if (c.bad)
              break;
            if (size > 0)
            {
              if (!c.need(size))
              {
                addIssue(issues, LasIssue::Severity::Warning,
                         LasIssue::Category::Truncated,
                         QStringLiteral("信息记录分量块 %1 值区截断").arg(mnemonic));
                c.bad = true;
                break;
              }
              if (reprc == LisReprcString)
                strValue = QString::fromLatin1(
                               reinterpret_cast<const char *>(c.p), size)
                               .trimmed();
              c.skip(size);
            }
            if (mnemonic == QLatin1String("WELL") && !strValue.isEmpty() &&
                typeNb != 0)
            {
              out.wellName = strValue;
              wellNameDone = true;
              break;
            }
          }
          break;
        }
        case RecDataFormatSpec:
        {
          LogSet ls;
          LisCur c{ reinterpret_cast<const uchar *>(rec.data.constData()),
                 reinterpret_cast<const uchar *>(rec.data.constData()) +
                     rec.data.size(),
                 false };
          bool terminator = false;
          int subtype = 0;
          while (!terminator && !c.bad)
          {
            if (c.left() < 3)
            {
              if (error)
                *error = QStringLiteral("DFSR 条目区截断");
              return LisWalkResult::Truncated;
            }
            const int eType = int(c.u8());
            const int eSize = int(c.u8());
            const int eReprc = int(c.u8());
            if (eSize < 0 || (eSize > 0 && !c.need(eSize)))
            {
              if (error)
                *error = QStringLiteral("DFSR 条目值区截断");
              return LisWalkResult::Truncated;
            }
            double num = 0;
            QString str;
            if (eSize > 0)
            {
              if (eReprc == LisReprcString)
              {
                str = QString::fromLatin1(reinterpret_cast<const char *>(c.p), eSize)
                          .trimmed();
                c.skip(eSize);
              }
              else
              {
                LisCur vc{ c.p, c.p + eSize, false };
                num = readLisNumeric(vc, eReprc);
                if (vc.bad)
                {
                  if (error)
                    *error = QStringLiteral("DFSR 条目值表示码 %1 非法").arg(eReprc);
                  return LisWalkResult::Invalid;
                }
                c.skip(eSize);
              }
            }
            switch (eType)
            {
              case 0: terminator = true; break;
              // LIS79：size=0 表示条目值缺席——保持缺省，不砸成 0/空
              //（absentValue=0 会把真实 0.0 样本静默映射成 NaN）。
              case 4: if (eSize > 0) ls.direction = int(num); break;
              case 8: if (eSize > 0) ls.spacing = num; break;
              case 12: if (eSize > 0) ls.absentValue = num; break;
              case 13: if (eSize > 0) ls.depthMode = int(num); break;
              case 14: if (eSize > 0) ls.depthUnits = str; break;
              case 15: ls.depthReprc = eReprc; break;
              case 16: if (int(num) == 1) subtype = 1; break;
              default: break;
            }
          }
          if (c.bad)
          {
            if (error)
              *error = QStringLiteral("DFSR 条目区非法");
            return LisWalkResult::Invalid;
          }
          while (c.left() >= 40 && !c.bad)
          {
            SpecBlock sp;
            sp.mnemonic = c.fixed(4).trimmed();
            c.fixed(6); // service_id
            c.fixed(8); // service_order_nr
            sp.units = c.fixed(4).trimmed();
            c.skip(4);  // 子类型相关
            c.u16();    // filenr
            sp.reservedSize = qint16(c.u16());
            c.skip(2);  // 填充
            c.skip(1);  // 子类型相关
            sp.samples = int(c.u8());
            sp.reprc = int(c.u8());
            const QString tail = c.fixed(5); // 子类型尾（含过程指示器掩码）
            if (subtype == 1 && tail.size() == 5)
              sp.tvdCorrected = (uchar(tail.at(0).toLatin1()) & 0x20) != 0;
            sp.suppressed = sp.reservedSize < 0;
            ls.specs.append(sp);
          }
          if (c.bad)
          {
            if (error)
              *error = QStringLiteral("DFSR Spec Block 区非法/截断");
            return LisWalkResult::Truncated;
          }
          if (ls.specs.isEmpty())
          {
            if (error)
              *error = QStringLiteral("DFSR 无 Spec Block（曲线目录为空）");
            return LisWalkResult::Invalid;
          }
          if (ls.depthMode == 0)
          {
            // 模式 0：索引道 = 首 spec，必须数值
            const SpecBlock &idx = ls.specs.first();
            if (!lisNumericRep(idx.reprc))
            {
              if (error)
                *error = QStringLiteral("索引道（%1）表示码 %2 非数值")
                             .arg(idx.mnemonic)
                             .arg(idx.reprc);
              return LisWalkResult::Invalid;
            }
          }
          else
          {
            if (lisFixedRepSize(ls.depthReprc) <= 0)
            {
              if (error)
                *error = QStringLiteral("模式 1 缺输出深度表示码（条目 15）");
              return LisWalkResult::Invalid;
            }
            if (ls.direction != 1 && ls.direction != 255)
            {
              if (error)
                *error = QStringLiteral("模式 1 且 UP/DOWN 方向未定义（%1）——"
                                        "不猜步进方向")
                             .arg(ls.direction);
              return LisWalkResult::Invalid;
            }
          }
          ls.whitelistReason = logsetWhitelistReason(ls);
          ls.whitelisted = !ls.whitelistReason.isEmpty();
          activeLogset = out.logsets.size();
          out.logsets.append(ls);
          break;
        }
        case RecNormalData:
        case RecAlternateData:
        {
          if (activeLogset < 0)
          {
            if (error)
              *error = QStringLiteral("数据记录先于任何 DFSR（无格式定义）");
            return LisWalkResult::Invalid;
          }
          LogSet &ls = out.logsets[activeLogset];
          if (!primaryChosen && !ls.whitelisted)
          {
            primaryChosen = true;
            out.primaryLogset = activeLogset;
          }
          if (ls.whitelisted)
          {
            if (!ls.sawData)
              addIssue(issues, LasIssue::Severity::Warning,
                       LasIssue::Category::Format,
                       QStringLiteral("logset %1 整体未进表：%2")
                           .arg(activeLogset + 1)
                           .arg(ls.whitelistReason));
            ls.sawData = true;
            break;
          }
          if (activeLogset != out.primaryLogset)
          {
            if (!ls.sawData)
              addIssue(issues, LasIssue::Severity::Warning,
                       LasIssue::Category::Format,
                       QStringLiteral("logset %1 的数据未进表——白名单："
                                      "单井单深度轴表格")
                           .arg(activeLogset + 1));
            ls.sawData = true;
            break;
          }
          ls.sawData = true;

          LisCur c{ reinterpret_cast<const uchar *>(rec.data.constData()),
                 reinterpret_cast<const uchar *>(rec.data.constData()) +
                     rec.data.size(),
                 false };
          double depth = 0;
          if (ls.depthMode == 1)
          {
            depth = readLisNumeric(c, ls.depthReprc);
            if (c.bad)
            {
              if (error)
                *error = QStringLiteral("模式 1 深度值截断");
              return LisWalkResult::Truncated;
            }
          }
          int frameSize = 0;
          for (const SpecBlock &sp : ls.specs)
          {
            const int b = specFrameBytes(sp);
            if (b <= 0)
            {
              if (error)
                *error = QStringLiteral("通道 %1 帧宽无法确定（reprc %2）")
                             .arg(sp.mnemonic)
                             .arg(sp.reprc);
              return LisWalkResult::Invalid;
            }
            frameSize += b;
          }
          while (c.left() > 0 && !c.bad)
          {
            if (c.left() < frameSize)
            {
              const QString msg =
                  QStringLiteral("数据记录尾部半帧（剩 %1 字节 < 帧宽 %2）——"
                                 "半帧丢弃并记截断")
                      .arg(c.left())
                      .arg(frameSize);
              addIssue(issues, LasIssue::Severity::Error,
                       LasIssue::Category::Truncated, msg);
              if (error)
                *error = msg;
              return LisWalkResult::Truncated;
            }
            for (int si = 0; si < ls.specs.size(); ++si)
            {
              const SpecBlock &sp = ls.specs.at(si);
              const int bytes = specFrameBytes(sp);
              if (sp.suppressed)
              {
                c.skip(bytes);
                continue;
              }
              const double v = readLisNumeric(c, sp.reprc);
              if (c.bad)
              {
                if (error)
                  *error = QStringLiteral("通道 %1 样本截断").arg(sp.mnemonic);
                return LisWalkResult::Truncated;
              }
              const int fixed = lisFixedRepSize(sp.reprc);
              const int slack = bytes - fixed * sp.samples;
              if (slack > 0)
                c.skip(slack);
              if (v == ls.absentValue)
                ls.flat.append(std::numeric_limits<double>::quiet_NaN());
              else
                ls.flat.append(v);
              if (ls.depthMode == 0 && si == 0)
                ls.depths.append(v);
            }
            if (ls.depthMode == 1)
            {
              ls.depths.append(depth);
              depth += (ls.direction == 1) ? -ls.spacing : ls.spacing;
            }
          }
          if (stopAtFirstData)
            return LisWalkResult::Ok;
          break;
        }
        case RecEncTableDump:
        case RecTableDump:
          addIssue(issues, LasIssue::Severity::Warning, LasIssue::Category::Format,
                   QStringLiteral("表转储记录（类型 %1）不读——白名单：无表布局语义")
                       .arg(int(rec.type)));
          break;
        default:
          break; // 文本/引导/程序载入等：结构性跳过（无井曲线语义）
      }
      if (out.stoppedAtTrailer)
        break;
    }
    return LisWalkResult::Ok;
  }
} // namespace

bool LisParser::sniff(const QString &path)
{
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
    return false;
  if (f.size() < 6)
    return false;
  // TIF：首 12 字节磁带标头轻量判定（全链验证归解析路径）
  if (looksLikeTifHead(f))
    return true;
  // 裸流：首 PRH 长度可容纳 LRH 且记录类型在表内
  uchar hdr[6];
  if (!f.seek(0) || f.read(reinterpret_cast<char *>(hdr), 6) != 6)
    return false;
  const quint16 len = quint16(hdr[0]) << 8 | quint16(hdr[1]);
  if (len < 6 || qint64(len) > f.size())
    return false;
  return validRecordType(hdr[4]);
}

bool LisParser::parse(const QString &path, LasHeaderInfo &header,
                      QList<LasCurve> &curves, QString *error,
                      QList<LasIssue> *issues)
{
  if (error)
    error->clear();
  header = LasHeaderInfo{};
  curves.clear();

  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
  {
    if (error)
      *error = QStringLiteral("无法打开 %1").arg(path);
    return false;
  }

  // 与 LasParser::parseDoc 同口径的大文件防护（头扫描不受限）
  if (QFileInfo(path).size() > LasParser::fileSizeLimit())
  {
    if (issues)
    {
      LasIssue issue;
      issue.severity = LasIssue::Severity::Error;
      issue.category = LasIssue::Category::Oversize;
      issue.message = QStringLiteral("文件 %1 超过整读上限（%2 MB）——井曲线体拒绝整读")
                           .arg(QFileInfo(path).fileName())
                           .arg(LasParser::fileSizeLimit() / (1024 * 1024));
      issues->append(issue);
    }
    if (error)
      *error = QStringLiteral("文件超过整读上限（Oversize）");
    return false;
  }

  WalkOut out;
  const LisWalkResult r =
      walkRecords(f, /*stopAtFirstData=*/false, out, issues, error);
  header.wellName = out.wellName; // 截断时井名仍如实带出（诚实面）
  if (r != LisWalkResult::Ok)
    return false; // 截断/非法：error 已由走查给因；已解部分不冒充完整
  if (out.primaryLogset < 0)
  {
    for (const LogSet &ls : out.logsets)
      if (ls.sawData && ls.whitelisted)
      {
        if (error)
          *error = QStringLiteral("logset 全部为白名单子结构：%1")
                       .arg(ls.whitelistReason);
        return false;
      }
    if (error)
      *error = QStringLiteral("无数据记录（Normal Data）——井曲线读面需要帧数据");
    return false;
  }

  const LogSet &ls = out.logsets.at(out.primaryLogset);
  header.sawAscii = ls.sawData;
  header.nullValue = paleo::wellnumeric::kLasDefaultNull; // 缺席值已按条目 12 映射为 NaN
  bool tvd = false;
  for (const SpecBlock &sp : ls.specs)
    if (sp.tvdCorrected)
      tvd = true;
  header.indexBasis = tvd ? QStringLiteral("TVD") : QStringLiteral("MD");

  // 列组装：curves[0] = 深度道（模式 0 = 索引 spec；模式 1 = 推算深度）
  const int cols = ls.columns();
  const int rows = ls.depths.size();
  if (rows == 0 || cols == 0 || ls.flat.size() != rows * cols)
  {
    if (error)
      *error = QStringLiteral("帧数据与目录不齐（帧 %1 × 列 %2 = %3，实得 %4）")
                   .arg(rows)
                   .arg(cols)
                   .arg(rows * cols)
                   .arg(ls.flat.size());
    return false;
  }

  int flatCol = 0; // flat 列号（非抑制道序，含索引道——错位即取错列）
  for (int si = 0; si < ls.specs.size(); ++si)
  {
    const SpecBlock &sp = ls.specs.at(si);
    if (sp.suppressed)
      continue;
    const bool isIndex = (ls.depthMode == 0 && si == 0);
    LasCurve curve;
    curve.name = sp.mnemonic;
    curve.unit = sp.units;
    curve.values.reserve(rows);
    for (int r = 0; r < rows; ++r)
    {
      if (isIndex)
        curve.values.append(ls.depths.at(r));
      else
        curve.values.append(ls.flat.at(r * cols + flatCol));
    }
    ++flatCol;
    curves.append(curve);
  }
  // 模式 1 无索引 spec：合成深度道置首（名称 DEPT，单位 = 条目 14）
  if (ls.depthMode == 1)
  {
    LasCurve depthCurve;
    depthCurve.name = QStringLiteral("DEPT");
    depthCurve.unit = ls.depthUnits;
    depthCurve.values = ls.depths;
    curves.prepend(depthCurve);
  }
  for (const LasCurve &c : curves)
    header.curveNames.append(c.name);
  return true;
}

bool LisParser::parseHeader(const QString &path, LasHeaderInfo &out,
                            QString *error, QList<LasIssue> *issues)
{
  if (error)
    error->clear();
  out = LasHeaderInfo{};

  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
  {
    if (error)
      *error = QStringLiteral("无法打开 %1").arg(path);
    return false;
  }
  WalkOut wout;
  const LisWalkResult r =
      walkRecords(f, /*stopAtFirstData=*/true, wout, issues, error);
  if (r == LisWalkResult::Invalid)
    return false;

  out.wellName = wout.wellName;
  if (wout.primaryLogset < 0)
  {
    if (error)
      *error = QStringLiteral("头扫描未取得曲线目录（无 DFSR/数据记录或全部为"
                              "白名单子结构）");
    return false;
  }
  const LogSet &ls = wout.logsets.at(wout.primaryLogset);
  out.sawAscii = ls.sawData;
  out.nullValue = paleo::wellnumeric::kLasDefaultNull;
  bool tvd = false;
  for (const SpecBlock &sp : ls.specs)
    if (sp.tvdCorrected)
      tvd = true;
  out.indexBasis = tvd ? QStringLiteral("TVD") : QStringLiteral("MD");
  if (ls.depthMode == 1)
    out.curveNames.append(QStringLiteral("DEPT"));
  for (const SpecBlock &sp : ls.specs)
    if (!sp.suppressed)
      out.curveNames.append(sp.mnemonic);
  return true;
}
