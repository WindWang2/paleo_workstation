// 层：数据
#include "hostcallcontext.h"

namespace paleo::host {

QString checkContext(const CallContext &live, const CallContext &expected,
                     unsigned effects, bool requireRevision) {
  if (!live.open || live.documentId.isEmpty())
    return QStringLiteral("document_unavailable");
  if (live.documentId != expected.documentId || live.epoch != expected.epoch)
    return QStringLiteral("stale_session");
  if (requireRevision && (live.catalogRevision != expected.catalogRevision ||
                          live.mutationSequence != expected.mutationSequence))
    return QStringLiteral("stale_command");
  if ((effects & ~live.allowedEffects) != 0U)
    return QStringLiteral("effect_denied");
  const bool projectWrite = (effects & (PrivateStage | CatalogPublish | Presentation)) != 0U;
  if (projectWrite && !live.persistentIdentity)
    return QStringLiteral("persistent_identity_required");
  if (projectWrite && live.readOnly)
    return QStringLiteral("project_read_only");
  if (projectWrite && !live.ownsWrites)
    return QStringLiteral("project_write_ownership_required");
  if (projectWrite && live.recoveryRequired)
    return QStringLiteral("recovery_required");
  return {};
}

QString checkRequiredContext(const CallContext &live, const QStringList &required) {
  for (const auto &key : required) {
    if (key == QLatin1String("project")) {
      if (!live.open || live.documentId.isEmpty()) return QStringLiteral("document_unavailable");
    } else if (key == QLatin1String("activeHorizon")) {
      if (live.horizon.isEmpty()) return QStringLiteral("horizon_required");
    } else if (key.startsWith(QLatin1String("activeDocument:"))) {
      if (live.activeDocumentKind != key.mid(15)) return QStringLiteral("active_document_required");
    } else if (key.startsWith(QLatin1String("selection:"))) {
      if (live.selectionIds.isEmpty()) return QStringLiteral("selection_required");
    } else {
      return QStringLiteral("unsupported_required_context");
    }
  }
  return {};
}

unsigned computeEffects(const QString &intent) {
  if (intent == QLatin1String("publish")) return Read | PrivateStage | CatalogPublish | Presentation;
  if (intent.isEmpty() || intent == QLatin1String("prepare") || intent == QLatin1String("compute-only"))
    return Read | PrivateStage;
  return 0U;
}

} // namespace paleo::host
