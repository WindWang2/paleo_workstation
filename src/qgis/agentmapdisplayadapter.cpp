// 层：QGIS 封装
// qgis/agentmapdisplayadapter.cpp — I3：Agent 地图显示访问的生产实现。
//
// 身份与状态口径：
//   · 业务身份 = manifest layerId（稳定）；QgsMapLayer::id()/树节点只是
//     当前实例的内部事实，留在回执的 state_token 材料里，不外发。
//   · 修改只认「声明存在 + 已实例化 + 实例自证 paleoLayerId 一致」的受管
//     层；查询可见未实例化声明但绝不触发实例化、不新建画布。
//   · 可见性锚原生树节点（own checked 与 effective visible 分立），透明
//     度锚 QgsMapLayer::opacity()，样式锚 styleManager/currentStyle 与
//     paleo/symbolSemantics 版本锚——回执一律回读原生状态（节点回读重找，
//     不吃旧指针），不凭信号发射判定完成。
//   · 修改成功后 setDirty(true)：工程脏标记如实前进；本适配器从不写
//     .qgz、不动 manifest、不落 Catalog 版本（persistence=
//     unsaved_project_display）。同值设置 no_change、零刷新、零脏标记。
//   · state_token/游标是无状态摘要（哈希当前事实），不是授权凭证也不是
//     第二套事务系统；无长期观察连接（数量与寿命上限 = 0）。
#include "agentmapdisplayadapter.h"

#include "geopatterns.h"
#include "qgiscanvascontroller.h"
#include "qgislayerservice.h"
#include "qgisprojectservice.h"
#include "qgisstyleservice.h"

#include <qgsrenderer.h>
#include <qgslayertree.h>
#include <qgslayertreelayer.h>
#include <qgsmapcanvas.h>
#include <qgsmaplayer.h>
#include <qgsmaplayerstylemanager.h>
#include <qgsproject.h>
#include <qgsrasterlayer.h>
#include <qgsvectorlayer.h>

#include <QCryptographicHash>
#include <QJsonArray>
#include <QThread>
#include <algorithm>
#include <cmath>

namespace {
using paleo::agent::mapdisplay::kContract;

// 有界输出：目录分页、预设/词表选项都封顶，超出打计数字段（不截断值）。
constexpr int kDefaultPageSize = 16;
constexpr int kMaxPresetsListed = 32;
constexpr int kMaxSymbolIdsListed = 24;

QString hexDigest(const QString &material) {
  return QString::fromLatin1(QCryptographicHash::hash(
                                 material.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
}

QgsProject *projectOf(QgisProjectService *svc) {
  return svc ? svc->project() : QgsProject::instance();
}

QJsonObject semanticsOf(const QgsMapLayer *layer) {
  const QVariant value = layer->customProperty(QStringLiteral("paleo/symbolSemantics"));
  if (value.userType() != QMetaType::QVariantMap)
    return {};
  const auto map = value.toMap();
  return {{QStringLiteral("family"), map.value(QStringLiteral("family")).toString()},
          {QStringLiteral("id"), map.value(QStringLiteral("id")).toString()},
          {QStringLiteral("version"), map.value(QStringLiteral("version")).toInt()}};
}

bool vectorIsEditing(const QgsMapLayer *layer) {
  const auto *vector = qobject_cast<const QgsVectorLayer *>(layer);
  return vector && vector->isEditable();
}

// 回读用的最新树节点：每次重找（QGIS 信号可能在同步回调里重排/删节点）。
QgsLayerTreeLayer *treeNodeOf(QgsProject *project, const QgsMapLayer *layer) {
  return project && layer ? project->layerTreeRoot()->findLayer(layer->id()) : nullptr;
}

// 父组隐藏时给出第一个隐藏祖先组名（不代为打开任何组）。node 取非 const：
// QgsLayerTreeNode::parent() 在本 QGIS 版本是非 const 访问器。
QString visibilityNoteOf(QgsLayerTreeLayer *node) {
  if (!node)
    return QStringLiteral("tree_node_missing");
  if (!node->itemVisibilityChecked())
    return QStringLiteral("layer_unchecked");
  for (QgsLayerTreeNode *p = node->parent(); p; p = p->parent())
    if (QgsLayerTree::isGroup(p) && !p->itemVisibilityChecked())
      return QStringLiteral("parent_group_hidden:") + p->name();
  return QString();
}

// 显示状态标记：当前实例身份 + 可见性 + 透明度 + 样式 + 语义锚 + 编辑态
// 的摘要。任何事实变化（用户操作、重实例化、工程重开）都使旧标记失配——
// 调用方被导向重新查询，而不是带着陈旧视图继续操作。
QString stateTokenOf(const QString &layerId, const QgsMapLayer *layer,
                     QgsLayerTreeLayer *node) {
  QStringList parts{QStringLiteral("v1"), layerId, layer->id()};
  if (node) {
    parts << QString::number(node->itemVisibilityChecked() ? 1 : 0)
          << QString::number(node->isVisible() ? 1 : 0) << visibilityNoteOf(node);
  } else {
    parts << QStringLiteral("no-tree-node");
  }
  parts << QString::number(layer->opacity(), 'f', 6);
  if (const auto *sm = layer->styleManager())
    parts << sm->currentStyle();
  const QJsonObject semantics = semanticsOf(layer);
  parts << semantics.value(QStringLiteral("family")).toString() + QLatin1Char(':') +
           semantics.value(QStringLiteral("id")).toString() + QLatin1Char(':') +
           QString::number(semantics.value(QStringLiteral("version")).toInt());
  parts << QString::number(vectorIsEditing(layer) ? 1 : 0)
        << QString::number(layer->isValid() ? 1 : 0);
  return hexDigest(parts.join(QChar(1)));
}

QVariantList symbolOptions(const QVariantList &definitions) {
  QVariantList out;
  for (const QVariant &entry : definitions) {
    if (out.size() >= kMaxSymbolIdsListed)
      break;
    const auto map = entry.toMap();
    out.append(QVariantMap{{QStringLiteral("id"), map.value(QStringLiteral("id")).toString()},
                           {QStringLiteral("title"), map.value(QStringLiteral("title")).toString()}});
  }
  return out;
}

// 语义符号适用族：按几何适配（lithology/facies→Polygon，line→Line；点/
// 栅格不开放——GeoPatterns 无点符号，不适合单符号覆盖的层诚实不列选项）。
QJsonArray semanticFamiliesFor(const QgsMapLayer *layer) {
  QJsonArray families;
  const auto *vector = qobject_cast<const QgsVectorLayer *>(layer);
  if (!vector)
    return families;
  const auto append = [&families](const char *family, const QVariantList &defs) {
    QJsonArray ids;
    for (const QVariant &entry : symbolOptions(defs))
      ids.append(QJsonObject::fromVariantMap(entry.toMap()));
    families.append(QJsonObject{{QStringLiteral("family"), QLatin1String(family)},
                                {QStringLiteral("symbol_table_version"),
                                 QgisStyleService::symbolTableVersion()},
                                {QStringLiteral("ids"), ids}});
  };
  if (vector->geometryType() == Qgis::GeometryType::Polygon) {
    append("lithology", GeoPatterns::lithologyDefinitions());
    append("facies", GeoPatterns::faciesDefinitions());
  } else if (vector->geometryType() == Qgis::GeometryType::Line) {
    append("line", GeoPatterns::lineStyleDefinitions());
  }
  return families;
}

QString treeGroupOf(QgsLayerTreeLayer *node) {
  QStringList names;
  for (QgsLayerTreeNode *p = node ? node->parent() : nullptr; p; p = p->parent())
    if (QgsLayerTree::isGroup(p))
      names.prepend(p->name());
  return names.join(QLatin1Char('/'));
}

QJsonObject failPayload(const QString &code, const QString &layerId, const QString &note = {}) {
  QJsonObject payload{{QStringLiteral("ok"), false},
                      {QStringLiteral("code"), code},
                      {QStringLiteral("contract"), kContract}};
  if (!layerId.isEmpty())
    payload.insert(QStringLiteral("layer_id"), layerId);
  if (!note.isEmpty())
    payload.insert(QStringLiteral("note"), note);
  return payload;
}

QJsonObject unavailablePayload(const QString &reason) {
  return {{QStringLiteral("ok"), false}, {QStringLiteral("code"), reason},
          {QStringLiteral("contract"), kContract}};
}
} // namespace

AgentMapDisplayAdapter::AgentMapDisplayAdapter(QgisProjectService *projectSvc,
                                               QgisLayerService *layerSvc,
                                               QgisCanvasController *canvasCtl,
                                               QObject *parent)
    : QObject(parent), m_projectSvc(projectSvc), m_layerSvc(layerSvc), m_canvasCtl(canvasCtl) {}

QString AgentMapDisplayAdapter::unavailabilityReason() const {
  if (QThread::currentThread() != thread())
    return QStringLiteral("owner_thread_required");
  if (!m_layerSvc)
    return QStringLiteral("map_display_provider_unavailable");
  if (!projectOf(m_projectSvc))
    return QStringLiteral("project_unavailable");
  return {};
}

QgsMapLayer *AgentMapDisplayAdapter::resolveManagedLayer(const QString &layerId,
                                                          QJsonObject *result) const {
  const LayerDeclaration decl = m_layerSvc->declaration(layerId);
  if (decl.layerId != layerId) {
    *result = failPayload(QStringLiteral("layer_not_declared"), layerId,
                          QStringLiteral("layer_id 不在 manifest 受管目录内（手工层不可经本工具操作）"));
    return nullptr;
  }
  QgsMapLayer *layer = m_layerSvc->layer(layerId); // 已实例化 ONLY：不触发加载
  if (!layer) {
    *result = failPayload(QStringLiteral("layer_not_materialized"), layerId,
                          QStringLiteral("声明存在但当前未实例化（不自动加载；先经原生层位切换/导入实例化）"));
    return nullptr;
  }
  if (layer->customProperty(QStringLiteral("paleoLayerId")).toString() != layerId) {
    *result = failPayload(QStringLiteral("layer_identity_mismatch"), layerId,
                          QStringLiteral("实例的受管身份标记与 layer_id 不一致（疑似手工仿造层），拒绝操作"));
    return nullptr;
  }
  return layer;
}

QString AgentMapDisplayAdapter::displayRevision() const {
  QVector<LayerDeclaration> decls;
  if (!m_layerSvc->tryDeclared(&decls, nullptr))
    return hexDigest(QStringLiteral("manifest-unavailable"));
  std::sort(decls.begin(), decls.end(),
            [](const LayerDeclaration &a, const LayerDeclaration &b) { return a.layerId < b.layerId; });
  QgsProject *project = projectOf(m_projectSvc);
  QStringList parts{QStringLiteral("v1")};
  for (const LayerDeclaration &decl : decls) {
    QgsMapLayer *layer = m_layerSvc->isInstantiated(decl.layerId) ? m_layerSvc->layer(decl.layerId)
                                                                  : nullptr;
    parts << decl.layerId
          << (layer ? stateTokenOf(decl.layerId, layer, treeNodeOf(project, layer))
                    : QStringLiteral("declared-only"));
  }
  return hexDigest(parts.join(QChar(1)));
}

QJsonObject AgentMapDisplayAdapter::list(const QJsonObject &request) {
  if (const QString reason = unavailabilityReason(); !reason.isEmpty())
    return unavailablePayload(reason);
  QVector<LayerDeclaration> decls;
  QString error;
  if (!m_layerSvc->tryDeclared(&decls, &error)) {
    QJsonObject payload = unavailablePayload(QStringLiteral("manifest_unavailable"));
    payload.insert(QStringLiteral("error"), error);
    return payload;
  }
  const QString horizon = request.value(QStringLiteral("horizon")).toString();
  const QString type = request.value(QStringLiteral("type")).toString();
  const int pageSize = request.value(QStringLiteral("page_size")).toInt(kDefaultPageSize);
  std::sort(decls.begin(), decls.end(),
            [](const LayerDeclaration &a, const LayerDeclaration &b) { return a.layerId < b.layerId; });

  QgsProject *project = projectOf(m_projectSvc);
  struct Row {
    LayerDeclaration decl;
    QgsMapLayer *layer = nullptr;
    QgsLayerTreeLayer *node = nullptr;
    QString token;
  };
  QVector<Row> rows;
  for (const LayerDeclaration &decl : decls) {
    Row row;
    row.decl = decl;
    if (m_layerSvc->isInstantiated(decl.layerId)) {
      row.layer = m_layerSvc->layer(decl.layerId);
      if (row.layer) {
        row.node = treeNodeOf(project, row.layer);
        row.token = stateTokenOf(decl.layerId, row.layer, row.node);
      }
    }
    rows.append(row);
  }
  // display_revision 单一来源（displayRevision()）：list 游标与修改回执共
  // 用同一摘要口径，覆盖全部受管层——任何显示事实变化都让在途游标失配。
  const QString revision = displayRevision();
  const QString cursorFilter = horizon + QChar(1) + type;

  int offset = 0;
  if (request.contains(QStringLiteral("cursor"))) {
    const QString cursor = request.value(QStringLiteral("cursor")).toString();
    const int split = cursor.lastIndexOf(QLatin1Char('|'));
    bool ok = false;
    offset = split > 0 ? cursor.mid(split + 1).toInt(&ok) : 0;
    if (!ok || offset < 0 || cursor.left(split) != hexDigest(revision + QChar(1) + cursorFilter))
      return failPayload(QStringLiteral("cursor_expired"), QString(),
                         QStringLiteral("显示状态或筛选条件已变化，游标作废；重新查询首页"));
  }

  QJsonArray layers;
  int matched = 0, returned = 0;
  bool more = false;
  for (const Row &row : rows) {
    if (!horizon.isEmpty() && row.decl.horizon != horizon)
      continue;
    if (!type.isEmpty() && row.decl.type != type)
      continue;
    ++matched;
    if (matched - 1 < offset)
      continue; // 游标偏移：跳过已交付的前 offset 条
    if (returned >= pageSize) {
      more = true;
      break;
    }
    QJsonObject entry{{QStringLiteral("layer_id"), row.decl.layerId},
                      {QStringLiteral("horizon"), row.decl.horizon},
                      {QStringLiteral("type"), row.decl.type},
                      {QStringLiteral("group"), row.decl.group},
                      {QStringLiteral("title"),
                       row.decl.title.isEmpty() ? row.decl.layerId : row.decl.title},
                      {QStringLiteral("materialized"), bool(row.layer)}};
    if (row.layer) {
      entry.insert(QStringLiteral("valid"), row.layer->isValid());
      if (row.node) {
        entry.insert(QStringLiteral("checked"), row.node->itemVisibilityChecked());
        entry.insert(QStringLiteral("effective_visible"), row.node->isVisible());
        if (row.node->itemVisibilityChecked() && !row.node->isVisible())
          entry.insert(QStringLiteral("visibility_note"), visibilityNoteOf(row.node));
      }
      entry.insert(QStringLiteral("opacity"), row.layer->opacity());
      if (const auto *sm = row.layer->styleManager())
        entry.insert(QStringLiteral("current_style"), sm->currentStyle());
      const QJsonObject semantics = semanticsOf(row.layer);
      if (!semantics.isEmpty())
        entry.insert(QStringLiteral("symbol_semantics"), semantics);
      entry.insert(QStringLiteral("editing"), vectorIsEditing(row.layer));
      entry.insert(QStringLiteral("state_token"), row.token);
    }
    layers.append(entry);
    ++returned;
  }
  QJsonObject payload{{QStringLiteral("ok"), true},
                      {QStringLiteral("code"), QStringLiteral("ok")},
                      {QStringLiteral("display_revision"), revision},
                      {QStringLiteral("canvas_present"),
                       bool(m_canvasCtl && m_canvasCtl->canvasIfCreated())},
                      {QStringLiteral("total"), matched},
                      {QStringLiteral("returned"), returned},
                      {QStringLiteral("has_more"), more},
                      {QStringLiteral("layers"), layers},
                      {QStringLiteral("note"),
                       QStringLiteral("只读快照：未实例化层不触发加载；显示修改未保存为工程文件")},
                      {QStringLiteral("contract"), kContract}};
  if (more)
    payload.insert(QStringLiteral("next_cursor"),
                   hexDigest(revision + QChar(1) + cursorFilter) + QLatin1Char('|') +
                       QString::number(offset + returned));
  return payload;
}

QJsonObject AgentMapDisplayAdapter::get(const QString &layerId) {
  if (const QString reason = unavailabilityReason(); !reason.isEmpty())
    return unavailablePayload(reason);
  QJsonObject result;
  QgsMapLayer *layer = resolveManagedLayer(layerId, &result);
  if (!layer && result.value(QStringLiteral("code")).toString() != QLatin1String("layer_not_materialized"))
    return result; // layer_not_declared / identity_mismatch：诚实错误
  const LayerDeclaration decl = m_layerSvc->declaration(layerId);
  QJsonObject payload{{QStringLiteral("ok"), true},
                      {QStringLiteral("code"), QStringLiteral("ok")},
                      {QStringLiteral("layer_id"), layerId},
                      {QStringLiteral("materialized"), bool(layer)},
                      {QStringLiteral("display_revision"), displayRevision()},
                      {QStringLiteral("canvas_present"),
                       bool(m_canvasCtl && m_canvasCtl->canvasIfCreated())},
                      {QStringLiteral("contract"), kContract}};
  if (decl.layerId == layerId) {
    payload.insert(QStringLiteral("horizon"), decl.horizon);
    payload.insert(QStringLiteral("type"), decl.type);
    payload.insert(QStringLiteral("group"), decl.group);
    payload.insert(QStringLiteral("title"), decl.title.isEmpty() ? decl.layerId : decl.title);
  }
  if (!layer) {
    // 未实例化是可接受的诚实结果：查询不触发加载，修改须先经原生入口。
    payload.insert(QStringLiteral("code"), QStringLiteral("not_materialized"));
    payload.insert(QStringLiteral("note"),
                   QStringLiteral("声明存在但未实例化；查询不触发加载，修改须先经原生入口实例化"));
    return payload;
  }
  QgsProject *project = projectOf(m_projectSvc);
  QgsLayerTreeLayer *node = treeNodeOf(project, layer);
  payload.insert(QStringLiteral("valid"), layer->isValid());
  if (node) {
    payload.insert(QStringLiteral("tree_group"), treeGroupOf(node));
    payload.insert(QStringLiteral("checked"), node->itemVisibilityChecked());
    payload.insert(QStringLiteral("effective_visible"), node->isVisible());
    const QString note = visibilityNoteOf(node);
    if (!note.isEmpty())
      payload.insert(QStringLiteral("visibility_note"), note);
  }
  payload.insert(QStringLiteral("opacity"), layer->opacity());
  if (const auto *sm = layer->styleManager()) {
    payload.insert(QStringLiteral("current_style"), sm->currentStyle());
    const QStringList presets = sm->styles();
    QJsonArray presetArray;
    for (int i = 0; i < presets.size() && i < kMaxPresetsListed; ++i)
      presetArray.append(presets.at(i));
    payload.insert(QStringLiteral("presets"), presetArray);
    if (presets.size() > kMaxPresetsListed)
      payload.insert(QStringLiteral("presets_truncated"), presets.size());
  }
  const QJsonObject semantics = semanticsOf(layer);
  if (!semantics.isEmpty())
    payload.insert(QStringLiteral("symbol_semantics"), semantics);
  payload.insert(QStringLiteral("semantic_families"), semanticFamiliesFor(layer));
  payload.insert(QStringLiteral("editing"), vectorIsEditing(layer));
  payload.insert(QStringLiteral("state_token"), stateTokenOf(layerId, layer, node));
  return payload;
}

QJsonObject AgentMapDisplayAdapter::setVisibility(const QString &layerId, bool visible,
                                                  const QString &stateToken) {
  if (const QString reason = unavailabilityReason(); !reason.isEmpty())
    return unavailablePayload(reason);
  QJsonObject result;
  QgsMapLayer *layer = resolveManagedLayer(layerId, &result);
  if (!layer)
    return result;
  QgsProject *project = projectOf(m_projectSvc);
  QgsLayerTreeLayer *node = treeNodeOf(project, layer);
  if (!node)
    return failPayload(QStringLiteral("tree_node_missing"), layerId,
                       QStringLiteral("实例不在当前图层树（工程树已重排/清理）"));
  if (stateToken != stateTokenOf(layerId, layer, node))
    return failPayload(QStringLiteral("state_token_stale"), layerId,
                       QStringLiteral("显示状态与 state_token 不符（可能已被操作/重实例化）；"
                                      "重新 map_layer_get 获取新标记"));
  if (node->itemVisibilityChecked() == visible) {
    // 同值：no_change——不置脏、不刷新、不制造新修订事实。
    node = treeNodeOf(project, layer);
    QJsonObject payload{{QStringLiteral("ok"), true},
                        {QStringLiteral("code"), QStringLiteral("no_change")},
                        {QStringLiteral("layer_id"), layerId},
                        {QStringLiteral("requested"), visible},
                        {QStringLiteral("checked"), node && node->itemVisibilityChecked()},
                        {QStringLiteral("effective_visible"), node && node->isVisible()},
                        {QStringLiteral("mutated"), false},
                        {QStringLiteral("persistence"), QStringLiteral("unsaved_project_display")},
                        {QStringLiteral("canvas_present"),
                         bool(m_canvasCtl && m_canvasCtl->canvasIfCreated())},
                        {QStringLiteral("contract"), kContract}};
    if (node)
      payload.insert(QStringLiteral("state_token"), stateTokenOf(layerId, layer, node));
    return payload;
  }
  const QPointer<QgsMapLayer> layerGuard(layer);
  const QPointer<QgsLayerTreeNode> nodeGuard(node);
  node->setItemVisibilityChecked(visible); // 同步信号：面板/桥接自行刷新
  if (!layerGuard || !nodeGuard)
    return failPayload(QStringLiteral("state_changed_during_apply"), layerId,
                       QStringLiteral("应用过程中图层/树节点被删除；无法证明终态"));
  if (nodeGuard->itemVisibilityChecked() != visible)
    return failPayload(QStringLiteral("apply_failed"), layerId,
                       QStringLiteral("设置后回读与请求不一致（同步回调改写了状态）"));
  project->setDirty(true);
  if (m_canvasCtl)
    if (auto *canvas = m_canvasCtl->canvasIfCreated())
      canvas->refresh();
  node = treeNodeOf(project, layer); // 回读重找节点：以原生终态为准
  QJsonObject payload{{QStringLiteral("ok"), true},
                      {QStringLiteral("code"), QStringLiteral("applied")},
                      {QStringLiteral("layer_id"), layerId},
                      {QStringLiteral("requested"), visible},
                      {QStringLiteral("checked"), node && node->itemVisibilityChecked()},
                      {QStringLiteral("effective_visible"), node && node->isVisible()},
                      {QStringLiteral("mutated"), true},
                      {QStringLiteral("display_revision"), displayRevision()},
                      {QStringLiteral("persistence"), QStringLiteral("unsaved_project_display")},
                      {QStringLiteral("canvas_present"),
                       bool(m_canvasCtl && m_canvasCtl->canvasIfCreated())},
                      {QStringLiteral("contract"), kContract}};
  if (node) {
    const QString note = visibilityNoteOf(node);
    if (!note.isEmpty())
      payload.insert(QStringLiteral("visibility_note"), note);
    payload.insert(QStringLiteral("state_token"), stateTokenOf(layerId, layer, node));
  }
  return payload;
}

QJsonObject AgentMapDisplayAdapter::setOpacity(const QString &layerId, double opacity,
                                                const QString &stateToken) {
  if (const QString reason = unavailabilityReason(); !reason.isEmpty())
    return unavailablePayload(reason);
  QJsonObject result;
  QgsMapLayer *layer = resolveManagedLayer(layerId, &result);
  if (!layer)
    return result;
  if (vectorIsEditing(layer))
    return failPayload(QStringLiteral("layer_editing"), layerId,
                       QStringLiteral("目标图层处于编辑会话；先结束编辑再改显示状态"));
  const bool raster = qobject_cast<const QgsRasterLayer *>(layer) != nullptr;
  const bool vector = qobject_cast<const QgsVectorLayer *>(layer) != nullptr;
  if (!raster && !vector)
    return failPayload(QStringLiteral("unsupported_layer_type"), layerId,
                       QStringLiteral("透明度只支持矢量/栅格受管图层"));
  QgsProject *project = projectOf(m_projectSvc);
  QgsLayerTreeLayer *node = treeNodeOf(project, layer);
  if (stateToken != stateTokenOf(layerId, layer, node))
    return failPayload(QStringLiteral("state_token_stale"), layerId,
                       QStringLiteral("显示状态与 state_token 不符；重新 map_layer_get 获取新标记"));
  const auto receipt = [&](const char *code, bool mutated) {
    QgsLayerTreeLayer *fresh = treeNodeOf(project, layer);
    QJsonObject payload{{QStringLiteral("ok"), true},
                        {QStringLiteral("code"), QLatin1String(code)},
                        {QStringLiteral("layer_id"), layerId},
                        {QStringLiteral("requested"), opacity},
                        {QStringLiteral("opacity"), layer->opacity()},
                        {QStringLiteral("mutated"), mutated},
                        {QStringLiteral("display_revision"), displayRevision()},
                        {QStringLiteral("persistence"), QStringLiteral("unsaved_project_display")},
                        {QStringLiteral("canvas_present"),
                         bool(m_canvasCtl && m_canvasCtl->canvasIfCreated())},
                        {QStringLiteral("contract"), kContract}};
    if (std::abs(layer->opacity() - opacity) > 1e-9)
      payload.insert(QStringLiteral("note"),
                     QStringLiteral("回读值与请求不同：应用过程中有并发修改，以回读为准"));
    if (fresh)
      payload.insert(QStringLiteral("state_token"), stateTokenOf(layerId, layer, fresh));
    return payload;
  };
  if (std::abs(layer->opacity() - opacity) <= 1e-9)
    return receipt("no_change", false);
  const QPointer<QgsMapLayer> layerGuard(layer);
  layer->setOpacity(opacity); // opacityChanged 同步发射：并发修改以回读为准
  if (!layerGuard)
    return failPayload(QStringLiteral("state_changed_during_apply"), layerId,
                       QStringLiteral("应用过程中图层被删除；无法证明终态"));
  project->setDirty(true);
  if (m_canvasCtl)
    if (auto *canvas = m_canvasCtl->canvasIfCreated())
      canvas->refresh();
  return receipt("applied", true);
}

QJsonObject AgentMapDisplayAdapter::applyStyle(const QString &layerId, const QString &namedPreset,
                                               const QString &semanticFamily,
                                               const QString &semanticId,
                                               const QString &stateToken) {
  if (const QString reason = unavailabilityReason(); !reason.isEmpty())
    return unavailablePayload(reason);
  const bool hasPreset = !namedPreset.isEmpty();
  const bool hasSemantic = !semanticFamily.isEmpty() || !semanticId.isEmpty();
  if (hasPreset == hasSemantic)
    return failPayload(QStringLiteral("style_target_required"), layerId,
                       QStringLiteral("named_preset 与 semantic_symbol 必须恰好给一个"));
  QJsonObject result;
  QgsMapLayer *layer = resolveManagedLayer(layerId, &result);
  if (!layer)
    return result;
  if (vectorIsEditing(layer))
    return failPayload(QStringLiteral("layer_editing"), layerId,
                       QStringLiteral("目标图层处于编辑会话；先结束编辑再改显示状态"));
  QgsProject *project = projectOf(m_projectSvc);
  QgsLayerTreeLayer *node = treeNodeOf(project, layer);
  if (stateToken != stateTokenOf(layerId, layer, node))
    return failPayload(QStringLiteral("state_token_stale"), layerId,
                       QStringLiteral("显示状态与 state_token 不符；重新 map_layer_get 获取新标记"));
  const auto base = [&](const char *code, bool mutated, const char *kind) {
    QgsLayerTreeLayer *fresh = treeNodeOf(project, layer);
    QJsonObject payload{{QStringLiteral("ok"), true},
                        {QStringLiteral("code"), QLatin1String(code)},
                        {QStringLiteral("layer_id"), layerId},
                        {QStringLiteral("applied_kind"), QLatin1String(kind)},
                        {QStringLiteral("mutated"), mutated},
                        {QStringLiteral("display_revision"), displayRevision()},
                        {QStringLiteral("persistence"), QStringLiteral("unsaved_project_display")},
                        {QStringLiteral("canvas_present"),
                         bool(m_canvasCtl && m_canvasCtl->canvasIfCreated())},
                        {QStringLiteral("contract"), kContract}};
    if (const auto *sm = layer->styleManager())
      payload.insert(QStringLiteral("current_style"), sm->currentStyle());
    const QJsonObject semantics = semanticsOf(layer);
    if (!semantics.isEmpty())
      payload.insert(QStringLiteral("symbol_semantics"), semantics);
    if (fresh)
      payload.insert(QStringLiteral("state_token"), stateTokenOf(layerId, layer, fresh));
    return payload;
  };
  const QPointer<QgsMapLayer> layerGuard(layer);
  if (hasPreset) {
    auto *sm = layer->styleManager();
    if (!sm)
      return failPayload(QStringLiteral("style_manager_unavailable"), layerId);
    const QStringList presets = sm->styles();
    if (!presets.contains(namedPreset))
      return failPayload(QStringLiteral("preset_not_found"), layerId,
                         QStringLiteral("预设名不在当前图层样式管理器（选项以 map_layer_get 的 "
                                        "presets 为准；预设内容变更后按名应用即取新内容）"));
    if (sm->currentStyle() == namedPreset)
      return base("no_change", false, "named_preset");
    // setCurrentStyle 同步发 styleChanged：先经守卫重取 styleManager 回读，
    // 不在 layerGuard 失效后继续解引用旧 sm（review R2）。
    const bool switched = sm->setCurrentStyle(namedPreset);
    if (!layerGuard)
      return failPayload(QStringLiteral("state_changed_during_apply"), layerId,
                         QStringLiteral("应用过程中图层被删除；无法证明终态"));
    if (!switched || layerGuard->styleManager() == nullptr ||
        layerGuard->styleManager()->currentStyle() != namedPreset)
      return failPayload(QStringLiteral("style_apply_failed"), layerId,
                         QStringLiteral("setCurrentStyle 失败或回读不一致；未证明已呈现"));
    project->setDirty(true);
    if (m_canvasCtl)
      if (auto *canvas = m_canvasCtl->canvasIfCreated())
        canvas->refresh();
    return base("applied", true, "named_preset");
  }
  // 语义符号：词表/几何适配由 GeoPatterns + QgisStyleService 单一权威裁决；
  // 不适用时 applySymbolOverride 不动 renderer、不写语义锚（零副作用）。
  auto *vector = qobject_cast<QgsVectorLayer *>(layer);
  if (!vector)
    return failPayload(QStringLiteral("semantic_symbol_unsupported_geometry"), layerId,
                       QStringLiteral("语义符号只适用于矢量图层"));
  if (semanticFamily != QLatin1String("lithology") && semanticFamily != QLatin1String("facies") &&
      semanticFamily != QLatin1String("line"))
    return failPayload(QStringLiteral("invalid_semantic_family"), layerId);
  if (GeoPatterns::familyOf(semanticId) != semanticFamily)
    return failPayload(QStringLiteral("semantic_symbol_unknown"), layerId,
                       QStringLiteral("id 不在 GeoPatterns 词表（family/id 以 map_layer_get 选项为准）"));
  const QJsonObject semantics = semanticsOf(layer);
  if (semantics.value(QStringLiteral("family")).toString() == semanticFamily &&
      semantics.value(QStringLiteral("id")).toString() == semanticId &&
      semantics.value(QStringLiteral("version")).toInt() == QgisStyleService::symbolTableVersion())
    return base("no_change", false, "semantic_symbol");
  if (!QgisStyleService::applySymbolOverride(vector, semanticFamily, semanticId) || !layerGuard)
    return failPayload(QStringLiteral("semantic_symbol_inapplicable"), layerId,
                       QStringLiteral("几何不匹配或符号构造失败；renderer 与语义锚未改动"));
  const QJsonObject appliedSemantics = semanticsOf(layer);
  const bool verified =
      appliedSemantics.value(QStringLiteral("family")).toString() == semanticFamily &&
      appliedSemantics.value(QStringLiteral("id")).toString() == semanticId && vector->renderer() &&
      vector->renderer()->type() == QLatin1String("singleSymbol");
  if (!verified)
    return failPayload(QStringLiteral("style_apply_failed"), layerId,
                       QStringLiteral("已调用样式入口但回读语义/renderer 不一致；以回读为准，未证明已呈现"));
  project->setDirty(true);
  if (m_canvasCtl)
    if (auto *canvas = m_canvasCtl->canvasIfCreated())
      canvas->refresh();
  return base("applied", true, "semantic_symbol");
}
