// 层：数据
#include "crossplotsources.h"
#include "fspathutils.h"
#include "domain/seismic/sgycoordinatemapper.h"
#include "domain/seismic/sgyvolume.h"
#include "io/lascache.h"
#include "io/sattrio.h"
#include "io/segyreader.h"
#include "welllogset.h"
#include <QDataStream>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <limits>
#include <ogr_srs_api.h>
namespace paleo::crossplot {
QVector<SourceSpec>
CrossplotSources::inventory(DataCatalog *cat, const QString &dir,
                            const QVector<LayerDeclaration> &decls) {
  QVector<SourceSpec> out;
  if (!cat || !cat->isOpen())
    return out;
  for (const auto &entity : cat->entities())
    if (entity.entityType == QLatin1String("well"))
      for (const auto &ref : WellLogSet::wellCurveIndex(cat, dir, entity.id)) {
        // 头解析失败的文件已由 WellLogSet 跳过。sha/managed 按源版本回查。
        const CatalogVersion version = cat->versionById(ref.sourceVersionId);
        SourceSpec s;
        s.choice.id = entity.id + QLatin1Char('|') + ref.sourceVersionId +
                      QLatin1Char('|') + ref.mnemonic;
        s.choice.title = entity.name + QStringLiteral(" · ") + ref.mnemonic +
                         QStringLiteral(" · ") + QFileInfo(ref.path).fileName();
        s.choice.kind = QStringLiteral("well");
        s.path = ref.path;
        s.curve = ref.mnemonic;
        s.versionId = ref.sourceVersionId;
        s.sha256 = version.sha256;
        s.managed = version.managed;
        s.well.wellId = entity.id;
        s.well.x = entity.surfaceX;
        s.well.y = entity.surfaceY;
        s.well.hasXY = entity.hasSurface;
        out << s;
      }
  for (const auto &asset : cat->assets()) {
    const auto version = cat->currentVersion(asset.id);
    const auto path = DataCatalog::resolvedVersionPath(dir, version);
    const auto suffix = QFileInfo(path).suffix().toLower();
    if (suffix != QLatin1String("tif") && suffix != QLatin1String("tiff") &&
        suffix != QLatin1String("sattr"))
      continue;
    SourceSpec s;
    s.choice = {version.id, asset.displayName,
                suffix == QLatin1String("sattr") ? QStringLiteral("attribute")
                                                 : QStringLiteral("raster")};
    s.path = path;
    s.versionId = version.id;
    s.timeHorizon = asset.type == QLatin1String("horizon");
    s.sha256 = version.sha256;
    s.managed = version.managed;
    for (const auto &d : decls)
      if (QFileInfo(d.source).absoluteFilePath() ==
          QFileInfo(path).absoluteFilePath())
        s.layerId = d.layerId;
    out << s;
  }
  return out;
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
  // 读回器收敛在 io/sattrio（与服务写端同一格式定义，防漂移）；协作取消
  // 语义保持：读值途中取消 → 如实报已取消。
  paleo::sattr::SattrSectionHeader header;
  QVector<float> values;
  const std::function<bool()> cancelled =
      ctl.cancelled ? std::function<bool()>([&ctl] { return !ctl.cancelled(); })
                    : std::function<bool()>();
  if (!paleo::sattr::readSattrSection(path, &header, &values, error, cancelled))
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
      if (hash != s.sha256)
        return fail(
            QStringLiteral("源文件与登记 SHA-256 不一致：%1").arg(s.path));
    }
  }
  if (specs[0].choice.kind == "well") {
    QVector<Channel> channels;
    for (const auto &s : specs) {
      if (s.choice.kind != "well" || s.well.wellId != specs[0].well.wellId)
        return fail(QStringLiteral("井曲线交会须选择同一口井的通道"));
      const auto doc = LasCache::shared().load(s.path);
      if (!doc.ok || doc.curves.isEmpty())
        return fail(doc.error);
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
                          doc.curves[0].values,
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
