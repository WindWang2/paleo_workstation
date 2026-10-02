// 层：数据
#include "crossplotsources.h"
#include "domain/seismic/sgycoordinatemapper.h"
#include "domain/seismic/sgyvolume.h"
#include "io/lascache.h"
#include "io/lasparser.h"
#include "io/segyreader.h"
#include <QDataStream>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
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
      for (const auto &link : cat->linksForEntity(entity.id))
        if (!link.unresolved && link.role == QLatin1String("well_log")) {
          const auto version = cat->currentVersion(link.assetId);
          const auto path = DataCatalog::resolvedVersionPath(dir, version);
          LasHeaderInfo header;
          if (!LasParser::parseHeader(path, header))
            continue;
          for (int i = 1; i < header.curveNames.size(); ++i) {
            SourceSpec s;
            s.choice.id = version.id + QLatin1Char('|') + header.curveNames[i];
            s.choice.title = entity.name + QStringLiteral(" · ") +
                             header.curveNames[i] + QStringLiteral(" · ") +
                             QFileInfo(path).fileName();
            s.choice.kind = QStringLiteral("well");
            s.path = path;
            s.curve = header.curveNames[i];
            s.versionId = version.id;
            s.sha256 = version.sha256;
            s.managed = version.managed;
            s.well.wellId = entity.id;
            s.well.x = entity.surfaceX;
            s.well.y = entity.surfaceY;
            s.well.hasXY = entity.hasSurface;
            out << s;
          }
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
  QFile file(path);
  if (!out || !file.open(QIODevice::ReadOnly))
    return fail(QStringLiteral("SATR 文件无法打开"));
  QDataStream stream(&file);
  stream.setByteOrder(QDataStream::LittleEndian);
  stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
  char magic[4];
  quint32 version = 0, jsonSize = 0;
  qint32 width = 0, height = 0;
  if (stream.readRawData(magic, 4) != 4 || QByteArray(magic, 4) != "SATR")
    return fail(QStringLiteral("SATR 魔数无效"));
  stream >> version >> width >> height >> jsonSize;
  if (version != 1 || width <= 0 || height <= 0 ||
      qint64(width) * height > std::numeric_limits<int>::max() ||
      jsonSize > 1024 * 1024 ||
      file.size() != 20 + qint64(jsonSize) + qint64(width) * height * 4)
    return fail(QStringLiteral("SATR 版本或载荷大小无效"));
  QByteArray json(int(jsonSize), Qt::Uninitialized);
  if (stream.readRawData(json.data(), json.size()) != json.size())
    return fail(QStringLiteral("SATR 头截断"));
  QJsonParseError parseError;
  const auto header = QJsonDocument::fromJson(json, &parseError).object();
  if (parseError.error != QJsonParseError::NoError)
    return fail(QStringLiteral("SATR JSON 无效"));
  const auto section = header.value("section").toString();
  if (section != "il" && section != "xl")
    return fail(QStringLiteral("SATR 剖面方向不支持"));
  AttributeSection a;
  a.plane.name = header.value("attrId").toString();
  a.plane.grid.rows = height;
  a.plane.grid.cols = width;
  a.plane.values.resize(std::size_t(width) * height);
  for (auto &v : a.plane.values) {
    if (ctl.cancelled && ctl.cancelled())
      return fail(QStringLiteral("已取消"));
    float sample = 0;
    stream >> sample;
    v = sample;
  }
  if (stream.status() != QDataStream::Ok)
    return fail(QStringLiteral("SATR 值块截断"));
  const QString source = header.value("sourceSgyPath").toString();
  if (source.isEmpty())
    return fail(QStringLiteral("SATR 缺源测网路径"));
  seismic::SgyVolume volume;
  std::string numericError;
  if (!volume.Load(
          std::filesystem::path(source.toStdString()), numericError,
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
  if (axis.size() != std::size_t(width) || volume.SampleCount() != height)
    return fail(QStringLiteral("SATR 与源测网尺寸不符"));
  const int line = header.value("sectionIndex").toInt();
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
      auto it =
          std::find_if(doc.curves.begin(), doc.curves.end(),
                       [&](const LasCurve &c) { return c.name == s.curve; });
      if (it == doc.curves.end())
        return fail(QStringLiteral("曲线已不存在"));
      channels << Channel{s.curve,
                          it->unit,
                          s.well.wellId,
                          s.versionId,
                          doc.curves[0].values,
                          it->values,
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
  if (!horizons[0].grid.crs.isEmpty()) {
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
  for (const auto &s : rasters)
    if (!s.layerId.isEmpty())
      result.samples.sourceLayerIds << s.layerId;
  return result;
}
} // namespace paleo::crossplot
