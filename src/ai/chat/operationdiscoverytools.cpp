// 层：功能
// ai/chat/operationdiscoverytools.cpp — H4：paleo.operation_list 工具声明。
// 唯一不要求已知 operation_id 的 operation 前缀工具：在当前工程与当前
// 聊天作用域内有界发现既有合法持久操作（只读快照），或以锚点分页其直接
// 关系。旧六工具（get/events/resume/cancel/results/read）的必填与效果
// 不变；本工具效果恒为 Read。分页数值域与运行期（operationdiscovery.cpp
// 的 integerArgument 校验、queryDigest 归一缺省）同值。
#include "domaintools.h"

void appendAgentOperationDiscoveryTools(QVector<AiToolSpec> &specs) {
  const auto text = [](const char *key, bool required) {
    return AiToolParamSpec{QLatin1String(key), QStringLiteral("string"), QString(), required, {}};
  };
  const auto integer = [](const char *key, int minimum, int maximum) {
    AiToolParamSpec spec{QLatin1String(key), QStringLiteral("integer"), QString(), false, {}};
    spec.minimum = minimum;
    spec.maximum = maximum;
    return spec;
  };
  const auto response = QStringLiteral(R"({"type":"object","required":["code"],"properties":{"ok":{"type":"boolean"},"code":{"type":"string"},"view":{"type":"string"},"operationId":{"type":"string"},"operations":{"type":"array"},"relations":{"type":"array"},"returnedCount":{"type":"integer"},"hasMore":{"type":"boolean"},"nextCursor":{"type":"string"},"coverage":{"type":"object"}}})");
  AiToolSpec spec;
  spec.name = QStringLiteral("paleo.operation_list");
  spec.title = QObject::tr("发现持久操作");
  spec.description = QObject::tr(
      "在当前工程与当前聊天作用域内列出既有合法持久操作的准确摘要，或分页一个已授权操作的直接关系。"
      "只读快照：不执行、不恢复、不对账、不写任何业务数据；列出的是日志记录事实，"
      "当前状态须再调 paleo.operation_get 对账。清单默认按 operationId 字典序分页；"
      "relations 视图以 anchor_operation_id 为锚点呈现来源操作、同作用域消费者、"
      "计划内步骤（不可据此独立 get/resume）与父版本（catalog 版本，非 operationId）。"
      "发现不要求已知 operation_id；得到 ID 后显式调用 get/results/read 等既有工具。");
  auto view = text("view", false);
  view.enumValues = {QStringLiteral("operations"), QStringLiteral("relations")};
  view.description = QObject::tr("视图：operations=作用域清单（缺省）；relations=锚点直接关系");
  auto state = text("execution_state", false);
  state.enumValues = {QStringLiteral("queued"),   QStringLiteral("running"),
                      QStringLiteral("prepared"), QStringLiteral("succeeded"),
                      QStringLiteral("failed"),   QStringLiteral("cancelled"),
                      QStringLiteral("interrupted"), QStringLiteral("external_unknown")};
  state.description = QObject::tr("按记录的 execution 字段精确筛选（仅 operations 视图）");
  auto capability = text("capability_id", false);
  capability.description = QObject::tr("按记录的 capability_id 精确筛选（仅 operations 视图）");
  auto anchor = text("anchor_operation_id", false);
  anchor.description = QObject::tr("relations 视图必填：锚点操作 ID（须为当前作用域内合法 v2 操作）");
  auto pageSize = integer("page_size", 1, 32);
  pageSize.description = QObject::tr("每页条数（1–32，缺省 16）");
  auto maxBytes = integer("max_bytes", 2048, 24576);
  maxBytes.description = QObject::tr("本页 JSON 字节预算（2048–24576，缺省 8192）");
  auto cursor = text("cursor", false);
  cursor.description = QObject::tr("上一页返回的续页游标（绑定查询与快照；过期/换条件明确报错）");
  spec.parameters = {view, state, capability, anchor, pageSize, maxBytes, cursor};
  spec.effects = paleo::host::Read;
  spec.resultSchemaJson = response;
  specs.append(spec);
}
