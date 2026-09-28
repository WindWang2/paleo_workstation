// 层：数据
#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

// §37 — declared-layer manifest is the layer-SET authority (not .qgz).
// Persisted in metadata/project.sqlite table `layer_declarations`.
// A declaration exists independent of whether its QgsMapLayer is instantiated.
struct LayerDeclaration {
  QString layerId;      // stable id, e.g. "facies.T1"
  QString horizon;      // horizon this layer instance belongs to ("" = horizon-agnostic)
  QString type;         // "vector" | "raster" | "annotations" ...
  QString source;       // provider URI (gpkg table / raster path / memory)
  QString styleRef;     // style identifier in styles/
  QString group;        // layer-tree group；canonical 词表单一权威见 src/qgis/layervocabulary.h（旧名→canonical 别名同处）
  QString title;        // 显示名（图层树/图例）；空 → 用 layerId
  bool instantiated = false; // runtime-only, not persisted
};

// Manifest persistence over the project.sqlite metadata store.
class LayerManifest
{
  public:
    explicit LayerManifest(const QString &metaSqlitePath);

    bool open(QString *error = nullptr);       // creates schema if absent

    // 单写实例降级（T4/SCHEMA_MIGRATION.md §6）：工程目录被另一实例持锁时
    // 由组装根置 true——upsert/remove 如实失败（锁错误文案），读面照常。
    // 值语义：拷贝/赋值带回默认可写态，组装根在重绑后重设。
    void setReadOnly(bool readOnly) { m_readOnly = readOnly; }
    bool isReadOnly() const { return m_readOnly; }

    bool upsert(const LayerDeclaration &decl, QString *error = nullptr);
    bool remove(const QString &layerId, QString *error = nullptr);
    QVector<LayerDeclaration> all() const;                       // full declared set; empty on failure
    // False on open/query failure. Empty success is a zero-length vector.
    bool readAll(QVector<LayerDeclaration> *out, QString *error = nullptr) const;
    QVector<LayerDeclaration> forHorizon(const QString &h) const;
    QStringList horizons() const;

  private:
    QString m_dbPath;
    bool m_readOnly = false;
};
