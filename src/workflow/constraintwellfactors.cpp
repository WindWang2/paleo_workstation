// 层：功能
#include "workflows.h"
#include "constraintfactorjobs_internal.h"
#include "workflows_internal.h"
#include "wellsectionworkflow.h"
#include "algorithms/singlefactor/wellacquisition.h"
#include "domain/litholexicon.h"
#include "domain/mappinghorizons.h"
#include "domain/singlefactorstrategy.h"
#include "services/projectdata.h"
#include "services/singlefactordef.h"
#include "derivedassets.h"
#include "qgis/wellattributestore.h"
#include <QSet>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QThread>
#include "services/paleotaskservice.h"
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsfield.h>
#include <qgsgeometry.h>
#include <qgsvectordataprovider.h>
#include <qgsvectorfilewriter.h>
#include <qgsvectorlayer.h>
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
  bool maintainedThickness = false, maintainedSand = false;
};
struct Source {
  QVector<Input> rows;
  QStringList paths;
  QgsCoordinateReferenceSystem crs;
  bool hasMapSource = false;
};

// 同类重叠合并；跨岩性重叠拒绝。覆盖不足不把未解释区间当泥岩。
std::optional<double> sandIn(const QVector<wellsection::LithoSegment> &segments,
                             double top, double base, QString *reason,
                             const paleo::domain::LithoLexicon &lexicon = paleo::domain::LithoLexicon::defaultLexicon(),
                             QStringList *auditRules = nullptr) {
  struct Span { double a, b; bool sand; };
  QVector<Span> spans;
  for (const auto &s : segments) {
    if (s.source != wellsection::LithoSource::Interpreted ||
        !std::isfinite(s.topMd) || !std::isfinite(s.baseMd) || s.baseMd <= s.topMd)
      continue;
    const double a = std::max(top, s.topMd), b = std::min(base, s.baseMd);
    if (b <= a) continue;
    const auto cl = lexicon.classify(s.litho);
    if (cl.isKnown) {
      spans.push_back({a, b, cl.isSand});
      if (auditRules && !cl.matchedPattern.isEmpty()) {
        const QString ruleDesc = QStringLiteral("%1→%2").arg(s.litho.trimmed(), cl.groupName);
        if (!auditRules->contains(ruleDesc))
          auditRules->append(ruleDesc);
      }
    }
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

Source sourceFor(ConstraintWorkflow *wf, const QString &horizon, const QVariantMap &params,
                 QString *error, const Source *captured = nullptr,
                 const QVariantList *factorRows = nullptr,
                 const std::function<bool()> &cancelled = {}, bool captureOnly = false) {
  Source out = captured ? *captured : Source{};
  if (!captured)
    out.crs = QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt());
  auto *layers = wf->layerService();
  const QString pointsId = params.value(QStringLiteral("pointsLayerId"),
      paleo::constraint_detail::wellsLayerIdFor(layers, horizon)).toString();
  if (!captured && !pointsId.isEmpty() && layers) {
    out.hasMapSource = true;
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
  if (captureOnly)
    return out;
  if (!wf->catalog()) return out;
  QHash<QString, QVariantList> maintainedByWell;
  for (const auto &v :
       (factorRows ? *factorRows
                   : WellAttributeStore::rows(wf->projectDir(), layers, true, {}, horizon)))
    maintainedByWell[v.toMap().value("well_id").toString()] << v;
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
  // 岩性词表是循环不变量（projectDir/catalog 在整轮里不变）：提前一次构造。
  // 原实现逐井调 fromProject——W 口井 = W×(读盘+JSON 解析+两遍默认词表重建)。
  const paleo::domain::LithoLexicon lex = metrics
      ? paleo::domain::LithoLexicon::fromProject(wf->projectDir(), wf->catalog())
      : paleo::domain::LithoLexicon::defaultLexicon();
  QHash<QString, int> byId, byName;
  for (int i = 0; i < out.rows.size(); ++i) {
    if (!byId.contains(out.rows[i].id))
      byId.insert(out.rows[i].id, i);
    const auto name = DataCatalog::normalizeWellName(out.rows[i].name);
    if (!byName.contains(name))
      byName.insert(name, i);
  }
  for (const ProjectWell &well : wells) {
    if (cancelled && cancelled()) {
      if (error)
        *error = ConstraintWorkflow::tr("提取已取消");
      return {};
    }
    int index = byId.value(well.id, byName.value(DataCatalog::normalizeWellName(well.name), -1));
    if (index < 0) {
      Input row; row.id = well.id; row.name = well.name;
      // 无地图源时 catalog 井位属于工程局部米坐标；已有地图源时缺井不猜 CRS。
      if (!out.hasMapSource &&
          (well.coordinateStatus == QLatin1String("ok") ||
           well.coordinateStatus == QLatin1String("untransformed")) &&
          std::isfinite(well.surfaceX) && std::isfinite(well.surfaceY))
        row.geometry = QgsGeometry::fromPointXY(QgsPointXY(well.surfaceX, well.surfaceY));
      out.rows << row;
      index = out.rows.size() - 1;
    }
    Input &row = out.rows[index];
    row.id = well.id;
    row.name = well.name;
    byId.insert(well.id, index);
    byName.insert(DataCatalog::normalizeWellName(well.name), index);
    const auto maintained = maintainedByWell.value(well.id);
    if (maintained.size() > 1) {
      if (error) *error = ConstraintWorkflow::tr("井点属性表中同井/层位存在重复行，请先修正");
      return {};
    }
    if (!maintained.isEmpty()) {
      const auto values = maintained.first().toMap();
      for (auto i = values.cbegin(); i != values.cend(); ++i)
        if (i.key() != QLatin1String("well_id") && i.key() != QLatin1String("well_name") && i.key() != QLatin1String("horizon"))
          row.attributes.insert(i.key(), i.value());
      row.maintainedThickness = values.contains(layerThickness);
      row.maintainedSand = values.contains(sandThickness);
      row.sandSource = ConstraintWorkflow::tr("井点单因素属性表");
    }
    if (!metrics) continue;
    double top = qQNaN(), base = qQNaN();
    for (const WellTop &t : data.topsFor(well.id)) {
      if (t.horizon == horizon) top = t.md;
      if (t.horizon == baseName) base = t.md;
    }
    row.thicknessReason = ConstraintWorkflow::tr("缺少顶/底分层 MD（%1→%2）").arg(horizon, baseName);
    row.sandReason = row.thicknessReason;
    if (std::isfinite(top) && std::isfinite(base) && base > top) {
      if (!row.maintainedThickness) row.attributes.insert(layerThickness, base - top);
      row.thicknessReason.clear();
      const auto segs = lithologies.value(well.id);
      QStringList auditRules;
      const auto sand = sandIn(segs, top, base, &row.sandReason, lex, &auditRules);
      if (sand) {
        if (!row.maintainedSand) row.attributes.insert(sandThickness, *sand);
        row.sandReason.clear();
        QStringList sources;
        for (const auto &s : segs) if (!s.provenance.isEmpty() && !sources.contains(s.provenance)) sources << s.provenance;
        QString sourceBase = sources.isEmpty() ? ConstraintWorkflow::tr("工程解释岩性 / 岩屑录井（MD）") : sources.join(QStringLiteral("; "));
        if (!auditRules.isEmpty()) {
          sourceBase += QStringLiteral(" [词表：%1]").arg(auditRules.join(QStringLiteral(", ")));
        }
        if (!row.maintainedSand) row.sandSource = sourceBase;
      } else if (!lithologyWarnings.isEmpty()) row.sandReason += QStringLiteral("; ") + lithologyWarnings.join(QStringLiteral("; "));
    } else if (std::isfinite(top) && std::isfinite(base)) {
      row.thicknessReason = ConstraintWorkflow::tr("分层倒置或层厚为零"); row.sandReason = row.thicknessReason;
    }
    if (row.maintainedThickness) row.thicknessReason = ConstraintWorkflow::tr("维护的层厚为空或不是有限数值");
    if (row.maintainedSand) row.sandReason = ConstraintWorkflow::tr("维护的砂厚为空或不是有限数值");
    for (const auto &link : wf->catalog()->linksForEntity(well.id))
      if (!link.unresolved && (link.role == QLatin1String("tops") || (sandMetric && (link.role == QLatin1String("interpretation") ||
                              link.role == QLatin1String("cuttings"))))) {
        const auto v = wf->catalog()->currentVersion(link.assetId);
        if (!v.id.isEmpty()) out.paths << DataCatalog::resolvedVersionPath(wf->projectDir(), v);
      }
  }
  if (layers && !maintainedByWell.isEmpty())
    for (const auto &declaration : layers->declared())
      if (declaration.layerId == WellAttributeStore::factorLayerId()) out.paths << declaration.source.section('|', 0, 0);
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
  watchWellFactorAttributes();
  const Source source = sourceFor(this, horizon, {}, error, nullptr, nullptr, {}, true);
  QSet<QString> keys;
  for (const Input &r : source.rows)
    for (auto i = r.attributes.cbegin(); i != r.attributes.cend(); ++i)
      if (paleo::singlefactor::parseNumeric(i.value())) keys.insert(i.key());
  const auto maintained = WellAttributeStore::rows(projectDir(), layerService(), true, {}, horizon);
  for (const auto &row : maintained) {
    const auto values = row.toMap();
    for (auto it = values.cbegin(); it != values.cend(); ++it)
      if (paleo::singlefactor::parseNumeric(it.value()))
        keys.insert(it.key());
  }
  keys.insert(layerThickness);
  keys.insert(sandThickness);
  QStringList names = keys.values(); names.sort(Qt::CaseInsensitive);
  QVariantList fields;
  for (const QString &name : names) {
    QString label = name;
    if (name == layerThickness) label = tr("分层层厚（MD，m）");
    if (name == sandThickness) label = tr("解释砂岩厚度（MD，m）");
    if (name == QLatin1String("sand_ratio")) label = tr("砂地比（属性，0–1）");
    fields << QVariantMap{{QStringLiteral("id"), name}, {QStringLiteral("label"), label}};
  }
  return fields;
}

namespace {
struct FactorExtraction {
  QVariantList rows;
  QString message, path;
  QStringList sourcePaths;
  DerivedFileFingerprint fingerprint;
  QVariantMap sourceFileStates;
  bool registered = false;
};
bool computeExtraction(const Source &source, const QVariantMap &params, FactorExtraction &result,
                       QString *error, const std::function<bool()> &cancelled = {}) {
  const auto failure = [&](const QString &message) {
    result.message = message;
    if (error)
      *error = message;
    return false;
  };
  paleo::singlefactor::WellAcquisitionRequest request;
  request.strictFields = true;
  request.factorMode = params.value(QStringLiteral("factorMode"), QStringLiteral("direct")).toString();
  request.valueField = params.value(QStringLiteral("valueField"), params.value(QStringLiteral("field"))).toString();
  request.numeratorField = params.value(QStringLiteral("numeratorField")).toString();
  request.denominatorField = params.value(QStringLiteral("denominatorField")).toString();
  if (request.factorMode != QLatin1String("direct") && request.factorMode != QLatin1String("ratio"))
    return failure(ConstraintWorkflow::tr("未知因子提取口径"));
  if ((request.factorMode == QLatin1String("direct") && request.valueField.isEmpty()) ||
      (request.factorMode == QLatin1String("ratio") && (request.numeratorField.isEmpty() || request.denominatorField.isEmpty())))
    return failure(ConstraintWorkflow::tr("请选择因子提取字段"));
  QgsVectorLayer samples(QStringLiteral("Point"), QStringLiteral("well_factors"), QStringLiteral("memory"));
  samples.setCrs(source.crs);
  samples.dataProvider()->addAttributes({QgsField(QStringLiteral("well_id"), QMetaType::Type::QString),
      QgsField(QStringLiteral("well_name"), QMetaType::Type::QString), QgsField(QStringLiteral("factor_value"), QMetaType::Type::Double)});
  samples.updateFields();
  QgsFeatureList features;
  for (const Input &r : source.rows) {
    if (cancelled && cancelled())
      return failure(ConstraintWorkflow::tr("提取已取消"));
    paleo::singlefactor::FeatureAttributes attrs;
    for (auto i = r.attributes.cbegin(); i != r.attributes.cend(); ++i) attrs.emplace_back(i.key(), i.value());
    const auto value = paleo::singlefactor::resolveCurrentFactorValue(attrs, request);
    QString reason;
    if (request.factorMode == QLatin1String("ratio")) {
      const auto numerator = paleo::singlefactor::parseNumeric(r.attributes.value(request.numeratorField));
      const auto denominator = paleo::singlefactor::parseNumeric(r.attributes.value(request.denominatorField));
      if (!numerator) reason = fieldReason(r, request.numeratorField);
      else if (!denominator) reason = fieldReason(r, request.denominatorField);
      else if (*denominator == 0)
        reason = ConstraintWorkflow::tr("分母为零");
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
    if (!located)
      reason += (reason.isEmpty() ? QString() : QStringLiteral("; ")) +
                ConstraintWorkflow::tr("缺少有效井点坐标");
    if (!value && reason.isEmpty())
      reason = ConstraintWorkflow::tr("比值不是有限数值");
    // 砂地比物理域门（SINGLE_FACTOR.md：砂厚/层厚 ∈ [0,1]）：分子分母正是
    // 解释/维护砂厚与层厚时，越界说明两字段矛盾——属性表单字段校验只挡
    // 各自非负与 sand_ratio ≤1，维护值组合（如砂厚 25/层厚 20）可绕过。
    // 该井不参与插值并按井注明原因，不静默裁剪、不带病插值；任意其他
    // 分子/分母字段组合无 0–1 物理域承诺，不设此门。
    if (value && reason.isEmpty() && request.factorMode == QLatin1String("ratio") &&
        request.numeratorField == sandThickness && request.denominatorField == layerThickness &&
        (*value < -1e-9 || *value > 1.0 + 1e-9)) {
      reason = ConstraintWorkflow::tr("砂地比超出 [0,1]（值 %1，砂厚与层厚数据矛盾）")
                   .arg(QString::number(*value, 'g', 6));
    }
    const auto sourceOfField = [&](const QString &field) {
      if (field == layerThickness)
        return r.maintainedThickness ? ConstraintWorkflow::tr("井点单因素属性表")
                                     : ConstraintWorkflow::tr("工程分层（MD）");
      if (field == sandThickness)
        return r.sandSource;
      return ConstraintWorkflow::tr("井点属性 · %1").arg(field);
    };
    const QString valueSource = request.factorMode == QLatin1String("ratio")
                                    ? sourceOfField(request.numeratorField) +
                                          ConstraintWorkflow::tr(" ÷ ") +
                                          sourceOfField(request.denominatorField)
                                    : sourceOfField(request.valueField);
    const bool contributing = value && reason.isEmpty();
    QVariantMap row{{QStringLiteral("well_id"), r.id}, {QStringLiteral("well_name"), r.name},
      {QStringLiteral("reason"), reason}, {QStringLiteral("source"), valueSource}, {QStringLiteral("contributing"), contributing}};
    if (value) row.insert(QStringLiteral("value"), *value);
    result.rows << row;
    if (contributing) {
      QgsFeature f(samples.fields()); f.setGeometry(pointGeometry);
      QgsAttributes attributes(3); attributes[0] = r.id; attributes[1] = r.name; attributes[2] = *value;
      f.setAttributes(attributes); features << f;
    }
  }
  result.message =
      ConstraintWorkflow::tr("%1 口井可参与插值；%2 口井缺失（原因见表）。解释厚度采用 MD 层段。")
          .arg(features.size())
          .arg(source.rows.size() - features.size());
  if (features.isEmpty())
    return failure(result.message);
  samples.dataProvider()->addFeatures(features); samples.updateExtents();
  QgsVectorFileWriter::SaveVectorOptions options;
  options.driverName = QStringLiteral("GPKG");
  options.layerName = QStringLiteral("samples");
  // Worker owns both source and destination; no live QgsProject transform context.
  if (QgsVectorFileWriter::writeAsVectorFormatV3(&samples, result.path,
                                                 QgsCoordinateTransformContext(), options,
                                                 error) != QgsVectorFileWriter::NoError)
    return false;
  result.sourcePaths = source.paths;
  result.fingerprint = DerivedFileFingerprint::read(result.path, cancelled, error);
  return !result.fingerprint.sha256.isEmpty();
}
} // namespace

namespace {
QVariantMap extractionParams(const QVariantMap &params) {
  QVariantMap result;
  for (const auto &key :
       {"factorMode", "valueField", "numeratorField", "denominatorField", "baseHorizon"})
    if (params.contains(key))
      result.insert(QString::fromLatin1(key), params.value(key));
  if (!result.contains("valueField") && params.contains("field"))
    result.insert("valueField", params.value("field"));
  return result;
}
} // namespace

QString ConstraintWorkflow::preparedWellFactorVersion(const QString &horizon,
                                                      const QString &factorId,
                                                      const QVariantMap &params) const {
  if (m_wellFactorAttributeLayer && m_wellFactorAttributeLayer->isModified()) return {};
  if (!catalog() || !layerService() || m_preparedWellFactors.value("horizon") != horizon ||
      m_preparedWellFactors.value("factor_id") != factorId ||
      m_preparedWellFactors.value("params").toMap() != extractionParams(params))
    return {};
  const auto v = catalog()->versionById(m_preparedWellFactors.value("version_id").toString());
  if (v.id.isEmpty() || !QFile::exists(DataCatalog::resolvedVersionPath(projectDir(), v)))
    return {};
  for (const auto &parent : v.parentVersionIds) {
    const auto input = catalog()->versionById(parent);
    if (input.id.isEmpty() || catalog()->currentVersion(input.assetId).id != parent)
      return {};
  }
  const auto states = v.extra.value("source_file_states").toMap();
  for (auto it = states.cbegin(); it != states.cend(); ++it) {
    const QFileInfo file(QDir(projectDir()).absoluteFilePath(it.key()));
    const auto state = it.value().toMap();
    if (!file.exists() || file.size() != state.value("size").toLongLong() ||
        file.lastModified().toMSecsSinceEpoch() != state.value("modified").toLongLong())
      return {};
  }
  for (const auto &d : layerService()->declared())
    if (d.layerId == m_preparedWellFactors.value("points_layer_id").toString() &&
        d.source.section('|', 0, 0) == DataCatalog::resolvedVersionPath(projectDir(), v))
      return v.id;
  return {};
}

void ConstraintWorkflow::restoreWellFactors(const QString &horizon, const QString &factorId,
                                            const QVariantMap &params) {
  if (factorBusy() || !catalog() || !layerService())
    return;
  if (!preparedWellFactorVersion(horizon, factorId, params).isEmpty())
    return;
  // Reopen and factor/horizon switching reuse the saved snapshot only while its inputs are current.
  for (const auto &asset : catalog()->assets()) {
    if (asset.type != QLatin1String("well_factor_points"))
      continue;
    const auto v = catalog()->currentVersion(asset.id);
    if (v.extra.value("horizon") != horizon || v.extra.value("factor_id") != factorId ||
        extractionParams(v.extra) != extractionParams(params))
      continue;
    const auto old = m_preparedWellFactors;
    m_preparedWellFactors = {
        {"horizon", horizon},
        {"factor_id", factorId},
        {"params", extractionParams(params)},
        {"version_id", v.id},
        {"points_layer_id", QStringLiteral("wellfactor.%1.%2").arg(horizon, factorId)}};
    if (preparedWellFactorVersion(horizon, factorId, params).isEmpty()) {
      m_preparedWellFactors = old;
      continue;
    }
    m_wellFactorRows = v.extra.value("rows").toList();
    m_wellFactorMessage =
        tr("已读取本层位已保存的井点数据 v%1；缺失原因见逐井表。").arg(v.versionNumber);
    return;
  }
}

bool ConstraintWorkflow::startWellFactorExtraction(const QString &horizon, const QString &factorId,
                                                   const QVariantMap &params, QString *error) {
  return extractWellFactorsImpl(horizon, factorId, params, error, false);
}

bool ConstraintWorkflow::extractWellFactorsImpl(const QString &horizon, const QString &factorId,
                                                const QVariantMap &params, QString *error,
                                                bool synchronous) {
  if (factorBusy()) {
    if (error)
      *error = tr("已有单因素任务，请在任务中心查看进度");
    return false;
  }
  if (!catalog() || !layerService()) {
    if (error)
      *error = tr("请先打开工程");
    return false;
  }
  bool known = false;
  const auto def = SingleFactorRegistry::byId(factorId, &known);
  if (!known) {
    if (error)
      *error = tr("未知单因素 id：%1").arg(factorId);
    return false;
  }
  if (m_wellFactorAttributeLayer && m_wellFactorAttributeLayer->isModified()) {
    if (error) *error = tr("请先保存或撤销井点属性修改，再提取数据");
    return false;
  }
  m_preparedWellFactors.clear();
  m_wellFactorRows.clear();
  m_wellFactorMessage.clear();
  QString err;
  const Source captured = sourceFor(this, horizon, params, &err, nullptr, nullptr, {}, true);
  if (!err.isEmpty()) {
    if (error)
      *error = err;
    return false;
  }
  const auto maintained = WellAttributeStore::rows(projectDir(), layerService(), true, {}, horizon);
  const auto dir = projectDir();
  auto snapshot = std::shared_ptr<DataCatalog>(catalog()->createStagingCopy(dir));
  // Preserve the editable factors table as an explicit input even with detached layers.
  Source frozen = captured;
  for (const auto &d : layerService()->declared())
    if (d.layerId == WellAttributeStore::factorLayerId())
      frozen.paths << d.source.section('|', 0, 0);
  DerivedAssetRegistrar registrar(catalog(), dir);
  const auto st = registrar.stage(QStringLiteral("well_factor_points"), tr("井点因子·%1·%2").arg(horizon, def.title),
                                  QStringLiteral("well_factors.gpkg"), &err);
  if (!st.isValid()) {
    if (error)
      *error = err;
    return false;
  }
  auto result = std::make_shared<FactorExtraction>();
  result->path = st.absolutePath;
  for (const auto &path : frozen.paths) {
    const QFileInfo file(path);
    if (!file.isFile())
      continue;
    result->sourceFileStates.insert(
        QDir(dir).relativeFilePath(path),
        QVariantMap{{"size", file.size()}, {"modified", file.lastModified().toMSecsSinceEpoch()}});
  }
  auto publish = [this, result, st, horizon, factorId, params, def](QString *publishError) {
    DerivedAssetRegistrar registrar(catalog(), projectDir());
    QString err;
    for (auto it = result->sourceFileStates.cbegin(); it != result->sourceFileStates.cend(); ++it) {
      const QFileInfo file(QDir(projectDir()).absoluteFilePath(it.key()));
      const auto state = it.value().toMap();
      if (!file.exists() || file.size() != state.value("size").toLongLong() ||
          file.lastModified().toMSecsSinceEpoch() != state.value("modified").toLongLong()) {
        if (publishError)
          *publishError = tr("井点来源在提取期间已变更，请重新提取");
        return false;
      }
    }
    const QString id = QStringLiteral("wellfactor.%1.%2").arg(horizon, factorId);
    QVariantMap extra = params;
    extra.insert(QStringLiteral("factor_id"), factorId);
    extra.insert(QStringLiteral("horizon"), horizon);
    extra.insert(
        QStringLiteral("sample_count"),
        result->rows.size() -
            std::count_if(result->rows.cbegin(), result->rows.cend(), [](const QVariant &row) {
              return !row.toMap().value("contributing").toBool();
            }));
    extra.insert(QStringLiteral("rows"), result->rows);
    extra.insert(QStringLiteral("depth_domain"), QStringLiteral("MD"));
    extra.insert(QStringLiteral("layer_id"), id);
    extra.insert(QStringLiteral("manifest_layer_id"), id);
    extra.insert(QStringLiteral("layer_id"), QStringLiteral("product.%1").arg(st.versionId));
    extra.insert(QStringLiteral("mapping_product"), true);
    extra.insert(QStringLiteral("layer_type"), QStringLiteral("vector"));
    extra.insert(QStringLiteral("source_suffix"), QStringLiteral("|layername=samples"));
    extra.insert(QStringLiteral("title"), tr("井点因子·%1").arg(def.title));
    extra.insert(QStringLiteral("group"), QStringLiteral("04_SingleFactor/Samples"));
    extra.insert(QStringLiteral("kind"), QStringLiteral("well_factor_points"));
    extra.insert(QStringLiteral("source_file_states"), result->sourceFileStates);
    if (!registrar.commit(st, registrar.parentVersionIdsFor(result->sourcePaths),
                          result->sourcePaths.join(QLatin1Char(';')), extra, &err,
                          &result->fingerprint)) {
      if (publishError)
        *publishError = err;
      return false;
    }
    result->registered = true;
    LayerDeclaration d;
    d.layerId = id;
    d.horizon = horizon;
    d.type = QStringLiteral("vector");
    d.source = st.absolutePath + QStringLiteral("|layername=samples");
    d.group = QStringLiteral("04_SingleFactor/Samples");
    d.title = tr("井点因子·%1").arg(def.title);
    if (!layerService()->declare(d, &err)) {
      if (publishError)
        *publishError = err;
      return false;
    }
    m_wellFactorRows = result->rows;
    m_wellFactorMessage = result->message + tr(" 井点结果已保存为 v%1。").arg(st.versionNumber);
    m_preparedWellFactors = {{"horizon", horizon},
                             {"factor_id", factorId},
                             {"params", extractionParams(params)},
                             {"version_id", st.versionId},
                             {"points_layer_id", id}};
    if (m_factorJobTask && m_factorJobTask->running())
      m_factorJobTask->setNavigationContext(
          {{"workspace", "constraint"}, {"asset_id", st.assetId}, {"version_id", st.versionId}});
    emit wellFactorsExtracted(horizon, factorId);
    return true;
  };

  auto compute = [snapshot, frozen, maintained, dir, horizon, params, result](PaleoTask *task) {
    snapshot->moveToThread(QThread::currentThread());
    ConstraintWorkflow detached(nullptr, nullptr);
    detached.setCatalog(snapshot.get(), dir);
    const auto cancelled = [task] { return task && task->cancelRequested(); };
    if (task)
      task->reportStage("parse", 5);
    QString err;
    const auto source =
        sourceFor(&detached, horizon, params, &err, &frozen, &maintained, cancelled);
    if (!err.isEmpty())
      return err;
    if (task)
      task->reportStage("build", 60);
    if (!computeExtraction(source, params, *result, &err, cancelled))
      return err.isEmpty() ? tr("井点提取失败") : err;
    if (task)
      task->reportStage("hash", 95);
    return QString();
  };
  if (synchronous) {
    const auto failure = compute(nullptr);
    if (!failure.isEmpty()) {
      QFile::remove(result->path);
      if (error)
        *error = failure;
      return false;
    }
    const bool ok = publish(error);
    if (!ok && !result->registered)
      QFile::remove(result->path);
    return ok;
  }
  auto failureConnection = std::make_shared<QMetaObject::Connection>();
  *failureConnection = connect(this, &ConstraintWorkflow::factorJobFailed, this,
                               [this, result, horizon, factorId](const QString &message) {
                                 m_wellFactorRows = result->rows;
                                 m_wellFactorMessage = message;
                                 emit wellFactorsExtracted(horizon, factorId);
                               });
  auto endConnection = std::make_shared<QMetaObject::Connection>();
  *endConnection = connect(this, &ConstraintWorkflow::factorJobStateChanged, this,
                           [this, failureConnection, endConnection](bool busy) {
                             if (!busy) {
                               disconnect(*failureConnection);
                               disconnect(*endConnection);
                             }
                           });
  const bool ok = runFactorWork(
      tr("%1 · 提取%2井点数据").arg(horizon, def.title), compute, publish,
      [result] {
        if (!result->registered)
          QFile::remove(result->path);
      },
      error);
  if (!ok || !m_factorTasks) {
    disconnect(*failureConnection);
    disconnect(*endConnection);
  }
  return ok;
}

bool ConstraintWorkflow::extractWellFactors(const QString &horizon, const QString &factorId,
                                            const QVariantMap &params, QString *error,
                                            QString *pointsLayerId) {
  // Legacy synchronous callers share the same capture/compute/publish path.
  const bool ok = extractWellFactorsImpl(horizon, factorId, params, error, true);
  if (ok && pointsLayerId)
    *pointsLayerId = m_preparedWellFactors.value("points_layer_id").toString();
  return ok;
}

void ConstraintWorkflow::watchWellFactorAttributes() {
  auto *layer = WellAttributeStore::open(catalog(), projectDir(), layerService(), true, false);
  if (!layer || layer == m_wellFactorAttributeLayer) return;
  if (m_wellFactorAttributeLayer) disconnect(m_wellFactorAttributeLayer, nullptr, this, nullptr);
  m_wellFactorAttributeLayer = layer;
  connect(layer, &QgsVectorLayer::afterCommitChanges, this, [this] {
    m_preparedWellFactors.clear();
    m_wellFactorRows.clear();
    m_wellFactorMessage.clear();
    emit wellAttributesChanged();
  });
}

bool ConstraintWorkflow::maintainWellFactors(const QString &horizon, QString *error) {
  if (horizon.isEmpty()) { if (error) *error = tr("请先选择层位"); return false; }
  if (catalog() && catalog()->refusesWrites()) {
    if (!WellAttributeStore::open(catalog(), projectDir(), layerService(), true, false, error)) return false;
    emit wellAttributeTableRequested(WellAttributeStore::factorLayerId()); return true;
  }
  if (factorBusy()) {
    if (error)
      *error = tr("已有单因素任务");
    return false;
  }
  if (!catalog() || !layerService()) {
    if (error)
      *error = tr("请先打开工程");
    return false;
  }
  QString captureError;
  const Source captured = sourceFor(this, horizon, {}, &captureError, nullptr, nullptr, {}, true);
  if (!captureError.isEmpty()) {
    if (error)
      *error = captureError;
    return false;
  }
  const auto maintained = WellAttributeStore::rows(projectDir(), layerService(), true, {}, horizon);
  auto snapshot = std::shared_ptr<DataCatalog>(catalog()->createStagingCopy(projectDir()));
  const auto dir = projectDir();
  auto source = std::make_shared<Source>();
  auto publish = [this, source, horizon](QString *error) {
    QVariantList rows;
    for (const auto &r : source->rows) {
      if (!catalog() || catalog()->entityById(r.id).entityType != QLatin1String("well"))
        continue;
      QVariantMap row{{"well_id", r.id}, {"horizon", horizon}};
      for (const auto &field : {layerThickness, sandThickness, QStringLiteral("sand_ratio"),
                                QStringLiteral("porosity"), QStringLiteral("permeability")})
        if (r.attributes.contains(field))
          row.insert(field, r.attributes.value(field));
      const auto sand = paleo::singlefactor::parseNumeric(row.value(sandThickness));
      const auto thickness = paleo::singlefactor::parseNumeric(row.value(layerThickness));
      if (!row.contains("sand_ratio") && sand && thickness && *thickness > 0)
        row.insert("sand_ratio", *sand / *thickness);
      rows << row;
    }
    if (!WellAttributeStore::seedFactors(catalog(), projectDir(), layerService(), rows, error))
      return false;
    if (!WellAttributeStore::open(catalog(), projectDir(), layerService(), true, true, error))
      return false;
    watchWellFactorAttributes();
    emit wellAttributesChanged();
    emit wellAttributeTableRequested(WellAttributeStore::factorLayerId());
    return true;
  };

  return runFactorWork(
      tr("%1 · 获取并维护砂厚、层厚与砂地比").arg(horizon),
      [snapshot, captured, maintained, dir, horizon, source](PaleoTask *task) {
        snapshot->moveToThread(QThread::currentThread());
        ConstraintWorkflow detached(nullptr, nullptr);
        detached.setCatalog(snapshot.get(), dir);
        if (task)
          task->reportStage("parse", 5);
        QString error;
        *source = sourceFor(&detached, horizon, {}, &error, &captured, &maintained,
                            [task] { return task && task->cancelRequested(); });
        if (task)
          task->reportStage("build", 95);
        return error;
      },
      publish, {}, error);
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
  QString version = preparedWellFactorVersion(horizon, factorId, params);
  QString points = m_preparedWellFactors.value("points_layer_id").toString();
  if (version.isEmpty()) {
    if (m_factorTasks) {
      if (error)
        *error = tr("请先提取并保存当前地层的井点数据，再计算等值面");
      return false;
    }
    if (!extractWellFactors(horizon, factorId, params, error, &points))
      return false;
    version = preparedWellFactorVersion(horizon, factorId, params);
  }
  auto parents = params.value("parentVersionIds").toStringList();
  parents << version;
  parents.removeDuplicates();
  params.insert("parentVersionIds", parents);
  params.insert(QStringLiteral("extraction"), params);
  params.insert(QStringLiteral("pointsLayerId"), points);
  params.insert(QStringLiteral("field"), QStringLiteral("factor_value"));
  params.insert(QStringLiteral("valueField"), QStringLiteral("factor_value"));
  params.insert(QStringLiteral("factorMode"), QStringLiteral("direct"));
  params.remove(QStringLiteral("numeratorField")); params.remove(QStringLiteral("denominatorField"));
  params.insert(QStringLiteral("factorInputPrepared"), true);
  return true;
}

QVariantList ConstraintWorkflow::savedFactorResults(const QString &horizon,
                                                    const QString &factorId) const {
  QVariantList rows;
  if (!catalog() || horizon.isEmpty() || factorId.isEmpty())
    return rows;
  for (const auto &v : catalog()->versions()) {
    const auto extra = v.extra;
    const auto kind = extra.value("kind").toString();
    const auto factorLayer = extra.value("factor_layer_id").toString();
    if (extra.value("horizon") != horizon ||
        (extra.value("factor_id") != factorId &&
         factorLayer != QStringLiteral("factor.%1.%2").arg(horizon, factorId)))
      continue;
    QString type;
    if (kind == QLatin1String("well_factor_points"))
      type = tr("井点数据");
    else if (kind == QLatin1String("single_factor_raster"))
      type = tr("等值面");
    else if (kind == QLatin1String("contour_lines"))
      type = tr("等值线");
    else if (kind == QLatin1String("single_factor_cartographic_contour"))
      type = tr("解释性等值线");
    else
      continue;
    rows << QVariantMap{{"type", type},
                        {"version_id", v.id},
                        {"asset_id", v.assetId},
                        {"version", v.versionNumber},
                        {"path", DataCatalog::resolvedVersionPath(projectDir(), v)},
                        {"parents", v.parentVersionIds}};
  }
  std::stable_sort(rows.begin(), rows.end(), [](const QVariant &a, const QVariant &b) {
    return a.toMap().value("version").toInt() > b.toMap().value("version").toInt();
  });
  return rows;
}
