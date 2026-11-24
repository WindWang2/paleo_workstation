// 层：功能
// workflow/aichattoolrunner_display.cpp — I3：paleo.map_layer* 五工具的
// runner 分支。宿主效果门（查询 Read / 显示修改 Read|Presentation）在
// 委托前用 live context 复核；provider/工程可用性来自适配器同一来源；
// 回执带 contract=map-display.v1 的回读事实，经 finishActive 异步回报
//（#281 纪律）。本层不出现 Qgs 符号（qgis-spread 闸）。
#include "aichattoolrunner.h"
#include "agentmapdisplay.h"
#include "ai/chat/mapdisplaytools.h"
#include "capabilityhost.h"
#include "qgis/agentmapdisplayadapter.h"
#include "services/hostcallcontext.h"

void AiChatToolRunner::setMapDisplay(paleo::agent::mapdisplay::Access *access, QObject *owner) {
  // owner 与 access 成对供给：无 owner 的供给无法守卫寿命，按未安装处理
  //（诚实缺位优于悬垂可用性）。
  if (access && !owner)
    access = nullptr;
  if (m_mapDisplay == access && m_mapDisplayOwner == owner)
    return;
  m_mapDisplay = access;
  m_mapDisplayOwner = owner;
  ++m_bindingRevision;
  invalidateContext(); // 供给变化即作废在途资格判断与排队调用
}

void AiChatToolRunner::runMapDisplayTool(const QString &name, const QJsonObject &args) {
  namespace display = paleo::agent::mapdisplay;
  const auto fail = [this](const QString &code) {
    finishActive(false, QJsonObject{{QStringLiteral("ok"), false},
                                    {QStringLiteral("code"), code},
                                    {QStringLiteral("contract"), QLatin1String(display::kContract)}});
  };
  // QPointer 的 bool 语义即 !isNull()：owner 存活判定只能用 isNull()
  //（owner 被销毁后 QPointer 清空，这里如实拒绝而非解引用悬垂指针）。
  if (!m_mapDisplay || m_mapDisplayOwner.isNull())
    return fail(QStringLiteral("map_display_provider_unavailable"));
  // 严格外部档之外的互斥分支/内部形状/长度界（与 readiness 同一实现）。
  {
    QString field;
    const QString code = display::validateMapDisplayArguments(name, args, &field);
    if (!code.isEmpty()) {
      QJsonObject payload{{QStringLiteral("ok"), false},
                          {QStringLiteral("code"), code},
                          {QStringLiteral("contract"), QLatin1String(display::kContract)}};
      if (!field.isEmpty())
        payload.insert(QStringLiteral("field"), field);
      finishActive(false, payload);
      return;
    }
  }
  // 效果随工具静态划分：查询 Read；显示修改 Read|Presentation。宿主门用
  // live context 复核（文档/世代/只读/写锁/恢复态），模型无法经实参自授。
  const unsigned effects = display::isMapDisplayMutationTool(name)
                               ? unsigned(paleo::host::Read | paleo::host::Presentation)
                               : unsigned(paleo::host::Read);
  if (!m_context.capabilityHost)
    return fail(QStringLiteral("host_unavailable"));
  const auto live = m_context.capabilityHost->context();
  auto expected = live;
  expected.documentId = m_context.documentId;
  expected.epoch = m_context.documentEpoch;
  const QString denied = paleo::host::checkContext(live, expected, effects);
  if (!denied.isEmpty())
    return finishActive(false, QJsonObject{{QStringLiteral("ok"), false},
                                           {QStringLiteral("code"), denied},
                                           {QStringLiteral("contract"),
                                            QLatin1String(display::kContract)}});
  if (const QString reason = m_mapDisplay->unavailabilityReason(); !reason.isEmpty())
    return fail(reason);

  const QString layerId = args.value(QStringLiteral("layer_id")).toString();
  const QString token = args.value(QStringLiteral("state_token")).toString();
  QJsonObject result;
  if (name == QLatin1String(display::kLayersTool))
    result = m_mapDisplay->list(args);
  else if (name == QLatin1String(display::kGetTool))
    result = m_mapDisplay->get(layerId);
  else if (name == QLatin1String(display::kSetVisibilityTool))
    result = m_mapDisplay->setVisibility(layerId, args.value(QStringLiteral("visible")).toBool(),
                                         token);
  else if (name == QLatin1String(display::kSetOpacityTool))
    result = m_mapDisplay->setOpacity(layerId, args.value(QStringLiteral("opacity")).toDouble(),
                                      token);
  else if (name == QLatin1String(display::kApplyStyleTool)) {
    const auto symbol = args.value(QStringLiteral("semantic_symbol")).toObject();
    result = m_mapDisplay->applyStyle(layerId, args.value(QStringLiteral("named_preset")).toString(),
                                      symbol.value(QStringLiteral("family")).toString(),
                                      symbol.value(QStringLiteral("id")).toString(), token);
  } else
    return fail(QStringLiteral("tool_not_found"));
  if (!result.contains(QStringLiteral("contract")))
    result.insert(QStringLiteral("contract"), QLatin1String(display::kContract));
  finishActive(result.value(QStringLiteral("ok")).toBool(), result);
}
