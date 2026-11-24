// 层：数据
// services/capabilityspec.cpp — 见 capabilityspec.h。算法参数仍只读 AlgorithmSpec。
#include "capabilityspec.h"
#include "pythoncapabilityspec.h"
#include "pythonanalysiscapabilityspec.h"
#include "pythonanalysis.h"
#include "pythongeneratedspec.h"
#include "pythonnumerictablespec.h"
#include "pythonseismictracespec.h"
#include "pythontableoutputspec.h"
#include "pythongeneratedoutputspec.h"
#include "geostaticapabilityspec.h"

namespace {

CapabilitySpec make(const QString &id, CapabilityKind kind, const QString &algorithmId, CapabilityExposure exposure,
                    const QString &reason, const QStringList &aliases, const QStringList &shapes) {
  CapabilitySpec spec;
  spec.capabilityId = id;
  spec.contractVersion = 1;
  spec.kind = kind;
  spec.algorithmId = algorithmId;
  spec.algorithmContractVersion = 1;
  spec.exposure = exposure;
  spec.exposureReason = reason;
  spec.aliases = aliases;
  spec.operatorShapes = shapes;
  spec.publishEffect = exposure == CapabilityExposure::Callable ? QStringLiteral("optional") : QStringLiteral("none");
  spec.cancelSupported = exposure == CapabilityExposure::Callable;
  spec.resumeSupported = false;
  spec.restartable = true;
  return spec;
}

CapabilitySpec wellFaciesAdapter() {
  auto spec=make(QStringLiteral("wellfacies.predict"),CapabilityKind::Compute,{},CapabilityExposure::LegacyAdapter,
    QStringLiteral("typed v2 external adapter; resume original durable job or cached result only; native DAG unavailable"),
    {QStringLiteral("paleo.well_facies_prediction")},{});
  spec.contractVersion=2;
  spec.cancelSupported=false; // Local cancellation cannot prove rollback at an external endpoint.
  spec.resumeSupported=true;
  spec.restartable=false;
  return spec;
}

const QVector<CapabilitySpec> &table() {
  static const QVector<CapabilitySpec> specs = {
      make(QStringLiteral("depth.convert_batch"), CapabilityKind::Compute,
           QStringLiteral("native:depth_batch_convert"), CapabilityExposure::Callable,
           QStringLiteral("well-sample batch over VelocityModel; raster grid keeps its own policy"),
           {QStringLiteral("paleo.depth_convert_batch")}, {QStringLiteral("batch")}),
      make(QStringLiteral("seismic.attributes.compute"), CapabilityKind::Compute,
           QStringLiteral("native:seismic_attribute_group"), CapabilityExposure::Callable,
           QStringLiteral("RMS, envelope and coherence share one owned buffer"),
           {QStringLiteral("paleo.seismic_attribute_group")},
           {QStringLiteral("rms:finite-halo"), QStringLiteral("envelope:whole-trace"),
            QStringLiteral("coherence:finite-halo")}),
      make(QStringLiteral("constraint.idw"), CapabilityKind::Compute, QStringLiteral("paleo:paleo_constraint_idw"),
           CapabilityExposure::Callable,
           QStringLiteral("parameter contract is shared; the numeric kernel remains ConstraintIDWAlgorithm"),
           {QStringLiteral("paleo.constraint_idw")}, {QStringLiteral("global")}),
      make(QStringLiteral("facies.draft.publish"), CapabilityKind::Command,
           QStringLiteral("paleo:facies_draft_mapping"), CapabilityExposure::Internal,
           QStringLiteral("output-set spec is shared; agent execution is not opened"), {},
           {QStringLiteral("global")}),
      make(QStringLiteral("singlefactor.contours"), CapabilityKind::Compute, QStringLiteral("native:factor_contour"),
           CapabilityExposure::Callable, QStringLiteral("reads an issued raster result and extracts fixed-level contours"),
           {}, {QStringLiteral("global")}),
      // I1 地统计原子：引用 geostat v2 精确算法契约（显式变差参数；旧 v1
      // 保持 UI 语义不动）。工厂与语义见 services/geostaticapabilityspec。
      paleo::geostat_atom::krigingCapabilitySpec(),
      paleo::geostat_atom::sgsCapabilitySpec(),
      wellFaciesAdapter(),
      paleo::python::summaryCapabilitySpec(),
      paleo::python::analysisCapabilitySpec(paleo::python::AnalysisKind::WellQuality),
      paleo::python::analysisCapabilitySpec(paleo::python::AnalysisKind::AttributeCorrelation),
      paleo::python::generatedCapabilitySpec(),
      paleo::python::numericTableCapabilitySpec(),
      paleo::python::seismicTraceCapabilitySpec(),
      paleo::python::tableOutputsCapabilitySpec(),
      paleo::python::generatedOutputsCapabilitySpec(),
  };
  return specs;
}

} // namespace

QString capabilityExposureName(CapabilityExposure exposure) {
  switch (exposure) {
  case CapabilityExposure::Internal:
    return QStringLiteral("internal");
  case CapabilityExposure::LegacyAdapter:
    return QStringLiteral("legacy-adapter");
  case CapabilityExposure::Callable:
    return QStringLiteral("callable");
  case CapabilityExposure::Unavailable:
    return QStringLiteral("unavailable");
  }
  return QStringLiteral("unavailable");
}

QString executionShapeName(AlgorithmExecutionShape shape) {
  switch (shape) {
  case AlgorithmExecutionShape::Scalar:
    return QStringLiteral("scalar");
  case AlgorithmExecutionShape::Batch:
    return QStringLiteral("batch");
  case AlgorithmExecutionShape::FiniteHalo:
    return QStringLiteral("finite-halo");
  case AlgorithmExecutionShape::WholeTrace:
    return QStringLiteral("whole-trace");
  case AlgorithmExecutionShape::Reduction:
    return QStringLiteral("reduction");
  case AlgorithmExecutionShape::GlobalNode:
    return QStringLiteral("global");
  case AlgorithmExecutionShape::Unspecified:
    break;
  }
  return QStringLiteral("unspecified");
}

QVector<CapabilitySpec> CapabilityRegistry::all() { return table(); }
unsigned CapabilityRegistry::executionEffects(const QString &id,int version) { return spec(id,version).executionEffects; }

CapabilitySpec CapabilityRegistry::spec(const QString &capabilityId, int contractVersion) {
  for (const CapabilitySpec &spec : table()) {
    if (spec.capabilityId == capabilityId && spec.contractVersion == contractVersion)
      return spec;
  }
  return {};
}

bool CapabilityRegistry::contains(const QString &capabilityId) {
  for(const auto &spec:table())if(spec.capabilityId==capabilityId)return true;
  return false;
}

QString CapabilityRegistry::resolveAlias(const QString &name, QString *code) {
  QString found;
  int hits = 0;
  for (const CapabilitySpec &spec : table()) {
    if (spec.capabilityId == name || spec.aliases.contains(name)) {
      ++hits;
      found = spec.capabilityId;
    }
  }
  if (hits > 1) {
    if (code)
      *code = QStringLiteral("alias_collision");
    return {};
  }
  if (hits == 0) {
    if (code)
      *code = QStringLiteral("unknown_capability");
    return {};
  }
  if (code)
    code->clear();
  return found;
}

QStringList CapabilityRegistry::aliasCollisions() {
  QHash<QString, int> counts;
  for (const CapabilitySpec &spec : table()) {
    counts[spec.capabilityId] += 1;
    for (const QString &alias : spec.aliases)
      counts[alias] += 1;
  }
  QStringList collisions;
  for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
    if (it.value() > 1)
      collisions.append(it.key());
  }
  collisions.sort();
  return collisions;
}

CapabilityExposure CapabilityRegistry::exposureForAlgorithm(const QString &algorithmId, QString *reason) {
  for (const CapabilitySpec &spec : table()) {
    // 版本精确：能力行引用的算法版本须等于该算法的默认版本（spec(id) 首项），
    // 才算「该算法对 Agent 可调用」——同 id 的其他 exact-version 描述（如 I1
    // 的 geostat v2）只经其能力行暴露，不把旧版本算法标成 callable。
    if (spec.algorithmId == algorithmId &&
        spec.algorithmContractVersion == AlgorithmSpecRegistry::spec(algorithmId).contractVersion) {
      if (reason)
        *reason = spec.exposureReason;
      return spec.exposure;
    }
  }
  const AlgorithmSpec algorithm = AlgorithmSpecRegistry::spec(algorithmId);
  if (algorithm.id.isEmpty()) {
    if (reason)
      *reason = QStringLiteral("not registered");
    return CapabilityExposure::Unavailable;
  }
  if (algorithm.engine == QLatin1String("processing")) {
    if (reason)
      *reason = QStringLiteral("processing entry remains; public capability gate is not opened");
    return CapabilityExposure::LegacyAdapter;
  }
  if (reason)
    *reason = QStringLiteral("typed native entry; not a public capability");
  return CapabilityExposure::Internal;
}

QStringList CapabilityRegistry::callableIds() {
  QStringList ids;
  for (const CapabilitySpec &spec : table()) {
    if (spec.exposure == CapabilityExposure::Callable)
      ids.append(spec.capabilityId);
  }
  return ids;
}
