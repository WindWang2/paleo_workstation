// 层：功能
// workflow/aichattoolrunner_navigation.cpp — I4：六个语义导航原子的
// runner 分支。宿主效果门（Read）用 live context 复核（文档/世代/只读/
// 恢复态——模型无法经实参自授）；严格外部档之外的互斥/集合/长度界与
// readiness 共用 workflow/agentnavigation 的同一实现。异步完成（井资料
// SHA 复验后打开、地震测线真实解码）经编排层回调回 finishActive（#281
// 纪律：迟到回执按世代/调用号作废，不产生第二终态）。取消/interrupt 只
// 作废本任务请求（编排层 cancel），不改回合状态机。
#include "aichattoolrunner.h"
#include "agentnavigation.h"
#include "ai/chat/navigationtools.h"
#include "capabilityhost.h"
#include "services/hostcallcontext.h"

void AiChatToolRunner::setNavigation(paleo::agent::navigation::AgentNavigation *navigation,
                                     QObject *owner) {
  // owner 与 navigation 成对供给：无 owner 的供给无法守卫寿命，按未安装
  // 处理（诚实缺位优于悬垂可用性）。
  if (navigation && !owner)
    navigation = nullptr;
  if (m_navigation == navigation && m_navigationOwner == owner)
    return;
  if (m_navigation)
    m_navigation->cancel(); // 旧供给的在途导航请求作废（不回调）
  m_navigation = navigation;
  m_navigationOwner = owner;
  ++m_bindingRevision;
  invalidateContext(); // 供给变化即作废在途资格判断与排队调用
}

void AiChatToolRunner::runNavigationTool(const QString &name, const QJsonObject &args) {
  namespace navigation = paleo::agent::navigation;
  const auto fail = [this](const QString &code) {
    finishActive(false, QJsonObject{{QStringLiteral("ok"), false},
                                    {QStringLiteral("code"), code},
                                    {QStringLiteral("contract"),
                                     QLatin1String(navigation::kContract)}});
  };
  if (!m_navigation || m_navigationOwner.isNull())
    return fail(QStringLiteral("navigation_provider_unavailable"));
  // 严格外部档之外的互斥分支/集合形状/长度界（与 readiness 同一实现）。
  {
    QString field;
    const QString code = navigation::validateNavigationArguments(name, args, &field);
    if (!code.isEmpty()) {
      QJsonObject payload{{QStringLiteral("ok"), false},
                          {QStringLiteral("code"), code},
                          {QStringLiteral("contract"), QLatin1String(navigation::kContract)}};
      if (!field.isEmpty())
        payload.insert(QStringLiteral("field"), field);
      finishActive(false, payload);
      return;
    }
  }
  // 效果恒 Read（临时选择/视图导航；不改 Presentation 写门）。宿主门用
  // live context 复核。
  if (!m_context.capabilityHost)
    return fail(QStringLiteral("host_unavailable"));
  const QPointer<AiChatToolRunner> guard(this);
  const auto live = m_context.capabilityHost->context();
  if (!guard)
    return;
  auto expected = live;
  expected.documentId = m_context.documentId;
  expected.epoch = m_context.documentEpoch;
  const QString denied =
      paleo::host::checkContext(live, expected, paleo::host::Read);
  if (!denied.isEmpty())
    return finishActive(false, QJsonObject{{QStringLiteral("ok"), false},
                                           {QStringLiteral("code"), denied},
                                           {QStringLiteral("contract"),
                                            QLatin1String(navigation::kContract)}});
  const int generation = m_generation;
  const QString callId = m_activeCall.id;
  navigation::AgentNavigation::Environment environment;
  environment.catalog = m_context.catalog;
  environment.documentId = m_context.documentId;
  m_navigation->run(
      name, args, environment,
      [this, guard, generation, callId](bool ok, const QJsonObject &payload) {
        // 回执恰一次；已取消/作废/串台的迟到回执不回灌（cancel 走编排层
        // 请求代次，不误杀普通 UI 或其他视图的共享读取）。
        if (!guard || generation != m_generation || !m_active || m_activeCall.id != callId)
          return;
        finishActive(ok, payload);
      });
}
