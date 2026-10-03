// 层：功能
#include "sectionworkbench.h"
#include "derivedassets.h"
#include "io/lasparser.h"
#include "services/welllogset.h"
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QStringList>
#include <QSaveFile>
#include <algorithm>
#include <cmath>

SectionWorkbench::SectionWorkbench(DataCatalog *catalog, QObject *parent)
    : QObject(parent), m_catalog(catalog) {
  syncProject();
}
QString SectionWorkbench::projectDir() const {
  return m_catalog ? QDir::cleanPath(m_catalog->catalogPath() + "/../../..")
                   : QString();
}
void SectionWorkbench::syncProject() {
  const auto path = m_catalog ? m_catalog->catalogPath() : QString();
  if (path != m_catalogPath) {
    m_calibrations.clear();
    m_logs.clear();
    m_parentVersion.clear();
    m_catalogPath = path;
  }
  m_data.setCatalog(m_catalog, projectDir());
}
QVariantMap SectionWorkbench::calibration(const QString &id) const {
  auto c = m_calibrations.value(id).toMap();
  if (c.isEmpty())
    c = {{"constant", false}, {"velocity", 2500.0}, {"shift", 0.0}};
  return c;
}
seismic::TimeDepthModel SectionWorkbench::modelFor(const QString &id, bool md,
                                                   bool *ok) const {
  const auto c = calibration(id);
  seismic::TimeDepthModel model(c.value("velocity").toDouble());
  *ok = c.value("constant").toBool();
  if (*ok)
    return model;
  std::vector<seismic::TdPoint> points;
  for (const auto &p : m_data.tdTableFor(id)) {
    double d = md ? p.md : p.tvd;
    if (std::isfinite(d) && std::isfinite(p.timeMs))
      points.push_back({d, p.timeMs});
  }
  *ok = model.setCheckshots(points);
  return model;
}
bool SectionWorkbench::setCalibration(const QString &id, bool constant,
                                      double velocity, double shift,
                                      QString *error) {
  syncProject();
  if (!m_catalog || !m_catalog->hasEntity(id) || !std::isfinite(velocity) ||
      velocity <= 100 || velocity >= 20000 || !std::isfinite(shift) ||
      std::abs(shift) > 10000) {
    if (error)
      *error = tr(
          "请选择有效井，速度需在 100–20000 m/s 之间，时间平移在 ±10000 ms 内");
    return false;
  }
  m_calibrations[id] = QVariantMap{
      {"constant", constant}, {"velocity", velocity}, {"shift", shift}};
  return true;
}
QVariantList SectionWorkbench::wells() {
  syncProject();
  QVariantList result;
  for (const auto &w : m_data.wells()) {
    auto c = calibration(w.id);
    bool tvdOk = false, mdOk = false;
    modelFor(w.id, false, &tvdOk);
    modelFor(w.id, true, &mdOk);
    const auto samples = m_data.tdTableFor(w.id);
    QString status =
        c.value("constant").toBool()
            ? tr("常速近似（需复核）")
            : (samples.isEmpty()
                   ? tr("无时深表")
                   : (!tvdOk && !mdOk
                          ? tr("时深表无序或有效样点不足")
                          : (tvdOk && mdOk ? tr("时深表：TVD / MD")
                             : tvdOk       ? tr("仅 TVD；测井曲线缺 MD 对齐")
                                           : tr("仅 MD；TVD 分层不可对齐"))));
    QVariantList table;
    for (const auto &p : samples)
      table << QVariantMap{{"tvd", p.tvd}, {"md", p.md}, {"time", p.timeMs}};
    c.insert("id", w.id);
    c.insert("name", w.name);
    c.insert("status", status);
    c.insert("samples", table);
    c.insert("coordinates", (w.coordinateStatus == "ok" ||
                             w.coordinateStatus == "untransformed") &&
                                std::isfinite(w.surfaceX) &&
                                std::isfinite(w.surfaceY));
    result << c;
  }
  return result;
}
bool SectionWorkbench::mdTimeDepth(const QString &wellId,
                                   seismic::TimeDepthModel *model,
                                   double *shiftMs, QString *status) {
  syncProject();
  const auto c = calibration(wellId);
  bool ok = false;
  const auto m = modelFor(wellId, true, &ok);
  if (!ok) {
    if (status)
      *status = m_data.tdTableFor(wellId).isEmpty()
                    ? tr("无时深表")
                    : tr("时深表无序或有效样点不足");
    return false;
  }
  if (model)
    *model = m;
  if (shiftMs)
    *shiftMs = c.value("shift").toDouble();
  if (status)
    *status =
        c.value("constant").toBool() ? tr("常速校正") : tr("时深表");
  return true;
}
std::vector<glm::dvec2> SectionWorkbench::wellRoute(const QStringList &ids,
                                                    QString *error) {
  syncProject();
  std::vector<glm::dvec2> route;
  const auto candidates = m_data.wells();
  for (const auto &id : ids) {
    const auto it = std::find_if(candidates.begin(), candidates.end(),
                                 [&](const auto &w) { return w.id == id; });
    if (it == candidates.end() ||
        (it->coordinateStatus != "ok" &&
         it->coordinateStatus != "untransformed") ||
        !std::isfinite(it->surfaceX) || !std::isfinite(it->surfaceY)) {
      if (error)
        *error = tr("所选井缺少有效井口坐标");
      return {};
    }
    const glm::dvec2 p(it->surfaceX, it->surfaceY);
    if (route.empty() || glm::length(p - route.back()) > 1e-6)
      route.push_back(p);
  }
  if (route.size() < 2) {
    if (error)
      *error = tr("至少选择两口坐标不同的井（列表顺序即剖面顺序）");
    return {};
  }
  return route;
}
std::vector<seismic::SectionWellInfo> SectionWorkbench::sectionWells() {
  syncProject();
  std::vector<seismic::SectionWellInfo> result;
  for (const auto &w : m_data.wells()) {
    if (w.coordinateStatus != "ok" && w.coordinateStatus != "untransformed")
      continue;
    seismic::SectionWellInfo out;
    out.wellId = w.id;
    out.wellName = w.name;
    out.surfaceX = w.surfaceX;
    out.surfaceY = w.surfaceY;
    out.totalDepth = m_catalog->entityById(w.id).td;
    out.calibrated = true;
    bool tvdOk = false, mdOk = false;
    auto tvd = modelFor(w.id, false, &tvdOk), md = modelFor(w.id, true, &mdOk);
    const double shift = calibration(w.id).value("shift").toDouble();
    out.alignmentStatus = calibration(w.id).value("constant").toBool()
                              ? tr("常速近似")
                              : (tvdOk || mdOk ? tr("时深表") : tr("未对齐"));
    bool partial = false;
    auto timeFor = [&](double d, bool useMd) {
      const double result = (useMd ? mdOk : tvdOk)
                                ? (useMd ? md : tvd).DepthToTwtMs(d) + shift
                                : qQNaN();
      if (std::isfinite(d) && !std::isfinite(result))
        partial = true;
      return result;
    };
    out.bottomTwtMs = timeFor(out.totalDepth, true);
    // goal/well-trajectory：有测斜 → 剖面井轨按真实轨迹（xy = 井口 + 位移，
    // twt 优先 TVD 校准回退 MD）；无测斜 → trajectory 空，视图保持垂直简化
    // （不虚构造斜）。井底坐标同步补齐（平面轨迹线/两点简化的数据源）。
    const auto survey = m_data.trajectoryFor(w.id);
    if (survey)
    {
      const auto trajTwt = [&](const paleo::TrajectoryPoint &pt) {
        const double viaTvd = timeFor(pt.tvd, false);
        return std::isfinite(viaTvd) ? viaTvd : timeFor(pt.md, true);
      };
      for (const paleo::TrajectoryPoint &sp : survey->points())
      {
        seismic::WellTrajSample sample;
        sample.md = sp.md;
        sample.tvd = sp.tvd;
        sample.x = w.surfaceX + sp.east;
        sample.y = w.surfaceY + sp.north;
        sample.twtMs = trajTwt(sp);
        out.trajectory.push_back(sample);
        if (std::isfinite(sample.twtMs))
          out.bottomTwtMs = std::isfinite(out.bottomTwtMs)
                                ? std::max(out.bottomTwtMs, sample.twtMs)
                                : sample.twtMs;
      }
      if (out.totalDepth > 0.0)
      {
        const paleo::TrajectoryPoint tip = survey->pointAt(out.totalDepth);
        out.bottomX = w.surfaceX + tip.east;
        out.bottomY = w.surfaceY + tip.north;
      }
    }
    for (const auto &t : m_data.topsFor(w.id)) {
      seismic::WellTopItem top;
      top.topName = t.horizon;
      top.md = t.md;
      top.tvd = t.tvd;
      const bool useMd = !std::isfinite(t.tvd);
      top.twtMs = timeFor(useMd ? t.md : t.tvd, useMd);
      if (std::isfinite(top.twtMs))
        out.bottomTwtMs = std::isfinite(out.bottomTwtMs)
                              ? std::max(out.bottomTwtMs, top.twtMs)
                              : top.twtMs;
      out.tops.push_back(top);
    }
    const QVector<WellCurveRef> curveIndex =
        WellLogSet::wellCurveIndex(m_catalog, projectDir(), w.id);
    QStringList fileOrder;
    QHash<QString, QVector<WellCurveRef>> byFile;
    for (const WellCurveRef &ref : curveIndex) {
      const QString key =
          ref.sourceVersionId.isEmpty() ? ref.path : ref.sourceVersionId;
      if (key.isEmpty() || ref.path.isEmpty())
        continue;
      if (!byFile.contains(key))
        fileOrder.append(key);
      byFile[key].append(ref);
    }
    for (const QString &key : fileOrder) {
      const QVector<WellCurveRef> refs = byFile.value(key);
      if (refs.isEmpty())
        continue;
      const QString cacheKey = refs.first().sourceVersionId.isEmpty()
                                   ? key
                                   : refs.first().sourceVersionId;
      if (!m_logs.contains(cacheKey)) {
        LasDoc doc;
        doc.ok = LasParser::parse(refs.first().path, doc.curveNames, doc.curves,
                                  &doc.error);
        m_logs.insert(cacheKey, doc);
      }
      const auto &doc = m_logs[cacheKey];
      if (!doc.ok || doc.curves.size() < 2)
        continue;
      const WellCurveRef *gr = nullptr;
      const WellCurveRef *first = nullptr;
      for (const WellCurveRef &ref : refs) {
        if (ref.column < 1 || ref.column >= doc.curves.size())
          continue;
        if (!first || ref.column < first->column)
          first = &ref;
        const bool columnGr =
            doc.curves.at(ref.column).name.compare("GR", Qt::CaseInsensitive) ==
            0;
        const bool mnemonicGr =
            ref.mnemonic.compare("GR", Qt::CaseInsensitive) == 0;
        if ((columnGr || mnemonicGr) && (!gr || ref.column < gr->column))
          gr = &ref;
      }
      const WellCurveRef *chosen = gr ? gr : first;
      if (!chosen)
        continue;
      const int column = chosen->column;
      const QString curveName = chosen->mnemonic;
      const auto &depths = doc.curves.at(0);
      const auto &values = doc.curves.at(column);
      double scale = 1;
      const auto unit = depths.unit.trimmed().toUpper();
      if (unit == "FT" || unit == "F")
        scale = .3048;
      else if (unit != "M") {
        // 单位未知只跳过这一份 LAS，同井其它文件继续。
        out.alignmentStatus += tr(" · 深度单位未知，曲线未叠加");
        continue;
      }
      seismic::WellCurveItem curve;
      curve.curveName = curveName;
      const int size = std::min(depths.values.size(), values.values.size());
      const int step = std::max(1, size / 4000);
      for (int i = 0; i < size; i += step) {
        const double d = depths.values[i] * scale, t = timeFor(d, true);
        curve.depthsM.push_back(d);
        curve.twtMs.push_back(t);
        curve.values.push_back(static_cast<float>(values.values[i]));
        if (std::isfinite(t))
          out.bottomTwtMs =
              std::isfinite(out.bottomTwtMs) ? std::max(out.bottomTwtMs, t) : t;
      }
      out.curves.push_back(curve);
    }
    if (partial && (mdOk || tvdOk))
      out.alignmentStatus += tr(" · 部分未对齐");
    result.push_back(out);
  }
  return result;
}
QVariantList SectionWorkbench::savedSections() const {
  QVariantList result;
  if (m_catalog)
    for (const auto &v : m_catalog->versions())
      if (v.extra.value("kind") == "section_session")
        result.prepend(
            QVariantMap{{"id", v.id},
                        {"name", m_catalog->assetById(v.assetId).displayName +
                                     tr(" · v%1").arg(v.versionNumber)}});
  return result;
}
bool SectionWorkbench::save(const QString &name,
                            const std::vector<glm::dvec2> &route,
                            const QString &seismicPath, const QString &horizon,
                            QString *error) {
  syncProject();
  if (!m_catalog || !m_catalog->isOpen() || route.size() < 2 ||
      name.trimmed().isEmpty()) {
    if (error)
      *error = tr("请打开工程并生成剖面后命名保存");
    return false;
  }
  DerivedAssetRegistrar registrar(m_catalog, projectDir());
  auto staging =
      registrar.stage("section", name.trimmed(), "section.json", error);
  if (!staging.isValid())
    return false;
  QVariantList points;
  for (const auto &p : route)
    points << QVariantMap{{"x", p.x}, {"y", p.y}};
  QVariantMap data{{"kind", "section_session"},
                   {"route", points},
                   {"calibrations", m_calibrations},
                   {"seismic_path", seismicPath},
                   {"horizon", horizon},
                   {"coordinate_unit", "m"}};
  QStringList parents = registrar.parentVersionIdsFor({seismicPath});
  if (!parents.isEmpty())
    data["seismic_version"] = parents.front();
  QVariantMap sources;
  for (const auto &link : m_catalog->links())
    if (link.entityType == "well" && link.isPrimary && !link.unresolved &&
        (link.role == "well_log" || link.role == "tops" ||
         link.role == "time_depth")) {
      const auto v = m_catalog->currentVersion(link.assetId);
      if (!v.id.isEmpty()) {
        parents << v.id;
        sources[link.entityId + ":" + link.role] = v.id;
      }
    }
  if (!m_parentVersion.isEmpty())
    parents << m_parentVersion;
  parents.removeDuplicates();
  data["sources"] = sources;
  QSaveFile file(staging.absolutePath);
  const auto bytes = QJsonDocument::fromVariant(data).toJson();
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
      !file.commit()) {
    if (error)
      *error = file.errorString();
    return false;
  }
  if (!registrar.commit(staging, parents, "paleo:section", data, error))
    return false;
  m_parentVersion = staging.versionId;
  return true;
}
QVariantMap SectionWorkbench::restore(const QString &id, QString *error,
                                      const QString &activeSeismicPath) {
  syncProject();
  const auto v = m_catalog ? m_catalog->versionById(id) : CatalogVersion();
  if (v.extra.value("kind") != "section_session") {
    if (error)
      *error = tr("未找到保存的剖面版本");
    return {};
  }
  const auto sources = v.extra.value("sources").toMap();
  QVariantMap current;
  for (const auto &link : m_catalog->links())
    if (link.isPrimary && !link.unresolved)
      current[link.entityId + ":" + link.role] =
          m_catalog->currentVersion(link.assetId).id;
  for (auto it = sources.cbegin(); it != sources.cend(); ++it) {
    if (current.value(it.key()) != it.value()) {
      if (error)
        *error = tr("源数据版本已变化，请按当前数据重新生成剖面；保存版本保留原"
                    "始来源关系。");
      return {};
    }
  }
  auto result = v.extra;
  const auto seismic =
      m_catalog->versionById(v.extra.value("seismic_version").toString());
  if (!seismic.id.isEmpty())
    result["seismic_path"] =
        DataCatalog::resolvedVersionPath(projectDir(), seismic);
  if (!activeSeismicPath.isEmpty() &&
      QDir::cleanPath(activeSeismicPath) !=
          QDir::cleanPath(result.value("seismic_path").toString())) {
    if (error)
      *error = tr("此版本使用其他地震体，请先加载对应地震体：%1")
                   .arg(result.value("seismic_path").toString());
    return {};
  }
  m_calibrations = v.extra.value("calibrations").toMap();
  m_parentVersion = id;
  return result;
}
