// 层：功能
#include "workflows.h"
#include "constraintfactorjobs_internal.h"
#include "workflows_internal.h"
#include "wellsectionworkflow.h"
#include "algorithms/singlefactor/wellacquisition.h"
#include "domain/mappinghorizons.h"
#include "domain/singlefactorstrategy.h"
#include "services/projectdata.h"
#include "services/singlefactordef.h"
#include "derivedassets.h"
#include <QSet>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsfield.h>
#include <qgsgeometry.h>
#include <qgsvectordataprovider.h>
#include <qgsvectorfilewriter.h>
#include <qgsvectorlayer.h>
#include <qgsproject.h>
#include <algorithm>
#include <cmath>

namespace {
const QString layerThickness = QStringLiteral("log_layer_thickness_md");
const QString sandThickness = QStringLiteral("log_sand_thickness_md");
struct Input {
  QString id, name;
  QVariantMap attributes;
  QgsGeometry geometry;
  QString thicknessReason, sandReason, sandSource;
};
struct Source {
  QVector<Input> rows;
  QStringList paths;
  QgsCoordinateReferenceSystem crs;
};

// 同类重叠合并；跨岩性重叠拒绝。覆盖不足不把未解释区间当泥岩。
std::optional<double> sandIn(const QVector<wellsection::LithoSegment> &segments,
                             double top, double base, QString *reason) {
  struct Span { double a, b; bool sand; };
  QVector<Span> spans;
  for (const auto &s : segments) {
    if (s.source != wellsection::LithoSource::Interpreted ||
        !std::isfinite(s.topMd) || !std::isfinite(s.baseMd) || s.baseMd <= s.topMd)
      continue;
    const double a = std::max(top, s.topMd), b = std::min(base, s.baseMd);
    if (b <= a) continue;
    const QString litho = s.litho.trimmed().toLower();
    const bool sand = litho.contains(QStringLiteral("砂岩")) || litho == QStringLiteral("砂") ||
                      litho.contains(QStringLiteral("sandstone")) || litho == QStringLiteral("sand");
    const bool known = sand || litho.contains(QStringLiteral("泥")) || litho.contains(QStringLiteral("页岩")) ||
                       litho.contains(QStringLiteral("灰岩")) || litho.contains(QStringLiteral("白云岩")) ||
                       litho.contains(QStringLiteral("砾岩")) || litho.contains(QStringLiteral("shale")) ||
                       litho.contains(QStringLiteral("mudstone")) || litho.contains(QStringLiteral("limestone"));
    if (known) spans.push_back({a, b, sand});
  }
  std::sort(spans.begin(), spans.end(), [](const Span &a, const Span &b) { return a.a < b.a; });
  double coveredTo = top, sand = 0;
  bool previousSand = false;
  for (const Span &s : spans) {
    if (s.a > coveredTo + 1e-6) {
      *reason = ConstraintWorkflow::tr("层段解释岩性覆盖不完整"); return std::nullopt;
    }
    if (s.a < coveredTo - 1e-6 && s.a > top - 1e-6 && coveredTo > top && s.sand != previousSand) {
      *reason = ConstraintWorkflow::tr("重叠岩性解释冲突"); return std::nullopt;
    }
    if (s.sand) sand += std::max(0.0, s.b - coveredTo);
    if (s.b > coveredTo) { coveredTo = s.b; previousSand = s.sand; }
  }
  if (coveredTo < base - 1e-6) {
    *reason = spans.isEmpty() ? ConstraintWorkflow::tr("缺少层段解释岩性")
                             : ConstraintWorkflow::tr("层段解释岩性覆盖不完整");
    return std::nullopt;
  }
  return sand;
}

Source sourceFor(ConstraintWorkflow *wf, const QString &horizon, const QVariantMap &params, QString *error) {
  Source out;
  out.crs = QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt());
  auto *layers = wf->layerService();
  const QString pointsId = params.value(QStringLiteral("pointsLayerId"),
      paleo::constraint_detail::wellsLayerIdFor(layers, horizon)).toString();
  if (!pointsId.isEmpty() && layers) {
    auto *layer = qobject_cast<QgsVectorLayer *>(layers->instantiate(pointsId, error));
    if (!layer || layer->geometryType() != Qgis::GeometryType::Point) return out;
    out.crs = layer->crs();
    out.paths << layer->source().section(QLatin1Char('|'), 0, 0);
    QgsFeature f; auto it = layer->getFeatures();
    while (it.nextFeature(f)) {
      Input row;
      row.geometry = f.geometry();
      for (int i = 0; i < layer->fields().size(); ++i)
        row.attributes.insert(layer->fields().at(i).name(), f.attribute(i));
      for (const QString &key : {QStringLiteral("well_id"), QStringLiteral("id"), QStringLiteral("well_name"), QStringLiteral("name"), QStringLiteral("井名")}) {
        if (row.id.isEmpty()) row.id = row.attributes.value(key).toString();
      }
      row.name = row.attributes.value(QStringLiteral("well_name"), row.attributes.value(QStringLiteral("name"), row.id)).toString();
      if (row.id.isEmpty()) row.id = QStringLiteral("WELL_%1").arg(out.rows.size() + 1);
      if (row.name.isEmpty()) row.name = row.id;
      out.rows << row;
    }
  }
  if (!wf->catalog()) return out;
  ProjectDataFacade data; data.setCatalog(wf->catalog(), wf->projectDir());
  WellSectionWorkflow lithology(wf->catalog());
  const QString baseName = params.value(QStringLiteral("baseHorizon"), baseHorizonFor(horizon)).toString();
  const QStringList requested{params.value(QStringLiteral("valueField"), params.value(QStringLiteral("field"))).toString(),
      params.value(QStringLiteral("numeratorField")).toString(), params.value(QStringLiteral("denominatorField")).toString()};
  const bool metrics = params.isEmpty() || requested.contains(layerThickness) || requested.contains(sandThickness);
  const bool sandMetric = params.isEmpty() || requested.contains(sandThickness);
  const auto wells = data.wells();
  QStringList ids, lithologyWarnings;
  for (const auto &well : wells) ids << well.id;
  const auto lithologies = sandMetric ? lithology.lithologiesFor(ids, &lithologyWarnings) : QHash<QString, QVector<wellsection::LithoSegment>>();
  for (const ProjectWell &well : wells) {
    auto found = std::find_if(out.rows.begin(), out.rows.end(), [&](const Input &r) {
      return r.id == well.id || DataCatalog::normalizeWellName(r.name) == DataCatalog::normalizeWellName(well.name);
    });
    if (found == out.rows.end()) {
      Input row; row.id = well.id; row.name = well.name;
      // 无地图源时 catalog 井位属于工程局部米坐标；已有地图源时缺井不猜 CRS。
      if (pointsId.isEmpty() && (well.coordinateStatus == QLatin1String("ok") ||
                               well.coordinateStatus == QLatin1String("untransformed")) &&
          std::isfinite(well.surfaceX) && std::isfinite(well.surfaceY))
        row.geometry = QgsGeometry::fromPointXY(QgsPointXY(well.surfaceX, well.surfaceY));
      out.rows << row; found = out.rows.end() - 1;
    }
    Input &row = *found; row.id = well.id; row.name = well.name;
    if (!metrics) continue;
    double top = qQNaN(), base = qQNaN();
    for (const WellTop &t : data.topsFor(well.id)) {
      if (t.horizon == horizon) top = t.md;
      if (t.horizon == baseName) base = t.md;
    }
    row.thicknessReason = ConstraintWorkflow::tr("缺少顶/底分层 MD（%1→%2）").arg(horizon, baseName);
    row.sandReason = row.thicknessReason;
    if (std::isfinite(top) && std::isfinite(base) && base > top) {
      row.attributes.insert(layerThickness, base - top); row.thicknessReason.clear();
      const auto segs = lithologies.value(well.id);
      const auto sand = sandIn(segs, top, base, &row.sandReason);
      if (sand) {
        row.attributes.insert(sandThickness, *sand); row.sandReason.clear();
        QStringList sources;
        for (const auto &s : segs) if (!s.provenance.isEmpty() && !sources.contains(s.provenance)) sources << s.provenance;
        row.sandSource = sources.isEmpty() ? ConstraintWorkflow::tr("工程解释岩性 / 岩屑录井（MD）") : sources.join(QStringLiteral("; "));
      } else if (!lithologyWarnings.isEmpty()) row.sandReason += QStringLiteral("; ") + lithologyWarnings.join(QStringLiteral("; "));
    } else if (std::isfinite(top) && std::isfinite(base)) {
      row.thicknessReason = ConstraintWorkflow::tr("分层倒置或层厚为零"); row.sandReason = row.thicknessReason;
    }
    for (const auto &link : wf->catalog()->linksForEntity(well.id))
      if (!link.unresolved && (link.role == QLatin1String("tops") || (sandMetric && (link.role == QLatin1String("interpretation") ||
                              link.role == QLatin1String("cuttings"))))) {
        const auto v = wf->catalog()->currentVersion(link.assetId);
        if (!v.id.isEmpty()) out.paths << DataCatalog::resolvedVersionPath(wf->projectDir(), v);
      }
  }
  out.paths.removeDuplicates();
  return out;
}
QString fieldReason(const Input &row, const QString &field) {
  if (field == layerThickness) return row.thicknessReason;
  if (field == sandThickness) return row.sandReason;
  return ConstraintWorkflow::tr("字段 %1 缺失或不是有限数值").arg(field);
}
} // namespace

QVariantList ConstraintWorkflow::wellFactorFields(const QString &horizon, QString *error) {
  const Source source = sourceFor(this, horizon, {}, error);
  QSet<QString> keys;
  for (const Input &r : source.rows)
    for (auto i = r.attributes.cbegin(); i != r.attributes.cend(); ++i)
      if (paleo::singlefactor::parseNumeric(i.value())) keys.insert(i.key());
  QStringList names = keys.values(); names.sort(Qt::CaseInsensitive);
  QVariantList fields;
  for (const QString &name : names) {
    QString label = name;
    if (name == layerThickness) label = tr("分层层厚（MD，m）");
    if (name == sandThickness) label = tr("解释砂岩厚度（MD，m）");
    fields << QVariantMap{{QStringLiteral("id"), name}, {QStringLiteral("label"), label}};
  }
  return fields;
}

bool ConstraintWorkflow::extractWellFactors(const QString &horizon, const QString &factorId,
    const QVariantMap &params, QString *error, QString *pointsLayerId) {
  m_wellFactorRows.clear(); m_wellFactorMessage.clear();
  const auto fail = [&](const QString &message) {
    m_wellFactorMessage = message; paleo::workflow_detail::setError(error, message);
    emit wellFactorsExtracted(horizon, factorId); return false;
  };
  bool known = false; const auto def = SingleFactorRegistry::byId(factorId, &known);
  if (!known) return fail(tr("未知单因素 id：%1").arg(factorId));
  paleo::singlefactor::WellAcquisitionRequest request;
  request.strictFields = true;
  request.factorMode = params.value(QStringLiteral("factorMode"), QStringLiteral("direct")).toString();
  request.valueField = params.value(QStringLiteral("valueField"), params.value(QStringLiteral("field"))).toString();
  request.numeratorField = params.value(QStringLiteral("numeratorField")).toString();
  request.denominatorField = params.value(QStringLiteral("denominatorField")).toString();
  if (request.factorMode != QLatin1String("direct") && request.factorMode != QLatin1String("ratio"))
    return fail(tr("未知因子提取口径"));
  if ((request.factorMode == QLatin1String("direct") && request.valueField.isEmpty()) ||
      (request.factorMode == QLatin1String("ratio") && (request.numeratorField.isEmpty() || request.denominatorField.isEmpty())))
    return fail(tr("请选择因子提取字段"));
  QString sourceError; const Source source = sourceFor(this, horizon, params, &sourceError);
  if (!sourceError.isEmpty()) return fail(sourceError);
  QgsVectorLayer samples(QStringLiteral("Point"), QStringLiteral("well_factors"), QStringLiteral("memory"));
  samples.setCrs(source.crs);
  samples.dataProvider()->addAttributes({QgsField(QStringLiteral("well_id"), QMetaType::Type::QString),
      QgsField(QStringLiteral("well_name"), QMetaType::Type::QString), QgsField(QStringLiteral("factor_value"), QMetaType::Type::Double)});
  samples.updateFields();
  QgsFeatureList features;
  for (const Input &r : source.rows) {
    paleo::singlefactor::FeatureAttributes attrs;
    for (auto i = r.attributes.cbegin(); i != r.attributes.cend(); ++i) attrs.emplace_back(i.key(), i.value());
    const auto value = paleo::singlefactor::resolveCurrentFactorValue(attrs, request);
    QString reason;
    if (request.factorMode == QLatin1String("ratio")) {
      const auto numerator = paleo::singlefactor::parseNumeric(r.attributes.value(request.numeratorField));
      const auto denominator = paleo::singlefactor::parseNumeric(r.attributes.value(request.denominatorField));
      if (!numerator) reason = fieldReason(r, request.numeratorField);
      else if (!denominator) reason = fieldReason(r, request.denominatorField);
      else if (*denominator == 0) reason = tr("分母为零");
    } else if (!value) reason = fieldReason(r, request.valueField);
    QgsGeometry pointGeometry = r.geometry;
    bool located = !r.geometry.isNull() && !r.geometry.isEmpty();
    if (located) {
      const QgsPointXY point = r.geometry.isMultipart() ? r.geometry.asMultiPoint().value(0) : r.geometry.asPoint();
      located = std::isfinite(point.x()) && std::isfinite(point.y());
      pointGeometry = QgsGeometry::fromPointXY(point);
    }
    const QString coordinateStatus = r.attributes.value(QStringLiteral("coordinate_status")).toString();
    located = located && coordinateStatus != QLatin1String("invalid") && coordinateStatus != QLatin1String("missing");
    if (!located) reason += (reason.isEmpty() ? QString() : QStringLiteral("; ")) + tr("缺少有效井点坐标");
    if (!value && reason.isEmpty()) reason = tr("比值不是有限数值");
    const auto sourceOfField = [&](const QString &field) {
      if (field == layerThickness) return tr("工程分层（MD）");
      if (field == sandThickness) return r.sandSource;
      return tr("井点属性 · %1").arg(field);
    };
    const QString valueSource = request.factorMode == QLatin1String("ratio")
        ? sourceOfField(request.numeratorField) + tr(" ÷ ") + sourceOfField(request.denominatorField)
        : sourceOfField(request.valueField);
    const bool contributing = value && reason.isEmpty();
    QVariantMap row{{QStringLiteral("well_id"), r.id}, {QStringLiteral("well_name"), r.name},
      {QStringLiteral("reason"), reason}, {QStringLiteral("source"), valueSource}, {QStringLiteral("contributing"), contributing}};
    if (value) row.insert(QStringLiteral("value"), *value);
    m_wellFactorRows << row;
    if (contributing) {
      QgsFeature f(samples.fields()); f.setGeometry(pointGeometry);
      QgsAttributes attributes(3); attributes[0] = r.id; attributes[1] = r.name; attributes[2] = *value;
      f.setAttributes(attributes); features << f;
    }
  }
  m_wellFactorMessage = tr("%1 口井可参与插值；%2 口井缺失（原因见表）。解释厚度采用 MD 层段。")
      .arg(features.size()).arg(source.rows.size() - features.size());
  if (features.isEmpty()) return fail(m_wellFactorMessage);
  samples.dataProvider()->addFeatures(features); samples.updateExtents();
  DerivedAssetRegistrar registrar(catalog(), projectDir());
  if (!registrar.isBound() || !layerService()) return fail(tr("井点提取尚未绑定工程服务"));
  QString err;
  const auto st = registrar.stage(QStringLiteral("well_factor_points"), tr("井点因子·%1·%2").arg(horizon, def.title),
                                  QStringLiteral("well_factors.gpkg"), &err);
  if (!st.isValid()) return fail(err);
  QgsVectorFileWriter::SaveVectorOptions options; options.driverName = QStringLiteral("GPKG"); options.layerName = QStringLiteral("samples");
  if (QgsVectorFileWriter::writeAsVectorFormatV3(&samples, st.absolutePath, QgsProject::instance()->transformContext(), options, &err)
      != QgsVectorFileWriter::NoError) return fail(err);
  const QString id = QStringLiteral("wellfactor.%1.%2").arg(horizon, factorId);
  QVariantMap extra = params;
  extra.insert(QStringLiteral("factor_id"), factorId); extra.insert(QStringLiteral("horizon"), horizon);
  extra.insert(QStringLiteral("sample_count"), features.size()); extra.insert(QStringLiteral("rows"), m_wellFactorRows);
  extra.insert(QStringLiteral("depth_domain"), QStringLiteral("MD")); extra.insert(QStringLiteral("layer_id"), id);
  extra.insert(QStringLiteral("manifest_layer_id"), id);
  extra.insert(QStringLiteral("layer_id"), QStringLiteral("product.%1").arg(st.versionId));
  extra.insert(QStringLiteral("mapping_product"), true); extra.insert(QStringLiteral("layer_type"), QStringLiteral("vector"));
  extra.insert(QStringLiteral("source_suffix"), QStringLiteral("|layername=samples"));
  extra.insert(QStringLiteral("title"), tr("井点因子·%1").arg(def.title));
  extra.insert(QStringLiteral("group"), QStringLiteral("04_SingleFactor/Samples"));
  extra.insert(QStringLiteral("kind"), QStringLiteral("well_factor_points"));
  if (!registrar.commit(st, registrar.parentVersionIdsFor(source.paths), source.paths.join(QLatin1Char(';')), extra, &err)) return fail(err);
  LayerDeclaration d; d.layerId = id; d.horizon = horizon; d.type = QStringLiteral("vector");
  d.source = st.absolutePath + QStringLiteral("|layername=samples"); d.group = QStringLiteral("04_SingleFactor/Samples");
  d.title = tr("井点因子·%1").arg(def.title);
  if (!layerService()->declare(d, &err)) return fail(err);
  if (pointsLayerId) *pointsLayerId = id;
  emit wellFactorsExtracted(horizon, factorId); return true;
}

bool ConstraintWorkflow::prepareFactorInputs(const QString &horizon, const QString &factorId,
                                              QVariantMap &params, QString *error) {
  if (!params.contains(QStringLiteral("factorMode")) || params.value(QStringLiteral("factorInputPrepared")).toBool()) return true;
  bool known = false; const auto def = SingleFactorRegistry::byId(factorId, &known);
  if (!known) { paleo::workflow_detail::setError(error, tr("未知单因素 id：%1").arg(factorId)); return false; }
  if (def.processingAlgId != QLatin1String("paleo:paleo_constraint_idw")) return true;
  const QString strategy = params.value(QStringLiteral("strategy_id")).toString();
  if (!strategy.isEmpty() && !paleo::singlefactor::surfaceMethodPack(strategy)) {
    paleo::workflow_detail::setError(error, tr("未知制图策略 id：%1").arg(strategy)); return false;
  }
  const QString method = params.value(QStringLiteral("method")).toString();
  if (!method.isEmpty() && method != QLatin1String("sgs") && !paleo::singlefactor::surfaceMethodPack(method)) {
    paleo::workflow_detail::setError(error, tr("未知单因素方法：%1").arg(method)); return false;
  }
  QString points;
  if (!extractWellFactors(horizon, factorId, params, error, &points)) return false;
  params.insert(QStringLiteral("extraction"), params);
  params.insert(QStringLiteral("pointsLayerId"), points);
  params.insert(QStringLiteral("field"), QStringLiteral("factor_value"));
  params.insert(QStringLiteral("valueField"), QStringLiteral("factor_value"));
  params.insert(QStringLiteral("factorMode"), QStringLiteral("direct"));
  params.remove(QStringLiteral("numeratorField")); params.remove(QStringLiteral("denominatorField"));
  params.insert(QStringLiteral("factorInputPrepared"), true);
  return true;
}
