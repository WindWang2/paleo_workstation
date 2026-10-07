// 层：数据
#include "dlisparser.h"

#include "../domain/wellnumeric.h"
#include "lasparser.h" // LasParser::fileSizeLimit（大文件防护共用口径）
#include <QFile>
#include <QFileInfo>

#include <cmath>
#include <cstring>
#include <limits>

// RP66 v1 字节布局（全大端；dlisparser.h 头注释 + ledger 逐条对账）：
//   SUL 80B：[0..3] 序号 [4..8] "V1.00" [9..14] "RECORD" [15..19] VR 上限
//            [20..79] 存储单元标识。
//   VR：[len u16][0xFF][0x01]，len 含 4B 头；一个 VR 装整数个 LRS——可来自
//        多个 LR，一个 LR 也可跨多个 VR 分段（前驱/后继位 0x40/0x20）。
//   LRS：[len u16][attrs u8][type u8][body][trailer]，len 含头尾；attrs 位
//        0x80=EFLR 0x40=前驱 0x20=后继 0x10=加密 0x08=加密包
//        0x04=校验和 0x02=尾长 0x01=填充；trailer 自尾部剥：
//        [pad: padcount 1B + pad 字节][traillen 2B][checksum 2B]。
//   组件描述符：role=高 3 位（SET 0xE0 / RSET 0xC0 / RDSET 0xA0 / 保留 0x80 /
//        OBJECT 0x60 / INVATR 0x40 / ATTRIB 0x20 / ABSATR 0x00）；
//        SET 位 0x10=type 0x08=name；OBJECT 位 0x10=name；
//        ATTRIB 位 0x10=label 0x08=count 0x04=repc 0x02=units 0x01=value，
//        特征按位序出现（label→count→repc→units→value）。
//   FDATA 体：[OBNAME 帧引用][UVARI 帧号][slots...]，每 slot 一个通道样本
//        （元素数 = DIMENSION 积，元素宽 = 表示码宽）。

namespace
{
  // 表示码（RP66 v1 磁盘编号）
  enum RepCode
  {
    RepFshort = 1, RepFsingl = 2, RepFsing1 = 3, RepFsing2 = 4,
    RepIsingl = 5, RepVsingl = 6, RepFdoubl = 7, RepFdoub1 = 8,
    RepFdoub2 = 9, RepCsingl = 10, RepCdoubl = 11, RepSshort = 12,
    RepSnorm = 13, RepSlong = 14, RepUshort = 15, RepUnorm = 16,
    RepUlong = 17, RepUvari = 18, RepIdent = 19, RepAscii = 20,
    RepDtime = 21, RepOrigin = 22, RepObname = 23, RepObjref = 24,
    RepAttref = 25, RepStatus = 26, RepUnits = 27,
  };

  int fixedRepSize(int repc)
  {
    switch (repc)
    {
      case RepFshort: return 2;
      case RepFsingl: case RepIsingl: case RepVsingl: return 4;
      case RepFsing1: return 8;
      case RepFsing2: return 12;
      case RepFdoubl: return 8;
      case RepFdoub1: return 16;
      case RepFdoub2: return 24;
      case RepCsingl: return 8;
      case RepCdoubl: return 16;
      case RepSshort: case RepUshort: case RepStatus: return 1;
      case RepSnorm: case RepUnorm: return 2;
      case RepSlong: case RepUlong: return 4;
      case RepDtime: return 8;
      default: return -1; // 变长（18/19/20/22/23/24/25/27）或未定义（66）
    }
  }

  bool numericRep(int repc)
  {
    switch (repc)
    {
      case RepFshort: case RepFsingl: case RepFsing1: case RepFsing2:
      case RepIsingl: case RepVsingl: case RepFdoubl: case RepFdoub1:
      case RepFdoub2: case RepSshort: case RepSnorm: case RepSlong:
      case RepUshort: case RepUnorm: case RepUlong: case RepUvari:
      case RepOrigin: case RepStatus:
        return true;
      default:
        return false;
    }
  }

  // ---- 字节游标：越界置 bad，调用方统一收口报错（不抛异常） ----
  struct Cur
  {
    const uchar *p = nullptr;
    const uchar *end = nullptr;
    bool bad = false;

    int left() const { return int(end - p); }
    bool need(int n) const { return left() >= n; }
    void fail() { bad = true; }

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
    qint64 uvari()
    {
      const uchar b = u8();
      if (bad)
        return 0;
      switch (b & 0xC0)
      {
        case 0x00: return qint64(b & 0x3F);
        case 0x40: return (qint64(b & 0x3F) << 8) | u8();
        case 0x80: return (qint64(b & 0x3F) << 24) | (qint64(u8()) << 16) |
                          (qint64(u8()) << 8) | u8();
        default: bad = true; return 0;
      }
    }
    QString ident()
    {
      const uchar n = u8();
      if (bad)
        return QString();
      if (n == 0)
        return QString(); // RP66：0 长度 = 空标识，合法
      if (!need(n)) { bad = true; return QString(); }
      const QString s = QString::fromLatin1(reinterpret_cast<const char *>(p), int(n));
      p += n;
      return s;
    }
    DlisParser::Obname obname()
    {
      DlisParser::Obname o;
      o.origin = uvari();
      o.copy = u8();
      o.id = ident();
      return o;
    }
    void skip(int n)
    {
      if (!need(n)) { bad = true; return; }
      p += n;
    }
  };

  double bitsToFloat(quint32 bits)
  {
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return double(f);
  }
  double bitsToDouble(quint64 bits)
  {
    double d;
    std::memcpy(&d, &bits, sizeof(d));
    return d;
  }

  // FSHORT（RP66 低精度浮点）：sign 1 / exp 4（无偏置）/ frac 12，
  // 负数 frac 取 2 补码——按规范实现（与 dlisio 参考实现一致）。
  double decodeFshort(quint16 v)
  {
    const bool sign = v & 0x8000;
    const int exp = int(v & 0x000F);
    quint16 frac = (v & 0xFFF0) >> 4;
    if (sign)
      frac = quint16(((~frac) & 0x0FFF) + 1);
    const double fraction = double(frac) / double(0x0800);
    return (sign ? -1.0 : 1.0) * fraction * std::pow(2.0, double(exp));
  }

  // ISINGL（IBM 单精度）：sign 1 / exp 7 偏置 64 / 底 16 尾数 24。
  // IEEE 换算走查表算法（Schryer 1983，RP66 附录 B 引用）。
  double decodeIbm(quint32 v)
  {
    static const quint32 ieeemax = 0x7FFFFFFF;
    static const quint32 iemaxib = 0x611FFFFF;
    static const quint32 ieminib = 0x21200000;
    static const quint32 it[8] = { 0x21800000, 0x21400000, 0x21000000, 0x21000000,
                                   0x20C00000, 0x20C00000, 0x20C00000, 0x20C00000 };
    static const quint32 mt[8] = { 8, 4, 2, 2, 1, 1, 1, 1 };
    quint32 manthi = v & 0x00FFFFFF;
    const quint32 ix = manthi >> 21;
    const quint32 iexp = ((v & 0x7F000000) - it[ix]) << 1;
    manthi = manthi * mt[ix] + iexp;
    const quint32 inabs = v & 0x7FFFFFFF;
    if (inabs > iemaxib)
      manthi = ieeemax;
    manthi |= (v & 0x80000000);
    const quint32 bits = (inabs < ieminib) ? quint32(0) : manthi;
    return bitsToFloat(bits);
  }

  // VSINGL（VAX F）：字序交换的类 IEEE——sign 1 / exp 8 偏置 128 /
  // 隐藏位尾数 23；磁盘字节 [m0 m1 e0 e1] 组装为 v = x1<<24|x0<<16|x3<<8|x2。
  double decodeVax(const uchar *xs)
  {
    const quint32 v = quint32(xs[1]) << 24 | quint32(xs[0]) << 16 |
                      quint32(xs[3]) << 8 | quint32(xs[2]);
    const bool sign = v & 0x80000000;
    const quint32 frac = v & 0x007FFFFF;
    const quint32 exp = (v & 0x7F800000) >> 23;
    if (exp == 0)
      return sign ? std::numeric_limits<double>::quiet_NaN() : 0.0;
    const double significand = double(frac | 0x00800000) / std::pow(2.0, 24);
    return (sign ? -1.0 : 1.0) * significand * std::pow(2.0, double(exp) - 128.0);
  }

  // 读一个数值元素（调用方保证 repc 数值类且字节够）。校验对（FSING1/2、
  // FDOUB1/2）只取 V 值，校验位按 RP66 是写入方断言，读侧不拦截。
  double readNumeric(Cur &c, int repc)
  {
    switch (repc)
    {
      case RepFshort: return decodeFshort(c.u16());
      case RepFsingl: return bitsToFloat(c.u32());
      case RepFsing1: { const double v = bitsToFloat(c.u32()); c.skip(4); return v; }
      case RepFsing2: { const double v = bitsToFloat(c.u32()); c.skip(8); return v; }
      case RepIsingl: return decodeIbm(c.u32());
      case RepVsingl:
      {
        if (!c.need(4)) { c.fail(); return 0.0; }
        const double v = decodeVax(c.p);
        c.p += 4;
        return v;
      }
      case RepFdoubl:
      {
        const quint64 bits = (quint64(c.u32()) << 32) | c.u32();
        return bitsToDouble(bits);
      }
      case RepFdoub1: case RepFdoub2:
      {
        const quint64 bits = (quint64(c.u32()) << 32) | c.u32();
        const double v = bitsToDouble(bits);
        c.skip(repc == RepFdoub1 ? 8 : 16);
        return v;
      }
      case RepSshort: return qint8(c.u8());
      case RepSnorm: return qint16(c.u16());
      case RepSlong: return qint32(c.u32());
      case RepUshort: return c.u8();
      case RepUnorm: return c.u16();
      case RepUlong: return c.u32();
      case RepUvari: return double(c.uvari());
      case RepOrigin: return double(c.uvari());
      case RepStatus: return c.u8();
      default: c.fail(); return 0.0;
    }
  }

  // ---- EFLR 中间表示 ----
  struct ElValue
  {
    enum Kind { Empty, Num, Str, Ob, ObList, NumList, Opaque } kind = Empty;
    double num = 0;
    QString str;
    DlisParser::Obname ob;
    QVector<DlisParser::Obname> obList;
    QVector<double> numList;
  };

  struct ElAttr
  {
    QString label;
    qint64 count = 1;   // 显式或模板默认；0 = 无值
    int repc = RepIdent; // RP66 全局默认 IDENT
    QString units;
    bool invariant = false;
    bool absent = false; // ABSATR：连默认语义都没有
    ElValue value;
  };

  struct ElObject
  {
    DlisParser::Obname name;
    QVector<ElAttr> attrs; // 与模板同序
  };

  struct ElSet
  {
    QString type;
    DlisParser::Obname name;
    bool redundant = false;
    QVector<ElAttr> tmpl;
    QVector<ElObject> objects;
  };

  QString obnameKey(const DlisParser::Obname &o)
  {
    return QStringLiteral("%1#%2#%3").arg(o.origin).arg(o.copy).arg(o.id);
  }

  // EFLR 通用机器：Set → 模板 → 对象。结构解析不了 → false（error 给因）。
  bool parseEflr(const uchar *data, int size, ElSet &out, QString *error)
  {
    Cur c{ data, data + size, false };
    const uchar desc = c.u8();
    if (c.bad)
    {
      if (error)
        *error = QStringLiteral("EFLR 空 body");
      return false;
    }
    const int role = desc & 0xE0;
    if (role != 0xE0 && role != 0xC0 && role != 0xA0)
    {
      if (error)
        *error = QStringLiteral("EFLR 首组件不是 Set（descriptor 0x%1）")
                     .arg(desc, 2, 16, QLatin1Char('0'));
      return false;
    }
    out.redundant = (role == 0xA0);
    if (desc & 0x10)
      out.type = c.ident();
    if (!c.bad && (desc & 0x08))
      out.name = c.obname();
    if (c.bad)
    {
      if (error)
        *error = QStringLiteral("EFLR Set 头截断/非法");
      return false;
    }
    if (out.type.isEmpty() && !(role == 0xA0))
    {
      // RP66：Set Type 必须显式非空（冗余集可空并按唯一同型回指）
      if (error)
        *error = QStringLiteral("EFLR Set Type 缺失");
      return false;
    }

    // 读一个属性组件（模板态或对象态）。absent=true 表示 ABSATR。
    auto readAttr = [&c](ElAttr &attr, bool *absent, QString *err) -> bool
    {
      *absent = false;
      const uchar d = c.u8();
      if (c.bad) { *err = QStringLiteral("组件描述符越界"); return false; }
      const int r = d & 0xE0;
      if (r == 0x00) { *absent = true; return true; } // ABSATR：无特征
      if (r != 0x20 && r != 0x40)
      {
        *err = QStringLiteral("期待属性组件，得到 role 0x%1")
                   .arg(r, 2, 16, QLatin1Char('0'));
        return false;
      }
      attr.invariant = (r == 0x40);
      if (d & 0x10) attr.label = c.ident();
      if (!c.bad && (d & 0x08)) attr.count = c.uvari();
      if (!c.bad && (d & 0x04)) attr.repc = int(c.u8());
      if (!c.bad && (d & 0x02)) attr.units = c.ident();
      if (c.bad) { *err = QStringLiteral("属性特征序列截断"); return false; }
      if (attr.repc < 0 || attr.repc > RepUnits)
      {
        *err = QStringLiteral("属性 %1 表示码 %2 越界")
                   .arg(attr.label.isEmpty() ? QStringLiteral("<模板>") : attr.label)
                   .arg(attr.repc);
        return false;
      }
      if (d & 0x01)
      {
        // Value：count 个 repc 元素
        const int fixed = fixedRepSize(attr.repc);
        for (qint64 i = 0; i < attr.count && !c.bad; ++i)
        {
          if (fixed > 0)
          {
            if (numericRep(attr.repc))
            {
              if (attr.value.kind != ElValue::NumList &&
                  attr.value.kind != ElValue::Num)
              {
                attr.value.kind = attr.count > 1 ? ElValue::NumList : ElValue::Num;
              }
              const double v = readNumeric(c, attr.repc);
              if (attr.count > 1)
                attr.value.numList.append(v);
              else
                attr.value.num = v;
            }
            else
            {
              // DTIME / CSINGL / CDOUBL：结构有效但无标量语义 → Opaque
              attr.value.kind = ElValue::Opaque;
              c.skip(fixed);
            }
          }
          else if (attr.repc == RepIdent || attr.repc == RepAscii ||
                   attr.repc == RepUnits)
          {
            const QString s = c.ident();
            if (attr.value.kind == ElValue::Empty)
              attr.value.kind = ElValue::Str;
            if (attr.count > 1 && attr.value.kind == ElValue::Str)
              attr.value.str += QStringLiteral(" ");
            attr.value.str += s;
          }
          else if (attr.repc == RepObname)
          {
            const DlisParser::Obname o = c.obname();
            attr.value.kind = ElValue::ObList;
            attr.value.obList.append(o);
            if (attr.count == 1)
              attr.value.ob = o;
          }
          else if (attr.repc == RepObjref)
          {
            c.ident(); // 引用类型名（本读面不消费）
            const DlisParser::Obname o = c.obname();
            attr.value.kind = ElValue::ObList;
            attr.value.obList.append(o);
            if (attr.count == 1)
              attr.value.ob = o;
          }
          else if (attr.repc == RepAttref)
          {
            c.ident();
            c.obname();
            c.ident();
            attr.value.kind = ElValue::Opaque;
          }
          else if (attr.repc == RepUvari || attr.repc == RepOrigin)
          {
            const double v = double(c.uvari());
            if (attr.value.kind != ElValue::NumList &&
                attr.value.kind != ElValue::Num)
              attr.value.kind = attr.count > 1 ? ElValue::NumList : ElValue::Num;
            if (attr.count > 1)
              attr.value.numList.append(v);
            else
              attr.value.num = v;
          }
          else
          {
            *err = QStringLiteral("属性 %1 表示码 %2 无法解码")
                       .arg(attr.label).arg(attr.repc);
            return false;
          }
        }
        if (c.bad)
        {
          *err = QStringLiteral("属性 %1 值序列截断").arg(attr.label);
          return false;
        }
      }
      return true;
    };

    // 模板：读到 OBJECT 为止
    bool inTemplate = true;
    ElObject current;
    while (c.left() > 0 && !c.bad)
    {
      if (inTemplate)
      {
        const uchar d = *c.p;
        if ((d & 0xE0) == 0x60) // OBJECT
        {
          c.u8();
          current = ElObject{};
          if (d & 0x10)
            current.name = c.obname();
          if (c.bad)
          {
            if (error)
              *error = QStringLiteral("对象名截断");
            return false;
          }
          inTemplate = false;
          continue;
        }
        ElAttr attr;
        bool absent = false;
        QString err;
        if (!readAttr(attr, &absent, &err))
        {
          if (error)
            *error = err;
          return false;
        }
        if (absent)
        {
          if (error)
            *error = QStringLiteral("模板中出现 ABSATR（非法）");
          return false;
        }
        out.tmpl.append(attr);
      }
      else
      {
        const uchar d = *c.p;
        if ((d & 0xE0) == 0x60)
        {
          out.objects.append(current);
          current = ElObject{};
          c.u8();
          if (d & 0x10)
            current.name = c.obname();
          if (c.bad)
          {
            if (error)
              *error = QStringLiteral("对象名截断");
            return false;
          }
          continue;
        }
        // 对象行属性：按模板列序落位；缺尾 = 用模板默认（复制模板值）
        int col = 0;
        bool rowDone = false;
        while (!rowDone && c.left() > 0 && !c.bad)
        {
          const uchar ad = *c.p;
          if ((ad & 0xE0) == 0x60 || (ad & 0xE0) == 0xE0 || (ad & 0xE0) == 0xC0 ||
              (ad & 0xE0) == 0xA0)
            break; // 下一对象 / 下一 Set
          if (col >= out.tmpl.size())
          {
            if (error)
              *error = QStringLiteral("对象 %1 属性数超出模板列数")
                           .arg(current.name.id);
            return false;
          }
          ElAttr attr = out.tmpl.at(col); // 继承模板默认
          attr.invariant = false;
          bool absent = false;
          QString err;
          if (!readAttr(attr, &absent, &err))
          {
            if (error)
              *error = err;
            return false;
          }
          if (absent)
            attr.absent = true;
          if (attr.invariant)
          {
            if (error)
              *error = QStringLiteral("对象行出现 INVATR（非法）");
            return false;
          }
          current.attrs.append(attr);
          ++col;
        }
        if (c.bad)
        {
          if (error)
            *error = QStringLiteral("对象 %1 属性序列截断").arg(current.name.id);
          return false;
        }
        // 模板不变列 + 缺尾列补默认（不变列在模板里已有值，逐列对齐）
        for (int rest = current.attrs.size(); rest < out.tmpl.size(); ++rest)
          current.attrs.append(out.tmpl.at(rest));
        rowDone = true;
      }
    }
    if (c.bad)
    {
      if (error)
        *error = QStringLiteral("EFLR 组件流截断");
      return false;
    }
    if (c.left() > 0 && !inTemplate)
    {
      // 上面行循环因下一 Set 组件跳出而停——单 EFLR 只允许一个 Set
      const uchar d = *c.p;
      if ((d & 0xE0) == 0xE0 || (d & 0xE0) == 0xC0 || (d & 0xE0) == 0xA0)
      {
        if (error)
          *error = QStringLiteral("EFLR 内出现第二个 Set 组件");
        return false;
      }
    }
    if (!inTemplate)
      out.objects.append(current); // 最后一个对象（行尾缺列已补模板默认）
    return true;
  }

  // ---- 流式文件源：QFile 顺序读 + 滚动缓冲 ----
  class ByteSource
  {
  public:
    explicit ByteSource(QFile &f) : m_f(f) {}

    // 保证缓冲从 ptr() 起至少 n 字节（或到 EOF 全部给出）
    bool ensure(int n)
    {
      if (m_off + n <= m_buf.size())
        return true;
      if (m_off > 0)
      {
        m_buf.remove(0, m_off);
        m_base += m_off;
        m_off = 0;
      }
      while (m_buf.size() < n)
      {
        const int oldSize = m_buf.size();
        m_buf.resize(oldSize + qMax(65536, n - oldSize));
        const qint64 got = m_f.read(m_buf.data() + oldSize,
                                    m_buf.size() - oldSize);
        if (got <= 0)
        {
          m_buf.resize(oldSize);
          return m_buf.size() >= n; // EOF 或读错误：有多少算多少
        }
        m_buf.resize(oldSize + int(got));
      }
      return true;
    }
    const uchar *ptr() const
    {
      return reinterpret_cast<const uchar *>(m_buf.constData()) + m_off;
    }
    void advance(int n) { m_off += n; }
    int buffered() const { return m_buf.size() - m_off; }
    qint64 offset() const { return m_base + m_off; }
    bool atEnd()
    {
      if (buffered() > 0)
        return false;
      return !ensure(1);
    }

  private:
    QFile &m_f;
    QByteArray m_buf;
    qint64 m_base = 0;
    int m_off = 0;
  };

  void addIssue(QList<LasIssue> *issues, LasIssue::Severity sev,
                LasIssue::Category cat, const QString &msg)
  {
    if (!issues)
      return;
    LasIssue issue;
    issue.severity = sev;
    issue.category = cat;
    issue.message = msg;
    issues->append(issue);
  }

  struct ChannelState
  {
    DlisParser::Obname name;
    QString units;
    int repc = -1;
    QVector<qint64> dimension;
    QString longNameRef;
    QString descr;
    bool unusableNoted = false;
  };

  struct FrameState
  {
    DlisParser::Obname name;
    QVector<DlisParser::Obname> channels;
    QString indexType;
    QString direction;
    bool encrypted = false;
  };

  enum class WalkResult { Ok, StoppedAtNextFile, Error };

  // 单遍走查器：EFLR 状态机 + FDATA 解码（解码模式 = withData）
  struct Walker
  {
    QHash<QString, ChannelState> channels;   // key = obnameKey
    QHash<QString, FrameState> frames;
    QHash<QString, QString> longNames;       // obnameKey → 描述文本
    QString wellName;
    bool sawFirstFhlr = false;
    bool sawAnyFdata = false;
    bool catalogFrozen = false;              // 首帧后目录冻结（多遍文件二次改写不进目录）
    QString firstDataFrameKey;               // 主帧（首个有 FDATA 的帧类型）
    QHash<QString, QVector<double>> values;  // obnameKey → 列值（含索引道）
    QHash<QString, qint64> lastFrameNo;
    QList<QPair<QString, qint64>> frameOrderIssues;
    bool noFormatNoted = false;
    bool multiLfNoted = false;

    void noteFrameNumber(const QString &key, qint64 no, QList<LasIssue> *issues)
    {
      const qint64 prev = lastFrameNo.value(key, 0);
      if (no <= prev && prev > 0)
        addIssue(issues, LasIssue::Severity::Warning, LasIssue::Category::Format,
                 QStringLiteral("帧类型 %1 帧号 %2 未递增（前值 %3）——按文件序采值")
                     .arg(key.section(QLatin1Char('#'), -1))
                     .arg(no)
                     .arg(prev));
      lastFrameNo.insert(key, no);
    }

    void consumeEflr(int lrType, const ElSet &set, QList<LasIssue> *issues)
    {
      Q_UNUSED(lrType);
      if (set.redundant)
        return; // RP66：冗余集是前文副本，语义消费跳过（结构已验）
      if (set.type == QLatin1String("FILE-HEADER"))
        return; // 首个 FHLR 由调用方记（多逻辑文件边界）
      if (set.type == QLatin1String("ORIGIN"))
      {
        if (!wellName.isEmpty())
          return;
        for (const ElObject &o : set.objects)
          for (const ElAttr &v : o.attrs)
            if (!v.absent && v.label == QLatin1String("WELL-NAME") &&
                v.value.kind == ElValue::Str && !v.value.str.trimmed().isEmpty())
            {
              wellName = v.value.str.trimmed();
              return;
            }
        return;
      }
      if (set.type == QLatin1String("LONG-NAME"))
      {
        for (const ElObject &o : set.objects)
        {
          for (const ElAttr &v : o.attrs)
          {
            if (!v.absent && v.value.kind == ElValue::Str &&
                !v.value.str.trimmed().isEmpty())
            {
              longNames.insert(obnameKey(o.name), v.value.str.trimmed());
              break;
            }
          }
        }
        return;
      }
      if (set.type == QLatin1String("CHANNEL") || set.type == QLatin1String("FRAME"))
      {
        // 主帧确定后目录/解码布局冻结：数据流中后到的定义改写（二次下井
        // 回写头）沿用冻结布局，如实记 issue——否则目录与值会静默错位。
        if (catalogFrozen)
        {
          addIssue(issues, LasIssue::Severity::Warning, LasIssue::Category::Format,
                   QStringLiteral("首个 FDATA 后又遇 %1 集——目录与解码布局已冻结，"
                                  "改写不生效").arg(set.type));
          return;
        }
      }
      if (set.type == QLatin1String("CHANNEL"))
      {
        for (const ElObject &o : set.objects)
        {
          ChannelState ch;
          ch.name = o.name;
          for (const ElAttr &v : o.attrs)
          {
            if (v.absent)
              continue;
            if (v.label == QLatin1String("REPRESENTATION-CODE") &&
                v.value.kind == ElValue::Num)
              ch.repc = int(v.value.num);
            else if (v.label == QLatin1String("UNITS") &&
                     v.value.kind == ElValue::Str)
              ch.units = v.value.str.trimmed();
            else if (v.label == QLatin1String("DIMENSION"))
            {
              ch.dimension.clear();
              if (v.value.kind == ElValue::NumList)
                for (double d : v.value.numList)
                  ch.dimension.append(qint64(d));
              else if (v.value.kind == ElValue::Num)
                ch.dimension.append(qint64(v.value.num));
            }
            else if (v.label == QLatin1String("LONG-NAME") &&
                     v.value.kind == ElValue::ObList && !v.value.obList.isEmpty())
              ch.longNameRef = obnameKey(v.value.obList.first());
          }
          channels.insert(obnameKey(o.name), ch);
        }
        return;
      }
      if (set.type == QLatin1String("FRAME"))
      {
        for (const ElObject &o : set.objects)
        {
          FrameState fr;
          fr.name = o.name;
          for (const ElAttr &v : o.attrs)
          {
            if (v.absent)
              continue;
            if (v.label == QLatin1String("CHANNELS") &&
                v.value.kind == ElValue::ObList)
              fr.channels = v.value.obList;
            else if (v.label == QLatin1String("INDEX-TYPE") &&
                     v.value.kind == ElValue::Str)
              fr.indexType = v.value.str.trimmed();
            else if (v.label == QLatin1String("DIRECTION") &&
                     v.value.kind == ElValue::Str)
              fr.direction = v.value.str.trimmed();
            else if (v.label == QLatin1String("ENCRYPTED") &&
                     v.value.kind == ElValue::Num)
              fr.encrypted = (v.value.num != 0.0);
          }
          frames.insert(obnameKey(o.name), fr);
        }
        return;
      }
      // 其余 Set（AXIS/CALIBRATION/PARAMETER/TOOL/…）：结构已通用解析，
      // 语义不消费——井曲线读面只取上述五类（ledger 白名单外默认可解析）。
    }

    // FDATA 体解码。返回 false = 结构坏（截断/布局不合法）。
    bool consumeFdata(const uchar *data, int size, QList<LasIssue> *issues,
                      QString *error)
    {
      Cur c{ data, data + size, false };
      const DlisParser::Obname ref = c.obname();
      if (c.bad)
      {
        if (error)
          *error = QStringLiteral("FDATA 帧引用截断");
        return false;
      }
      const qint64 frameNo = c.uvari();
      if (c.bad)
      {
        if (error)
          *error = QStringLiteral("FDATA 帧号截断");
        return false;
      }
      const QString key = obnameKey(ref);
      if (!frames.contains(key))
      {
        // 帧未定义（RP66 要求引用对象先于 IFLR）——如实报错，不猜
        if (error)
          *error = QStringLiteral("FDATA 引用未定义帧 %1").arg(ref.id);
        return false;
      }
      const FrameState fr = frames.value(key);
      if (fr.encrypted)
      {
        addIssue(issues, LasIssue::Severity::Warning, LasIssue::Category::Format,
                 QStringLiteral("帧类型 %1 已加密——跳过（白名单：加密记录）").arg(ref.id));
        return true;
      }
      if (firstDataFrameKey.isEmpty())
      {
        firstDataFrameKey = key;
        catalogFrozen = true; // 主帧确定：目录冻结（其余帧类型走白名单）
        if (fr.direction == QLatin1String("DECREASING"))
          addIssue(issues, LasIssue::Severity::Info, LasIssue::Category::Format,
                   QStringLiteral("帧类型 %1 深度递减——按文件序保留").arg(ref.id));
      }
      noteFrameNumber(key, frameNo, issues);
      if (key != firstDataFrameKey)
      {
        // 白名单：非主帧类型的帧数据不解码（单深度轴表格口径）
        return true;
      }
      for (const DlisParser::Obname &chRef : fr.channels)
      {
        const QString ck = obnameKey(chRef);
        const ChannelState ch = channels.value(ck);
        if (!channels.contains(ck))
        {
          if (error)
            *error = QStringLiteral("帧 %1 引用未定义通道 %2").arg(ref.id, chRef.id);
          return false;
        }
        qint64 elems = 1;
        if (!ch.dimension.isEmpty())
        {
          elems = 1;
          for (qint64 d : ch.dimension)
            elems *= qMax(d, qint64(0));
        }
        const int fixed = fixedRepSize(ch.repc);
        const bool usable = elems == 1 && numericRep(ch.repc) && fixed > 0;
        if (!usable)
        {
          if (!ch.unusableNoted)
          {
            channels[ck].unusableNoted = true;
            addIssue(issues, LasIssue::Severity::Warning, LasIssue::Category::Format,
                     QStringLiteral("通道 %1 非标量（元素数 %2，表示码 %3）——"
                                    "白名单：多维/非数值通道不进表，槽位按宽度跳过")
                         .arg(chRef.id)
                         .arg(elems)
                         .arg(ch.repc));
          }
          // 槽位仍按宽度推进（多维 × 元素宽），保证后续通道不错位
          if (fixed <= 0)
          {
            if (error)
              *error = QStringLiteral("通道 %1 表示码 %2 无固定宽度，无法跳过槽位")
                           .arg(chRef.id)
                           .arg(ch.repc);
            return false;
          }
          c.skip(int(elems) * fixed);
          if (c.bad)
          {
            if (error)
              *error = QStringLiteral("帧 %1 于通道 %2 处截断").arg(ref.id, chRef.id);
            return false;
          }
          continue;
        }
        const double v = readNumeric(c, ch.repc);
        if (c.bad)
        {
          if (error)
            *error = QStringLiteral("帧 %1 于通道 %2 处截断").arg(ref.id, chRef.id);
          return false;
        }
        values[ck].append(v);
      }
      if (c.left() > 0)
        addIssue(issues, LasIssue::Severity::Warning, LasIssue::Category::Format,
                 QStringLiteral("帧 %1（帧号 %2）有 %3 字节残余——畸形帧边界，"
                                "已按帧布局解出全部通道")
                     .arg(ref.id)
                     .arg(frameNo)
                     .arg(c.left()));
      return true;
    }
  };

  QString indexBasisFor(const Walker &w)
  {
    const FrameState fr = w.frames.value(w.firstDataFrameKey);
    if (fr.indexType == QLatin1String("BOREHOLE-DEPTH"))
      return QStringLiteral("MD");
    if (fr.indexType == QLatin1String("VERTICAL-DEPTH"))
      return QStringLiteral("TVD");
    if (fr.indexType.isEmpty())
    {
      // INDEX-TYPE 缺失：按索引道单位判定时间轴，否则如实未知
      if (!fr.channels.isEmpty())
      {
        const ChannelState ch = w.channels.value(obnameKey(fr.channels.first()));
        const QString u = ch.units.toUpper();
        if (u == QLatin1String("S") || u == QLatin1String("MS") ||
            u == QLatin1String("SEC") || u == QLatin1String("SECONDS"))
          return QStringLiteral("TIME");
      }
    }
    return QString(); // 未知：不冒充
  }

  // 组装目录（parseHeader 与 parse 共用；目录口径 = 冻结于首帧的主帧通道）
  QStringList catalogNames(const Walker &w)
  {
    QStringList names;
    if (w.firstDataFrameKey.isEmpty())
      return names;
    const FrameState fr = w.frames.value(w.firstDataFrameKey);
    for (const DlisParser::Obname &chRef : fr.channels)
    {
      const ChannelState ch = w.channels.value(obnameKey(chRef));
      qint64 elems = 1;
      if (!ch.dimension.isEmpty())
      {
        elems = 1;
        for (qint64 d : ch.dimension)
          elems *= qMax(d, qint64(0));
      }
      if (elems == 1 && numericRep(ch.repc) && fixedRepSize(ch.repc) > 0)
        names.append(chRef.id);
    }
    return names;
  }

  LasIssue truncatedIssue(const QString &where)
  {
    LasIssue issue;
    issue.severity = LasIssue::Severity::Error;
    issue.category = LasIssue::Category::Truncated;
    issue.message = where;
    return issue;
  }

  // 走查主循环。stopAtFirstFdata = parseHeader 模式。
  WalkResult walk(QFile &f, Walker &w, bool stopAtFirstFdata,
                  QList<LasIssue> *issues, QString *error)
  {
    ByteSource src(f);
    // ---- SUL：前 200 字节内找 80 字节 RECORD 标签 ----
    if (!src.ensure(80))
    {
      if (error)
        *error = QStringLiteral("文件不足 80 字节，无 DLIS 存储标签");
      return WalkResult::Error;
    }
    int sulOff = -1;
    const int scanMax = qMin(src.buffered(), 200);
    const uchar *scan = src.ptr();
    for (int o = 0; o + 80 <= scanMax; ++o)
    {
      const bool rev = scan[o + 4] == 'V' && scan[o + 5] >= '0' && scan[o + 5] <= '9' &&
                       scan[o + 6] == '.' && scan[o + 7] >= '0' && scan[o + 7] <= '9' &&
                       scan[o + 8] >= '0' && scan[o + 8] <= '9';
      const bool rec = scan[o + 9] == 'R' && scan[o + 10] == 'E' && scan[o + 11] == 'C' &&
                       scan[o + 12] == 'O' && scan[o + 13] == 'R' && scan[o + 14] == 'D';
      if (rev && rec)
      {
        sulOff = o;
        break;
      }
    }
    if (sulOff < 0)
    {
      if (error)
        *error = QStringLiteral("前 %1 字节未找到 DLIS 存储标签（SUL）").arg(scanMax);
      return WalkResult::Error;
    }
    if (sulOff > 0)
      addIssue(issues, LasIssue::Severity::Info, LasIssue::Category::Format,
               QStringLiteral("SUL 前有 %1 字节杂数据，已跳过").arg(sulOff));
    src.advance(sulOff + 80);

    // ---- VR / LRS 循环 ----
    // RP66 §2.2.3：一个 VR 装整数个 LRS——这些段可来自多个 LR，一个 LR 也
    // 可跨多个 VR 分段（前驱/后继位 0x40/0x20）。段流跨 VR 边界累积进
    // lrBuf，遇无后继段即分派，随后在同一 VR 内继续读下一个 LRS。
    bool stopped = false;
    QByteArray lrBuf;
    int lrType = -1;
    bool explicitRec = false;
    bool encryptedRec = false;
    bool firstSeg = true;
    bool continuation = false; // 上段带后继位：期待下一段（可跨 VR）带前驱位
    while (!stopped)
    {
      if (src.atEnd())
        break;
      if (!src.ensure(4))
      {
        if (error)
          *error = QStringLiteral("文件在可见记录头处截断");
        if (issues)
          issues->append(truncatedIssue(QStringLiteral("可见记录头截断")));
        return WalkResult::Error;
      }
      const uchar *vh = src.ptr();
      const quint16 vrLen = quint16(vh[0]) << 8 | quint16(vh[1]);
      const uchar mark = vh[2];
      const uchar ver = vh[3];
      src.advance(4);
      if (mark != 0xFF || ver != 0x01)
      {
        if (error)
          *error = QStringLiteral("可见记录头非法（0x%1 0x%2，期待 0xFF 0x01）")
                       .arg(mark, 2, 16, QLatin1Char('0'))
                       .arg(ver, 2, 16, QLatin1Char('0'));
        return WalkResult::Error;
      }
      if (vrLen < 4)
      {
        if (error)
          *error = QStringLiteral("可见记录长度 %1 非法").arg(vrLen);
        return WalkResult::Error;
      }
      const qint64 vrEnd = src.offset() + vrLen - 4;

      // ---- LRS 循环：一个 VR 内可装多个 LR 的段（多 LR 打包）----
      while (src.offset() < vrEnd)
      {
        if (!src.ensure(4))
        {
          if (error)
            *error = QStringLiteral("文件在逻辑记录段头处截断");
          if (issues)
            issues->append(truncatedIssue(QStringLiteral("逻辑记录段头截断")));
          return WalkResult::Error;
        }
        const uchar *lh = src.ptr();
        const quint16 segLen = quint16(lh[0]) << 8 | quint16(lh[1]);
        const uchar attrs = lh[2];
        const uchar type = lh[3];
        src.advance(4);
        if (segLen < 4)
        {
          if (error)
            *error = QStringLiteral("逻辑记录段长 %1 非法").arg(segLen);
          return WalkResult::Error;
        }
        if ((attrs & 0x40) && !continuation)
        {
          // 段声明前驱却无待续 LR——分段状态与字节流不一致（结构损坏）
          if (error)
            *error = QStringLiteral("逻辑记录段带前驱位但无待续逻辑记录");
          return WalkResult::Error;
        }
        qint64 bodyLen = segLen - 4;
        qint64 roomInVr = vrEnd - src.offset();
        if (bodyLen > roomInVr)
        {
          if (error)
            *error = QStringLiteral("逻辑记录段（%1 字节）越过可见记录边界")
                         .arg(segLen);
          return WalkResult::Error;
        }
        if (!src.ensure(int(bodyLen)))
        {
          if (error)
            *error = QStringLiteral("文件在逻辑记录段体处截断（%1 字节）").arg(bodyLen);
          if (issues)
            issues->append(truncatedIssue(QStringLiteral("逻辑记录段体截断")));
          return WalkResult::Error;
        }
        if (firstSeg)
        {
          lrType = type;
          explicitRec = attrs & 0x80;
          encryptedRec = attrs & 0x10;
          firstSeg = false;
        }
        if (encryptedRec)
        {
          // 白名单：加密记录——段体按长度推进保持同步，整 LR 跳过
          src.advance(int(bodyLen));
        }
        else
        {
          // 段体（尾随 trailer 剥离前）读入 lrBuf
          lrBuf.append(reinterpret_cast<const char *>(src.ptr()), int(bodyLen));
          src.advance(int(bodyLen));
          // trailer 自尾部剥：pad / traillen / checksum
          int trim = 0;
          if (attrs & 0x04)
            trim += 2; // checksum
          if (attrs & 0x02)
            trim += 2; // trailing length
          if (attrs & 0x01)
          {
            // padcount 位于段尾（含上述 trim 之后的位置起算：从最尾剥）
            const int padPos = lrBuf.size() - trim - 1;
            if (padPos < 0)
            {
              if (error)
                *error = QStringLiteral("填充字节计数越界（段长 %1）").arg(segLen);
              return WalkResult::Error;
            }
            const uchar padCount = uchar(lrBuf.at(padPos));
            trim += 1 + int(padCount);
          }
          if (trim > lrBuf.size())
          {
            if (error)
              *error = QStringLiteral("段尾（校验/尾长/填充）越界（段长 %1）").arg(segLen);
            return WalkResult::Error;
          }
          lrBuf.chop(trim);
        }
        continuation = attrs & 0x20;
        if (continuation)
          continue; // LR 未完：续读同 VR 下一段，或跨 VR 读下一可见记录

        // ---- 分派（无后继段：LR 至此完整）----
        if (encryptedRec)
        {
          addIssue(issues, LasIssue::Severity::Warning, LasIssue::Category::Format,
                   QStringLiteral("逻辑记录类型 %1 加密——跳过（白名单：加密记录）")
                       .arg(lrType));
        }
        else if (explicitRec)
        {
          if (lrType == 0 && w.sawFirstFhlr)
          {
            if (!w.multiLfNoted)
            {
              w.multiLfNoted = true;
              addIssue(issues, LasIssue::Severity::Warning, LasIssue::Category::Truncated,
                       QStringLiteral("遇第二个文件头（多逻辑文件）——只读第一个逻辑文件"
                                      "（白名单：多逻辑文件）"));
            }
            return WalkResult::StoppedAtNextFile;
          }
          if (lrType == 0)
            w.sawFirstFhlr = true;
          ElSet set;
          QString eflrErr;
          if (!parseEflr(reinterpret_cast<const uchar *>(lrBuf.constData()),
                         lrBuf.size(), set, &eflrErr))
          {
            if (error)
              *error = QStringLiteral("EFLR（类型 %1）解析失败：%2").arg(lrType).arg(eflrErr);
            return WalkResult::Error;
          }
          w.consumeEflr(lrType, set, issues);
        }
        else
        {
          if (lrType == 0)
          {
            w.sawAnyFdata = true;
            const bool ok = w.consumeFdata(
                reinterpret_cast<const uchar *>(lrBuf.constData()), lrBuf.size(),
                issues, error);
            if (!ok)
              return WalkResult::Error;
            if (stopAtFirstFdata && !w.firstDataFrameKey.isEmpty())
            {
              // 头部模式：主帧确定即停（目录冻结点已达成）
              stopped = true;
            }
          }
          else if (lrType == 1)
          {
            if (!w.noFormatNoted)
            {
              w.noFormatNoted = true;
              addIssue(issues, LasIssue::Severity::Warning, LasIssue::Category::Format,
                       QStringLiteral("NO-FORMAT 记录不读（白名单：RP66 不定义其布局）"));
            }
          }
          else
          {
            if (error)
              *error = QStringLiteral("隐式记录类型 %1 不在 RP66 v1 定义内（0=FDATA，"
                                      "1=NO-FORMAT）")
                           .arg(lrType);
            return WalkResult::Error;
          }
        }
        lrBuf.clear();
        firstSeg = true;
        if (stopped)
          break;
      }
      if (stopped)
        break;
      // VR 结束而 continuation 仍为真 = LR 跨 VR 分段（前驱/后继位闭合）：
      // 直接读下一个 VR 头继续累积——段必须落在单个 VR 内，VR 头即边界
    }
    if (continuation)
    {
      // 文件结束但 LR 未闭合（末段后继位悬置）——半截 LR 不得当完整数据交出
      if (error)
        *error = QStringLiteral("文件结束时逻辑记录后继位悬置（跨可见记录未闭合）");
      if (issues)
        issues->append(truncatedIssue(QStringLiteral("逻辑记录跨可见记录未闭合")));
      return WalkResult::Error;
    }
    return WalkResult::Ok;
  }
} // namespace

bool DlisParser::sniff(const QString &path)
{
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
    return false;
  const QByteArray head = f.read(200);
  if (head.size() < 80)
    return false;
  const uchar *p = reinterpret_cast<const uchar *>(head.constData());
  for (int o = 0; o + 80 <= head.size(); ++o)
  {
    const bool rev = p[o + 4] == 'V' && p[o + 5] >= '0' && p[o + 5] <= '9' &&
                     p[o + 6] == '.' && p[o + 7] >= '0' && p[o + 7] <= '9' &&
                     p[o + 8] >= '0' && p[o + 8] <= '9';
    const bool rec = p[o + 9] == 'R' && p[o + 10] == 'E' && p[o + 11] == 'C' &&
                     p[o + 12] == 'O' && p[o + 13] == 'R' && p[o + 14] == 'D';
    if (rev && rec)
      return true;
  }
  return false;
}

bool DlisParser::parse(const QString &path, LasHeaderInfo &header,
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

  Walker w;
  const WalkResult r = walk(f, w, /*stopAtFirstFdata=*/false, issues, error);
  if (r == WalkResult::Error)
    return false;
  if (r == WalkResult::StoppedAtNextFile)
  {
    // 多逻辑文件：已解部分保留（诚实面），调用方按 issues 知晓边界
  }

  header.wellName = w.wellName;
  header.sawAscii = w.sawAnyFdata;
  header.nullValue = paleo::wellnumeric::kLasDefaultNull; // DLIS 无全局 NULL：缺失值本身是 NaN/省略帧
  header.indexBasis = indexBasisFor(w);
  const QStringList names = catalogNames(w);
  header.curveNames = names;
  if (names.isEmpty())
  {
    if (error)
      *error = w.sawAnyFdata
                   ? QStringLiteral("无可用曲线（主帧通道全部为白名单子结构）")
                   : QStringLiteral("无帧数据（FDATA）——井曲线读面需要至少一帧");
    return false;
  }

  const FrameState fr = w.frames.value(w.firstDataFrameKey);
  for (const DlisParser::Obname &chRef : fr.channels)
  {
    const ChannelState ch = w.channels.value(obnameKey(chRef));
    qint64 elems = 1;
    if (!ch.dimension.isEmpty())
    {
      elems = 1;
      for (qint64 d : ch.dimension)
        elems *= qMax(d, qint64(0));
    }
    if (elems != 1 || !numericRep(ch.repc) || fixedRepSize(ch.repc) <= 0)
      continue; // 白名单通道：目录里没有（catalogNames 同口径）
    LasCurve curve;
    curve.name = chRef.id;
    curve.unit = ch.units;
    curve.descr = ch.descr.isEmpty() ? w.longNames.value(ch.longNameRef)
                                     : ch.descr;
    curve.values = w.values.value(obnameKey(chRef));
    curves.append(curve);
  }
  // 行对齐：以索引道为准（缺帧通道补 NaN，不截断别道——半帧诚实面）
  const int rows = curves.isEmpty() ? 0 : curves.first().values.size();
  for (LasCurve &c : curves)
  {
    if (c.values.size() != rows)
    {
      addIssue(issues, LasIssue::Severity::Warning, LasIssue::Category::Format,
               QStringLiteral("通道 %1 行数 %2 与索引道 %3 不齐——补 NaN")
                   .arg(c.name)
                   .arg(c.values.size())
                   .arg(rows));
      c.values.resize(rows, std::numeric_limits<double>::quiet_NaN());
    }
  }
  return true;
}

bool DlisParser::parseHeader(const QString &path, LasHeaderInfo &out,
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

  Walker w;
  const WalkResult r = walk(f, w, /*stopAtFirstFdata=*/true, issues, error);
  if (r == WalkResult::Error)
    return false;

  out.wellName = w.wellName;
  out.sawAscii = w.sawAnyFdata;
  out.nullValue = paleo::wellnumeric::kLasDefaultNull;
  out.indexBasis = indexBasisFor(w);
  out.curveNames = catalogNames(w);
  if (out.curveNames.isEmpty())
  {
    if (error)
      *error = QStringLiteral("头扫描未取得曲线目录（无 CHANNEL/FRAME 或全部为"
                              "白名单子结构）");
    return false;
  }
  return true;
}
