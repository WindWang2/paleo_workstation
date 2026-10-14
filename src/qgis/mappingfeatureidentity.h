// 层：QGIS 封装
#pragma once
#include <QString>
#include <QStringList>
#include <QObject>
#include <QPointer>
#include <QHash>
#include <QSet>
#include <QVariantList>
#include <QVector>
#include <qgsfeature.h>
class QgsVectorLayer;
class QUndoCommand;

namespace MappingFeatureIdentity {
inline const QString UidField = QStringLiteral("paleo_feature_uid");
inline const QString ParentsField = QStringLiteral("paleo_parent_uids");
inline const QString ReviewField = QStringLiteral("paleo_identity_review");
// Only call on a newly generated artifact or an editable working copy.
// Duplicate identities (native split/copy) become children requiring review.
bool ensure(QgsVectorLayer *layer, QString *error = nullptr);
QString uid(const QgsFeature &feature);
QString geometryFingerprint(const QgsFeature &feature);
// Invalid feature means absent or ambiguous identity; never fall back to FID.
QgsFeature find(QgsVectorLayer *layer, const QString &uid);
// Explicit merge lineage. Native geometry merging alone cannot infer evidence.
bool setParents(QgsVectorLayer *layer, QgsFeatureId fid,
                const QStringList &parentUids, QString *error = nullptr);

// Working-copy observer: all identity writes join the native edit command.
// Undo/redo replays that command without producing new audit or identities.
class Observer : public QObject {
  Q_OBJECT
public:
  static Observer *attach(QgsVectorLayer *layer, QString *error = nullptr);
  static Observer *of(QgsVectorLayer *layer);
  QVariantList changes() const;
  QVariantMap editBefore(QgsFeatureId fid) const;
  int identityIndexBuildCount() const { return m_indexBuilds; }
  void acknowledge(); // only after the new immutable version was registered
signals:
  void failed(const QString &reason);
  void businessEdit(qint64 featureId, const QString &kind);
private:
  friend bool setParents(QgsVectorLayer *, QgsFeatureId, const QStringList &, QString *);
  explicit Observer(QgsVectorLayer *layer);
  QgsFeature before(QgsFeatureId fid);
  void startCommand();
  void changed(QgsFeatureId fid);
  void added(QgsFeatureId fid);
  void deleted(QgsFeatureId fid);
  void inferMerges();
  bool rewrite(QgsFeatureId fid, const QStringList &parents);
  void rebuildIds();
  void finishCommand();
  void discardCommand();
  void registerAdded(QgsFeatureId fid);
  void reportFailure(const QString &reason);
  QPointer<QgsVectorLayer> m_layer;
  QHash<QgsFeatureId, QgsFeature> m_known, m_before, m_initial;
  QHash<QgsFeatureId, QgsFeature> m_recentBefore;
  QHash<QgsFeatureId, QgsFeature> m_sessionInitial, m_sessionRecentBefore;
  QHash<QString, QgsFeatureIds> m_ids;
  QHash<QString, QStringList> m_roots;
  QSet<QgsFeatureId> m_touched, m_deleted;
  QSet<QgsFeatureId> m_added;
  QSet<QgsFeatureId> m_cacheTouched;
  QHash<QgsFeatureId, QString> m_kinds;
  bool m_writing = false, m_finishing = false, m_macroOpen = false;
  int m_commandDepth = 0;
  int m_indexBuilds = 0;
  bool m_idsDirty = false;
  QUndoCommand *m_wrapper = nullptr;
  struct NestedCommand {
    QUndoCommand *command = nullptr;
    QHash<QgsFeatureId, QgsFeature> before, recentBefore;
    QSet<QgsFeatureId> touched, deleted, added, cacheTouched;
    QHash<QgsFeatureId, QString> kinds;
  };
  QVector<NestedCommand> m_nested;
};
}
