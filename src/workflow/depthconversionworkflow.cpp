// 层：功能
#include "depthconversionworkflow.h"
#include "workflowerrors_internal.h"

#include "../catalog/datacatalog.h"
#include "../io/horizonbinner.h"
#include "../io/timedeptool.h"
#include "../io/wellfileparsers.h"
#include "../metadata/layermanifest.h"
#include "../qgis/qgislayerservice.h"
#include "../services/jobrunner.h"
#include "../services/paleotaskservice.h"
#include <QSaveFile>

#include <QDir>
#include <QFile>
#include <QHash>
#include <QSet>
#include <QJsonDocument>
#include <QJsonObject>

#include <gdal.h>
#include <cpl_string.h>

#include <cmath>
#include <limits>

namespace
{

using paleo::workflow_detail::setError;

// ---- 栅格读取（同厚度链 readDcGrid 口径：整幅 Float32 + geotransform）----------
struct DcGridSpec
{
  int cols = 0, rows = 0;
  double gt[6] = {0, 0, 0, 0, 0, 0};
  bool hasNodata = false;
  double nodata = 0.0;
  QVector<float> px;
  int inlineMin = 0, inlineMax = 0, xlineMin = 0, xlineMax = 0;
  bool hasInlineRange = false, hasXlineRange = false;
};

bool readDcGrid(const QString &path, DcGridSpec *g)
{
  GDALAllRegister();
  GDALDatasetH ds = GDALOpen(path.toUtf8().constData(), GA_ReadOnly);
  if (!ds)
    return false;
  g->cols = GDALGetRasterXSize(ds);
  g->rows = GDALGetRasterYSize(ds);
  GDALGetGeoTransform(ds, g->gt);
  if (GDALGetRasterCount(ds) < 1)
  {
    GDALClose(ds);
    return false;
  }
  GDALRasterBandH band = GDALGetRasterBand(ds, 1);
  if (!band)
  {
    GDALClose(ds);
    return false;
  }
  int flag = 0;
  g->nodata = GDALGetRasterNoDataValue(band, &flag);
  g->hasNodata = flag != 0;
  g->px.resize(g->cols * g->rows);
  const CPLErr err = GDALRasterIO(band, GF_Read, 0, 0, g->cols, g->rows,
                                   g->px.data(), g->cols, g->rows, GDT_Float32, 0, 0);
  // PALEO_INLINE_*/PALEO_XLINE_* 测网号域透传（验证→地震剖面导航依赖）。
  CSLConstList meta = GDALGetMetadata(ds, nullptr);
  const auto intMeta = [&meta](const char *key) -> int {
    const char *v = CSLFetchNameValue(meta, key);
    bool ok = false;
    const int r = v ? QByteArray(v).toInt(&ok) : 0;
    return ok ? r : 0;
  };
  const char *ilMin = CSLFetchNameValue(meta, "PALEO_INLINE_MIN");
  const char *xlMin = CSLFetchNameValue(meta, "PALEO_XLINE_MIN");
  if (ilMin && xlMin)
  {
    g->inlineMin = intMeta("PALEO_INLINE_MIN");
    g->inlineMax = intMeta("PALEO_INLINE_MAX");
    g->xlineMin = intMeta("PALEO_XLINE_MIN");
    g->xlineMax = intMeta("PALEO_XLINE_MAX");
    g->hasInlineRange = true;
    g->hasXlineRange = true;
  }
  GDALClose(ds);
  return err == CE_None;
}

// ---- 井控制数据收集 ----------------------------------------------------------

struct WellCoord
{
  double x = qQNaN(), y = qQNaN();
};

QHash<QString, WellCoord> readWellCoords(const QStringList &wellHeadPaths)
{
  QHash<QString, WellCoord> out;
  for (const QString &path : wellHeadPaths)
  {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
      continue;
    const QVector<WellHeadRecord> heads = parseWellHeadText(f.readAll());
    for (const WellHeadRecord &h : heads)
    {
      if (!out.contains(h.name))
        out.insert(h.name, WellCoord{h.x, h.y});
    }
  }
  return out;
}

// DC.dat 多井分层 → 逐井 knots（Time/TVD 列有效行；坐标取行 x/y 或井位表兜底）。
void collectTopsControls(const QStringList &paths, const QHash<QString, WellCoord> &coords,
                         QHash<QString, paleo::velmodel::VelocityWellControl> &byWell,
                         QStringList *notes)
{
  for (const QString &path : paths)
  {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
    {
      *notes << QStringLiteral("读不了分层文件 %1").arg(path);
      continue;
    }
    const QVector<WellTopRecord> tops = parseWellTopsText(f.readAll());
    for (const WellTopRecord &t : tops)
    {
      if (!t.hasTime || !t.hasTvd || !(t.timeMs > 0.0) || !(t.tvd > 0.0))
        continue; // 哨兵/缺列行：不入模，也不臆造
      paleo::velmodel::VelocityWellControl &c = byWell[t.wellName];
      c.wellId = t.wellName;
      if (!std::isfinite(c.x))
      {
        if (t.hasX && t.hasY)
          c.x = t.x, c.y = t.y;
        else if (coords.contains(t.wellName))
          c.x = coords.value(t.wellName).x, c.y = coords.value(t.wellName).y;
      }
      paleo::velmodel::VelocityKnot kn;
      kn.twtMs = t.timeMs;
      kn.depthM = t.tvd;
      kn.topName = t.topName;
      c.knots.append(kn);
    }
  }
}

// TD/*.dat 校验炮表 → knots（TVD 列优先、MD 兜底；井名取表头 # Well）。
void collectTdControls(const QStringList &paths, const QHash<QString, WellCoord> &coords,
                       QHash<QString, paleo::velmodel::VelocityWellControl> &byWell,
                       QStringList *notes)
{
  for (const QString &path : paths)
  {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
    {
      *notes << QStringLiteral("读不了时深文件 %1").arg(path);
      continue;
    }
    const TimeDepthTable td = parseTimeDepthText(f.readAll());
    const QString id = td.wellName;
    if (id.isEmpty())
    {
      *notes << QStringLiteral("时深文件无井名 %1").arg(path);
      continue;
    }
    paleo::velmodel::VelocityWellControl c;
    c.wellId = id;
    if (coords.contains(id))
      c.x = coords.value(id).x, c.y = coords.value(id).y;
    for (const TdRow &row : td.rows)
    {
      const double depth = row.hasTvd ? row.tvd : (row.hasMd ? row.md : qQNaN());
      if (!(row.timeMs > 0.0) || !(depth > 0.0))
        continue;
      paleo::velmodel::VelocityKnot kn;
      kn.twtMs = row.timeMs;
      kn.depthM = depth;
      c.knots.append(kn);
    }
    if (byWell.contains(id))
      *notes << QStringLiteral("井 %1 同时有分层与校验炮：取校验炮（实测标定）").arg(id);
    byWell.insert(id, c);
  }
}

} // namespace

namespace
{
// UIS-20：换算核心（纯计算 + 文件写，无 catalog / 图层）——同步路径与后台
// 任务 compute 段共用。成功时 extra 写换算统计，文件落 outPath。
bool convertGridToDepthFile(const paleo::velmodel::VelocityModel &model,
                            const QString &horizon, const QString &timeRasterPath,
                            const QString &outPath, QVariantMap *extra, QString *error)
{
  const auto fail = [error](const QString &why) {
    if (error)
      *error = why;
    return false;
  };
  DcGridSpec g;
  if (!readDcGrid(timeRasterPath, &g))
    return fail(QStringLiteral("读不了时间域栅格 %1").arg(timeRasterPath));
  if (!g.hasNodata)
    g.nodata = -9999.0; // 惯用哨兵：与 horizonbinner 写出口径一致

  const paleo::velmodel::DepthGridResult r = paleo::velmodel::convertTimeGridToDepth(
      model, g.px, g.rows, g.cols, g.gt, g.nodata);

  BinnedHorizon b;
  b.rows = g.rows;
  b.cols = g.cols;
  b.dx = g.gt[1];
  b.dy = -g.gt[5]; // 北向上：行方向 gt[5] 为负
  b.originX = g.gt[0];
  b.originY = g.gt[3];
  b.hasInlineRange = g.hasInlineRange;
  b.hasXlineRange = g.hasXlineRange;
  b.inlineMin = g.inlineMin;
  b.inlineMax = g.inlineMax;
  b.xlineMin = g.xlineMin;
  b.xlineMax = g.xlineMax;
  b.z.resize(g.rows * g.cols);
  const float kNoData = -9999.0f;
  double zMin = std::numeric_limits<double>::max();
  double zMax = -std::numeric_limits<double>::max();
  for (int i = 0; i < g.rows * g.cols; ++i)
  {
    if (std::isnan(r.depthM[i]))
    {
      b.z[i] = kNoData;
      continue;
    }
    b.z[i] = r.depthM[i];
    b.filledCells++;
    zMin = std::min(zMin, static_cast<double>(r.depthM[i]));
    zMax = std::max(zMax, static_cast<double>(r.depthM[i]));
  }
  if (b.filledCells == 0)
    return fail(QStringLiteral("层位 %1 没有任何像元落在模型覆盖内").arg(horizon));
  b.zMin = zMin;
  b.zMax = zMax;
  QString writeErr;
  if (!writeHorizonGeoTiff(b, outPath, &writeErr))
    return fail(QStringLiteral("深度栅格写入失败：%1").arg(writeErr));
  if (extra)
  {
    extra->insert(QStringLiteral("model_type"), paleo::velmodel::modelTypeId(model.type()));
    extra->insert(QStringLiteral("converted_cells"), r.convertedCells);
    extra->insert(QStringLiteral("nodata_cells"), r.nodataCells);
    extra->insert(QStringLiteral("outside_model_cells"), r.outsideModelCells);
    extra->insert(QStringLiteral("depth_min_m"), b.zMin);
    extra->insert(QStringLiteral("depth_max_m"), b.zMax);
  }
  return true;
}
} // namespace

DepthConversionWorkflow::DepthConversionWorkflow(DataCatalog *catalog, const QString &projectDir,
                                                 QObject *parent)
  : QObject(parent), m_catalog(catalog), m_projectDir(projectDir), m_registrar(catalog, projectDir)
{
}

void DepthConversionWorkflow::rebind(DataCatalog *catalog, const QString &projectDir)
{
  cancelDepth(); // 工程切换：在途换算作废（commit 侧另有 catalog/目录守卫）
  m_catalog = catalog;
  m_projectDir = projectDir;
  m_registrar = DerivedAssetRegistrar(catalog, projectDir);
}

paleo::velmodel::VelocityModel DepthConversionWorkflow::buildModel(
    const VelocityModelBuildRequest &req, QString *error, QStringList *notesOut)
{
  const QHash<QString, WellCoord> coords = readWellCoords(req.wellHeadFilePaths);
  QHash<QString, paleo::velmodel::VelocityWellControl> byWell;
  QStringList notes;
  collectTopsControls(req.topsFilePaths, coords, byWell, &notes);
  collectTdControls(req.tdFilePaths, coords, byWell, &notes);

  const QVector<paleo::velmodel::VelocityWellControl> controls = byWell.values();
  QString fitErr;
  const paleo::velmodel::VelocityModel model =
      paleo::velmodel::VelocityModel::fit(controls, req.type, &fitErr);
  if (!model.isValid())
  {
    setError(error, QStringLiteral("速度模型拟合失败：%1").arg(fitErr));
    return paleo::velmodel::VelocityModel();
  }
  notes += model.notes();
  if (notesOut)
    *notesOut = notes;
  return model;
}

QString DepthConversionWorkflow::buildAndStoreModel(const VelocityModelBuildRequest &req,
                                                    QString *error)
{
  if (!m_catalog || m_projectDir.isEmpty())
  {
    setError(error, QStringLiteral("时深编排未绑定 catalog/工程目录"));
    return QString();
  }

  QStringList notes;
  const paleo::velmodel::VelocityModel model = buildModel(req, error, &notes);
  if (!model.isValid())
    return QString();

  const QString displayName = req.type == paleo::velmodel::ModelType::V0kLinear
      ? tr("速度模型（V0-k 线性）")
      : tr("速度模型（层间平均）");
  QString stageErr;
  const DerivedStaging st = m_registrar.stage(
      QStringLiteral("velocity_model"), displayName,
      QStringLiteral("VELOCITY_MODEL.json"), &stageErr);
  if (!st.isValid())
  {
    setError(error, stageErr);
    return QString();
  }
  {
    QFile out(st.absolutePath);
    if (!out.open(QIODevice::WriteOnly))
    {
      setError(error, QStringLiteral("写不了模型文件 %1").arg(st.absolutePath));
      return QString();
    }
    out.write(QJsonDocument(model.toJson()).toJson(QJsonDocument::Indented));
  }

  QVariantMap extra;
  extra.insert(QStringLiteral("model_type"),
               paleo::velmodel::modelTypeId(req.type));
  extra.insert(QStringLiteral("wells"), model.wells().size());
  int knotCount = 0;
  for (const auto &w : model.wells())
    knotCount += w.knots.size();
  extra.insert(QStringLiteral("knots"), knotCount);
  if (!notes.isEmpty())
    extra.insert(QStringLiteral("notes"), notes.join(QLatin1Char('\n')));
  const QStringList parents = m_registrar.parentVersionIdsFor(
      req.topsFilePaths + req.tdFilePaths + req.wellHeadFilePaths);
  QString commitErr;
  if (!m_registrar.commit(st, parents, QStringLiteral("depthconversion/build-model"), extra, &commitErr))
  {
    setError(error, commitErr);
    return QString();
  }
  emit modelStored(st.absolutePath);
  return st.absolutePath;
}

bool DepthConversionWorkflow::convertRasterToDepth(const QString &horizon,
                                                   const QString &timeRasterPath,
                                                   const QString &modelJsonPath,
                                                   QString *layerId, QString *error)
{
  const auto fail = [this, &horizon, &error](const QString &why) {
    setError(error, why);
    emit conversionFailed(horizon, why);
    return false;
  };
  if (horizon.isEmpty())
    return fail(QStringLiteral("层位名为空"));
  if (!m_catalog)
    return fail(QStringLiteral("时深编排未绑定 catalog"));

  QString modelErr;
  const paleo::velmodel::VelocityModel model = loadModel(modelJsonPath, &modelErr);
  if (!model.isValid())
    return fail(QStringLiteral("速度模型不可用：%1").arg(modelErr));

  QString stageErr;
  const DerivedStaging st = m_registrar.stage(
      QStringLiteral("depth_raster"), tr("%1 深度域").arg(horizon),
      QStringLiteral("DEPTH_%1.tif").arg(horizon), &stageErr);
  if (!st.isValid())
    return fail(stageErr);
  QVariantMap extra;
  QString convertErr;
  if (!convertGridToDepthFile(model, horizon, timeRasterPath, st.absolutePath, &extra, &convertErr))
    return fail(convertErr);
  const QStringList parents = m_registrar.parentVersionIdsFor(
      QStringList{timeRasterPath, modelJsonPath});
  QString commitErr;
  if (!m_registrar.commit(st, parents, QStringLiteral("depthconversion/convert-horizon"),
                          extra, &commitErr))
    return fail(commitErr);

  const QString id = QStringLiteral("depth.%1").arg(horizon);
  if (m_layers)
  {
    LayerDeclaration decl;
    decl.layerId = id;
    decl.horizon = horizon;
    decl.type = QStringLiteral("raster");
    decl.source = st.absolutePath;
    decl.group = QStringLiteral("00_Data");
    decl.title = tr("%1 深度域（米，TVD）").arg(horizon);
    QString declErr;
    if (!m_layers->declare(decl, &declErr))
      return fail(QStringLiteral("无法声明深度图层：%1").arg(declErr));
  }
  if (layerId)
    *layerId = id;
  emit conversionDone(horizon, id);
  return true;
}

paleo::velmodel::VelocityModel DepthConversionWorkflow::loadModel(const QString &modelJsonPath,
                                                                  QString *error)
{
  QFile f(modelJsonPath);
  if (!f.open(QIODevice::ReadOnly))
  {
    setError(error, QStringLiteral("读不了模型文件 %1").arg(modelJsonPath));
    return paleo::velmodel::VelocityModel();
  }
  const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
  if (!doc.isObject())
  {
    setError(error, QStringLiteral("模型文件不是 JSON 对象：%1").arg(modelJsonPath));
    return paleo::velmodel::VelocityModel();
  }
  return paleo::velmodel::VelocityModel::fromJson(doc.object(), error);
}

QString DepthConversionWorkflow::latestModelPath(DataCatalog *catalog, const QString &projectDir)
{
  if (!catalog)
    return QString();
  // #127：不同模型类型是不同资产，versionNumber 各自从 1 起——跨资产比较会让
  // 早期资产的 v2 永远压过后建资产的 v1。改用 catalog 版本表的提交序（行序，
  // 增量落盘按行序写、重开稳定）：最后提交的 velocity_model 版本即「最新」。
  QSet<QString> modelAssets;
  for (const CatalogAsset &asset : catalog->assets())
    if (asset.type == QLatin1String("velocity_model"))
      modelAssets.insert(asset.id);
  if (modelAssets.isEmpty())
    return QString();
  const QVector<CatalogVersion> all = catalog->versions();
  for (auto it = all.crbegin(); it != all.crend(); ++it)
  {
    if (!modelAssets.contains(it->assetId))
      continue;
    const QString path = DataCatalog::resolvedVersionPath(projectDir, *it);
    if (!path.isEmpty())
      return path;
  }
  return QString();
}

QString DepthConversionWorkflow::latestModelPath() const
{
  return latestModelPath(m_catalog, m_projectDir);
}

VelocityModelBuildRequest DepthConversionWorkflow::requestFromCatalog() const
{
  return requestFromCatalog(m_catalog, m_projectDir);
}

VelocityModelBuildRequest DepthConversionWorkflow::requestFromCatalog(DataCatalog *catalog,
                                                                      const QString &projectDir)
{
  VelocityModelBuildRequest req;
  if (!catalog)
    return req;
  // 同一多井文件会挂多条井关联（role 相同、asset 相同）——按 "role/assetId"
  // 去重；每资产取最高版本号（TD 是逐井多资产，跨资产全收）。
  QHash<QString, QString> latestByAsset;
  for (const EntityAssetLink &link : catalog->links())
  {
    if (link.entityType != QLatin1String("well") || link.unresolved)
      continue;
    if (link.role != QLatin1String("tops") && link.role != QLatin1String("time_depth") &&
        link.role != QLatin1String("well_head"))
      continue;
    const QString key = link.role + QLatin1Char('/') + link.assetId;
    if (latestByAsset.contains(key))
      continue;
    int bestNum = -1;
    QString bestPath;
    for (const CatalogVersion &v : catalog->versionsForAsset(link.assetId))
    {
      if (v.versionNumber > bestNum)
      {
        bestNum = v.versionNumber;
        bestPath = DataCatalog::resolvedVersionPath(projectDir, v);
      }
    }
    if (bestNum >= 0)
      latestByAsset.insert(key, bestPath);
  }
  for (auto it = latestByAsset.cbegin(); it != latestByAsset.cend(); ++it)
  {
    const QString role = it.key().section(QLatin1Char('/'), 0, 0);
    if (role == QLatin1String("tops"))
      req.topsFilePaths << it.value();
    else if (role == QLatin1String("time_depth"))
      req.tdFilePaths << it.value();
    else if (role == QLatin1String("well_head"))
      req.wellHeadFilePaths << it.value();
  }
  return req;
}

// ---- facies-ux 6 / UIS-20：任务中心版 ---------------------------------------
DepthConversionWorkflow::~DepthConversionWorkflow() = default;

void DepthConversionWorkflow::setTaskService(PaleoTaskService *tasks)
{
  m_taskSvc = tasks;
}

bool DepthConversionWorkflow::depthBusy() const
{
  return m_depthRunner && m_depthRunner->busy();
}

void DepthConversionWorkflow::cancelDepth()
{
  if (m_depthRunner)
    m_depthRunner->requestCancel();
}

bool DepthConversionWorkflow::computeDepth(DepthJob &job, const std::function<bool()> &cancelled)
{
  const auto fail = [&job](const QString &why) {
    job.error = why;
    return false;
  };
  const auto stop = [&cancelled] { return cancelled && cancelled(); };
  paleo::velmodel::VelocityModel model;
  if (job.buildModel)
  {
    QStringList notes;
    QString err;
    model = buildModel(job.buildRequest, &err, &notes);
    if (!model.isValid())
      return fail(QStringLiteral("速度模型建立失败：%1").arg(err));
    QSaveFile out(job.modelStaging.absolutePath);
    const QByteArray bytes = QJsonDocument(model.toJson()).toJson(QJsonDocument::Indented);
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit())
      return fail(QStringLiteral("写不了模型文件 %1").arg(job.modelStaging.absolutePath));
    job.modelJsonPath = job.modelStaging.absolutePath;
    job.modelExtra.insert(QStringLiteral("model_type"), paleo::velmodel::modelTypeId(job.buildRequest.type));
    job.modelExtra.insert(QStringLiteral("wells"), model.wells().size());
    int knotCount = 0;
    for (const auto &w : model.wells())
      knotCount += w.knots.size();
    job.modelExtra.insert(QStringLiteral("knots"), knotCount);
    if (!notes.isEmpty())
      job.modelExtra.insert(QStringLiteral("notes"), notes.join(QLatin1Char('\n')));
  }
  else
  {
    QString modelErr;
    model = loadModel(job.modelJsonPath, &modelErr);
    if (!model.isValid())
      return fail(QStringLiteral("速度模型不可用：%1").arg(modelErr));
  }
  if (stop())
    return fail(QStringLiteral("已取消"));
  QString err;
  if (!convertGridToDepthFile(model, job.horizon, job.timeRasterPath, job.depthStaging.absolutePath,
                              &job.depthExtra, &err))
    return fail(err);
  return !stop() || fail(QStringLiteral("已取消"));
}

bool DepthConversionWorkflow::commitDepth(DepthJob &job, QString *error)
{
  bool modelCommitted = false;
  const auto fail = [this, &job, error, &modelCommitted](const QString &why) {
    // 未登记的受管落位文件不留游离（已登记的模型保留在册）。
    if (job.buildModel && !modelCommitted && job.modelStaging.isValid())
      QFile::remove(job.modelStaging.absolutePath);
    if (job.depthStaging.isValid())
      QFile::remove(job.depthStaging.absolutePath);
    setError(error, why);
    emit conversionFailed(job.horizon, why);
    return false;
  };
  if (!job.error.isEmpty())
    return fail(job.error);
  // 会话守卫：任务期间工程切换 / 重绑 → 旧结果不写进新工程。
  if (!m_catalog || m_catalog != job.catalog.data() || m_projectDir != job.projectDir)
    return fail(QStringLiteral("工程已切换，深度域换算结果已丢弃"));
  QString commitErr;
  if (job.buildModel)
  {
    const QStringList parents = m_registrar.parentVersionIdsFor(
        job.buildRequest.topsFilePaths + job.buildRequest.tdFilePaths +
        job.buildRequest.wellHeadFilePaths);
    if (!m_registrar.commit(job.modelStaging, parents, QStringLiteral("depthconversion/build-model"),
                            job.modelExtra, &commitErr))
      return fail(commitErr);
    modelCommitted = true;
    emit modelStored(job.modelStaging.absolutePath);
  }
  const QStringList parents = m_registrar.parentVersionIdsFor(
      QStringList{job.timeRasterPath, job.modelJsonPath});
  if (!m_registrar.commit(job.depthStaging, parents, QStringLiteral("depthconversion/convert-horizon"),
                          job.depthExtra, &commitErr))
    return fail(commitErr);
  const QString id = QStringLiteral("depth.%1").arg(job.horizon);
  if (m_layers)
  {
    LayerDeclaration decl;
    decl.layerId = id;
    decl.horizon = job.horizon;
    decl.type = QStringLiteral("raster");
    decl.source = job.depthStaging.absolutePath;
    decl.group = QStringLiteral("00_Data");
    decl.title = tr("%1 深度域（米，TVD）").arg(job.horizon);
    QString declErr;
    if (!m_layers->declare(decl, &declErr))
      return fail(QStringLiteral("无法声明深度图层：%1").arg(declErr));
  }
  emit conversionDone(job.horizon, id);
  return true;
}

bool DepthConversionWorkflow::startConvertRasterToDepth(const QString &horizon,
                                                        const QString &timeRasterPath,
                                                        const QString &modelJsonPath,
                                                        QString *error)
{
  if (!m_taskSvc)
  {
    // 无任务服务（测试壳 / 早期启动）：同步路径，语义一致。
    QString modelPath = modelJsonPath;
    if (modelPath.isEmpty())
    {
      modelPath = buildAndStoreModel(requestFromCatalog(), error);
      if (modelPath.isEmpty())
        return false;
    }
    return convertRasterToDepth(horizon, timeRasterPath, modelPath, nullptr, error);
  }
  using paleo::jobs::JobRunner;
  if (!m_depthRunner)
    m_depthRunner = std::make_unique<JobRunner<DepthJob>>(this);
  m_depthRunner->setTaskService(m_taskSvc);
  if (m_depthRunner->busy())
  {
    setError(error, tr("已有深度域换算任务在进行，请等待完成或在任务中心取消"));
    return false;
  }
  if (horizon.isEmpty())
  {
    setError(error, QStringLiteral("层位名为空"));
    return false;
  }
  if (!m_catalog || m_projectDir.isEmpty())
  {
    setError(error, QStringLiteral("时深编排未绑定 catalog/工程目录"));
    return false;
  }
  auto job = std::make_shared<DepthJob>();
  job->horizon = horizon;
  job->timeRasterPath = timeRasterPath;
  job->modelJsonPath = modelJsonPath;
  job->catalog = m_catalog;
  job->projectDir = m_projectDir;
  QString stageErr;
  if (modelJsonPath.isEmpty())
  {
    job->buildModel = true;
    job->buildRequest = requestFromCatalog();
    if (job->buildRequest.topsFilePaths.isEmpty() && job->buildRequest.tdFilePaths.isEmpty())
    {
      setError(error, tr("没有可用的井分层/校验炮数据——先导入再转换"));
      return false;
    }
    job->modelStaging = m_registrar.stage(QStringLiteral("velocity_model"), tr("速度模型（层间平均）"),
                                          QStringLiteral("VELOCITY_MODEL.json"), &stageErr);
    if (!job->modelStaging.isValid())
    {
      setError(error, stageErr);
      return false;
    }
  }
  job->depthStaging = m_registrar.stage(QStringLiteral("depth_raster"), tr("%1 深度域").arg(horizon),
                                        QStringLiteral("DEPTH_%1.tif").arg(horizon), &stageErr);
  if (!job->depthStaging.isValid())
  {
    setError(error, stageErr);
    return false;
  }
  JobRunner<DepthJob>::Callbacks cb;
  cb.compute = [](DepthJob &j, const paleo::jobs::CancelFn &cancel,
                  const paleo::jobs::ProgressFn &progress) {
    if (progress)
      progress(5.0, QStringLiteral("build"));
    const bool ok = computeDepth(j, cancel);
    if (progress)
      progress(100.0, QStringLiteral("publish"));
    return ok;
  };
  cb.commit = [this](DepthJob &j, QString *err) { return commitDepth(j, err); };
  cb.onDropped = [this](DepthJob &j, paleo::jobs::DropReason reason) {
    if (reason != paleo::jobs::DropReason::PrepareFailed)
      emit conversionFailed(j.horizon, paleo::jobs::dropReasonText(reason));
  };
  cb.cleanup = [](DepthJob &j) {
    // 未登记的受管落位文件：失败 / 取消时清掉（登记成功后文件已只读入册）。
    for (const DerivedStaging *st : {&j.modelStaging, &j.depthStaging})
      if (st->isValid())
        QFile::remove(st->absolutePath);
  };
  if (!m_depthRunner->start(tr("深度域换算 · %1").arg(horizon), job, cb, QString(),
                            /*quiet=*/false))
  {
    setError(error, tr("深度域换算任务未能启动"));
    return false;
  }
  return true;
}
