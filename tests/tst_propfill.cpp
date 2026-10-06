// 层：测试壳
#include <QtTest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>

#include "../src/algorithms/stratgrid/propfill.h"
#include "../src/algorithms/stratgrid/stratgrid.h"

#include <cmath>
#include <limits>
#include <vector>

using namespace paleo::stratgrid;

namespace
{

// 空花括号同时匹配 FaultSegment 与 FaultTriangle 两个 fillIdw。
const std::vector<FaultSegment> kNoFaults;

ZoneGrid box(int ni, int nj, int nk, float top = 0.0f, float bot = 10.0f, double dx = 1.0,
             double dy = 1.0)
{
  SurfaceGrid a;
  SurfaceGrid b;
  a.cols = b.cols = ni;
  a.rows = b.rows = nj;
  a.dx = b.dx = dx;
  a.dy = b.dy = dy;
  a.z.assign(static_cast<std::size_t>(ni * nj), top);
  b.z.assign(static_cast<std::size_t>(ni * nj), bot);
  ZoneGrid grid;
  const bool ok = buildZoneGrid(a, b, nk, &grid, nullptr);
  Q_ASSERT(ok);
  return grid;
}

float at(const PropertyVolume &vol, int i, int j, int k)
{
  return vol.values[static_cast<std::size_t>(vol.grid.cellIndex(i, j, k))];
}

} // namespace

class TestPropFill : public QObject
{
  Q_OBJECT
private slots:
  void singleSeedFillsConstantField();
  void symmetricMidpointIsMean();
  void faultBlocksCrossTalk();
  void polylineVertexSplitsEdge();
  void collinearFaultBlocksOverlap();
  void endpointTouchDoesNotIsolate();
  void power2WeightsLock();
  void cancelLeavesOutputUntouched();
  void sectionProjectionAndBlobRoundTrip();
  void readBlobRejectsOverflowingAxes();
};

void TestPropFill::singleSeedFillsConstantField()
{
  const ZoneGrid grid = box(4, 3, 2);
  PropertyVolume vol;
  QString err;
  std::vector<double> progress;
  QVERIFY2(fillIdw(grid, {Seed{1, 1, 0, 4.0}}, kNoFaults, 2.0, &vol,
                   [&](double f) {
                     progress.push_back(f);
                     return true;
                   },
                   &err),
           qPrintable(err));
  QCOMPARE(vol.blockCount, 1);
  QCOMPARE(vol.unfilledLiveCells, 0);
  for (int k = 0; k < 2; ++k)
    for (int j = 0; j < 3; ++j)
      for (int i = 0; i < 4; ++i)
        QCOMPARE(at(vol, i, j, k), 4.0f);
  QVERIFY(!progress.empty());
  QVERIFY(progress.front() == 0.0);
  QCOMPARE(progress.back(), 1.0);
  for (std::size_t n = 1; n < progress.size(); ++n)
    QVERIFY(progress[n] + 1e-12 >= progress[n - 1]);

  // 没有种子：活单元保持 NaN，不补 0。
  PropertyVolume empty;
  QVERIFY(fillIdw(grid, {}, kNoFaults, 2.0, &empty, {}, &err));
  QVERIFY(std::isnan(at(empty, 0, 0, 0)));
  QCOMPARE(empty.filledCells, 0);
  QVERIFY(empty.unfilledLiveCells > 0);
}

void TestPropFill::symmetricMidpointIsMean()
{
  const ZoneGrid grid = box(5, 1, 1);
  PropertyVolume vol;
  QVERIFY(fillIdw(grid, {Seed{0, 0, 0, 2.0}, Seed{4, 0, 0, 8.0}}, kNoFaults, 2.0, &vol, {}, nullptr));
  QVERIFY(std::fabs(at(vol, 2, 0, 0) - 5.0f) < 1e-4f);
  QCOMPARE(at(vol, 0, 0, 0), 2.0f);
  QCOMPARE(at(vol, 4, 0, 0), 8.0f);

  // 垂向对称：层厚均匀时 k 中面两侧地图距离相等，中面仍是均值。
  const ZoneGrid tall = box(1, 1, 5);
  PropertyVolume vk;
  QVERIFY(fillIdw(tall, {Seed{0, 0, 0, 0.0}, Seed{0, 0, 4, 10.0}}, kNoFaults, 2.0, &vk, {}, nullptr));
  QVERIFY(std::fabs(at(vk, 0, 0, 2) - 5.0f) < 1e-4f);
}

void TestPropFill::faultBlocksCrossTalk()
{
  const ZoneGrid grid = box(6, 1, 1);
  // 柱心 x = i+0.5。i=2 与 i=3 的连线穿过 x=3。
  const FaultSegment wall{3.0, -1.0, 3.0, 2.0};
  const std::vector<Seed> seeds{Seed{0, 0, 0, 1.0}, Seed{5, 0, 0, 9.0}};

  PropertyVolume blocked;
  QVERIFY(fillIdw(grid, seeds, {wall}, 2.0, &blocked, {}, nullptr));
  QCOMPARE(blocked.blockCount, 2);
  for (int i = 0; i < 3; ++i)
    QCOMPARE(at(blocked, i, 0, 0), 1.0f);
  for (int i = 3; i < 6; ++i)
    QCOMPARE(at(blocked, i, 0, 0), 9.0f);

  PropertyVolume open;
  QVERIFY(fillIdw(grid, seeds, kNoFaults, 2.0, &open, {}, nullptr));
  QVERIFY(std::fabs(at(open, 2, 0, 0) - 1.0f) > 0.5f);

  // 断层另一侧没有种子 → 保持 NaN，不被对侧拉动。
  PropertyVolume oneSide;
  QVERIFY(fillIdw(grid, {Seed{0, 0, 0, 3.0}}, {wall}, 2.0, &oneSide, {}, nullptr));
  QCOMPARE(at(oneSide, 1, 0, 0), 3.0f);
  QVERIFY(std::isnan(at(oneSide, 4, 0, 0)));
}

void TestPropFill::polylineVertexSplitsEdge()
{
  // 柱心 (0.5,0.5)–(1.5,0.5)。折线在边内部 (1,0.5) 分开，两段都不是严格穿越。
  const ZoneGrid grid = box(2, 1, 1);
  const FaultSegment down{1.0, -2.0, 1.0, 0.5};
  const FaultSegment up{1.0, 0.5, 1.0, 3.0};
  PropertyVolume vol;
  QVERIFY(fillIdw(grid, {Seed{0, 0, 0, 4.0}}, {down, up}, 2.0, &vol, {}, nullptr));
  QCOMPARE(vol.blockCount, 2);
  QCOMPARE(at(vol, 0, 0, 0), 4.0f);
  QVERIFY(std::isnan(at(vol, 1, 0, 0)));
}

void TestPropFill::collinearFaultBlocksOverlap()
{
  // 柱心 x = 0.5、1.5、2.5。断层与 0.5–1.5、1.5–2.5 都有正长度重叠。
  const ZoneGrid grid = box(3, 1, 1);
  const FaultSegment fault{1.0, 0.5, 3.0, 0.5};
  PropertyVolume vol;
  QVERIFY(fillIdw(grid, {Seed{2, 0, 0, 8.0}}, {fault}, 2.0, &vol, {}, nullptr));
  QVERIFY(std::isnan(at(vol, 0, 0, 0)));
  QVERIFY(std::isnan(at(vol, 1, 0, 0)));
  QCOMPARE(at(vol, 2, 0, 0), 8.0f);
}

void TestPropFill::endpointTouchDoesNotIsolate()
{
  // 只接到柱心 (0.5,0.5) 后向左离开，不把该柱和右邻拆开。
  const ZoneGrid grid = box(3, 1, 1);
  const FaultSegment away{0.5, 0.5, -2.0, 0.5};
  PropertyVolume vol;
  QVERIFY(fillIdw(grid, {Seed{1, 0, 0, 6.0}}, {away}, 2.0, &vol, {}, nullptr));
  QCOMPARE(vol.blockCount, 1);
  QCOMPARE(at(vol, 0, 0, 0), 6.0f);
  QCOMPARE(at(vol, 1, 0, 0), 6.0f);

  // 横穿只点到端点，同样不阻断。
  const FaultSegment touch{0.5, 0.5, 0.5, -2.0};
  PropertyVolume crossed;
  QVERIFY(fillIdw(grid, {Seed{1, 0, 0, 6.0}}, {touch}, 2.0, &crossed, {}, nullptr));
  QCOMPARE(at(crossed, 0, 0, 0), 6.0f);

  // 零长度断层落在边内部也不阻断。
  const FaultSegment degenerate{1.0, 0.5, 1.0, 0.5};
  PropertyVolume zero;
  QVERIFY(fillIdw(grid, {Seed{1, 0, 0, 6.0}}, {degenerate}, 2.0, &zero, {}, nullptr));
  QCOMPARE(at(zero, 0, 0, 0), 6.0f);
}

void TestPropFill::power2WeightsLock()
{
  const ZoneGrid grid = box(6, 1, 1);
  PropertyVolume vol;
  QVERIFY(fillIdw(grid, {Seed{0, 0, 0, 1.0}, Seed{5, 0, 0, 9.0}}, kNoFaults, 2.0, &vol, {}, nullptr));
  // d=2 → w=1/4，d=3 → w=1/9。(1/4 + 1) / (1/4 + 1/9) = 45/13 ≈ 3.4615。
  QVERIFY(std::fabs(static_cast<double>(at(vol, 2, 0, 0)) - 3.4615) < 1e-3);
  QCOMPARE(at(vol, 0, 0, 0), 1.0f);
  QCOMPARE(at(vol, 5, 0, 0), 9.0f);
}

void TestPropFill::cancelLeavesOutputUntouched()
{
  const ZoneGrid grid = box(8, 6, 2);
  PropertyVolume vol;
  vol.filledCells = 123;
  QString err;
  QVERIFY(!fillIdw(
      grid, {Seed{0, 0, 0, 1.0}}, kNoFaults, 2.0, &vol, [](double) { return false; }, &err));
  QVERIFY(err.contains(QStringLiteral("已取消")));
  QCOMPARE(vol.filledCells, 123);
  QVERIFY(vol.values.empty());
}

void TestPropFill::sectionProjectionAndBlobRoundTrip()
{
  const ZoneGrid grid = box(3, 1, 2, 0.0f, 20.0f, 10.0, 10.0);
  PropertyVolume vol;
  QVERIFY(fillIdw(grid, {Seed{1, 0, 0, 4.0}}, kNoFaults, 2.0, &vol, {}, nullptr));

  SectionGeometry sec;
  sec.nTraces = 3;
  sec.nSamples = 4;
  sec.dz = 5.0;
  sec.z0 = 2.5; // 层心：k=0 在 z=5，k=1 在 z=15
  sec.traceX = {5.0, 15.0, 25.0};
  sec.traceY = {5.0, 5.0, 5.0};
  std::vector<float> image;
  float vmin = 0, vmax = 0;
  QVERIFY(projectToSection(vol, sec, &image, &vmin, &vmax, nullptr));
  QCOMPARE(static_cast<int>(image.size()), 12);
  // 采样 0、1 在第 0 层（z=2.5、7.5）；2、3 在第 1 层。
  QCOMPARE(image[0 * 3 + 1], 4.0f);
  QCOMPARE(image[2 * 3 + 1], 4.0f);
  QCOMPARE(vmin, 4.0f);

  SectionGeometry miss = sec;
  miss.z0 = 100.0;
  std::vector<float> outside;
  QVERIFY(projectToSection(vol, miss, &outside, nullptr, nullptr, nullptr));
  QVERIFY(std::isnan(outside[0]));

  QJsonObject prov;
  prov.insert(QStringLiteral("param_hash"), QStringLiteral("abc"));
  prov.insert(QStringLiteral("curve"), QStringLiteral("GR"));
  const QByteArray blob = writePropertyBlob(vol, prov);
  QVERIFY(!blob.isEmpty());
  const QByteArray again = writePropertyBlob(vol, prov);
  QCOMPARE(blob, again);

  PropertyVolume back;
  QJsonObject got;
  QString err;
  QVERIFY2(readPropertyBlob(blob, &back, &got, &err), qPrintable(err));
  QCOMPARE(back.grid.ni, 3);
  QCOMPARE(back.grid.nk, 2);
  QCOMPARE(at(back, 1, 0, 0), 4.0f);
  QCOMPARE(got.value(QStringLiteral("param_hash")).toString(), QStringLiteral("abc"));
  QVERIFY(back.grid.columnLive(0, 0));

  std::vector<float> slice;
  int w = 0, h = 0;
  QVERIFY(extractSlice(vol, 1, 0, &slice, &w, &h, &vmin, &vmax, &err));
  QCOMPARE(w, 3);
  QCOMPARE(h, 2);
  QCOMPARE(slice[0 * 3 + 1], 4.0f);
}

void TestPropFill::readBlobRejectsOverflowingAxes()
{
  // #219：损坏 .pprop 头声明的轴尺寸让 int 乘法回绕（65536×65536 → 0），need
  // 变小绕过长度闸；读侧必须与写侧同闸并以 qint64 计算后拒绝。
  const auto makeBlob = [](int ni, int nj, int nk) {
    QJsonObject obj;
    obj.insert(QStringLiteral("format"), QStringLiteral("paleo-property-volume"));
    obj.insert(QStringLiteral("version"), 1);
    obj.insert(QStringLiteral("ni"), ni);
    obj.insert(QStringLiteral("nj"), nj);
    obj.insert(QStringLiteral("nk"), nk);
    const QByteArray json = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    QByteArray blob("PPROP1\n", 7);
    char len[4];
    qToLittleEndian(static_cast<quint32>(json.size()), len);
    blob.append(len, 4);
    blob.append(json);
    blob.append(QByteArray(64, '\0')); // 少量尾随数据
    return blob;
  };
  PropertyVolume back;
  QString err;
  QVERIFY(!readPropertyBlob(makeBlob(65536, 65536, 1), &back, nullptr, &err));
  QVERIFY(!err.isEmpty());
  QVERIFY(back.values.empty());
  err.clear();
  QVERIFY(!readPropertyBlob(makeBlob(46341, 46341, 1), &back, nullptr, &err)); // n 回绕为负
  QVERIFY(!err.isEmpty());
  err.clear();
  QVERIFY(!readPropertyBlob(makeBlob((1 << 20) + 1, 1, 1), &back, nullptr, &err)); // 超轴上限
  QVERIFY(!err.isEmpty());
}

QTEST_GUILESS_MAIN(TestPropFill)
#include "tst_propfill.moc"
