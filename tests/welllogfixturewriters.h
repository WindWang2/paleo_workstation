// 层：测试壳
// 方向 44 测试共用：DLIS (RP66 v1) / LIS79 二进制夹具写入器。
// 夹具生成源即 Oracle 证据——tst_dlisparser / tst_lisparser /
// tst_welllog_multiformat 共用同一份逐字节写入原语，保证「读回值与
// 生成源一致」的断言全部对同一真源负责。
#pragma once
#include <QString>
#include <QByteArray>
#include <QList>
#include <QStringList>
#include <QVector>

#include <cmath>
#include <cstring>

namespace dlisfix
{
  // ---- RP66 v1 写入原语（全大端） ----
  QByteArray u8b(quint64 v)
  {
    return QByteArray(1, char(uchar(v)));
  }
  QByteArray u16be(quint64 v)
  {
    QByteArray b;
    b.append(char(uchar(v >> 8)));
    b.append(char(uchar(v)));
    return b;
  }
  QByteArray u32be(quint64 v)
  {
    QByteArray b;
    b.append(char(uchar(v >> 24)));
    b.append(char(uchar(v >> 16)));
    b.append(char(uchar(v >> 8)));
    b.append(char(uchar(v)));
    return b;
  }
  QByteArray uvariBytes(quint64 v)
  {
    if (v < 0x40)
      return u8b(v);
    if (v < 0x4000)
      return QByteArray(1, char(0x40 | (v >> 8))) + u8b(v & 0xFF);
    return QByteArray(1, char(0x80 | (v >> 24))) + u8b((v >> 16) & 0xFF) +
           u8b((v >> 8) & 0xFF) + u8b(v & 0xFF);
  }
  QByteArray identBytes(const QString &s)
  {
    const QByteArray latin = s.toLatin1();
    return u8b(uchar(latin.size())) + latin;
  }
  QByteArray obnameBytes(qint64 origin, int copy, const QString &id)
  {
    return uvariBytes(quint64(origin)) + u8b(quint64(copy)) + identBytes(id);
  }

  // ---- 数值表示码编码器（与解析器解码互逆；双射值均为精确二进分数） ----
  QByteArray fsinglBytes(float v)
  {
    quint32 bits;
    std::memcpy(&bits, &v, 4);
    return u32be(bits);
  }
  QByteArray fdoublBytes(double v)
  {
    quint64 bits;
    std::memcpy(&bits, &v, 8);
    QByteArray b = u32be(bits >> 32) + u32be(bits & 0xFFFFFFFFu);
    return b;
  }
  QByteArray ibmBytes(double v)
  {
    const bool sign = v < 0;
    double a = std::fabs(v);
    int exp = 64;
    if (a != 0.0)
    {
      while (a >= 1.0)
      {
        a /= 16.0;
        ++exp;
      }
      while (a < 1.0 / 16.0)
      {
        a *= 16.0;
        --exp;
      }
    }
    const quint32 mant = quint32(a * 16777216.0 + 0.5);
    const quint32 bits = (sign ? 0x80000000u : 0u) | (quint32(exp) << 24) | mant;
    return u32be(bits);
  }
  QByteArray vaxBytes(double v)
  {
    // VAX F：逻辑位型 IEEE 式（隐藏位），磁盘字序交换：[b1 b0 b3 b2]
    const bool sign = v < 0;
    double a = std::fabs(v);
    int exp = 128;
    if (a != 0.0)
    {
      while (a >= 1.0)
      {
        a /= 2.0;
        ++exp;
      }
      while (a < 0.5)
      {
        a *= 2.0;
        --exp;
      }
    }
    const quint32 frac = quint32(a * 16777216.0 + 0.5); // 0.5..1 → 0x800000..0x1000000
    const quint32 bits = (sign ? 0x80000000u : 0u) | (quint32(exp) << 23) |
                         (frac & 0x007FFFFFu);
    const uchar b0 = uchar((bits >> 16) & 0xFF);
    const uchar b1 = uchar((bits >> 24) & 0xFF);
    const uchar b2 = uchar(bits & 0xFF);
    const uchar b3 = uchar((bits >> 8) & 0xFF);
    QByteArray b;
    b.append(char(b0));
    b.append(char(b1));
    b.append(char(b2));
    b.append(char(b3));
    return b;
  }
  QByteArray fshortBytes(double v)
  {
    const bool sign = v < 0;
    double a = std::fabs(v);
    int exp = 0;
    if (a != 0.0)
    {
      while (a >= 1.0)
      {
        a /= 2.0;
        ++exp;
      }
    }
    quint16 frac = quint16(a * 2048.0 + 0.5);
    if (sign)
      frac = quint16(((~frac) & 0x0FFF) + 1);
    const quint16 w = (sign ? 0x8000 : 0) | quint16(frac << 4) | quint16(exp);
    return u16be(w);
  }

  // ---- EFLR 组件写入 ----
  constexpr int RepObname = 23;
  constexpr int RepUshort = 15;
  constexpr int RepUnits = 27;
  constexpr int RepUvari = 18;
  constexpr int RepIdent = 19;
  constexpr int RepAscii = 20;

  QByteArray setComponent(const QString &type, const QString &name)
  {
    QByteArray b;
    b.append(char(0xE0 | 0x10 | (name.isEmpty() ? 0 : 0x08)));
    b.append(identBytes(type));
    if (!name.isEmpty())
      b.append(obnameBytes(0, 0, name));
    return b;
  }
  QByteArray objectComponent(qint64 origin, int copy, const QString &id)
  {
    return QByteArray(1, char(0x60 | 0x10)) + obnameBytes(origin, copy, id);
  }
  QByteArray tmplAttr(const QString &label, int repc)
  {
    QByteArray b;
    b.append(char(0x20 | 0x10 | 0x04)); // label + repc
    b.append(identBytes(label));
    b.append(u8b(quint64(repc)));
    return b;
  }
  QByteArray valueAttr(const QByteArray &value)
  {
    return QByteArray(1, char(0x20 | 0x01)) + value; // 仅值（列特征走模板默认）
  }
  QByteArray countValueAttr(int count, const QByteArray &value)
  {
    return QByteArray(1, char(0x20 | 0x08 | 0x01)) + uvariBytes(count) + value;
  }
  QByteArray absentAttr()
  {
    return QByteArray(1, char(0x00));
  }

  // ---- 逻辑记录 / 可见记录封装 ----
  // 单 LRS 字节（attrs 由调用方给全：分段时置前驱/后继位）
  QByteArray logicalRecordSegment(uchar type, quint8 attrs, const QByteArray &body)
  {
    QByteArray lrs;
    lrs.append(u16be(4 + quint64(body.size())));
    lrs.append(char(attrs));
    lrs.append(char(type));
    lrs.append(body);
    return lrs;
  }
  // 带段尾（pad/checksum/trailing-length）的 LRS：attrs 置相应位，trailer 自尾剥
  // trailer 字节序（最尾起）：[padcount 1B][pad][traillen 2B][checksum 2B]
  QByteArray logicalRecordSegmentTrailer(uchar type, quint8 attrs,
                                         const QByteArray &body, int padCount,
                                         bool withTrailLen, bool withChecksum)
  {
    QByteArray trailer;
    trailer.append(char(uchar(padCount)));
    trailer.append(QByteArray(padCount, '\0'));
    const int trailerFixed = (withTrailLen ? 2 : 0) + (withChecksum ? 2 : 0);
    if (withTrailLen)
      trailer += u16be(quint16(4 + body.size() + trailer.size() + trailerFixed));
    if (withChecksum)
      trailer += u16be(0x1234); // 值不校验——解析只按位剥长度
    return logicalRecordSegment(type, attrs, body + trailer);
  }
  // 一个 VR 装整数个 LRS（RP66 §2.2.3：多 LR 打包）
  QByteArray visibleRecord(const QList<QByteArray> &segments)
  {
    QByteArray payload;
    for (const QByteArray &s : segments)
      payload += s;
    QByteArray vr;
    vr.append(u16be(4 + quint64(payload.size())));
    vr.append(char(0xFF));
    vr.append(char(0x01));
    vr.append(payload);
    return vr;
  }
  QByteArray wrapLogicalRecord(uchar type, bool explicitFmt, const QByteArray &body)
  {
    return visibleRecord({ logicalRecordSegment(type, explicitFmt ? 0x80 : 0x00, body) });
  }
  // 一个 LR 的体拆成多段跨多个 VR：除末段外带后继位，除首段外带前驱位
  QList<QByteArray> splitLogicalRecordAcrossVrs(uchar type, bool explicitFmt,
                                                const QByteArray &body, int chunk)
  {
    QList<QByteArray> vrs;
    const quint8 base = explicitFmt ? 0x80 : 0x00;
    const int n = qMax(body.size(), 1);
    const int segs = (n + chunk - 1) / chunk;
    for (int seg = 0; seg < segs; ++seg)
    {
      quint8 attrs = base;
      if (seg > 0)
        attrs |= 0x40; // 前驱
      if (seg < segs - 1)
        attrs |= 0x20; // 后继
      vrs.append(visibleRecord({ logicalRecordSegment(type, attrs, body.mid(seg * chunk, chunk)) }));
    }
    return vrs;
  }
  QByteArray sul()
  {
    QByteArray b = QByteArray(80, ' ');
    b.replace(0, 4, "0001");
    b.replace(4, 5, "V1.00");
    b.replace(9, 6, "RECORD");
    b.replace(15, 5, " 8192");
    b.replace(20, 10, "SMOKE-DLIS");
    return b;
  }

  // 一条曲线定义（name/units/repc/dimension；dimension 空 = 标量）
  struct ChannelDef
  {
    QString id;
    QString units;
    int repc = 2; // FSINGL
    QVector<int> dimension;
    QString longNameId;
  };
  QByteArray reprcValue(int repc, const QByteArray &encoded)
  {
    Q_UNUSED(repc);
    return encoded;
  }

  QByteArray channelEflrBody(const QVector<ChannelDef> &channels)
  {
    QByteArray body = setComponent(QStringLiteral("CHANNEL"), QString());
    body += tmplAttr(QStringLiteral("LONG-NAME"), RepObname);
    body += tmplAttr(QStringLiteral("REPRESENTATION-CODE"), RepUshort);
    body += tmplAttr(QStringLiteral("UNITS"), RepUnits);
    body += tmplAttr(QStringLiteral("DIMENSION"), RepUvari);
    for (const ChannelDef &ch : channels)
    {
      body += objectComponent(0, 0, ch.id);
      if (!ch.longNameId.isEmpty())
        body += valueAttr(obnameBytes(0, 0, ch.longNameId));
      else
        body += absentAttr();
      body += valueAttr(u8b(quint64(ch.repc)));
      if (!ch.units.isEmpty())
        body += valueAttr(identBytes(ch.units));
      else
        body += absentAttr();
      if (!ch.dimension.isEmpty())
      {
        QByteArray vals;
        for (int d : ch.dimension)
          vals += uvariBytes(quint64(d));
        body += countValueAttr(ch.dimension.size(), vals);
      }
      // 标量无 DIMENSION 列（缺尾列 = 模板默认）
    }
    return body;
  }
  QByteArray buildChannelEflr(const QVector<ChannelDef> &channels)
  {
    return wrapLogicalRecord(3, true, channelEflrBody(channels));
  }

  struct FrameDef
  {
    QString id;
    QStringList channels;
    QString indexType;
    QString direction;
  };
  QByteArray buildFrameEflr(const QVector<FrameDef> &frames)
  {
    QByteArray body = setComponent(QStringLiteral("FRAME"), QString());
    body += tmplAttr(QStringLiteral("CHANNELS"), RepObname);
    body += tmplAttr(QStringLiteral("INDEX-TYPE"), RepIdent);
    body += tmplAttr(QStringLiteral("DIRECTION"), RepIdent);
    body += tmplAttr(QStringLiteral("ENCRYPTED"), RepUshort);
    for (const FrameDef &fr : frames)
    {
      body += objectComponent(0, 0, fr.id);
      QByteArray chans;
      for (const QString &c : fr.channels)
        chans += obnameBytes(0, 0, c);
      body += countValueAttr(fr.channels.size(), chans);
      if (!fr.indexType.isEmpty())
        body += valueAttr(identBytes(fr.indexType));
      else
        body += absentAttr();
      if (!fr.direction.isEmpty())
        body += valueAttr(identBytes(fr.direction));
      else
        body += absentAttr();
      body += valueAttr(u8b(0));
    }
    return wrapLogicalRecord(4, true, body);
  }

  QByteArray buildOriginEflr(const QString &wellName)
  {
    QByteArray body = setComponent(QStringLiteral("ORIGIN"), QString());
    body += tmplAttr(QStringLiteral("WELL-NAME"), RepAscii);
    body += objectComponent(0, 0, QStringLiteral("1"));
    body += valueAttr(identBytes(wellName));
    return wrapLogicalRecord(1, true, body);
  }
  QByteArray buildFileHeaderEflr(const QString &id)
  {
    QByteArray body = setComponent(QStringLiteral("FILE-HEADER"), QString());
    body += tmplAttr(QStringLiteral("ID"), RepAscii);
    body += objectComponent(0, 0, QStringLiteral("n"));
    body += valueAttr(identBytes(id));
    return wrapLogicalRecord(0, true, body);
  }
  QByteArray buildLongNameEflr(const QString &id, const QString &text)
  {
    QByteArray body = setComponent(QStringLiteral("LONG-NAME"), QString());
    body += tmplAttr(QStringLiteral("DESCRIPTION"), RepAscii);
    body += objectComponent(0, 0, id);
    body += valueAttr(identBytes(text));
    return wrapLogicalRecord(5, true, body);
  }
  QByteArray fdataBody(const QString &frameId, quint64 frameNo,
                       const QByteArray &slotBytes)
  {
    return obnameBytes(0, 0, frameId) + uvariBytes(frameNo) + slotBytes;
  }
  QByteArray buildFdata(const QString &frameId, quint64 frameNo,
                        const QByteArray &slotBytes)
  {
    return wrapLogicalRecord(0, false, fdataBody(frameId, frameNo, slotBytes));
  }

} // namespace dlisfix

namespace lisfix
{
  QByteArray u8v(quint64 v) { return QByteArray(1, char(uchar(v))); }
  QByteArray u16be(quint64 v)
  {
    QByteArray b;
    b.append(char(uchar(v >> 8)));
    b.append(char(uchar(v)));
    return b;
  }
  QByteArray u32be(quint64 v)
  {
    QByteArray b;
    b.append(char(uchar(v >> 24)));
    b.append(char(uchar(v >> 16)));
    b.append(char(uchar(v >> 8)));
    b.append(char(uchar(v)));
    return b;
  }
  QByteArray leU32(quint32 v)
  {
    QByteArray b;
    b.append(char(uchar(v)));
    b.append(char(uchar(v >> 8)));
    b.append(char(uchar(v >> 16)));
    b.append(char(uchar(v >> 24)));
    return b;
  }
  QString pad4(const QString &s)
  {
    QString t = s.left(4);
    while (t.size() < 4)
      t += QLatin1Char(' ');
    return t;
  }
  QByteArray field(const QString &s, int n)
  {
    QByteArray b(n, ' ');
    b.replace(0, qMin<int>(n, s.toLatin1().size()), s.toLatin1());
    return b;
  }

  // LIS f32 编码（正负二进分数精确；负数尾数 2 补码、符号位独立）
  quint32 encLisF32(double v)
  {
    const bool sign = v < 0;
    double a = std::fabs(v);
    int exp = 128;
    if (a != 0.0)
    {
      while (a >= 1.0)
      {
        a /= 2.0;
        ++exp;
      }
      while (a < 0.5)
      {
        a *= 2.0;
        --exp;
      }
    }
    quint32 m = quint32(a * 8388608.0 + 0.5); // a∈[0.5,1) → m∈[0x400000,0x800000]
    if (sign)
      m = ((~m) & 0x7FFFFF) + 1;
    return (sign ? 0x80000000u : 0u) | (quint32(exp) << 23) | (m & 0x7FFFFFu);
  }
  QByteArray f32Bytes(double v) { return u32be(encLisF32(v)); }

  // ---- 逻辑记录 / 物理记录 ----
  // 单 PR 一段（后继/前驱位清）；attrs 可加校验位测 trailer 剥离
  QByteArray physicalRecord(uchar lrType, const QByteArray &payload,
                            quint16 extraAttrs = 0)
  {
    const QByteArray body = u8v(lrType) + u8v(0) + payload;
    quint16 attrs = extraAttrs;
    int trailer = 0;
    if (extraAttrs & 0x0400)
      trailer += 2;
    if (extraAttrs & 0x0200)
      trailer += 2;
    if (extraAttrs & 0x3000)
      trailer += 2;
    QByteArray pr = u16be(4 + body.size() + trailer) + u16be(attrs) + body;
    if (extraAttrs & 0x0400)
      pr += u16be(7); // 文件号
    if (extraAttrs & 0x0200)
      pr += u16be(1); // 记录号
    if (extraAttrs & 0x3000)
      pr += u16be(0); // 校验和
    return pr;
  }
  // 多 PR 拼一段 LR：中间段带后继/前驱位
  QByteArray splitPhysicalRecord(uchar lrType, const QByteArray &payload,
                                 int chunk)
  {
    QByteArray out;
    const int n = payload.size();
    int off = 0;
    int seg = 0;
    const int segs = (n + chunk - 1) / chunk;
    while (off < n)
    {
      const QByteArray part = payload.mid(off, chunk);
      const QByteArray body = (seg == 0 ? u8v(lrType) + u8v(0) : QByteArray()) + part;
      quint16 attrs = 0;
      if (seg > 0)
        attrs |= 0x0002; // 前驱
      if (seg < segs - 1)
        attrs |= 0x0001; // 后继
      out += u16be(4 + body.size()) + u16be(attrs) + body;
      off += chunk;
      ++seg;
    }
    return out;
  }

  // ---- 记录体构造 ----
  QByteArray fileHeaderPayload(const QString &name)
  {
    QByteArray p;
    p += field(name, 10);
    p += field(QString(), 2);
    p += field(QStringLiteral("SMOKE"), 6);
    p += field(QStringLiteral("1.0"), 8);
    p += field(QStringLiteral("20261004"), 8);
    p += field(QString(), 1);
    p += field(QStringLiteral("8192"), 5);
    p += field(QStringLiteral("LL"), 2);
    p += field(QString(), 2);
    p += field(QString(), 10);
    p += field(QString(), 4); // 对齐到 56
    return p;
  }
  QByteArray wellsitePayload(const QString &wellName)
  {
    // 分量块：[type 69][reprc 65][size][category 0][WELL ][    ][值]
    const QByteArray name = wellName.toLatin1();
    QByteArray c;
    c += u8v(69);
    c += u8v(65);
    c += u8v(quint64(name.size()));
    c += u8v(0);
    c += field(QStringLiteral("WELL"), 4);
    c += field(QString(), 4);
    c += name;
    return c;
  }
  struct LisSpec
  {
    QString mnemonic;
    QString units;
    int reprc = 68; // f32
    int samples = 1;
    qint16 reserved = 0; // 0 = 按表示码宽
    bool tvd = false;
  };
  struct LisDfsr
  {
    QList<LisSpec> specs;
    int depthMode = 0;
    int depthReprc = 68;
    QString depthUnits = QStringLiteral("M");
    double spacing = 1.0;
    int direction = 255; // DOWN
    int absentReprc = 0; // 非零 = 写条目 12（缺席值，数值）
    double absentValue = -999.0;
  };
  QByteArray dfsrPayload(const LisDfsr &d)
  {
    QByteArray p;
    auto entry = [&p](int type, int size, int reprc, const QByteArray &value) {
      p += u8v(type) + u8v(size) + u8v(reprc) + value;
    };
    bool subtype1 = false;
    for (const LisSpec &sp : d.specs)
      if (sp.tvd)
        subtype1 = true;
    if (subtype1)
      entry(16, 4, 73, u32be(1)); // Spec Block 子类型 1
    if (d.depthMode == 1)
    {
      entry(4, 4, 73, u32be(quint64(d.direction))); // UP/DOWN
      entry(8, 4, 68, f32Bytes(d.spacing));         // 帧距（f32：非整数帧距）
      entry(13, 4, 73, u32be(1));                   // 深度记录模式 1
      entry(14, 2, 65, field(d.depthUnits, 2));     // 深度单位
      entry(15, 0, d.depthReprc, QByteArray());     // 输出深度表示码
    }
    else if (d.absentReprc)
    {
      entry(12, 2, d.absentReprc, u16be(quint64(d.absentValue))); // 缺席值
    }
    entry(0, 0, 0, QByteArray()); // 终止条目
    for (const LisSpec &sp : d.specs)
    {
      QByteArray sb;
      sb += field(sp.mnemonic, 4);
      sb += field(QStringLiteral("SVC"), 6);
      sb += field(QStringLiteral("ORD"), 8);
      sb += field(sp.units, 4);
      sb += QByteArray(4, '\0');   // 子类型相关
      sb += u16be(0);              // filenr
      sb += u16be(quint16(sp.reserved));
      sb += QByteArray(2, '\0');   // 填充
      sb += u8v(0);                // 子类型相关
      sb += u8v(sp.samples);
      sb += u8v(sp.reprc);
      QByteArray tail(5, '\0');
      if (sp.tvd)
        tail[0] = char(0x20);      // 过程指示器 TVD 位
      sb += tail;
      p += sb;
    }
    return p;
  }

  // ---- TIF 封装（磁带标头链：小端 type/prev/next） ----
  QByteArray wrapTif(const QList<QByteArray> &prs)
  {
    QByteArray out;
    quint32 prev = 0;
    for (int i = 0; i < prs.size(); ++i)
    {
      const quint32 next = quint32(out.size() + 12 + prs.at(i).size());
      out += leU32(0) + leU32(prev) + leU32(next) + prs.at(i);
      prev = quint32(out.size());
    }
    // 尾带标（type 1）
    out += leU32(1) + leU32(prev) + leU32(quint32(out.size() + 12));
    return out;
  }

  // 标准主夹具：井名 + DEPT(f32)/GR(i16) 模式 0，三帧
  struct MainFixture
  {
    QByteArray raw; // 裸 PR 流
    QByteArray tif; // TIF 封装
  };
  MainFixture buildMain()
  {
    MainFixture fx;
    QByteArray b;
    b += physicalRecord(128, fileHeaderPayload(QStringLiteral("SMOKE-LIS")));
    b += physicalRecord(34, wellsitePayload(QStringLiteral("HZ28-6-2")));
    LisDfsr d;
    LisSpec dept;
    dept.mnemonic = QStringLiteral("DEPT");
    dept.units = QStringLiteral("M");
    dept.reprc = 68;
    LisSpec gr;
    gr.mnemonic = QStringLiteral("GR");
    gr.units = QStringLiteral("GAPI");
    gr.reprc = 79; // i16
    gr.reserved = 2;
    d.absentReprc = 79; // 缺席值 -999（i16 可表示）
    d.absentValue = -999;
    d.specs = { dept, gr };
    b += physicalRecord(64, dfsrPayload(d));
    QByteArray frames;
    const double depths[3] = { 1500.0, 1500.25, 1500.5 };
    const qint16 grs[3] = { 90, -35, -999 /*缺席*/ };
    for (int i = 0; i < 3; ++i)
    {
      frames += f32Bytes(depths[i]);
      frames += u16be(quint16(grs[i]));
    }
    b += physicalRecord(0, frames);
    fx.raw = b;
    fx.tif = wrapTif({ b });
    return fx;
  }

} // namespace lisfix
