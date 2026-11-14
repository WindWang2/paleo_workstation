// 层：数据
#pragma once
#include <QString>
#include <QStringList>

namespace paleo::host {

enum Effect : unsigned {
  Read = 1U, PrivateStage = 2U, CatalogPublish = 4U,
  Presentation = 8U, FileExport = 16U, ExternalTransfer = 32U
};
enum class Caller { Ui, Batch, Agent };

// Host-owned values. Paths, pointers, locks and credentials are deliberately absent.
// Source-value revisions and output CAS belong to the frozen plan, not this epoch.
struct CallContext {
  QString documentId;
  quint64 epoch = 0, catalogRevision = 0, mutationSequence = 0;
  QString horizon, unitId, activeDocumentKind;
  QStringList selectionIds;
  bool open = false, persistentIdentity = false, readOnly = true;
  bool recoveryRequired = false, ownsWrites = false;
  unsigned allowedEffects = Read;
};

// Empty means accepted. A caller cannot grant effects by editing an intent.
QString checkContext(const CallContext &current, const CallContext &expected,
                     unsigned effects, bool requireRevision = false);
QString checkRequiredContext(const CallContext &current, const QStringList &required);
unsigned computeEffects(const QString &intent);

} // namespace paleo::host
