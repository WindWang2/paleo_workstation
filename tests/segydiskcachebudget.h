#pragma once

#include "io/perffixtures.h"
#include "io/segyreader.h"
#include "perfbudget_relax.h"
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <array>

namespace paleo::test
{
// Measure the first disk reopen for each fresh index. A median of five pairs
// keeps a single Windows file-open delay from deciding the regression gate.
inline void verifySegyDiskCacheBudget(const QTemporaryDir &dir, double gate)
{
  const QString sgy = dir.filePath("gate.sgy");
  constexpr int traces = 200 * 200;
  QVERIFY(PerfFixtures::makeSyntheticSegy(sgy, 200, 200, 240) == traces);
  std::array<double, 5> ratios{};
  for (size_t i = 0; i < ratios.size(); ++i)
  {
    const QString idx = dir.filePath(QStringLiteral("segy-%1").arg(i));
    SegyReader cold, hit;
    QString err;
    QElapsedTimer t;
    t.start();
    QVERIFY2(cold.openCached(sgy, idx, &err), qPrintable(err));
    const auto buildNs = t.nsecsElapsed();
    QVERIFY(buildNs > 0);
    int progressCalls = 0;
    SegyOptions options;
    options.progress = [&](qint64, qint64) { ++progressCalls; };
    t.restart();
    QVERIFY2(hit.openCached(sgy, idx, &err, &options), qPrintable(err));
    const auto hitNs = t.nsecsElapsed();
    ratios[i] = double(hitNs) / buildNs;
    QVERIFY2(hitNs / 1.0e6 < paleo::perfbudget::relaxedBudgetMs(500), "Index cache hit exceeded 500 ms");
    QCOMPARE(progressCalls, 1); // an index hit reports completion without scanning
    QCOMPARE(hit.traceCount(), traces);
    QCOMPARE(hit.inlineNumbers(), cold.inlineNumbers());
    QCOMPARE(hit.crosslineNumbers(), cold.crosslineNumbers());
  }
  std::sort(ratios.begin(), ratios.end());
  const double ratio = ratios[ratios.size() / 2];
  qInfo("SEG-Y first disk hit/rebuild median %.4f, gate %.4f", ratio, gate);
  QVERIFY2(ratio <= gate,
           qPrintable(QStringLiteral("hit/rebuild=%1 > 门限 %2").arg(ratio, 0, 'f', 4).arg(gate, 0, 'f', 4)));
}
} // namespace paleo::test
