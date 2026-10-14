// 层：数据
#pragma once
#include <QStringList>
#include <QVariantMap>

// Compatible extension of facies_evidence: observations remain separate from
// the interpretation and its append-only review journal. No GIS/UI objects.
namespace paleo::evidence {
QStringList roles();
QStringList types();
QStringList interpretationStates();
bool canTransition(const QString &before, const QString &after);
bool decodeRecords(const QString &json, QVariantList *records, QString *error);
bool decodeInterpretation(const QString &json, QVariantMap *interpretation,
                          QString *error);
QString encode(const QVariant &value);
bool validateRecords(const QVariantList &records, QString *error);
bool validateInterpretation(const QVariantMap &interpretation, QString *error);

struct Revision {
  QVariantList records;
  QVariantMap interpretation;
};
// identity/geometry are frozen by the QGIS owner thread before this pure call.
// expectedRevision is optimistic concurrency control, including legacy 0.
bool revise(const QVariantList &records, const QVariantMap &interpretation,
            const QVariantMap &target, const QString &action,
            const QVariantMap &change, const QString &actor,
            const QString &reason, quint64 expectedRevision,
            Revision *result, QString *error);
// Facts and risks, not a fabricated reliability score. Each finding identifies
// the exact evidence source; verification is distinct from interpretation status.
QVariantList check(const QVariantList &records, const QVariantMap &interpretation,
                   const QVariantMap &target);
} // namespace paleo::evidence
