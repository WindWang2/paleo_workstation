// 层：功能
#include "aichattoolrunner.h"
#include "catalog/datacatalog.h"
#include "services/artifactversioncapability.h"
#include "services/projectquerycapability.h"
#include <QJsonArray>

void AiChatToolRunner::runQuery(const ChatToolCall &call, const QJsonObject &args) {
  Q_UNUSED(call)
  const auto topic=args.value("topic").toString();
  if(topic==QLatin1String("local_models")){runLocalModels(args);return;}
  if(topic==QLatin1String("well_facies_models") || topic==QLatin1String("well_facies_inputs")) {
    const QStringList allowed=topic==QLatin1String("well_facies_models")?QStringList{"topic","model_id"}:QStringList{"topic","well_ref"};
    for(auto it=args.begin();it!=args.end();++it)if(!allowed.contains(it.key())) {
      finishActive(false,{{"code","unsupported_topic_argument"},{"field",it.key()}});return;
    }
    if(topic==QLatin1String("well_facies_models"))runFaciesModels(args);else runFaciesInputs(args);
    return;
  }
  if (!m_context.catalog || !m_context.catalog->isOpen()) {
    finishActive(false, errorPayload(tr("未绑定工程目录（须先打开工程再查工程数据）"))); return;
  }
  if (args.value("topic").toString() == QLatin1String("version")) {
    paleo::capability::ArtifactVersionRequest request;
    request.documentId = m_context.documentId; request.versionId = args.value("version_id").toString();
    request.expectedCatalogRevision = args.value("catalog_revision").toInt(m_context.catalog->catalogRevision());
    const auto result = paleo::capability::ArtifactVersionCapability::describe(m_context.catalog, m_context.documentId, request);
    QJsonObject payload{{"tool","paleo.query_project"},{"topic","version"},{"version",result.toJson()}};
    if (!result.ok) payload.insert("error", result.error);
    finishActive(result.ok, payload); return;
  }
  auto result = m_query->query(args);
  if (result.value("code").toString() == QLatin1String("well_not_found")) {
    QStringList known;
    for (const auto &value : result.value("known_wells").toArray()) known.append(value.toString());
    const auto reference = args.value(args.contains("well_id") ? "well_id" : "well_name").toString();
    result.insert("error", tr("井不存在：%1；工程现有井示例：%2").arg(reference, known.join(QStringLiteral("、"))));
  }
  finishActive(result.value("ok").toBool(), result);
}
void AiChatToolRunner::runLineage(const ChatToolCall &call, const QJsonObject &args) {
  Q_UNUSED(call)
  if (!m_context.catalog || !m_context.catalog->isOpen()) {
    finishActive(false, errorPayload(tr("未绑定工程目录（须先打开工程再查血缘）"))); return;
  }
  const auto result = m_query->lineage(args);
  finishActive(result.value("ok").toBool(), result);
}
