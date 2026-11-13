// 层：数据
#include "seismicattributefusionclient_internal.h"
#include <QDir>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QStandardPaths>
#include <cmath>

namespace paleo {
namespace fusion_detail {
bool localUrl(const QUrl &url)
{
  const auto host = url.host();
  return url.isValid() && url.scheme() == QLatin1String("http") &&
         (host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0 ||
          QHostAddress(host).isLoopback()) && url.userInfo().isEmpty() &&
         !url.hasQuery() && !url.hasFragment() &&
         (url.path().isEmpty() || url.path() == QLatin1String("/"));
}

QString validate(const SeismicFusionRequest &r)
{
  if (r.timeoutMs <= 0 || r.maxResultCells <= 0 || r.maxResultCells > 64 * 1024 * 1024)
    return SeismicAttributeFusionClient::tr("Invalid attribute fusion time or memory budget.");
  if (!r.serverUrl.isEmpty() && !localUrl(QUrl(r.serverUrl)))
    return SeismicAttributeFusionClient::tr("Attribute fusion requires a local HTTP service URL.");
  if (r.clusters < 2 || r.clusters > 32 || r.subIntervalIndex < 0 || r.subIntervalIndex > 4 ||
      !std::isfinite(r.gridSpacingX) || !std::isfinite(r.gridSpacingY) ||
      r.gridSpacingX <= 0 || r.gridSpacingY <= 0)
    return SeismicAttributeFusionClient::tr("Invalid cluster count, interval or grid spacing.");
  const bool automatic = r.fractionTop == -1 && r.fractionBottom == -1;
  if (!automatic && (!std::isfinite(r.fractionTop) || !std::isfinite(r.fractionBottom) ||
      r.fractionTop < 0 || r.fractionBottom > 1 || r.fractionTop >= r.fractionBottom))
    return SeismicAttributeFusionClient::tr("Interval fractions must satisfy 0 <= top < bottom <= 1.");
  if (!r.bbox.isNull() && (!std::isfinite(r.bbox.x()) || !std::isfinite(r.bbox.y()) ||
      !std::isfinite(r.bbox.right()) || !std::isfinite(r.bbox.bottom()) ||
      r.bbox.width() <= 0 || r.bbox.height() <= 0))
    return SeismicAttributeFusionClient::tr("Invalid attribute fusion extent.");
  if (r.strategy != QLatin1String("auto") && r.strategy != QLatin1String("train") &&
      r.strategy != QLatin1String("pretrained"))
    return SeismicAttributeFusionClient::tr("Unknown attribute fusion strategy.");
  if (r.seismicPath.isEmpty() || (r.horizonWindowPath.isEmpty() && r.horizonPath.isEmpty() &&
      (r.topHorizonPath.isEmpty() || r.bottomHorizonPath.isEmpty())))
    return SeismicAttributeFusionClient::tr("Seismic data and a horizon or interval window are required.");
  const QStringList paths{r.seismicPath, r.topHorizonPath, r.bottomHorizonPath,
                          r.horizonPath, r.horizonWindowPath, r.wellsPath};
  for (const auto &path : paths) {
    if (!path.isEmpty() && (!QFileInfo(path).isFile() || !QFileInfo(path).isReadable()))
      return SeismicAttributeFusionClient::tr("Input file is not readable: %1").arg(path);
  }
  return {};
}
} // namespace fusion_detail

QString SeismicAttributeFusionClient::defaultServiceScript()
{
  return qEnvironmentVariable("PALEO_FUSION_SCRIPT");
}

QString SeismicAttributeFusionClient::defaultPythonExecutable()
{
  const auto configured = qEnvironmentVariable("PALEO_FUSION_PYTHON");
  if (!configured.isEmpty()) return configured;
  const auto script = defaultServiceScript();
  if (!script.isEmpty()) {
    const QDir dir(QFileInfo(script).absolutePath());
    for (const auto &name : {QStringLiteral(".venv/Scripts/python.exe"), QStringLiteral(".venv/bin/python")}) {
      const auto candidate = dir.filePath(name);
      if (QFileInfo(candidate).isExecutable()) return candidate;
    }
  }
  for (const auto &name : {QStringLiteral("python3"), QStringLiteral("python")}) {
    const auto candidate = QStandardPaths::findExecutable(name);
    if (!candidate.isEmpty()) return candidate;
  }
  return {};
}

bool SeismicAttributeFusionClient::isServerAlive(const QString &url, int timeoutMs)
{
  if (timeoutMs <= 0 || !fusion_detail::localUrl(QUrl(url))) return false;
  fusion_detail::Control control(timeoutMs, {});
  return fusion_detail::health(url, control);
}

QJsonObject SeismicAttributeFusionClient::buildRequestJson(const SeismicFusionRequest &r)
{
  QJsonObject object;
  const auto path = [&object](const char *key, const QString &value) {
    if (!value.isEmpty()) object.insert(QLatin1String(key), QFileInfo(value).absoluteFilePath());
  };
  path("seismic_path", r.seismicPath);
  path("top_horizon_path", r.topHorizonPath);
  path("bottom_horizon_path", r.bottomHorizonPath);
  path("horizon_path", r.horizonPath);
  path("horizon_window_path", r.horizonWindowPath);
  path("wells_path", r.wellsPath);
  path("output_dir", r.outputDir);
  if (!r.unitName.isEmpty()) object.insert(QStringLiteral("unit_name"), r.unitName);
  object.insert(QStringLiteral("sub_interval_index"), r.subIntervalIndex);
  if (r.fractionTop >= 0) object.insert(QStringLiteral("fraction_top"), r.fractionTop);
  if (r.fractionBottom >= 0) object.insert(QStringLiteral("fraction_bottom"), r.fractionBottom);
  object.insert(QStringLiteral("clusters"), r.clusters);
  object.insert(QStringLiteral("grid_spacing"), QJsonArray{r.gridSpacingX, r.gridSpacingY});
  if (!r.bbox.isNull()) object.insert(QStringLiteral("bbox"),
      QJsonArray{r.bbox.x(), r.bbox.y(), r.bbox.right(), r.bbox.bottom()});
  if (!r.targetCrs.isEmpty()) object.insert(QStringLiteral("target_crs"), r.targetCrs);
  object.insert(QStringLiteral("strategy"), r.strategy);
  return object;
}

SeismicFusionResult SeismicAttributeFusionClient::parseResultJson(
    const QJsonObject &root, const QString &output, qint64 maxCells)
{
  return fusion_detail::parse(root, output, maxCells);
}

SeismicFusionResult SeismicAttributeFusionClient::execute(
    const SeismicFusionRequest &request, const fusion_detail::Progress &progress,
    const fusion_detail::Cancel &cancel)
{
  fusion_detail::Control control(request.timeoutMs, cancel);
  auto error = control.interruption();
  if (error.isEmpty()) error = fusion_detail::validate(request);
  if (!error.isEmpty()) return fusion_detail::failure(error);
  if (!request.serverUrl.isEmpty() && fusion_detail::health(request.serverUrl, control))
    return fusion_detail::http(request, progress, control);
  error = control.interruption();
  if (!error.isEmpty()) return fusion_detail::failure(error);
  return fusion_detail::cli(request, progress, control);
}
} // namespace paleo
