// 层：数据
#include "welltable.h"
#include "wellfileparsers.h"
#include "wellcompositexml.h"
#include "dataimportservice.h"
#include "../domain/deviationsurvey.h"
#include <QFile>
#include <QCryptographicHash>
#include <QFileInfo>
#include <algorithm>
#include <cmath>

namespace {
QByteArray contents(DataCatalog *cat, const CatalogVersion &version) {
  QFile file(cat->versionFilePath(version));
  if (!file.open(QIODevice::ReadOnly)) return {};
  const auto bytes = file.readAll();
  return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) == version.sha256 ? bytes : QByteArray();
}
struct DepthModel {
  std::optional<paleo::WellDeviationSurvey> survey;
  QVector<QPair<double, double>> pairs;
  bool valid = false, identity = false;
  QStringList inputs;
  double measured(double tvd) const {
    if (survey) {
      const double md = survey->tvdToMd(tvd);
      return std::abs(survey->pointAt(md).tvd - tvd) < .01 ? md : qQNaN();
    }
    if (!valid) return qQNaN();
    const auto upper = std::lower_bound(pairs.cbegin(), pairs.cend(), tvd, [](const auto &p, double d) { return p.first < d; });
    if (upper != pairs.cend() && upper->first == tvd) return upper->second;
    if (upper == pairs.cbegin() || upper == pairs.cend()) return identity ? tvd : qQNaN();
    const auto lower = upper - 1;
    return lower->second + (tvd - lower->first) * (upper->second - lower->second) / (upper->first - lower->first);
  }
};
DepthModel modelFor(DataCatalog *cat, const QString &wellId) {
  DepthModel model; CatalogVersion deviation, td;
  for (const auto &link : cat->linksForEntity(wellId)) {
    if (link.unresolved || !link.isPrimary) continue;
    if (link.role == "trajectory") deviation = cat->currentVersion(link.assetId);
    if (link.role == "time_depth") td = cat->currentVersion(link.assetId);
  }
  if (!deviation.id.isEmpty()) {
    model.inputs << deviation.id;
    QVector<paleo::DeviationStation> stations;
    if (QFileInfo(deviation.fileName).suffix().compare("xml", Qt::CaseInsensitive) == 0) {
      QVector<WellComposite::XmlDeviationStation> rows;
      if (!WellComposite::parseDeviationSurvey(cat->versionFilePath(deviation), rows)) return model;
      for (const auto &r : rows) stations << paleo::DeviationStation{r.md, r.inclinationDeg, r.azimuthDeg};
    } else for (const auto &r : parseDeviationText(contents(cat, deviation)).stations)
      stations << paleo::DeviationStation{r.md, r.inclinationDeg, r.azimuthDeg};
    model.survey = paleo::WellDeviationSurvey::fromStations(stations);
    return model; // Invalid survey must not silently fall back to a different source.
  }
  if (td.id.isEmpty()) return model;
  model.inputs << td.id;
  for (const auto &r : parseTimeDepthText(contents(cat, td)).rows)
    if (r.hasMd && r.hasTvd && std::isfinite(r.md) && std::isfinite(r.tvd)) model.pairs << qMakePair(r.tvd, r.md);
  auto &pairs = model.pairs;
  std::sort(pairs.begin(), pairs.end()); pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
  if (!pairs.isEmpty() && pairs.first().first > 0 && pairs.first().second > 0) pairs.prepend({0., 0.});
  model.valid = pairs.size() >= 2; model.identity = model.valid;
  for (int i = 0; i < pairs.size(); ++i) {
    model.identity &= std::abs(pairs[i].first - pairs[i].second) < 1e-6;
    if (i && (pairs[i].first <= pairs[i-1].first || pairs[i].second <= pairs[i-1].second)) model.valid = false;
  }
  return model;
}
}
namespace paleo::io {
bool resolveImportedWellDepths(ImportSession &s, QString *error) {
  QHash<QString, DepthModel> models; QSet<QString> ambiguous;
  for (const auto &well : s.cat->entities("well")) {
    const auto name = DataCatalog::normalizeWellName(well.name);
    if (models.contains(name)) { models.remove(name); ambiguous.insert(name); }
    else if (!ambiguous.contains(name)) models.insert(name, modelFor(s.cat.get(), well.id));
  }
  for (const auto &asset : s.cat->assets()) {
    if (asset.type != "well_stratification") continue;
    auto version = s.cat->currentVersion(asset.id);
    // Only normalize this import's accepted tables. Existing edited interpretations stay authoritative.
    if (!s.cat->isStagedVersion(version.id) || !version.extra.value("wellTableParsed").toBool()) continue;
    auto tops = parseWellTopsText(contents(s.cat.get(), version));
    auto bases = readWellTableBases(s.cat->versionFilePath(version));
    int resolved = 0, unresolved = 0; QStringList inputs;
    const auto convert = [&](WellTopRecord &record) {
      if (record.hasMd || !record.hasTvd) return;
      const auto model = models.value(DataCatalog::normalizeWellName(record.wellName));
      const double md = model.measured(record.tvd);
      if (std::isfinite(md)) { record.md = md; record.hasMd = true; ++resolved; inputs += model.inputs; }
      else ++unresolved;
    };
    for (auto &top : tops) convert(top);
    for (auto &base : bases) convert(base);
    if (!resolved) continue;
    inputs.removeDuplicates();
    version.extra.insert("depthResolution", QVariantMap{{"schema", 1}, {"resolved_boundaries", resolved},
        {"unresolved_boundaries", unresolved}, {"input_versions", inputs}, {"original_depths", "RAW"}});
    if (!storeNormalizedWellTable(s, version, writeWellLayerTable(tops, bases), error)) return false;
  }
  return true;
}
}
