// 层：功能
#include "capabilitygateway.h"
namespace paleo::capability {
QJsonObject runChatTool(const QString &toolName, const QJsonObject &arguments, const QString &hostScope,
                        const QString &documentId, const QString &operationDir, const QString &hostHead,
                        const std::function<bool()> &cancelled, DataCatalog *catalog, const QString &projectDir) {
  ExecuteRequest request;
  const auto error = prepareChatToolRequest(request, toolName, arguments, hostScope, documentId, operationDir, hostHead);
  if (!error.isEmpty())
    return error;
  request.catalog = catalog;
  request.projectDir = projectDir;
  request.cancelled = cancelled;
  return chatToolResponse(execute(request));
}
} // namespace paleo::capability
