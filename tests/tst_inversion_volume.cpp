#include <QtTest>
#include <QJsonObject>
#include <cmath>
#include <vector>

#include "../src/algorithms/inversion/volume.h"

using namespace paleo::inversion;

class TestInversionVolume : public QObject
{
  Q_OBJECT

private slots:
  void roundTripValidVolume();
  void magicMismatchFails();
  void truncatedBlobFails();
  void dimensionMismatchFails();
  void nanValuesPreserved();
  void mutationDemonstration_exactFloatEquality();
};

void TestInversionVolume::roundTripValidVolume()
{
  const int nIl = 2;
  const int nXl = 3;
  const int nS = 4;
  const int total = nIl * nXl * nS;

  std::vector<float> vol(total);
  for (int i = 0; i < total; ++i)
  {
    vol[i] = static_cast<float>(i * 100.5f);
  }

  QJsonObject hdr;
  hdr.insert(QStringLiteral("nIl"), nIl);
  hdr.insert(QStringLiteral("nXl"), nXl);
  hdr.insert(QStringLiteral("nS"), nS);
  hdr.insert(QStringLiteral("t0Ms"), 50.0);
  hdr.insert(QStringLiteral("dtMs"), 2.0);

  const QByteArray blob = writeImpedanceVolumeBlob(vol, hdr);
  QVERIFY(!blob.isEmpty());
  QVERIFY(blob.startsWith("IIMP1\n"));

  std::vector<float> readVol;
  QJsonObject readHdr;
  QString err;
  const bool ok = readImpedanceVolumeBlob(blob, &readVol, &readHdr, &err);
  QVERIFY2(ok, qPrintable(err));

  QCOMPARE(readVol.size(), vol.size());
  for (size_t i = 0; i < vol.size(); ++i)
  {
    QCOMPARE(readVol[i], vol[i]);
  }
  QCOMPARE(readHdr.value(QStringLiteral("nIl")).toInt(), nIl);
  QCOMPARE(readHdr.value(QStringLiteral("t0Ms")).toDouble(), 50.0);
}

void TestInversionVolume::magicMismatchFails()
{
  std::vector<float> vol = {1.0f, 2.0f};
  QJsonObject hdr;
  hdr.insert(QStringLiteral("nIl"), 1);
  hdr.insert(QStringLiteral("nXl"), 1);
  hdr.insert(QStringLiteral("nS"), 2);

  QByteArray blob = writeImpedanceVolumeBlob(vol, hdr);
  blob[0] = 'X'; // 破坏魔数

  std::vector<float> outVol;
  QJsonObject outHdr;
  QString err;
  const bool ok = readImpedanceVolumeBlob(blob, &outVol, &outHdr, &err);
  QVERIFY(!ok);
  QVERIFY(err.contains(QStringLiteral("IIMP1")) || err.contains(QStringLiteral("魔数")) || err.contains(QStringLiteral("magic")));
}

void TestInversionVolume::truncatedBlobFails()
{
  std::vector<float> vol = {1.0f, 2.0f, 3.0f, 4.0f};
  QJsonObject hdr;
  hdr.insert(QStringLiteral("nIl"), 1);
  hdr.insert(QStringLiteral("nXl"), 1);
  hdr.insert(QStringLiteral("nS"), 4);

  const QByteArray blob = writeImpedanceVolumeBlob(vol, hdr);
  const QByteArray truncated = blob.left(blob.size() - 4); // 截断 4 个字节

  std::vector<float> outVol;
  QJsonObject outHdr;
  QString err;
  const bool ok = readImpedanceVolumeBlob(truncated, &outVol, &outHdr, &err);
  QVERIFY(!ok);
  QVERIFY(!err.isEmpty());
}

void TestInversionVolume::dimensionMismatchFails()
{
  std::vector<float> vol = {1.0f, 2.0f, 3.0f, 4.0f};
  QJsonObject hdr;
  hdr.insert(QStringLiteral("nIl"), 2);
  hdr.insert(QStringLiteral("nXl"), 2);
  hdr.insert(QStringLiteral("nS"), 2); // 声明 8 个样点，但实际数据只有 4 个

  const QByteArray blob = writeImpedanceVolumeBlob(vol, hdr);

  std::vector<float> outVol;
  QJsonObject outHdr;
  QString err;
  const bool ok = readImpedanceVolumeBlob(blob, &outVol, &outHdr, &err);
  QVERIFY(!ok);
  QVERIFY(err.contains(QStringLiteral("不匹配")) || err.contains(QStringLiteral("字节")) || err.contains(QStringLiteral("尺寸")));
}

void TestInversionVolume::nanValuesPreserved()
{
  std::vector<float> vol = {1.0f, std::numeric_limits<float>::quiet_NaN(), 3.5f};
  QJsonObject hdr;
  hdr.insert(QStringLiteral("nIl"), 1);
  hdr.insert(QStringLiteral("nXl"), 1);
  hdr.insert(QStringLiteral("nS"), 3);

  const QByteArray blob = writeImpedanceVolumeBlob(vol, hdr);

  std::vector<float> outVol;
  QJsonObject outHdr;
  QString err;
  QVERIFY(readImpedanceVolumeBlob(blob, &outVol, &outHdr, &err));

  QCOMPARE(outVol.size(), 3);
  QCOMPARE(outVol[0], 1.0f);
  QVERIFY(std::isnan(outVol[1]));
  QCOMPARE(outVol[2], 3.5f);
}

void TestInversionVolume::mutationDemonstration_exactFloatEquality()
{
  // 变异测试示范：写出并读回的浮点数组必须与输入逐元素严格恒等
  std::vector<float> vol = {3.14159f, -0.00123f, 42.0f, -999.0f};
  QJsonObject hdr;
  hdr.insert(QStringLiteral("nIl"), 1);
  hdr.insert(QStringLiteral("nXl"), 1);
  hdr.insert(QStringLiteral("nS"), 4);

  const QByteArray blob = writeImpedanceVolumeBlob(vol, hdr);
  std::vector<float> outVol;
  QVERIFY(readImpedanceVolumeBlob(blob, &outVol, nullptr, nullptr));

  for (size_t i = 0; i < vol.size(); ++i)
  {
    QCOMPARE(outVol[i], vol[i]);
  }
}

QTEST_GUILESS_MAIN(TestInversionVolume)
#include "tst_inversion_volume.moc"
