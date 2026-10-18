// 层：QGIS 封装
#include "faciespartitioneditor.h"
#include "faciespartitionstate.h"
#include "faciespartitiontool.h"
#include "mappingfeatureidentity.h"
#include "../domain/faciescatalog.h"
#include "../domain/faciestaxonomy.h"
#include "../domain/interpretationevidence.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QThread>
#include <QUndoStack>
#include <QUuid>
#include <cmath>
#include <qgsgeometrycollection.h>
#include <qgsexception.h>
#include <qgslinestring.h>
#include <qgsmaptool.h>
#include <qgsvectorlayer.h>

namespace {
bool fail(QString *error, const QString &text) { if (error) *error = text; return false; }
QList<QgsGeometry> parts(const QgsGeometry &g) {
  if (g.isEmpty()) return {};
  if (!g.isMultipart()) return g.type() == Qgis::GeometryType::Polygon && g.area() > 0
      ? QList<QgsGeometry>{g} : QList<QgsGeometry>{};
  QList<QgsGeometry> polygons;
  for (const auto &part : g.asGeometryCollection()) polygons.append(parts(part));
  return polygons;
}
QgsGeometry united(QgsVectorLayer *v) {
  QVector<QgsGeometry> geometries;
  auto it = v->getFeatures(); QgsFeature f;
  while (it.nextFeature(f)) geometries << f.geometry();
  return QgsGeometry::unaryUnion(geometries);
}
QVariantList schema(QgsVectorLayer *v) { return FaciesPartitionState::schema(v); }
bool validCode(QgsVectorLayer *v, int code, QString *error) {
  return FaciesTaxonomy::complete(FaciesCatalog::find(schema(v), code)) ||
      fail(error, QObject::tr("请选择具有完整相、亚相、微相归属的目标微相"));
}
bool ready(QgsVectorLayer *v, QString *error) {
  if (!v || QThread::currentThread() != v->thread() || !FaciesPartitionEditor::isPartition(v) ||
      !v->isEditable() || v->readOnly())
    return fail(error, QObject::tr("请先选择可写的相面工作副本并开始编辑"));
  if (!FaciesPartitionState::evidenceFailure(v).isEmpty())
    return fail(error, FaciesPartitionState::evidenceFailure(v));
  if (!MappingFeatureIdentity::Observer::of(v))
    return fail(error, QObject::tr("相面图元身份观察器尚未就绪，请重新打开工作副本"));
  if (!FaciesPartitionEditor::validate(v, error)) return false;
  auto it = v->getFeatures(); QgsFeature f;
  while (it.nextFeature(f)) {
    QVariantList records; QVariantMap interpretation;
    if ((f.fields().indexOf("facies_evidence") >= 0 &&
         !paleo::evidence::decodeRecords(f.attribute("facies_evidence").toString(), &records, error)) ||
        (f.fields().indexOf("paleo_interpretation") >= 0 &&
         !paleo::evidence::decodeInterpretation(f.attribute("paleo_interpretation").toString(), &interpretation, error))) return false;
    if (!interpretation.isEmpty() && interpretation.value("feature_uid").toString() != MappingFeatureIdentity::uid(f))
      return fail(error, QObject::tr("原面解释与图元身份不一致，请先修复证据关联"));
    if ((!records.isEmpty() || !interpretation.isEmpty()) &&
        !FaciesPartitionState::workbenchBinding(MappingFeatureIdentity::Observer::of(v)))
      return fail(error, QObject::tr("证据修订服务未就绪，请在智能编图工作副本中进行相面编辑"));
  }
  return true;
}
QgsFeature child(QgsVectorLayer *v, const QgsFeatureList &sources,
                 const QgsGeometry &geometry, int code, bool keepRange) {
  QgsFeature f = sources.isEmpty() ? QgsFeature(v->fields()) : sources.first(); f.setId(FID_NULL); f.setGeometry(geometry);
  // Never present the first parent's conflicting measurements or remarks as
  // a value for the whole new face. All source attributes remain in lineage.
  if (sources.size() > 1)
    for (int index = 0; index < f.fields().size(); ++index) {
      const auto name = f.fields().at(index).name();
      if (name == "paleo_interpretation" || name == "facies_evidence") continue;
      for (const auto &source : sources) if (source.attribute(index) != sources.first().attribute(index)) {
        f.setAttribute(index, QVariant()); break;
      }
    }
  for (int index : v->primaryKeyAttributes()) f.setAttribute(index, QVariant());
  const auto attrs = FaciesCatalog::attributes(schema(v), code);
  f.setAttribute("facies_code", code);
  for (auto it = attrs.cbegin(); it != attrs.cend(); ++it)
    if (f.fields().indexOf(it.key()) >= 0) f.setAttribute(it.key(), it.value());
  if (!keepRange)
    for (const auto &field : {"value_min", "value_max"})
      if (f.fields().indexOf(field) >= 0) f.setAttribute(field, QVariant());
  for (const auto &field : {"confidence", "probability", "boundary_kind", "transition_width"})
    if (f.fields().indexOf(field) >= 0) f.setAttribute(field, QVariant());
  QStringList parents, roots;
  for (const auto &source : sources) {
    parents << MappingFeatureIdentity::uid(source);
    const auto sourceRoots = QJsonDocument::fromJson(source.attribute(MappingFeatureIdentity::RootsField).toString().toUtf8()).toVariant().toList();
    if (sourceRoots.isEmpty()) roots << MappingFeatureIdentity::uid(source);
    else for (const auto &root : sourceRoots) roots << root.toString();
  }
  parents.removeDuplicates(); parents.sort(); roots.removeDuplicates(); roots.sort();
  f.setAttribute(MappingFeatureIdentity::UidField, QUuid::createUuid().toString(QUuid::WithoutBraces));
  f.setAttribute(MappingFeatureIdentity::ParentsField, paleo::evidence::encode(parents));
  f.setAttribute(MappingFeatureIdentity::RootsField, paleo::evidence::encode(roots));
  f.setAttribute(MappingFeatureIdentity::ReviewField, true);
  if (QgsWkbTypes::isMultiType(v->wkbType())) { auto g = f.geometry(); g.convertToMultiType(); f.setGeometry(g); }
  return f;
}
struct Plan { QgsFeatureIds removed; QgsFeatureList added, changed; QStringList selectedUids; };
bool apply(QgsVectorLayer *v, Plan plan, const QString &kind, const QString &title, QString *error) {
  const auto previousOperation = FaciesPartitionState::operation(v);
  const auto previousFailures = FaciesPartitionState::evidenceFailures(v);
  const auto previousFailure = FaciesPartitionState::evidenceFailure(v);
  const auto previousSelection = v->selectedFeatureIds();
  const bool previouslyAllowed = v->allowCommit();
  FaciesPartitionState::setOperation(v, kind);
  v->beginEditCommand(title);
  bool ok = true;
  for (const auto &f : plan.changed) {
    auto geometry = f.geometry();
    // #541 子项 1：与 child() 的 add 路径同一规范化——GEOS difference 的单部件
    // 结果是 Polygon，直接 changeGeometry 会让 GeoPackage 的 MULTIPOLYGON 列
    // 混进 POLYGON 行。
    if (QgsWkbTypes::isMultiType(v->wkbType())) geometry.convertToMultiType();
    ok = v->changeGeometry(f.id(), geometry) && ok;
    for (const auto &field : {"confidence", "probability", "boundary_kind", "transition_width"})
      if (const int index = v->fields().indexOf(field); index >= 0)
        ok = v->changeAttributeValue(f.id(), index, QVariant()) && ok;
  }
  for (auto id : plan.removed) ok = v->deleteFeature(id) && ok;
  for (auto &f : plan.added) ok = v->addFeature(f) && ok;
  // Insert every new junction into BOTH faces, including unselected neighbors.
  auto junctionGeometries = plan.added + plan.changed;
  QgsRectangle affected; affected.setNull();
  for (const auto &f : junctionGeometries) affected.combineExtentWith(f.geometry().boundingBox());
  // Union/split may remove collinear vertices on an outer edge. Feed the
  // untouched neighbors' junctions back into the new faces as well.
  auto neighbors = v->getFeatures(QgsFeatureRequest().setFilterRect(affected)); QgsFeature neighbor;
  while (neighbors.nextFeature(neighbor)) junctionGeometries << neighbor;
  for (const auto &f : junctionGeometries) { const int result = v->addTopologicalPoints(f.geometry());
    if (result != 0 && result != 2) ok = false; }
  if (ok) ok = FaciesPartitionEditor::validate(v, error);
  if (!ok) {
    v->destroyEditCommand(); FaciesPartitionState::setOperation(v, previousOperation);
    v->selectByIds(previousSelection);
    if (error && error->isEmpty()) *error = QObject::tr("相面编辑失败，本次操作已整体撤销");
    return false;
  }
  v->endEditCommand(); FaciesPartitionState::setOperation(v, previousOperation);
  if (!FaciesPartitionState::evidenceFailure(v).isEmpty()) {
    const auto reason = FaciesPartitionState::evidenceFailure(v);
    v->undoStack()->undo();
    auto *discard = new QUndoCommand; discard->setObsolete(true); v->undoStack()->push(discard);
    FaciesPartitionState::setEvidenceFailures(v, previousFailures);
    FaciesPartitionState::setEvidenceFailure(v, previousFailure);
    v->setAllowCommit(previouslyAllowed);
    v->selectByIds(previousSelection);
    return fail(error, reason);
  }
  QgsFeatureIds selected;
  for (const auto &f : plan.added)
    if (plan.selectedUids.isEmpty() || plan.selectedUids.contains(MappingFeatureIdentity::uid(f))) selected.insert(f.id());
  v->selectByIds(selected); v->triggerRepaint(); return true;
}
}
// #535：跨层状态访问器的唯一实现处。键名与契约见 faciespartitionstate.h。
namespace FaciesPartitionState {
bool isPartition(const QgsVectorLayer *v) {
  return v && v->geometryType() == Qgis::GeometryType::Polygon &&
      v->customProperty(kRequireTopology).toBool();
}
void setRequireTopology(QgsVectorLayer *v, bool on) {
  if (v) v->setCustomProperty(kRequireTopology, on);
}
QVariantList schema(const QgsVectorLayer *v) { return v->customProperty(kSchema).toList(); }
void setSchema(QgsVectorLayer *v, const QVariantList &rows) {
  if (v) v->setCustomProperty(kSchema, rows);
}
QString frozenFootprint(const QgsVectorLayer *v) { return v->customProperty(kFrozenFootprint).toString(); }
bool hasFrozenFootprint(const QgsVectorLayer *v) { return v->customProperty(kFrozenFootprint).isValid(); }
void setFrozenFootprint(QgsVectorLayer *v, const QString &base64Wkb) {
  if (v) v->setCustomProperty(kFrozenFootprint, base64Wkb);
}
QString operation(const QgsVectorLayer *v) { return v->property(kOperation).toString(); }
void setOperation(QgsVectorLayer *v, const QString &kind) { v->setProperty(kOperation, kind); }
QString evidenceFailure(const QgsVectorLayer *v) { return v->property(kEvidenceFailure).toString(); }
void setEvidenceFailure(QgsVectorLayer *v, const QString &reason) {
  v->setProperty(kEvidenceFailure, reason.isEmpty() ? QVariant() : QVariant(reason));
}
QVariantMap evidenceFailures(const QgsVectorLayer *v) { return v->property(kEvidenceFailures).toMap(); }
void setEvidenceFailures(QgsVectorLayer *v, const QVariantMap &failures) {
  v->setProperty(kEvidenceFailures, failures.isEmpty() ? QVariant() : QVariant(failures));
}
bool commitRejected(const QgsVectorLayer *v) { return v->property(kCommitRejected).toBool(); }
void setCommitRejected(QgsVectorLayer *v, bool rejected) {
  v->setProperty(kCommitRejected, rejected ? QVariant(true) : QVariant());
}
QObject *workbenchBinding(const QObject *observer) {
  return observer ? observer->property(kWorkbenchBinding).value<QObject *>() : nullptr;
}
void setWorkbenchBinding(QObject *observer, QObject *workbench) {
  if (observer) observer->setProperty(kWorkbenchBinding, QVariant::fromValue(workbench));
}
QVariantMap saveFootprint(const QString &projectDir, const QString &workingPath,
                          const QString &base64Wkb, QString *error) {
  if (base64Wkb.isEmpty()) return {};
  const auto bytes = QByteArray::fromBase64(base64Wkb.toLatin1());
  const auto relative = footprintSidecarPath(workingPath);
  QDir().mkpath(QFileInfo(QDir(projectDir).filePath(relative)).absolutePath());
  QFile file(QDir(projectDir).filePath(relative));
  if (!file.open(QIODevice::WriteOnly)) {
    fail(error, QObject::tr("无法写入相图覆盖范围文件：%1").arg(file.errorString()));
    return {};
  }
  file.write(bytes);
  file.close();
  QgsGeometry geometry; geometry.fromWkb(bytes);
  const auto box = geometry.boundingBox();
  // 可读摘要：bbox / 面积 / 部件数 / sha256——脱离 GEOS 序列化格式也能看懂。
  QVariantMap summary{{"bbox", QVariantList{box.xMinimum(), box.yMinimum(),
                                            box.xMaximum(), box.yMaximum()}},
                      {"area", geometry.area()},
                      {"parts", geometry.isMultipart() ? geometry.asGeometryCollection().size() : 1},
                      {"sha256", QString::fromLatin1(
                                     QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}};
  return {{QString::fromLatin1(kFootprintPathKey), relative},
          {QString::fromLatin1(kFootprintSummaryKey), summary}};
}
QString loadFootprint(const QString &projectDir, const QVariantMap &extra) {
  const auto relative = extra.value(QString::fromLatin1(kFootprintPathKey)).toString();
  if (!relative.isEmpty()) {
    QFile file(QDir(projectDir).filePath(relative));
    if (file.open(QIODevice::ReadOnly))
      return QString::fromLatin1(file.readAll().toBase64());
  }
  // 读取兼容：#540 子项 2 之前的版本把 WKB base64 直接钉在 extra 里。
  return extra.value(QString::fromLatin1(kLegacyFootprintWkbKey)).toString();
}
} // namespace FaciesPartitionState
namespace FaciesPartitionEditor {
bool isPartition(const QgsVectorLayer *v) { return FaciesPartitionState::isPartition(v); }
QVariantList choices(const QgsVectorLayer *v) {
  QVariantList out;
  if (!isPartition(v)) return out;
  for (const auto &value : FaciesPartitionState::schema(v)) {
    const auto leaf = value.toMap();
    out << QVariantMap{{"code", leaf.value("code")}, {"name", QStringLiteral("%1 → %2 → %3")
      .arg(leaf.value("facies").toString(), leaf.value("subfacies").toString(), leaf.value("microfacies").toString())}};
  }
  return out;
}
int selectionSize(const QgsVectorLayer *v) { return v ? v->selectedFeatureCount() : 0; }
QString layerKey(const QgsVectorLayer *v) { return v ? v->id() : QString(); }
QString footprint(QgsVectorLayer *v, QString *error) {
  if (!v || !v->isValid()) { fail(error, QObject::tr("相面图层不可用")); return {}; }
  const auto g = united(v);
  if (g.isEmpty() || !g.isGeosValid()) { fail(error, QObject::tr("无法建立相图覆盖范围")); return {}; }
  return QString::fromLatin1(g.asWkb().toBase64());
}
bool install(QgsVectorLayer *v, const QVariantList &source, const QString &frozenFootprint, QString *error) {
  if (!v || v->geometryType() != Qgis::GeometryType::Polygon) return true;
  QVariantList normalized;
  if (!FaciesTaxonomy::normalize(source, &normalized, error)) return false;
  FaciesPartitionState::setSchema(v, normalized);
  FaciesPartitionState::setRequireTopology(v, true);
  // #541 子项 2：零要素相面（从零手绘的起点）没有覆盖范围可冻结——放行，不写
  // 冻结范围（validate 的 footprint 比对随之跳过）。
  // 首绘命令内不冻结：撤销须能回到空图。首次保存后按版本 sidecar 冻结。
  const bool nothingToFreeze = frozenFootprint.isEmpty() && v->featureCount() == 0;
  if (!FaciesPartitionState::hasFrozenFootprint(v) && !nothingToFreeze && (!v->isEditable() || !frozenFootprint.isEmpty())) {
    const auto frozen = frozenFootprint.isEmpty() ? footprint(v, error) : frozenFootprint;
    QgsGeometry g;
    g.fromWkb(QByteArray::fromBase64(frozen.toLatin1()));
    if (frozen.isEmpty() || g.isEmpty() || !g.isGeosValid())
      return fail(error, QObject::tr("相图原始覆盖范围无效，不能开启拓扑编辑"));
    FaciesPartitionState::setFrozenFootprint(v, frozen);
  }
  return true;
}
bool validate(QgsVectorLayer *v, QString *error) {
  if (!v || v->geometryType() != Qgis::GeometryType::Polygon) return true;
  const auto classification = schema(v);
  if (isPartition(v) && v->fields().indexOf("facies_code") < 0)
    return fail(error, QObject::tr("相面缺少微相编码字段，请恢复字段后继续编辑"));
  auto *collection = new QgsGeometryCollection;
  QgsGeometry coverage(collection);
  auto it = v->getFeatures(); QgsFeature f;
  while (it.nextFeature(f)) {
    if (!classification.isEmpty()) {
      bool parsed = false; const auto code = f.attribute("facies_code").toDouble(&parsed);
      if (!parsed || !std::isfinite(code) || code < 1 || code > 32767 || code != std::round(code) ||
          !FaciesTaxonomy::complete(FaciesCatalog::find(classification, int(code))))
        return fail(error, QObject::tr("相面含缺失或未定义的微相编码，请先在完整三级分类中明确归属"));
    }
    if (f.geometry().type() != Qgis::GeometryType::Polygon || f.geometry().isEmpty() || !f.geometry().isGeosValid())
      return fail(error, QObject::tr("相面含空几何或无效几何，请修复后保存"));
    collection->addGeometry(f.geometry().constGet()->clone());
  }
  try {
    if (collection->numGeometries() > 1 && coverage.validateCoverage(0) != Qgis::CoverageValidityResult::Valid)
      return fail(error, QObject::tr("相面存在重叠或共边节点不匹配，本次修改不能保存"));
  } catch (const QgsNotSupportedException &) {
    return fail(error, QObject::tr("当前 QGIS/GEOS 不支持相面覆盖拓扑检查"));
  }
  if (FaciesPartitionState::hasFrozenFootprint(v)) {
    QgsGeometry baseline;
    baseline.fromWkb(QByteArray::fromBase64(FaciesPartitionState::frozenFootprint(v).toLatin1()));
    const auto current = united(v);
    if (current.isEmpty() || !baseline.isGeosEqual(current))
      return fail(error, QObject::tr("相图覆盖范围发生变化，存在新增空洞、误删或越界；请撤销或使用覆盖绘制保持完整分区"));
  }
  return true;
}
bool split(QgsVectorLayer *v, const QgsGeometry &line, QString *error) {
  if (error) error->clear();
  if (!ready(v, error)) return false;
  if (line.type() != Qgis::GeometryType::Line || line.isMultipart() || line.isEmpty())
    return fail(error, QObject::tr("请画一条穿过所选相面的分割线"));
  if (v->selectedFeatureCount() == 0) return fail(error, QObject::tr("请先选择需要分割的相面"));
  const auto *curve = qgsgeometry_cast<const QgsCurve *>(line.constGet());
  QgsPointSequence points; curve->points(points);
  Plan plan;
  for (const auto &f : v->selectedFeatures()) {
    const int code = f.attribute("facies_code").toInt();
    if (!validCode(v, code, error)) return false;
    auto geometry = f.geometry(); QVector<QgsGeometry> pieces; QgsPointSequence junctions;
    const auto status = geometry.splitGeometry(points, pieces, true, junctions);
    if (status == Qgis::GeometryOperationResult::NothingHappened) continue;
    if (status != Qgis::GeometryOperationResult::Success)
      return fail(error, QObject::tr("分割线无法切开所选相面；原面保持不变"));
    if (pieces.isEmpty()) continue;
    plan.removed.insert(f.id()); pieces.prepend(geometry);
    for (const auto &piece : pieces) for (const auto &part : parts(piece))
      plan.added << child(v, {f}, part, code, true);
  }
  if (plan.removed.isEmpty()) return fail(error, QObject::tr("分割线没有穿过任何所选相面"));
  return apply(v, plan, "split", QObject::tr("分割相面并继承三级归属"), error);
}
bool paint(QgsVectorLayer *v, const QgsGeometry &polygon, int code, QString *error) {
  if (error) error->clear();
  if (!ready(v, error) || !validCode(v, code, error)) return false;
  if (polygon.type() != Qgis::GeometryType::Polygon || polygon.isEmpty() || !polygon.isGeosValid())
    return fail(error, QObject::tr("绘制的新相面无效，请检查自交和闭合边界"));
  if (v->featureCount() == 0 && !FaciesPartitionState::hasFrozenFootprint(v)) {
    Plan initial;
    for (const auto &part : parts(polygon)) {
      const auto feature = child(v, {}, part, code, false);
      initial.added << feature;
      initial.selectedUids << MappingFeatureIdentity::uid(feature);
    }
    return apply(v, initial, "create", QObject::tr("绘制首个相面及解释依据"), error);
  }
  const auto clipped = polygon.intersection(united(v));
  if (!clipped.lastError().isEmpty()) return fail(error, clipped.lastError());
  if (clipped.isEmpty() || clipped.area() <= 0) return fail(error, QObject::tr("新面必须覆盖现有相图中的有效区域"));
  Plan plan; QgsFeatureList sources;
  auto it = v->getFeatures(); QgsFeature f;
  while (it.nextFeature(f)) {
    if (!f.geometry().boundingBox().intersects(clipped.boundingBox())) continue;
    const auto intersection = f.geometry().intersection(clipped);
    if (!intersection.lastError().isEmpty()) return fail(error, intersection.lastError());
    if (intersection.isEmpty() || intersection.area() <= 0) continue;
    sources << f;
    const auto remainder = f.geometry().difference(clipped);
    if (!remainder.lastError().isEmpty()) return fail(error, remainder.lastError());
    const auto pieces = parts(remainder);
    if (pieces.size() == 1) { auto changed = f; changed.setGeometry(remainder); plan.changed << changed; }
    else {
      if (!pieces.isEmpty() && !validCode(v, f.attribute("facies_code").toInt(), error)) return false;
      plan.removed.insert(f.id());
      for (const auto &part : pieces) plan.added << child(v, {f}, part, f.attribute("facies_code").toInt(), true);
    }
  }
  for (const auto &part : parts(clipped)) {
    QgsFeatureList parents;
    for (const auto &source : sources) if (source.geometry().intersection(part).area() > 0) parents << source;
    if (!parents.isEmpty()) { const auto added = child(v, parents, part, code, false);
      plan.added << added; plan.selectedUids << MappingFeatureIdentity::uid(added); }
  }
  if (plan.added.isEmpty()) return fail(error, QObject::tr("没有可以新增的相面区域"));
  return apply(v, plan, "overlay", QObject::tr("覆盖绘制相面并裁切原分区"), error);
}
bool merge(QgsVectorLayer *v, int code, QString *error) {
  if (error) error->clear();
  if (!ready(v, error)) return false;
  const auto selected = v->selectedFeatures();
  if (selected.size() < 2) return fail(error, QObject::tr("请至少选择两个相邻相面"));
  bool same = true; const int firstCode = selected.first().attribute("facies_code").toInt();
  QVector<QgsGeometry> geometries; Plan plan;
  for (const auto &f : selected) { same &= f.attribute("facies_code").toInt() == firstCode;
    geometries << f.geometry(); plan.removed.insert(f.id()); }
  if (!code && !same) return fail(error, QObject::tr("所选面属于不同微相，请明确选择合并后的目标微相；父级聚合显示不会删除内部微相"));
  if (!code) code = firstCode;
  if (!validCode(v, code, error)) return false;
  const auto geometry = QgsGeometry::unaryUnion(geometries);
  if (geometry.isEmpty() || !geometry.isGeosValid() || parts(geometry).size() != 1)
    return fail(error, QObject::tr("只能合并具有公共边界的连通相面"));
  bool keepRange = same && code == firstCode;
  for (const auto &f : selected) for (const auto &field : {"value_min", "value_max"})
    if (f.fields().indexOf(field) >= 0 && f.attribute(field) != selected.first().attribute(field)) keepRange = false;
  plan.added << child(v, selected, geometry, code, keepRange);
  return apply(v, plan, "merge", QObject::tr("合并相面并记录全部来源"), error);
}
// #533：捕获工具的创建点。工具栏只经本门面拿 QgsMapTool*——faciespartitiontool.h
// 经 QgsMapToolCapture 传递引入的 QtWidgets 边留在本层。
QgsMapTool *createCaptureTool(QgsMapCanvas *canvas, QgsAdvancedDigitizingDockWidget *cad,
                              QgsVectorLayer *layer, PartitionOperation operation,
                              const std::function<int()> &targetCode,
                              const std::function<void()> &aborted) {
  auto *tool = new FaciesPartitionTool(canvas, cad, layer,
      operation == PartitionOperation::Split ? FaciesPartitionTool::Split
                                             : FaciesPartitionTool::Paint,
      targetCode);
  // Qt6 没有「字符串式信号 + lambda」的 connect 重载，而视图层看不见工具的具体
  // 类型——取消回调在此接线。
  QObject::connect(tool, &FaciesPartitionTool::editAborted, [aborted] { if (aborted) aborted(); });
  return tool;
}
}
