// 层：数据
#pragma once
#include <QJsonObject>
#include <QJsonArray>
#include <QString>

class DataCatalog;
namespace paleo::capability {
struct ArtifactVersionRequest {
  QString documentId, versionId;
  int expectedCatalogRevision = -1;
};
struct ArtifactVersionResult {
  bool ok = false;
  QString code, error, documentId;
  int catalogRevision = -1;
  QJsonObject version;
  QJsonObject toJson() const;
};
// One bounded metadata capability, shared by native callers and tool adapters.
// boundDocumentId comes from the host session, never from model parameters.
class ArtifactVersionCapability {
public:
  static QString id();
  static QJsonObject requestSchema();
  // Value-only descriptor is inline so schema adapters do not acquire the
  // execution service's link dependencies.
  static QJsonObject resultSchema() {
    const auto scalar=[](const char *type){return QJsonObject{{"type",QString::fromLatin1(type)}};};
    const auto object=[](const QJsonObject &properties){
      QJsonArray required;
      for(auto it=properties.begin();it!=properties.end();++it)required.append(it.key());
      return QJsonObject{{"type","object"},{"properties",properties},{"required",required},{"additionalProperties",false}};
    };
    const auto card=object({{"id",scalar("string")},{"assetId",scalar("string")},
        {"displayName",scalar("string")},{"fileName",scalar("string")},{"stage",scalar("string")},
        {"versionNumber",scalar("integer")},{"managed",scalar("boolean")},{"sha256",scalar("string")},
        {"stale",scalar("boolean")},{"labelsTruncated",scalar("boolean")},{"parentCount",scalar("string")},{"parentsTruncated",scalar("boolean")},
        {"parentVersionIds",QJsonObject{{"type","array"},{"maxItems",128},{"items",scalar("string")}}}});
    return object({{"ok",scalar("boolean")},{"code",scalar("string")},{"error",scalar("string")},
        {"documentId",scalar("string")},{"catalogRevision",scalar("integer")},
        {"version",QJsonObject{{"oneOf",QJsonArray{card,object({})}}}}});
  }
  static ArtifactVersionResult describe(const DataCatalog *catalog,
      const QString &boundDocumentId, const ArtifactVersionRequest &request);
};
}
