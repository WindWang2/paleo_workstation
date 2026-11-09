// 层：数据
#pragma once
#include <QJsonObject>
#include <QMetaType>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

namespace paleo::document {
enum class FingerprintKind { Unknown, Sha256, SegyQuickV1 };
struct FileStamp {
    qint64 size=-1,modified=0,changed=0,birth=0;
    bool operator==(const FileStamp &) const = default;
};
struct SourceFingerprint {
    FingerprintKind kind = FingerprintKind::Unknown;
    QString value;
    static SourceFingerprint fromStored(const QString &stored);
    QString stored() const;
    bool valid() const;
    bool operator==(const SourceFingerprint &) const = default;
};
struct SourceRef {
    QString documentId, modelStoreId;
    qlonglong sourceId = 0; // Zero is permitted only for the catalog-only store.
    QString fileRevisionId;
    bool valid() const;
    QString key() const;
    bool operator==(const SourceRef &) const = default;
};
enum class LocatorKind { File, Worksheet, RecordGroup, SegyWindow, ArchiveMember, PackageRelationship, ImageCalibration };
struct SourceLocator {
    LocatorKind kind = LocatorKind::File;
    QVariantMap fields; // Positions are interpreted only within SourceRef.fileRevisionId.
    bool valid() const;
    bool operator==(const SourceLocator &) const = default;
};
struct IdentityClaim {
    QString origin, value;
    bool operator==(const IdentityClaim &) const = default;
};
enum class BindingDecision { Candidate, Confirmed, Conflict, Unresolved, Rejected };
struct SourceBinding {
    QString id, resourceKind, resourceId, role, scope;
    SourceRef source;
    SourceLocator locator;
    BindingDecision decision = BindingDecision::Candidate;
    QVector<IdentityClaim> identityClaims;
    QString evidence, resolution;
    bool primary = false;
    quint64 revision = 0;
    QString resourceKey() const;
    QString slotKey() const;
    bool conflictingIdentities() const;
    bool valid(QString *error = nullptr) const;
    bool operator==(const SourceBinding &) const = default;
};
enum class FileAvailability { Unknown, Present, Missing, Unreadable };
enum class Verification { Unverified, FastVerified, HashVerified, Changed, Failed };
enum class ModelAvailability { Unbound, Registered, Ready, Review, Failed };
enum class DerivativeState { None, HeaderComplete, Incomplete, Failed };
struct SourceState {
    FileAvailability file = FileAvailability::Unknown;
    Verification verification = Verification::Unverified;
    ModelAvailability model = ModelAvailability::Unbound;
    DerivativeState derivative = DerivativeState::None;
    QString code, detail, observedUtc, parentPath;
    qint64 size = -1, modifiedMs = 0, bytesRead = 0;
    bool payloadVerified = false; // Header completion never asserts page CRC integrity.
};
struct SourceRecord {
    SourceRef ref;
    SourceFingerprint fingerprint;
    QString assetId, versionId, path, originalPath, kind, modelDirectory;
    bool managed = false;
    SourceState state;
};
struct SourceChangeSet {
    quint64 epoch = 0, revision = 0;
    QString operationId;
    QStringList bindingIds, sourceKeys;
};
struct SourceCommand {
    enum class Kind { RegisterBinding, ConfirmBinding, SelectPrimary, RemoveBinding, Inspect, Relocate, ResolveRelation, UpgradeModelMetadata, ChooseRelation };
    Kind kind = Kind::Inspect;
    quint64 expectedEpoch = 0, expectedRevision = 0;
    SourceBinding binding;
    SourceRef source;
    QString bindingId, note, path, operationId;
    bool verify = false;
    QVariantMap timeDepthSemantics;
};
struct SourceCommandResult {
    QString operationId, code, error, versionId;
    bool ok = false, pending = false;
    quint64 revision = 0;
    SourceState state;
    QVariantMap details;
};
QJsonObject sourceRefJson(const SourceRef &ref);
bool sourceRefFromJson(const QJsonObject &json, SourceRef &ref);
QJsonObject sourceBindingJson(const SourceBinding &binding);
bool sourceBindingFromJson(const QJsonObject &json, SourceBinding &binding, QString *error = nullptr);
}
Q_DECLARE_METATYPE(paleo::document::SourceRef)
Q_DECLARE_METATYPE(paleo::document::SourceState)
Q_DECLARE_METATYPE(paleo::document::SourceChangeSet)
Q_DECLARE_METATYPE(paleo::document::SourceCommandResult)
