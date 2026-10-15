// 层：QGIS 封装
#include "mappingfeatureidentity.h"
#include "../domain/interpretationevidence.h"
#include <QCryptographicHash>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QUuid>
#include <QScopedValueRollback>
#include <QUndoStack>
#include <QThread>
#include <qgsexpression.h>
#include <qgsfeaturerequest.h>
#include <qgsvectorlayer.h>
#include <qgsvectordataprovider.h>

namespace MappingFeatureIdentity {
namespace {
bool fail(QString *error, const QString &message) {
  if (error) *error = message;
  return false;
}
QString fresh() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
bool parents(const QVariant &value, QStringList *result) {
  const auto raw = value.toString().trimmed();
  if (raw.isEmpty()) return true;
  QJsonParseError parse;
  const auto document = QJsonDocument::fromJson(raw.toUtf8(), &parse);
  if (parse.error != QJsonParseError::NoError || !document.isArray()) return false;
  for (const auto &item : document.array()) {
    if (!item.isString() || item.toString().trimmed().isEmpty()) return false;
    if (!result->contains(item.toString())) result->append(item.toString());
  }
  return true;
}
QString encode(const QStringList &ids) {
  QJsonArray array;
  for (const auto &id : ids) array.append(id);
  return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}
bool finish(QgsVectorLayer *layer, bool own, bool ok, QString *error) {
  if (!ok) {
    layer->destroyEditCommand();
    if (own) layer->rollBack();
    return fail(error, QObject::tr("无法保存图元身份；本次修改已撤销"));
  }
  layer->endEditCommand();
  if (own && !layer->commitChanges()) {
    const auto detail = layer->commitErrors().join(QStringLiteral("；"));
    layer->rollBack();
    return fail(error, QObject::tr("无法提交图元身份：%1").arg(detail));
  }
  return true;
}
class CompensateCancelledCommand : public QUndoCommand {
public:
  explicit CompensateCancelledCommand(QUndoCommand *command) : m_command(command) {}
  void redo() override { m_command->undo(); }
  void undo() override { m_command->redo(); }
private:
  // The same outer macro owns both this compensation and its native command.
  // It therefore replays/destructs them together; this pointer is not owned.
  QUndoCommand *m_command;
};

QVariantMap snapshot(const QgsFeature &feature) {
  const auto value = [&feature](const QString &field) {
    return feature.fields().indexOf(field) < 0 ? QVariant() : feature.attribute(field);
  };
  return {{"feature_uid", uid(feature)}, {"facies_code", value("facies_code")},
          {"parent_uids", value(ParentsField)}, {"geometry_hash", geometryFingerprint(feature)},
          {"root_uids", value(RootsField)},
          {"geometry", feature.geometry().asWkt(17)},
          {"interpretation", value("paleo_interpretation")}, {"evidence", value("facies_evidence")}};
}
}
QString uid(const QgsFeature &feature) {
  const int index = feature.fields().indexOf(UidField);
  return index < 0 ? QString() : feature.attribute(index).toString().trimmed();
}
QString geometryFingerprint(const QgsFeature &feature) {
  if (!feature.isValid() || feature.geometry().isEmpty()) return {};
  // Exact geometry revision, not a spatial equivalence/confidence score.
  return QString::fromLatin1(QCryptographicHash::hash(
      feature.geometry().asWkb(), QCryptographicHash::Sha256).toHex());
}
bool ensure(QgsVectorLayer *layer, QString *error) {
  if (!layer || QThread::currentThread() != layer->thread() || !layer->isValid() || layer->readOnly())
    return fail(error, QObject::tr("图元身份只能写入有效、可写的新产物或工作副本"));
  QHash<QString, int> counts;
  struct Row { QgsFeatureId fid; QString id; QStringList ancestors, roots; };
  QVector<Row> rows;
  const int uidIndex = layer->fields().indexOf(UidField);
  const int parentIndex = layer->fields().indexOf(ParentsField);
  const int rootIndex = layer->fields().indexOf(RootsField);
  QgsFeatureRequest request;
  QgsAttributeList attributes;
  if (uidIndex >= 0) attributes << uidIndex;
  if (parentIndex >= 0) attributes << parentIndex;
  if (rootIndex >= 0) attributes << rootIndex;
  request.setSubsetOfAttributes(attributes);
  request.setFlags(Qgis::FeatureRequestFlag::NoGeometry);
  auto features = layer->getFeatures(request);
  QgsFeature feature;
  while (features.nextFeature(feature)) {
    Row row{feature.id(), uid(feature), {}, {}};
    if (parentIndex >= 0 && !parents(feature.attribute(parentIndex), &row.ancestors))
      return fail(error, QObject::tr("图元来源身份损坏，原关联未覆盖"));
    if (rootIndex >= 0 && !parents(feature.attribute(rootIndex), &row.roots))
      return fail(error, QObject::tr("图元根来源身份损坏，原关联未覆盖"));
    if (!row.id.isEmpty()) ++counts[row.id];
    rows.append(row);
  }
  features.close();
  bool needed = uidIndex < 0 || parentIndex < 0 || rootIndex < 0 || layer->fields().indexOf(ReviewField) < 0;
  for (const auto &row : rows) needed |= row.id.isEmpty() || counts.value(row.id) > 1;
  if (!needed) return true;
  struct Repair { QgsFeatureId fid; QString identity; QStringList ancestors, roots;
                  bool review; QString interpretation, records; };
  QVector<Repair> repairs;
  const int interpretationField = layer->fields().indexOf(QStringLiteral("paleo_interpretation"));
  const int recordsField = layer->fields().indexOf(QStringLiteral("facies_evidence"));
  // Validate/fork BEFORE any provider mutation. A damaged interpretation must
  // block normalization, never produce a successful UID-mismatched snapshot.
  for (const auto &row : rows) {
    if (!row.id.isEmpty() && counts.value(row.id) == 1) continue;
    Repair repair{row.fid, fresh(), row.id.isEmpty() ? row.ancestors : QStringList{row.id},
                  row.roots, !row.id.isEmpty(), {}, {}};
    if (repair.roots.isEmpty()) repair.roots = row.ancestors.isEmpty()
        ? (row.id.isEmpty() ? QStringList{repair.identity} : QStringList{row.id}) : row.ancestors;
    if (interpretationField >= 0) {
      const auto original = layer->getFeature(row.fid);
      QVariantMap inherited; QVariantList records;
      if (!paleo::evidence::decodeInterpretation(original.attribute(interpretationField).toString(), &inherited, error) ||
          (recordsField >= 0 && !paleo::evidence::decodeRecords(original.attribute(recordsField).toString(), &records, error))) return false;
      if (!inherited.isEmpty()) {
        const auto parentUid = inherited.value("feature_uid").toString();
        if (!parentUid.isEmpty() && !repair.ancestors.contains(parentUid)) repair.ancestors << parentUid;
        QVariantMap fork{{"id", fresh()}, {"assertion_id", fresh()}, {"feature_uid", repair.identity},
            {"horizon", inherited.value("horizon")}, {"status", "Proposed"}, {"revision", 0},
            {"inherited_interpretation", inherited}, {"inherited_evidence", records},
            {"parent_interpretation_id", inherited.value("id")}};
        for (auto &value : records) {
          auto record = value.toMap();
          record.insert("inherited_from_feature_uid", record.value("feature_uid", parentUid));
          record.insert("feature_uid", repair.identity); value = record;
        }
        const QVariantMap target{{"feature_uid", repair.identity}, {"horizon", inherited.value("horizon")},
            {"geometry_hash", geometryFingerprint(original)}, {"facies_code", snapshot(original).value("facies_code")},
            {"facies_schema_hash", inherited.value("facies_schema_hash")}};
        paleo::evidence::Revision revision;
        const auto actor = QStringLiteral("local:%1").arg(qEnvironmentVariable("USER", qEnvironmentVariable("USERNAME", "operator")));
        if (!paleo::evidence::revise(records, fork, target, "edit",
            {{"spatial_change", "identity_normalization"}, {"parent_interpretation_id", inherited.value("id")}},
            actor, QObject::tr("新快照图元身份正规化；继承的解释和空间证据需要复核"), 0, &revision, error)) return false;
        repair.interpretation = paleo::evidence::encode(revision.interpretation);
        repair.records = paleo::evidence::encode(revision.records); repair.review = true;
      }
    }
    repairs << repair;
  }
  const bool own = !layer->isEditable();
  if (own && !layer->startEditing())
    return fail(error, QObject::tr("无法开始图元身份编辑"));
  layer->beginEditCommand(QObject::tr("维护稳定图元身份与来源"));
  bool ok = true;
  for (const auto &name : {UidField, ParentsField, RootsField})
    if (layer->fields().indexOf(name) < 0)
      ok = layer->addAttribute(QgsField(name, QMetaType::Type::QString)) && ok;
  if (layer->fields().indexOf(ReviewField) < 0)
    ok = layer->addAttribute(QgsField(ReviewField, QMetaType::Type::Bool)) && ok;
  const int u = layer->fields().indexOf(UidField), p = layer->fields().indexOf(ParentsField),
            r = layer->fields().indexOf(ReviewField), root = layer->fields().indexOf(RootsField);
  if (rootIndex < 0) {
    for (const auto &row : rows) {
      if (!ok) break;
      if (row.id.isEmpty() || counts.value(row.id) > 1) continue;
      const auto roots = row.ancestors.isEmpty() ? QStringList{row.id} : row.ancestors;
      ok = layer->changeAttributeValue(row.fid, root, encode(roots));
    }
  }
  for (const auto &repair : repairs) {
    if (!ok) break;
    QgsAttributeMap changes{{u, repair.identity}, {p, encode(repair.ancestors)},
                            {root, encode(repair.roots)}, {r, repair.review}};
    if (!repair.interpretation.isEmpty()) {
      changes.insert(interpretationField, repair.interpretation);
      if (recordsField >= 0) changes.insert(recordsField, repair.records);
    }
    ok = layer->changeAttributeValues(repair.fid, changes);
  }
  return finish(layer, own, ok, error);
}
QgsFeature find(QgsVectorLayer *layer, const QString &uid) {
  if (!layer || QThread::currentThread() != layer->thread() || !layer->isValid() || uid.isEmpty() ||
      layer->fields().indexOf(UidField) < 0) return {};
  QgsFeatureRequest request;
  request.setFilterExpression(QStringLiteral("%1 = %2").arg(
      QgsExpression::quotedColumnRef(UidField), QgsExpression::quotedString(uid)));
  request.setLimit(2);
  auto features = layer->getFeatures(request);
  QgsFeature first, second;
  if (!features.nextFeature(first) || features.nextFeature(second)) return {};
  return first;
}
bool setParents(QgsVectorLayer *layer, QgsFeatureId fid,
                const QStringList &parentUids, QString *error) {
  QStringList normalized;
  for (const auto &id : parentUids) {
    if (id.trimmed().isEmpty()) return fail(error, QObject::tr("合并来源身份不能为空"));
    if (!normalized.contains(id.trimmed())) normalized << id.trimmed();
  }
  if (normalized.size() < 2)
    return fail(error, QObject::tr("合并图元需要至少两个明确的来源身份"));
  if (!layer || QThread::currentThread() != layer->thread() || !layer->isValid() || !layer->getFeature(fid).isValid())
    return fail(error, QObject::tr("合并目标图元已不可用"));
  if (!ensure(layer, error)) return false;
  const bool own = !layer->isEditable();
  if (own && !layer->startEditing()) return fail(error, QObject::tr("无法开始合并来源编辑"));
  layer->beginEditCommand(QObject::tr("登记合并图元来源（需复核）"));
  auto *observer = Observer::of(layer);
  if (observer) observer->before(fid);
  QStringList roots;
  for (const auto &parent : normalized) {
    const auto source = find(layer, parent);
    QStringList known;
    if (source.isValid()) parents(source.attribute(RootsField), &known);
    if (known.isEmpty()) known << parent; // unresolved historical anchor, review stays required
    for (const auto &root : known) if (!roots.contains(root)) roots << root;
  }
  const bool ok = layer->changeAttributeValues(fid,
      {{layer->fields().indexOf(UidField), fresh()},
       {layer->fields().indexOf(ParentsField), encode(normalized)},
       {layer->fields().indexOf(RootsField), encode(roots)},
       {layer->fields().indexOf(ReviewField), true}});
  if (ok && observer) {
    observer->m_touched.insert(fid);
    observer->m_kinds.insert(fid, QStringLiteral("merge"));
  }
  return finish(layer, own, ok, error);
}

Observer *Observer::of(QgsVectorLayer *layer) {
  return layer ? layer->findChild<Observer *>(QStringLiteral("paleoFeatureIdentityObserver"),
                                            Qt::FindDirectChildrenOnly) : nullptr;
}
Observer *Observer::attach(QgsVectorLayer *layer, QString *error) {
  if (auto *existing = of(layer)) return existing;
  if (!ensure(layer, error)) return nullptr;
  return new Observer(layer);
}
Observer::Observer(QgsVectorLayer *layer) : QObject(layer), m_layer(layer) {
  setObjectName(QStringLiteral("paleoFeatureIdentityObserver"));
  connect(layer, &QgsVectorLayer::editingStarted, this, [this] {
    // Unsaved spatial history survives normalization sessions and failed
    // version registration. Only successful registration acknowledges it.
    m_sessionInitial = m_initial; m_sessionRecentBefore = m_recentBefore;
    m_sessionHistoryOwner = m_historyOwner; m_sessionHistoryUids = m_historyUids;
    m_fidOwner.clear(); m_known.clear(); m_before.clear(); rebuildIds();
  });
  connect(layer, &QgsVectorLayer::editCommandStarted, this, &Observer::startCommand);
  connect(layer, &QgsVectorLayer::geometryChanged, this,
          [this](QgsFeatureId fid, const QgsGeometry &) { changed(fid); });
  connect(layer, &QgsVectorLayer::featureAdded, this, &Observer::added);
  connect(layer, &QgsVectorLayer::featureDeleted, this, &Observer::deleted);
  connect(layer, &QgsVectorLayer::editCommandEnded, this, &Observer::finishCommand);
  connect(layer, &QgsVectorLayer::editCommandDestroyed, this, &Observer::discardCommand);
  connect(layer, &QgsVectorLayer::attributeValueChanged, this,
          [this](QgsFeatureId fid, int field, const QVariant &) {
    if (!m_layer || m_writing || m_finishing) return;
    if (field == m_layer->fields().indexOf(UidField)) {
      m_idsDirty = true;
      trackIdentity(fid, uid(m_layer->getFeature(fid)));
    }
    if (!m_macroOpen) { m_known.insert(fid, m_layer->getFeature(fid)); return; }
    // Evidence/review fields only refresh the immutable before cache. They
    // never produce business edits or flip the user's review conclusion.
    m_cacheTouched.insert(fid);
    if (field != m_layer->fields().indexOf(QStringLiteral("facies_code"))) return;
    before(fid); m_touched.insert(fid);
    if (!m_kinds.contains(fid)) m_kinds.insert(fid, QStringLiteral("classification"));
  });
  // Undo/redo never creates records. Its restored state becomes the next
  // command's baseline; selected features are recaptured at command start.
  connect(layer, &QgsVectorLayer::afterRollBack, this, [this] {
    // Provider rollback restores only this session. Earlier committed edits
    // still await version registration and must keep their audit snapshots.
    m_initial = m_sessionInitial; m_recentBefore = m_sessionRecentBefore;
    m_historyOwner = m_sessionHistoryOwner; m_historyUids = m_sessionHistoryUids;
    m_fidOwner.clear(); m_known.clear(); m_before.clear(); rebuildIds();
  });
  connect(layer, &QgsVectorLayer::editingStopped, this, [this] { m_known.clear(); rebuildIds(); });
  connect(layer, &QgsVectorLayer::beforeCommitChanges, this, [this] {
    if (!m_layer) return;
    rebuildIds();
    bool valid = !m_ids.contains(QString());
    for (auto it = m_ids.cbegin(); it != m_ids.cend(); ++it) valid &= it.value().size() == 1;
    if (!valid) {
      reportFailure(tr("图元身份缺失或重复，请在工作副本中修复后保存；历史版本未改变"));
    }
  });
  rebuildIds();
}
void Observer::reportFailure(const QString &reason) {
  if (m_layer) {
    m_layer->setAllowCommit(false);
    m_layer->setProperty("paleo.evidenceEditFailure", reason);
  }
  emit failed(reason);
}
void Observer::rebuildIds() {
  m_ids.clear();
  if (!m_layer) return;
  ++m_indexBuilds; m_idsDirty = false;
  QgsFeatureRequest request;
  request.setSubsetOfAttributes({m_layer->fields().indexOf(UidField)});
  request.setFlags(Qgis::FeatureRequestFlag::NoGeometry);
  auto features = m_layer->getFeatures(request);
  QgsFeature feature;
  while (features.nextFeature(feature)) {
    if (m_finishing && m_added.contains(feature.id())) continue;
    m_ids[uid(feature)].insert(feature.id());
  }
}
QgsFeature Observer::before(QgsFeatureId fid) {
  if (m_before.contains(fid)) {
    const auto feature = m_before.value(fid);
    remember(fid, feature);
    return feature;
  }
  QgsFeature feature = m_known.value(fid);
  if (!feature.isValid() && m_layer && m_layer->dataProvider()) {
    QgsFeatureRequest request;
    request.setFilterFid(fid);
    auto features = m_layer->dataProvider()->getFeatures(request);
    features.nextFeature(feature);
  }
  m_before.insert(fid, feature);
  remember(fid, feature);
  return feature;
}
void Observer::remember(QgsFeatureId fid, const QgsFeature &feature) {
  const auto identity = uid(feature);
  if (!feature.isValid() || identity.isEmpty()) return;
  const auto owner = m_historyOwner.value(identity, identity);
  m_fidOwner.insert(fid, owner);
  if (!m_initial.contains(owner)) m_initial.insert(owner, feature);
  m_recentBefore.insert(owner, feature);
  m_historyOwner.insert(identity, owner);
  if (!m_historyUids[owner].contains(identity)) m_historyUids[owner].append(identity);
}
void Observer::trackIdentity(QgsFeatureId fid, const QString &identity) {
  if (identity.isEmpty()) return;
  const auto owner = m_fidOwner.value(fid, m_historyOwner.value(identity));
  if (owner.isEmpty()) return; // a newly created child has no before-history yet
  m_historyOwner.insert(identity, owner);
  if (!m_historyUids[owner].contains(identity)) m_historyUids[owner].append(identity);
}
void Observer::startCommand() {
  if (!m_layer || m_finishing) return;
  if (m_macroOpen) {
    auto *parent = m_nested.isEmpty() ? m_wrapper : m_nested.last().command;
    auto *command = parent && parent->childCount() > 0
        ? const_cast<QUndoCommand *>(parent->child(parent->childCount() - 1)) : nullptr;
    m_nested << NestedCommand{command, m_before, m_recentBefore, m_touched, m_deleted,
                             m_added, m_cacheTouched, m_kinds};
    ++m_commandDepth; return;
  }
  m_before.clear(); m_touched.clear(); m_deleted.clear(); m_added.clear(); m_kinds.clear(); m_cacheTouched.clear();
  m_nested.clear();
  m_macroOpen = true; m_commandDepth = 1;
  // Native primitive signals are emitted from QUndoCommand::redo(). Pushing
  // attributes from those signals would re-enter QUndoStack::push (undefined
  // Qt behavior). Keep a nested macro open instead: native end closes it, and
  // finishCommand can safely append attributes to the outer native macro.
  m_layer->undoStack()->beginMacro(tr("图元编辑与证据身份"));
  const auto *outer = m_layer->undoStack()->command(m_layer->undoStack()->index());
  m_wrapper = outer && outer->childCount() > 0
      ? const_cast<QUndoCommand *>(outer->child(outer->childCount() - 1)) : nullptr;
  QgsFeatureRequest request;
  request.setFilterFids(m_layer->selectedFeatureIds());
  auto features = m_layer->getFeatures(request);
  QgsFeature feature;
  while (features.nextFeature(feature)) m_before.insert(feature.id(), feature);
  // No full geometry scan: the provider baseline is fetched lazily for other
  // touched features; the identity index contains attributes only.
  if (m_idsDirty) rebuildIds();
}
bool Observer::rewrite(QgsFeatureId fid, const QStringList &parents, const QStringList &roots) {
  if (!m_layer || !m_macroOpen || !m_finishing) return false;
  const auto feature = m_layer->getFeature(fid);
  if (!feature.isValid()) return false;
  QScopedValueRollback<bool> guard(m_writing, true);
  const auto identity = fresh();
  auto resolvedRoots = roots;
  if (resolvedRoots.isEmpty()) MappingFeatureIdentity::parents(feature.attribute(RootsField), &resolvedRoots);
  if (resolvedRoots.isEmpty()) resolvedRoots = parents.isEmpty() ? QStringList{identity} : parents;

  QgsAttributeMap changedAttrs{
      {m_layer->fields().indexOf(UidField), identity},
      {m_layer->fields().indexOf(ParentsField), encode(parents)},
      {m_layer->fields().indexOf(RootsField), encode(resolvedRoots)},
      {m_layer->fields().indexOf(ReviewField), true}};

  const int interpretationField = m_layer->fields().indexOf(QStringLiteral("paleo_interpretation"));
  const int recordsField = m_layer->fields().indexOf(QStringLiteral("facies_evidence"));
  if (interpretationField >= 0) {
    QVariantMap inherited;
    if (paleo::evidence::decodeInterpretation(feature.attribute(interpretationField).toString(), &inherited, nullptr) && !inherited.isEmpty()) {
      const auto parentUid = inherited.value("feature_uid").toString();
      QVariantMap fork{
          {"id", fresh()},
          {"assertion_id", fresh()},
          {"feature_uid", identity},
          {"horizon", inherited.value("horizon")},
          {"status", QStringLiteral("Proposed")},
          {"revision", 0},
          {"inherited_interpretation", inherited},
          {"parent_interpretation_id", inherited.value("id")}};
      changedAttrs.insert(interpretationField, paleo::evidence::encode(fork));

      if (recordsField >= 0) {
        QVariantList records;
        if (paleo::evidence::decodeRecords(feature.attribute(recordsField).toString(), &records, nullptr)) {
          for (auto &value : records) {
            auto record = value.toMap();
            record.insert("inherited_from_feature_uid", record.value("feature_uid", parentUid));
            record.insert("feature_uid", identity);
            value = record;
          }
          changedAttrs.insert(recordsField, paleo::evidence::encode(records));
        }
      }
    }
  }

  const bool ok = m_layer->changeAttributeValues(fid, changedAttrs);
  if (!ok) { reportFailure(tr("原生编辑的图元来源登记失败")); return false; }
  m_ids[uid(feature)].remove(fid);
  if (m_ids[uid(feature)].isEmpty()) m_ids.remove(uid(feature));
  m_ids[identity].insert(fid); m_roots[identity] = resolvedRoots;
  trackIdentity(fid, identity);
  return true;
}
void Observer::changed(QgsFeatureId fid) {
  if (!m_layer || m_writing || m_finishing) return;
  if (!m_macroOpen) { m_known.insert(fid, m_layer->getFeature(fid)); return; }
  before(fid); m_touched.insert(fid);
  m_kinds.insert(fid, QStringLiteral("geometry"));
}
void Observer::added(QgsFeatureId fid) {
  if (!m_layer || m_writing || m_finishing) return;
  if (!m_macroOpen) { m_known.insert(fid, m_layer->getFeature(fid)); m_idsDirty = true; return; }
  m_touched.insert(fid); m_added.insert(fid);
  m_kinds.insert(fid, QStringLiteral("create"));
}
void Observer::registerAdded(QgsFeatureId fid) {
  m_touched.insert(fid);
  const auto feature = m_layer->getFeature(fid);
  if (!feature.isValid()) return;
  const auto identity = uid(feature);
  const auto siblings = m_ids.value(identity);
  if (identity.isEmpty()) rewrite(fid, {});
  else if (!siblings.isEmpty()) {
    const QStringList ancestors{identity}; // immediate parent, including later splits
    QStringList roots; parents(feature.attribute(RootsField), &roots);
    roots = m_roots.value(identity, roots.isEmpty() ? ancestors : roots);
    for (auto sibling : siblings) {
      before(sibling); m_touched.insert(sibling); rewrite(sibling, ancestors, roots);
      m_kinds.insert(sibling, QStringLiteral("split"));
    }
    rewrite(fid, ancestors, roots);
    m_kinds.insert(fid, QStringLiteral("split"));
    m_roots[identity] = roots;
  } else if (m_roots.contains(identity)) {
    rewrite(fid, {identity}, m_roots.value(identity)); m_kinds.insert(fid, QStringLiteral("split"));
  }
  else m_ids[identity].insert(fid);
}
void Observer::deleted(QgsFeatureId fid) {
  if (!m_layer || m_writing || m_finishing) return;
  if (!m_macroOpen) { m_known.remove(fid); m_idsDirty = true; return; }
  const auto feature = before(fid);
  m_touched.insert(fid); m_deleted.insert(fid);
  m_ids[uid(feature)].remove(fid);
  if (m_ids[uid(feature)].isEmpty()) m_ids.remove(uid(feature));
}
void Observer::inferMerges() {
  if (!m_layer || m_deleted.isEmpty() || m_deleted.size() > 64 || m_touched.size() > 128) return;
  for (auto fid : m_touched) {
    if (m_deleted.contains(fid)) continue;
    const auto result = m_layer->getFeature(fid);
    if (!result.isValid() || result.geometry().isEmpty()) continue;
    QVector<QgsGeometry> geometries;
    QStringList ancestors, roots;
    const auto original = before(fid);
    if (original.isValid()) { geometries << original.geometry(); ancestors << uid(original); parents(original.attribute(RootsField), &roots); }
    for (auto deletedId : m_deleted) {
      const auto removed = before(deletedId);
      if (!removed.isValid() || removed.geometry().isEmpty() ||
          !removed.geometry().boundingBox().intersects(result.geometry().boundingBox())) continue;
      geometries << removed.geometry(); ancestors << uid(removed);
      parents(removed.attribute(RootsField), &roots);
    }
    ancestors.removeAll(QString()); ancestors.removeDuplicates();
    ancestors.sort();
    if (ancestors.size() < 2) continue;
    const auto united = QgsGeometry::unaryUnion(geometries);
    // Bounding boxes only prune candidates. Exact topological equality is the
    // proof of merge lineage; failed/uncertain matches stay marked for review.
    if (!united.isEmpty() && united.isGeosEqual(result.geometry())) {
      QStringList existing;
      parents(result.attribute(ParentsField), &existing);
      if (existing != ancestors) rewrite(fid, ancestors, roots);
      m_kinds.insert(fid, QStringLiteral("merge"));
    }
  }
}
void Observer::finishCommand() {
  if (!m_layer || m_finishing || !m_macroOpen) return;
  if (--m_commandDepth > 0) { if (!m_nested.isEmpty()) m_nested.removeLast(); return; }
  QScopedValueRollback<bool> guard(m_finishing, true);
  if (m_idsDirty) rebuildIds();
  for (auto fid : m_added) registerAdded(fid);
  for (auto fid : m_touched) {
    if (m_deleted.contains(fid)) continue;
    const auto feature = m_layer->getFeature(fid);
    if (!feature.isValid()) continue;
    if (m_kinds.value(fid) == QLatin1String("classification") &&
        before(fid).attribute("facies_code") == feature.attribute("facies_code")) continue;
    const bool ok = m_layer->changeAttributeValue(fid, m_layer->fields().indexOf(ReviewField), true);
    if (!ok) reportFailure(tr("空间编辑的复核状态写入失败"));
  }
  inferMerges();
  for (auto it = m_kinds.cbegin(); it != m_kinds.cend(); ++it)
    if (!m_deleted.contains(it.key()) && m_layer->getFeature(it.key()).isValid() &&
        (it.value() != QLatin1String("classification") ||
         before(it.key()).attribute("facies_code") != m_layer->getFeature(it.key()).attribute("facies_code")))
      emit businessEdit(it.key(), it.value());
  // Root callbacks may append interpretation attributes; all remain children
  // of the same outer macro and replay together on native undo/redo.
  m_layer->undoStack()->endMacro(); m_macroOpen = false;
  for (auto fid : m_touched | m_cacheTouched) {
    const auto feature = m_layer->getFeature(fid);
    if (feature.isValid()) m_known.insert(fid, feature);
    else m_known.remove(fid);
  }
}
void Observer::discardCommand() {
  if (!m_layer || m_finishing || !m_macroOpen) return;
  QScopedValueRollback<bool> guard(m_finishing, true);
  if (m_commandDepth > 1 && !m_nested.isEmpty()) {
    const auto frame = m_nested.takeLast();
    if (!frame.command) {
      reportFailure(tr("无法安全解析嵌套编辑撤销命令，请放弃当前编辑会话"));
      return;
    }
    // Native destroy closed just the nested command. Its parent macro is
    // still open, so append a native Qt compensation which inverses that
    // exact command. Outer undo/redo replays both and preserves prior edits.
    m_layer->undoStack()->push(new CompensateCancelledCommand(frame.command));
    m_before = frame.before; m_recentBefore = frame.recentBefore;
    m_touched = frame.touched; m_deleted = frame.deleted; m_added = frame.added;
    m_cacheTouched = frame.cacheTouched; m_kinds = frame.kinds;
    m_idsDirty = true;
    --m_commandDepth;
    return;
  }
  // Native destroy already closed the inner macro. Its undo was inhibited by
  // our open outer macro, so close and perform the native discard now.
  m_layer->undoStack()->endMacro(); m_macroOpen = false; m_commandDepth = 0;
  m_layer->undoStack()->undo();
  auto *obsolete = new QUndoCommand;
  obsolete->setObsolete(true); m_layer->undoStack()->push(obsolete);
  m_touched.clear(); m_deleted.clear(); m_added.clear(); m_kinds.clear();
}
void Observer::acknowledge() {
  m_initial.clear(); m_recentBefore.clear(); m_roots.clear();
  m_sessionInitial.clear(); m_sessionRecentBefore.clear();
  m_historyOwner.clear(); m_historyUids.clear(); m_fidOwner.clear();
  m_sessionHistoryOwner.clear(); m_sessionHistoryUids.clear();
}
QVariantMap Observer::editBefore(QgsFeatureId fid) const {
  return snapshot(m_before.value(fid, m_recentBefore.value(m_fidOwner.value(fid))));
}
QVariantList Observer::changes() const {
  QVariantList result;
  if (!m_layer || QThread::currentThread() != thread() ||
      QThread::currentThread() != m_layer->thread()) return result;
  if (m_idsDirty) const_cast<Observer *>(this)->rebuildIds(); // owner-thread cache only
  for (auto it = m_initial.cbegin(); it != m_initial.cend(); ++it) {
    QgsFeature current;
    const auto aliases = m_historyUids.value(it.key());
    for (auto alias = aliases.crbegin(); alias != aliases.crend(); ++alias) {
      const auto ids = m_ids.value(*alias);
      if (ids.size() != 1) continue;
      const auto candidate = m_layer->getFeature(*ids.cbegin());
      if (candidate.isValid() && uid(candidate) == *alias) { current = candidate; break; }
    }
    if (current.isValid() && uid(current) == uid(it.value()) &&
        geometryFingerprint(current) == geometryFingerprint(it.value())) continue;
    const auto recent = m_recentBefore.value(it.key(), it.value());
    result << QVariantMap{{"before_uid", uid(recent)}, {"after_uid", uid(current)},
        {"before_geometry", recent.geometry().asWkt(17)},
        {"before_fingerprint", geometryFingerprint(recent)},
        {"after_fingerprint", geometryFingerprint(current)},
        {"deleted", !current.isValid()},
        {"before_evidence", snapshot(recent).value("evidence")},
        {"before_interpretation", snapshot(recent).value("interpretation")},
        {"before_facies_code", snapshot(recent).value("facies_code")},
        {"before_parent_uids", snapshot(recent).value("parent_uids")},
        {"before_root_uids", snapshot(recent).value("root_uids")},
        {"before_scope", "working_copy"},
        {"initial_feature", snapshot(it.value())}};
  }
  return result;
}
}
