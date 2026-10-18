#pragma once
#include <QByteArray>
#include <QSaveFile>
#include <QString>
#include <QtEndian>
#include <cstring>

namespace SeismicHorizonFixture
{
// 4 inline × 4 xline 方言合成体；dt=4ms、ns=50、t0=0。
// 第 (il,xl) 道在样本 22..28（=TWT 100ms ±12ms 窗）写入组签名：
//   xl 偶 → 恒 +3（低 RMS）；xl 奇 → ±30 交替（高 RMS/高过零率）。
// 其余样本恒 1.0。
inline QByteArray buildVolume()
{
  const int perLine = 4, lines = 4, ns = 50;
  QByteArray out(3200, ' ');
  QByteArray binHdr(400, 0);
  qToBigEndian<qint16>(static_cast<qint16>(4000), reinterpret_cast<uchar *>(binHdr.data() + 16)); // dt µs
  qToBigEndian<qint16>(static_cast<qint16>(ns), reinterpret_cast<uchar *>(binHdr.data() + 20));
  qToBigEndian<qint16>(qint16(5), reinterpret_cast<uchar *>(binHdr.data() + 24)); // IEEE f32
  qToBigEndian<qint16>(qint16(0x0100), reinterpret_cast<uchar *>(binHdr.data() + 300)); // rev1
  qToBigEndian<qint16>(qint16(0), reinterpret_cast<uchar *>(binHdr.data() + 304)); // 无扩展头
  out.append(binHdr);
  for (int i = 0; i < perLine * lines; ++i)
  {
    const int il = 100 + i / perLine, xl = 2000 + i % perLine;
    QByteArray trHdr(240, 0);
    qToBigEndian<qint32>(qint32(i + 1), reinterpret_cast<uchar *>(trHdr.data() + 0));
    qToBigEndian<qint32>(qint32(xl), reinterpret_cast<uchar *>(trHdr.data() + 20)); // CDP=方言 xline
    qToBigEndian<qint16>(static_cast<qint16>(ns), reinterpret_cast<uchar *>(trHdr.data() + 114));
    qToBigEndian<qint16>(static_cast<qint16>(4000), reinterpret_cast<uchar *>(trHdr.data() + 116));
    qToBigEndian<qint32>(qint32(10 * (xl - 2000)), reinterpret_cast<uchar *>(trHdr.data() + 180)); // CDP X
    qToBigEndian<qint32>(qint32(20 * (il - 100)), reinterpret_cast<uchar *>(trHdr.data() + 184)); // CDP Y
    qToBigEndian<qint32>(qint32(il), reinterpret_cast<uchar *>(trHdr.data() + 188)); // inline
    out.append(trHdr);
    for (int s = 0; s < ns; ++s)
    {
      float value = 1.0f;
      if (s >= 22 && s <= 28)
        value = (xl % 2 == 0) ? 3.0f : ((s % 2 == 0) ? 30.0f : -30.0f);
      quint32 raw;
      std::memcpy(&raw, &value, sizeof(float));
      const quint32 be = qToBigEndian<quint32>(raw);
      out.append(reinterpret_cast<const char *>(&be), sizeof(be));
    }
  }
  return out;
}

// SMI 散点层位：4×4 全网格，TWT 全部 timeMs（毫秒）。P1/P2/P3 与地震
// 测网角点一致（x=10·(xl-2000)、y=20·(il-100)，corner 校验 ≤1m）。
inline QByteArray buildHorizon(double timeMs)
{
  QByteArray text;
  text += "# Grid_size:4x4# Survey(Inline,Crossline,x,y)\n";
  text += "# P1:      100,      2000,     0.00000,     0.00000\n";
  text += "# P2:      100,      2003,    30.00000,     0.00000\n";
  text += "# P3:      103,      2003,    30.00000,    60.00000\n";
  text += "# Z_units: ms\n";
  for (int il = 100; il <= 103; ++il)
    for (int xl = 2000; xl <= 2003; ++xl)
      text += QStringLiteral("%1 %2 %3 %4 %5\n")
                  .arg(10.0 * (xl - 2000), 0, 'f', 5)
                  .arg(20.0 * (il - 100), 0, 'f', 5)
                  .arg(timeMs, 0, 'f', 1)
                  .arg(il)
                  .arg(xl)
                  .toUtf8();
  return text;
}

inline bool writeFile(const QString &path, const QByteArray &bytes)
{
  QSaveFile out(path);
  out.setDirectWriteFallback(false);
  if (!out.open(QIODevice::WriteOnly))
    return false;
  out.write(bytes);
  return out.commit();
}
}
