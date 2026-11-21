// 层：数据
#include "crossplotsources.h"
#include "wellreadiness.h"
#include "fspathutils.h"
#include "domain/seismic/sgycoordinatemapper.h"
#include "domain/seismic/sgyvolume.h"
#include "io/lascache.h"
#include "io/welllogread.h"
#include "domain/wellnumeric.h"
#include "io/sattrio.h"
#include "io/segyreader.h"
#include "welllogset.h"
#include <QDataStream>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <limits>
#include <ogr_srs_api.h>
namespace paleo::crossplot {
CrossplotSources::CatalogSnapshot
CrossplotSources::snapshotCatalog(DataCatalog *cat, const QString &dir,
                                  const QVector<LayerDeclaration> &decls) {
  CatalogSnapshot snap;
  if (!cat || !cat->isOpen())
    return snap;
  snap.valid = true;
  snap.projectDir = dir;
  snap.catalogMutationSeq = cat->mutationSeq();
  snap.declarations = decls;
  // 快照只做 catalog 只读（实体/链接/版本/元数据）——well_log 表头与
  // 时深判定的文件 I/O 全部留给 buildInventory 的消费线程。
  const auto readinessInputs = paleo::snapshotReadinessInputs(cat);
  for (const auto &entity : cat->entities(QStringLiteral("well"))) {
    WellSnapshot well;
    well.id = entity.id;
    well.name = entity.name;
    well.surfaceX = entity.surfaceX;
    well.surfaceY = entity.surfaceY;
    well.hasSurface = entity.hasSurface;
    well.readiness = readinessInputs.value(entity.id);
    well.logs = WellLogSet::snapshotWellLogLinks(cat, dir, entity.id);
    snap.wells << well;
  }
  for (const auto &asset : cat->assets()) {
    const auto version = cat->currentVersion(asset.id);
    AssetSnapshot entry;
    entry.assetId = asset.id;
    entry.displayName = asset.displayName;
    entry.type = asset.type;
    entry.versionId = version.id;
    entry.path = DataCatalog::resolvedVersionPath(dir, version);
    entry.sha256 = version.sha256;
    entry.managed = version.managed;
    snap.assets << entry;
  }
  return snap;
}

CrossplotSources::InventoryOutcome
CrossplotSources::buildInventory(const CatalogSnapshot &snap,
                                 const cluster::Control &ctl) {
  InventoryOutcome out;
  if (!snap.valid)
    return out;
  // 就绪井口径（HY 验收 2026-11-01）：交会图井源只进「曲线+可用时深」双全
  // 的井（stamp 缓存，确认/导入后自愈）；数据管理面不受此过滤。
  QHash<QString, WellReadinessInput> readinessInputs;
  for (const auto &well : snap.wells)
    readinessInputs.insert(well.id, well.readiness);
  const auto readiness = paleo::computeReadiness(readinessInputs, snap.projectDir);
  // 全局单调进度（cluster::Control 契约）：先按就绪井数出总文件数，再逐井
  // 以「累计完成 / 全局总数」上报——避免逐井分母导致的进度回跳。
  qint64 filesPlannedTotal = 0;
  for (const auto &well : snap.wells)
    if (readiness.value(well.id).analysisReady())
      filesPlannedTotal += well.logs.size();
  int filesDone = 0;
  for (const auto &well : snap.wells) {
    if (ctl.cancelled && ctl.cancelled())
      return out;
    if (!readiness.value(well.id).analysisReady())
      continue;
    out.filesPlanned += well.logs.size();
    WellLogWarnings warnings;
    const int base = filesDone; // 本井开始前的全局累计
    const auto refs = WellLogSet::curveIndexFromLinks(
        well.logs, &warnings, ctl.cancelled,
        [&ctl, &filesDone, base, filesPlannedTotal](int done, int) {
          filesDone = base + done; // 全局累计单调（well 内 done 单调）
          if (ctl.progress && filesPlannedTotal > 0)
            ctl.progress(double(qMin<qint64>(filesDone, filesPlannedTotal)) /
                         double(filesPlannedTotal));
        },
        &out.failedFiles);
    filesDone = out.filesPlanned; // 井完成：对齐全局累计
    if (ctl.cancelled && ctl.cancelled())
      return out; // 取消批次不产出部分清单
    for (const auto &ref : refs) {
      // sha/managed 按源版本回查：ref 的 sourceVersionId 来自该井的链接
      // 快照，逐条找回同版本的登记字段（与 versionById 回查同语义）。
      QString sha256;
      bool managed = true;
      for (const auto &log : well.logs)
        if (log.versionId == ref.sourceVersionId) {
          sha256 = log.sha256;
          managed = log.managed;
          break;
        }
      SourceSpec s;
      s.choice.id = well.id + QLatin1Char('|') + ref.sourceVersionId +
                    QLatin1Char('|') + ref.mnemonic;
      s.choice.title = well.name + QStringLiteral(" · ") + ref.mnemonic +
                       QStringLiteral(" · ") + QFileInfo(ref.path).fileName();
      s.choice.kind = QStringLiteral("well");
      s.path = ref.path;
      s.curve = ref.mnemonic;
      s.versionId = ref.sourceVersionId;
      s.sha256 = sha256;
      s.managed = managed;
      s.well.wellId = well.id;
      s.well.x = well.surfaceX;
      s.well.y = well.surfaceY;
      s.well.hasXY = well.hasSurface;
      out.sources << s;
    }
  }
  out.filesFailed = out.failedFiles.size();
  for (const auto &asset : snap.assets) {
    if (ctl.cancelled && ctl.cancelled())
      return out;
    const auto suffix = QFileInfo(asset.path).suffix().toLower();
    if (suffix != QLatin1String("tif") && suffix != QLatin1String("tiff") &&
        suffix != QLatin1String("sattr"))
      continue;
    SourceSpec s;
    s.choice = {asset.versionId, asset.displayName,
                suffix == QLatin1String("sattr") ? QStringLiteral("attribute")
                                                 : QStringLiteral("raster")};
    s.path = asset.path;
    s.versionId = asset.versionId;
    s.timeHorizon = asset.type == QLatin1String("horizon");
    s.sha256 = asset.sha256;
    s.managed = asset.managed;
    for (const auto &d : snap.declarations)
      if (QFileInfo(d.source).absoluteFilePath() ==
          QFileInfo(asset.path).absoluteFilePath())
        s.layerId = d.layerId;
    out.sources << s;
  }
  return out;
}

QVector<SourceSpec>
CrossplotSources::inventory(DataCatalog *cat, const QString &dir,
                            const QVector<LayerDeclaration> &decls) {
  // 同步兼容口径（既有调用方/测试）：与后台路径共享同一条快照+构建管线。
  return buildInventory(snapshotCatalog(cat, dir, decls)).sources;
}
bool CrossplotSources::attributeSection(const QString &path,
                                        AttributeSection *out, QString *error,
                                        const cluster::Control &ctl) {
  auto fail = [&](const QString &e) {
    if (error)
      *error = e;
    return false;
  };
  if (!out)
    return fail(QStringLiteral("SATR 输出为空"));
  // 读回器收敛在 io/sattrio（与服务写端同一格式定义，防漂移）；取消谓词
  // 直传「已取消？」语义（B-04 后不再需要调用方双负补偿）。
  paleo::sattr::SattrSectionHeader header;
  QVector<float> values;
  if (!paleo::sattr::readSattrSection(path, &header, &values, error, ctl.cancelled))
    return false;
  const QString section = header.section;
  AttributeSection a;
  a.plane.name = header.attrId;
  a.plane.grid.rows = header.height;
  a.plane.grid.cols = header.width;
  a.plane.values.assign(values.begin(), values.end());
  const QString source = header.sourceSgyPath;
  if (source.isEmpty())
    return fail(QStringLiteral("SATR 缺源测网路径"));
  seismic::SgyVolume volume;
  std::string numericError;
  if (!volume.Load(
          paleo::toFsPath(source), numericError,
          [&](auto, auto) { return !(ctl.cancelled && ctl.cancelled()); }))
    return fail(QString::fromStdString(numericError));
  auto mapper = seismic::SgyCoordinateMapper::Fit(*volume.Index());
  if (!mapper.valid())
    return fail(QStringLiteral("源测网无法定位：%1")
                    .arg(QString::fromStdString(mapper.fit().rejectionReason)));
  a.geometryRmsResidual = mapper.fit().rmsResidual;
  a.geometryMaxResidual = mapper.fit().maxResidual;
  SegyReader reader;
  SegyOptions options;
  options.cancel = ctl.cancelled;
  if (!reader.open(source, error, &options))
    return false;
  a.stepMs = volume.SampleIntervalUs() / 1000.;
  a.startTimeMs = reader.geometry().startTimeMs;
  const auto &axis =
      section == "il" ? volume.XlineValues() : volume.InlineValues();
  if (axis.size() != std::size_t(header.width) ||
      volume.SampleCount() != header.height)
    return fail(QStringLiteral("SATR 与源测网尺寸不符"));
  const int line = header.sectionIndex;
  for (int value : axis) {
    double x = 0, y = 0;
    if (!mapper.MapInlineXline(section == "il" ? line : value,
                               section == "il" ? value : line, x, y))
      return fail(QStringLiteral("属性道坐标不可解"));
    a.traceXY << QPointF(x, y);
  }
  *out = std::move(a);
  return true;
}
SampleResult CrossplotSources::load(const QVector<SourceSpec> &specs,
                                    const cluster::Control &ctl) {
  auto fail = [](const QString &e, bool c = false) {
    SampleResult r;
    r.error = e;
    r.cancelled = c;
    return r;
  };
  if (specs.size() < 2)
    return fail(QStringLiteral("请选择至少两个通道"));
  QHash<QString, QString> verifiedHashes;
  for (const auto &s : specs) {
    if (ctl.cancelled && ctl.cancelled())
      return fail(QStringLiteral("已取消"), true);
    if (!s.managed && !s.sha256.isEmpty()) {
      QString error;
      const auto hash = verifiedHashes.contains(s.path)
                            ? verifiedHashes.value(s.path)
                            : DataCatalog::sha256FileHex(s.path, &error);
      verifiedHashes.insert(s.path, hash);
      // 读盘失败（hash 为空）不是「不一致」——如实报无法读取，别把合法的
      // 外链资产误判成被篡改；sha 口径大小写不敏感（留底大写十六进制同样
      // 合法，全仓 externalShaMatches 同口径）。
      if (hash.isEmpty())
        return fail(QStringLiteral("无法读取源文件：%1").arg(s.path));
      if (hash.compare(s.sha256, Qt::CaseInsensitive) != 0)
        return fail(
            QStringLiteral("源文件与登记 SHA-256 不一致：%1").arg(s.path));
    }
  }
  if (specs[0].choice.kind == "well") {
    QVector<Channel> channels;
    QHash<QString, QPair<LasHeaderInfo, LasDoc>> documents;
    for (const auto &s : specs) {
      if (s.choice.kind != "well" || s.well.wellId != specs[0].well.wellId)
        return fail(QStringLiteral("井曲线交会须选择同一口井的通道"));
      if (!documents.contains(s.path)) {
        LasHeaderInfo header;
        LasDoc parsed;
        if (WellLogRead::detect(s.path) == WellLogFormat::Las) {
          parsed = LasCache::shared().load(s.path);
          header.indexBasis = QStringLiteral("MD");
        } else {
          parsed.ok = WellLogRead::parse(s.path, header, parsed.curves, &parsed.error);
          parsed.curveNames = header.curveNames;
        }
        documents.insert(s.path, {header, parsed});
      }
      const auto &header = documents[s.path].first;
      const auto &doc = documents[s.path].second;
      if (!doc.ok || doc.curves.isEmpty())
        return fail(doc.error);
      const bool explicitAxis = WellLogRead::detect(s.path) != WellLogFormat::Las;
      const auto scale = paleo::wellnumeric::depthScaleToMetres(doc.curves.first().unit);
      if (explicitAxis && (header.indexBasis != QLatin1String("MD") || !scale))
        return fail(QCoreApplication::translate("CrossplotSources", "%1：交会需要已声明米或英尺单位的 MD 测深；当前索引为 %2、单位为 %3，请先转换。")
            .arg(QFileInfo(s.path).fileName(), header.indexBasis.isEmpty() ? QCoreApplication::translate("CrossplotSources", "未声明") : header.indexBasis,
                 doc.curves.first().unit));
      QVector<double> depths = doc.curves.first().values;
      // Known units share one metre axis. Legacy LAS without usable metadata
      // retains its former numeric-as-metres convention.
      if (scale) for (double &depth : depths) depth *= *scale;
      // 并集名可能是 GR、GR#列号、GR@基名；文件里的列名仍是原助记符。
      const LasCurve *curve = nullptr;
      for (const LasCurve &c : doc.curves)
        if (c.name == s.curve) {
          curve = &c;
          break;
        }
      if (!curve) {
        QString raw = s.curve;
        const int at = raw.indexOf(QLatin1Char('@'));
        if (at >= 0)
          raw.truncate(at);
        int column = -1;
        const int hash = raw.lastIndexOf(QLatin1Char('#'));
        if (hash > 0) {
          bool ok = false;
          const int col = raw.mid(hash + 1).toInt(&ok);
          if (ok && col > 0) {
            column = col;
            raw.truncate(hash);
          }
        }
        if (column >= 0 && column < doc.curves.size() &&
            doc.curves.at(column).name == raw)
          curve = &doc.curves.at(column);
        else
          for (const LasCurve &c : doc.curves)
            if (c.name == raw) {
              curve = &c;
              break;
            }
      }
      if (!curve)
        return fail(QStringLiteral("曲线已不存在"));
      channels << Channel{s.curve,
                          curve->unit,
                          s.well.wellId,
                          s.versionId,
                          depths,
                          curve->values,
                          s.well.x,
                          s.well.y,
                          s.well.hasXY};
    }
    return CrossplotSamples::well(channels, ctl);
  }
  QVector<RasterSource> rasters;
  QVector<SourceSpec> attrs;
  for (const auto &s : specs) {
    if (s.choice.kind == "raster")
      rasters << RasterSource{s.path, s.choice.title, s.versionId, s.layerId};
    else if (s.choice.kind == "attribute")
      attrs << s;
    else
      return fail(QStringLiteral("不能混合井深与地图像元通道"));
  }
  if (attrs.isEmpty())
    return CrossplotSamples::rasters(rasters, ctl);
  if (rasters.isEmpty())
    return fail(QStringLiteral("SATR 交会需同时选择时间层位栅格（毫秒）"));
  for (const auto &spec : specs)
    if (spec.choice.kind == "raster" && !spec.timeHorizon)
      return fail(
          QStringLiteral("SATR 必须与 catalog "
                         "的时间层位（毫秒）交会，不能把普通栅格值当时间"));
  QVector<Plane> horizons;
  for (const auto &src : rasters) {
    cluster::Control silent{ctl.cancelled, {}};
    auto r = CrossplotSamples::rasters({src, src}, silent);
    if (!r.ok)
      return r;
    Plane p{src.name, src.versionId, r.samples.grid,
            std::vector<double>(std::size_t(r.samples.grid.rows) *
                                    r.samples.grid.cols,
                                std::numeric_limits<double>::quiet_NaN())};
    for (std::size_t i = 0; i < r.samples.rows(); ++i)
      p.values[std::size_t(r.samples.locations[qsizetype(i)].pixel)] =
          r.samples.values[i * 2];
    horizons << std::move(p);
  }
  // #221：空 CRS 层位（有效仿射但无投影）无法校验与 SEG-Y 同属局部工程坐标系，
  // 这里仍放行（契约测试与外委数据常见），但样本元数据如实标「未校验」，且
  // attributeHorizon 对全拒样本报错而非报成功。
  const bool horizonCrsUnknown = horizons[0].grid.crs.isEmpty();
  if (!horizonCrsUnknown) {
    auto crs =
        OSRNewSpatialReference(horizons[0].grid.crs.toUtf8().constData());
    const bool local = crs && OSRIsLocal(crs);
    if (crs)
      OSRDestroySpatialReference(crs);
    if (!local)
      return fail(QStringLiteral(
          "SEG-Y 缺显式投影，仅允许同一局部工程坐标系的时间层位交会"));
  }
  QVector<AttributeSection> sections;
  for (const auto &s : attrs) {
    AttributeSection a;
    QString error;
    if (!attributeSection(s.path, &a, &error, ctl))
      return fail(error, ctl.cancelled && ctl.cancelled());
    a.plane.versionId = s.versionId;
    a.plane.grid.crs = horizons[0].grid.crs;
    sections << std::move(a);
  }
  auto result = CrossplotSamples::attributeHorizon(sections, horizons, ctl);
  if (result.ok && horizonCrsUnknown)
    result.samples.samplingMetadata.insert("horizonCrs", QStringLiteral("unknown_unverified"));
  for (const auto &s : rasters)
    if (!s.layerId.isEmpty())
      result.samples.sourceLayerIds << s.layerId;
  return result;
}
} // namespace paleo::crossplot
