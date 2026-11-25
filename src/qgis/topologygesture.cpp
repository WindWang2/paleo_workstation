// 层：QGIS 封装
#include "topologygesture.h"
#include "faciespartitioneditor.h"
#include "qgiseditingservice.h"
#include <QCoreApplication>
#include <QPointer>
#include <QUndoStack>
#include <qgsvectorlayer.h>
#include <qgsvectorlayereditbuffer.h>
#include <memory>

namespace {
struct Member {
  QPointer<QgsVectorLayer> layer;
  QPointer<QgsVectorLayerEditBuffer> session;
  QString name;
  bool sameSession() const { return layer && session && layer->editBuffer() == session; }
  bool live() const { return sameSession() && layer->isEditable() && !layer->readOnly(); }
};
struct Group {
  QList<Member> members;
  QString title;
  bool stepping = false;
};
// A native macro owns its marker. If a stack is truncated, this token cannot
// be confused with a newer command that happens to have the same index/address.
class Marker final : public QUndoCommand {
public:
  explicit Marker(std::shared_ptr<Group> group) : group(std::move(group)) {}
  std::shared_ptr<Group> group;
};

std::shared_ptr<Group> adjacent(QgsVectorLayer *layer, bool undo) {
  if (!layer || !layer->isEditable()) return {};
  auto *stack = layer->undoStack();
  const int slot = stack->index() - (undo ? 1 : 0);
  if (slot < 0 || slot >= stack->count()) return {};
  const auto *command = stack->command(slot);
  if (!command->childCount()) return {};
  const auto *marker = dynamic_cast<const Marker *>(command->child(command->childCount() - 1));
  return marker ? marker->group : nullptr;
}

QString conflict(const std::shared_ptr<Group> &group, bool undo) {
  for (const auto &member : group->members) {
    if (!member.live() || member.layer->isEditCommandActive() || adjacent(member.layer, undo) != group)
      return QCoreApplication::translate("TopologyGesture", "图层「%1」的编辑历史已改变，无法整组撤销或重做").arg(member.name);
  }
  return {};
}

void discard(const QList<Member> &opened, const std::shared_ptr<Group> &group) {
  for (const auto &member : opened) {
    if (!member.sameSession()) continue;
    if (member.layer->isEditCommandActive())
      member.layer->destroyEditCommand();
    else if (adjacent(member.layer, true) == group)
      member.layer->undoStack()->undo();
    else
      continue;
    if (member.sameSession()) {
      // QGIS undoes the rejected macro but leaves its redo entry behind.
      auto *trim = new QUndoCommand;
      trim->setObsolete(true);
      member.layer->undoStack()->push(trim);
    }
  }
}
}

bool QgisTopologyGesture::crossLayerEligible(const QgsVectorLayer *layer) {
  return layer && layer->isEditable() && layer->isSpatial() && !layer->readOnly() &&
         !QgisEditingService::isConstraintLayer(layer) && !FaciesPartitionEditor::isPartition(layer);
}

bool QgisTopologyGesture::apply(const QList<QgsVectorLayer *> &layers, const QString &title,
                                 const std::function<bool(QgsVectorLayer *)> &write, QString *error) {
  if (layers.isEmpty()) return false;
  auto group = std::make_shared<Group>();
  group->title = title;
  for (auto *layer : layers) {
    if (!layer || !layer->isEditable() || layer->readOnly() || layer->isEditCommandActive() ||
        (layers.size() > 1 && (!crossLayerEligible(layer) || layer->crs() != layers.first()->crs()))) {
      if (error) *error = QCoreApplication::translate("TopologyGesture", "参与图层已停止编辑或不能共同修改");
      return false;
    }
    group->members.append({layer, layer->editBuffer(), layer->name()});
  }
  QList<Member> opened;
  const auto abort = [&](const Member &member) {
    discard(opened, group);
    if (error) *error = QCoreApplication::translate("TopologyGesture", "节点编辑未完成，已撤回本次修改：%1").arg(member.name);
    return false;
  };
  for (const auto &member : std::as_const(group->members)) {
    if (!member.live() || member.layer->isEditCommandActive()) return abort(member);
    member.layer->beginEditCommand(title);
    opened.append(member);
  }
  for (const auto &member : std::as_const(group->members)) {
    if (!member.live() || !member.layer->isEditCommandActive() || !write(member.layer)) return abort(member);
  }
  for (const auto &member : std::as_const(group->members)) {
    if (!member.live() || !member.layer->isEditCommandActive()) return abort(member);
    if (layers.size() > 1) member.layer->undoStack()->push(new Marker(group));
    member.layer->endEditCommand();
  }
  return true;
}

std::optional<QgisTopologyGesture::Step> QgisTopologyGesture::describe(QgsVectorLayer *layer, bool undo) {
  const auto group = adjacent(layer, undo);
  if (!group) return {};
  const auto reason = conflict(group, undo);
  if (!reason.isEmpty() || group->stepping) return Step{false, reason};
  return Step{true, QCoreApplication::translate("TopologyGesture", "%1（%2 个图层）").arg(group->title).arg(group->members.size())};
}

bool QgisTopologyGesture::step(QgsVectorLayer *layer, bool undo, QString *error) {
  const auto group = adjacent(layer, undo);
  if (!group || group->stepping) return false;
  const auto reason = conflict(group, undo);
  if (!reason.isEmpty()) {
    if (error) *error = reason;
    return false;
  }
  group->stepping = true;
  QList<Member> completed;
  for (const auto &member : std::as_const(group->members)) {
    if (!member.live() || adjacent(member.layer, undo) != group) {
      for (auto it = completed.crbegin(); it != completed.crend(); ++it) {
        if (it->live() && adjacent(it->layer, !undo) == group) {
          if (undo) it->layer->undoStack()->redo(); else it->layer->undoStack()->undo();
        }
      }
      group->stepping = false;
      if (error) *error = QCoreApplication::translate("TopologyGesture", "跨层编辑状态已变化，请重新检查图层");
      return false;
    }
    if (undo) member.layer->undoStack()->undo(); else member.layer->undoStack()->redo();
    completed.append(member);
  }
  group->stepping = false;
  return true;
}
