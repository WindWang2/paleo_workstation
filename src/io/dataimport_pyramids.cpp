// 层：数据
// 方向57：从 dataimportservice.cpp 按格式族析出。公共 API（dataimportservice.h）
// 零改动；跨族共享辅助（setError/readFileOrEmpty/FamilyContext）经
// dataimport_internal.h；importOneFile 分支体族函数亦声明于该头。
#include "dataimportservice.h"

#include "lascache.h"
#include "rasterpyramid.h"
#include "segyindexstore.h"
#include "shacache.h"

#include "../catalog/datacatalog.h"
#include "../metadata/layermanifest.h"
#include "../metadata/paleoprojectstore.h"
#include "../domain/arearules.h"
#include "horizonbinner.h"
#include "ingestplan.h"
#include "welllogread.h" // 方向44：井名提取分派
#include "../domain/projectclassifier.h"
#include "segyreader.h"
#include "wellcompositexml.h"
#include "wellfileparsers.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QThread>
#include <QUuid>

#include <cpl_conv.h>
#include <cpl_error.h>
#include <gdal.h>
#include <QStandardPaths>
#include <QUrl>

#include "../metadata/atomicfile.h"

#include <algorithm>
#include <cstdio>
#include "dataimport_internal.h"

using paleo::dataimport_detail::setError;
using paleo::dataimport_detail::readFileOrEmpty;

// ---- 栅格金字塔/概览族（B3 wave/deepen-perf：Lazy 瓦片 + GDAL .ovr）。


int DataImportService::ensureRasterPyramids(
    const QStringList &absPaths, QString *error,
    const std::function<bool(int, int, const QString &)> &progress)
{
  if (absPaths.isEmpty())
    return 0;
  const QString root = pyramidCacheDir();
  if (root.isEmpty())
  {
    if (error)
      *error = QStringLiteral("no project dir — pyramid cache root unavailable");
    return 0;
  }
  RasterPyramidService pyramids(root);
  int ok = 0;
  for (int i = 0; i < absPaths.size(); ++i)
  {
    const QString &path = absPaths.at(i);
    if (progress && !progress(i, absPaths.size(), path))
      break; // 协作取消：已 ensure 的保留
    QString err;
    RasterPyramidService::PyramidMeta meta;
    if (pyramids.ensure(path, &meta, &err,
                        RasterPyramidService::BuildStrategy::Lazy))
      ++ok;
    else
      qWarning("DataImportService: pyramid ensure failed for %s: %s",
               qPrintable(path), qPrintable(err));
  }
  if (progress)
    progress(absPaths.size(), absPaths.size(), QString());
  return ok;
}

// 单文件 GDAL 外部概览。C 回调桥（GDALProgressFunc：返回 0 = 用户终止）。
namespace
{
struct GdalOverviewCtx
{
  std::function<bool(double)> progress;
};

int gdalOverviewProgress(double pct, const char *, void *userData)
{
  auto *ctx = static_cast<GdalOverviewCtx *>(userData);
  if (!ctx || !ctx->progress)
    return 1;
  return ctx->progress(pct) ? 1 : 0;
}
} // namespace

bool DataImportService::buildRasterOverviews(
    const QString &absPath, QString *error,
    const std::function<bool(double)> &buildProgress)
{
  if (absPath.isEmpty())
  {
    if (error)
      *error = QStringLiteral("empty raster path");
    return false;
  }
  // 外部 .ovr 边车：受管 RAW 字节不动（SHA 留底与复验契约保持有效）。
  // GDAL 3.x 语义：概览落内/外由打开模式决定——UPDATE 打开写内部概览，
  // 只读打开自动落 <file>.ovr 边车（源字节零改动）。
  GDALDatasetH ds = GDALOpenEx(absPath.toUtf8().constData(),
                               GDAL_OF_RASTER, nullptr, nullptr, nullptr);
  if (!ds)
  {
    if (error)
      *error = QStringLiteral("cannot open raster: %1").arg(absPath);
    return false;
  }
  const int w = GDALGetRasterXSize(ds), h = GDALGetRasterYSize(ds);
  // 层级 2,4,8,… 至最小维 <512px（再细的概览对画布渲染无增益）；上限 8 级。
  QList<int> levels;
  for (int lv = 2; qMin(w, h) / lv >= 512 && levels.size() < 8; lv *= 2)
    levels.append(lv);
  if (levels.isEmpty())
  {
    GDALClose(ds);
    return true; // 小图无概览可建——成功语义（幂等）
  }
  GdalOverviewCtx ctx{buildProgress};
  const CPLErr err =
      GDALBuildOverviews(ds, "NEAREST", levels.size(), levels.constData(), 0,
                         nullptr, &gdalOverviewProgress, &ctx);
  GDALFlushCache(ds);
  GDALClose(ds);
  if (err != CE_None)
  {
    if (error)
      *error = QStringLiteral("GDALBuildOverviews failed: %1")
                   .arg(QString::fromUtf8(CPLGetLastErrorMsg()));
    return false;
  }
  return true;
}

int DataImportService::buildRasterOverviews(
    const QStringList &absPaths, QString *error,
    const std::function<bool(int, int, const QString &)> &progress)
{
  if (absPaths.isEmpty())
    return 0;
  int built = 0;
  for (int i = 0; i < absPaths.size(); ++i)
  {
    if (progress && !progress(i, absPaths.size(), absPaths.at(i)))
      break; // 协作取消
    QString err;
    if (buildRasterOverviews(absPaths.at(i), &err))
      ++built;
    else
      qWarning("DataImportService: overview build failed for %s: %s",
               qPrintable(absPaths.at(i)), qPrintable(err));
  }
  if (progress)
    progress(absPaths.size(), absPaths.size(), QString());
  if (error && built < absPaths.size())
    *error = QStringLiteral("%1/%2 built").arg(built).arg(absPaths.size());
  return built;
}
