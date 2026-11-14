// 层：组装根
#include "agentproviderwiring.h"
#include "services/trackinginput.h"
#include "workflow/documentsession.h"

AiChatToolRunner::TrackingInputProvider trackingInputProvider(paleo::document::DocumentSession *document) {
  const QPointer<paleo::document::DocumentSession> owner(document);
  return [owner](const std::function<bool()> &cancelled, AiChatToolRunner::TrackingInput *out, QString *error) {
    if (!owner || !owner->isOpen() || !out) {
      if (error) *error = QStringLiteral("document_unavailable"); return false;
    }
    paleo::tracking::ReadPlan plan;
    plan.interpretation = owner->interpretationSelection();
    if (!plan.interpretation.error.isEmpty() || !plan.interpretation.context ||
        !plan.interpretation.context->targetVolume || !plan.interpretation.volume.ref.valid()) {
      if (error) *error = plan.interpretation.error.isEmpty() ? QStringLiteral("tracking_volume_required")
                                                           : plan.interpretation.error;
      return false;
    }
    out->identity = {{QStringLiteral("volume"), paleo::document::modelObjectRefJson(*plan.interpretation.context->targetVolume)}};
    const auto epoch = owner->epoch();
    out->ownerValidation = [owner, epoch, plan]() -> QString {
      if (!owner || !owner->isOpen() || owner->epoch() != epoch) return QStringLiteral("stale_session");
      const auto current = owner->interpretationSelection();
      if (current.context != plan.interpretation.context || current.volume.ref != plan.interpretation.volume.ref)
        return QStringLiteral("tracking_binding_changed");
      const auto invalid = paleo::document::validateInterpretationSelection(plan.interpretation.volume.modelDirectory,
                                                                          plan.interpretation, nullptr);
      return invalid.isEmpty() ? QString() : QStringLiteral("tracking_context_changed");
    };
    out->fetch = [plan, cancelled](int il, int xl, int first, int count, QVector<float> &values, QString &detail) {
      auto result = paleo::tracking::readWindow(plan, il, xl, first, count, cancelled);
      if (!result.ok) { values.clear(); detail = result.code + QLatin1Char(':') + result.detail; return false; }
      values = std::move(result.samples); return true;
    };
    if (error) error->clear(); return true;
  };
}
