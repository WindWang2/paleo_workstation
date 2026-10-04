// 层：视图
// ui/pages/dataops/dataopsmodel — P3 数据管理页操作重构的核心值类型与
// 视图层 sidecar 存储。纯逻辑（无 QWidget 依赖），被 datalist/entitypanel/
// datapage 三个 TU include。
//
// 设计约束（docs/dataops/MODEL_MAP.md §5）：
//   · catalog 无实体更新/删除、资产删除/改型 API（datacatalog.h 只有
//     add*/attach/setLink*）——类型改写、实体名/坐标改写、软删走视图层
//     sidecar（.paleo/*.json，与 T28 undo vault 同一模式），写回服务层的
//     正路登记在 docs/dataops/GAPS.md；
//   · sidecar 路径从 catalogPath() 推导（<projectDir>/.paleo/<name>.json），
//     与 undo_stack.json 同一函数复刻；catalog 未开 → 所有 store 空转。
#pragma once

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

#include "../../../catalog/datacatalog.h"

namespace paleo::dataops
{

// ---- sidecar 路径与读写 ----------------------------------------------------
// catalog 在 <projectDir>/artifacts/metadata/catalog.json → 工程目录 =
// 从该文件上三级（metadata → artifacts → projectDir）。方向 30 修正：
// 原实现只退两级（落在 <projectDir>/artifacts，sidecar 实际写到
// artifacts/.paleo/ 下），与注释/tst_ui_blocking 探针声明的
// <projectDir>/.paleo/ 不符——探针此前空转通过（writes=0 ≤ 2）。
inline QString projectDirFor(DataCatalog *cat)
{
  if (!cat || !cat->isOpen())
    return QString();
  const QString cp = cat->catalogPath();
  if (cp.isEmpty())
    return QString();
  // 纯词法三级上退（QFileInfo::dir() 摘末段）：不查盘上存在性。
  return QFileInfo(QFileInfo(QFileInfo(cp).dir().absolutePath()).dir().absolutePath())
      .dir()
      .absolutePath();
}

inline QString sidecarPath(DataCatalog *cat, const QString &name)
{
  const QString pd = projectDirFor(cat);
  if (pd.isEmpty())
    return QString();
  return QDir(pd).filePath(QStringLiteral(".paleo/") + name);
}

inline QJsonObject loadSidecarObject(DataCatalog *cat, const QString &name)
{
  const QString path = sidecarPath(cat, name);
  if (path.isEmpty())
    return QJsonObject();
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
    return QJsonObject();
  QJsonParseError err;
  const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
  return err.error == QJsonParseError::NoError ? doc.object() : QJsonObject();
}

inline bool saveSidecarObject(DataCatalog *cat, const QString &name,
                              const QJsonObject &root)
{
  const QString path = sidecarPath(cat, name);
  if (path.isEmpty())
    return false;
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly))
  {
    qWarning() << "dataops sidecar write failed:" << path;
    return false;
  }
  f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
  return true;
}

// ---- 资产行快照（过滤/排序/分组/导出的统一输入） ---------------------------
// 一次从 catalog + 各 sidecar store 装配；视图过滤不再回查 catalog。
struct AssetRowInfo
{
  QString assetId;
  QString displayName;
  QString fileName;      // 当前版本文件名（无版本回退 displayName）
  QString type;          // catalog 原型
  QString effectiveType; // 应用用户改写后的类型（过滤/显示口径）
  QString status;        // "RAW" | "DERIVED"（当前版本 stage）
  bool unresolved = false;     // 任一链接未决
  bool removed = false;        // 软删（可回收清单）
  QStringList roles;           // 去重排序后的链接角色
  QStringList entityNames;     // 已决实体名（无则空）
  QStringList tags;            // 用户标签
  qint64 sizeBytes = 0;        // 当前版本文件大小（受管/外链均 stat）
  QDateTime lastModified;      // 文件 mtime；stat 不到 = 无效
  int versionCount = 0;
  int currentVersionNo = 1;

  bool matchesStatus(const QString &s) const { return status == s; }
};

// ---- D2.4 用户标签（.paleo/asset_tags.json） -------------------------------
// { "tags": { "<assetId>": ["标签", ...] } }
class TagStore
{
public:
  void load(DataCatalog *cat)
  {
    m_cat = cat;
    m_tags.clear();
    const QJsonObject root = loadSidecarObject(cat, QStringLiteral("asset_tags.json"));
    const QJsonObject tags = root.value(QStringLiteral("tags")).toObject();
    for (auto it = tags.begin(); it != tags.end(); ++it)
    {
      QStringList list;
      for (const auto &v : it.value().toArray())
      {
        const QString t = v.toString().trimmed();
        if (!t.isEmpty())
          list << t;
      }
      if (!list.isEmpty())
        m_tags.insert(it.key(), list);
    }
  }

  bool save() const
  {
    QJsonObject tags;
    for (auto it = m_tags.constBegin(); it != m_tags.constEnd(); ++it)
    {
      QJsonArray arr;
      for (const QString &t : it.value())
        arr.append(t);
      tags.insert(it.key(), arr);
    }
    QJsonObject root;
    root.insert(QStringLiteral("tags"), tags);
    return saveSidecarObject(m_cat, QStringLiteral("asset_tags.json"), root);
  }

  QStringList tagsFor(const QString &assetId) const
  {
    return m_tags.value(assetId);
  }
  // 打/去标签（幂等：已存在则不变，返回是否有变更）。
  bool addTag(const QString &assetId, const QString &tag)
  {
    const QString t = tag.trimmed();
    if (t.isEmpty() || t.size() > 40)
      return false;
    // 控制字符拒（标签会进 sidecar 键值与 UI chip 文案）。
    for (const QChar &ch : t)
      if (ch.category() == QChar::Other_Control || ch.category() == QChar::Other_Format)
        return false;
    QStringList &list = m_tags[assetId];
    if (list.contains(t, Qt::CaseInsensitive))
      return false;
    list << t;
    return true;
  }
  bool removeTag(const QString &assetId, const QString &tag)
  {
    auto it = m_tags.find(assetId);
    if (it == m_tags.end())
      return false;
    for (int i = 0; i < it.value().size(); ++i)
      if (it.value().at(i).compare(tag, Qt::CaseInsensitive) == 0)
      {
        it.value().removeAt(i);
        if (it.value().isEmpty())
          m_tags.erase(it);
        return true;
      }
    return false;
  }
  // 全部标签 + 使用计数（标签云）。
  QVector<QPair<QString, int>> tagCloud() const
  {
    QHash<QString, int> counts;
    for (auto it = m_tags.constBegin(); it != m_tags.constEnd(); ++it)
      for (const QString &t : it.value())
        counts[t] += 1;
    QVector<QPair<QString, int>> out;
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it)
      out.append({it.key(), it.value()});
    std::sort(out.begin(), out.end(),
              [](const auto &a, const auto &b) {
                return a.second != b.second ? a.second > b.second : a.first < b.first;
              });
    return out;
  }
  // 按标签反查资产（过滤用）。
  QSet<QString> assetsWithTag(const QString &tag) const
  {
    QSet<QString> out;
    for (auto it = m_tags.constBegin(); it != m_tags.constEnd(); ++it)
      if (it.value().contains(tag, Qt::CaseInsensitive))
        out.insert(it.key());
    return out;
  }

private:
  DataCatalog *m_cat = nullptr;
  QHash<QString, QStringList> m_tags;
};

// ---- D1.5 资产类型改写（.paleo/asset_overrides.json） ----------------------
// { "types": { "<assetId>": "<type>" } }
class AssetOverrideStore
{
public:
  void load(DataCatalog *cat)
  {
    m_cat = cat;
    m_types.clear();
    const QJsonObject root =
        loadSidecarObject(cat, QStringLiteral("asset_overrides.json"));
    const QJsonObject types = root.value(QStringLiteral("types")).toObject();
    for (auto it = types.begin(); it != types.end(); ++it)
      m_types.insert(it.key(), it.value().toString());
  }
  bool save() const
  {
    QJsonObject types;
    for (auto it = m_types.constBegin(); it != m_types.constEnd(); ++it)
      types.insert(it.key(), it.value());
    QJsonObject root;
    root.insert(QStringLiteral("types"), types);
    return saveSidecarObject(m_cat, QStringLiteral("asset_overrides.json"), root);
  }
  QString overriddenType(const QString &assetId) const
  {
    return m_types.value(assetId);
  }
  bool setType(const QString &assetId, const QString &type)
  {
    if (type.isEmpty())
      return m_types.remove(assetId) > 0;
    if (m_types.value(assetId) == type)
      return false;
    m_types.insert(assetId, type);
    return true;
  }
  bool clearType(const QString &assetId) { return m_types.remove(assetId) > 0; }

private:
  DataCatalog *m_cat = nullptr;
  QHash<QString, QString> m_types;
};

// ---- D4.1/D4.2 实体改写（.paleo/entity_overrides.json） --------------------
// { "entities": { "<entityId>": { "name": …, "surfaceX": …, "surfaceY": …,
//                                "note": … } } }——只覆盖显式给出的键。
struct EntityOverride
{
  QString name;
  bool hasCoords = false;
  double surfaceX = 0.0, surfaceY = 0.0;
  QString note;

  bool isEmpty() const { return name.isEmpty() && !hasCoords && note.isEmpty(); }
  QVariantMap toVariant() const
  {
    QVariantMap m;
    if (!name.isEmpty())
      m.insert(QStringLiteral("name"), name);
    if (hasCoords)
    {
      m.insert(QStringLiteral("surfaceX"), surfaceX);
      m.insert(QStringLiteral("surfaceY"), surfaceY);
    }
    if (!note.isEmpty())
      m.insert(QStringLiteral("note"), note);
    return m;
  }
  static EntityOverride fromVariant(const QVariantMap &m)
  {
    EntityOverride o;
    o.name = m.value(QStringLiteral("name")).toString();
    o.hasCoords = m.contains(QStringLiteral("surfaceX"));
    o.surfaceX = m.value(QStringLiteral("surfaceX")).toDouble();
    o.surfaceY = m.value(QStringLiteral("surfaceY")).toDouble();
    o.note = m.value(QStringLiteral("note")).toString();
    return o;
  }
};

class EntityOverrideStore
{
public:
  void load(DataCatalog *cat)
  {
    m_cat = cat;
    m_map.clear();
    const QJsonObject root =
        loadSidecarObject(cat, QStringLiteral("entity_overrides.json"));
    const QJsonObject ents = root.value(QStringLiteral("entities")).toObject();
    for (auto it = ents.begin(); it != ents.end(); ++it)
    {
      const EntityOverride o =
          EntityOverride::fromVariant(it.value().toObject().toVariantMap());
      if (!o.isEmpty())
        m_map.insert(it.key(), o);
    }
  }
  bool save() const
  {
    QJsonObject ents;
    for (auto it = m_map.constBegin(); it != m_map.constEnd(); ++it)
    {
      const QVariantMap m = it.value().toVariant();
      QJsonObject o;
      for (auto vit = m.constBegin(); vit != m.constEnd(); ++vit)
        o.insert(vit.key(), QJsonValue::fromVariant(vit.value()));
      ents.insert(it.key(), o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("entities"), ents);
    return saveSidecarObject(m_cat, QStringLiteral("entity_overrides.json"), root);
  }
  EntityOverride overrideFor(const QString &entityId) const
  {
    return m_map.value(entityId);
  }
  void setOverride(const QString &entityId, const EntityOverride &o)
  {
    if (o.isEmpty())
      m_map.remove(entityId);
    else
      m_map.insert(entityId, o);
  }
  // 显示名口径：override 优先，回退 catalog 名。
  QString displayName(const CatalogEntity &e) const
  {
    const EntityOverride o = overrideFor(e.id);
    return o.name.isEmpty() ? e.name : o.name;
  }

private:
  DataCatalog *m_cat = nullptr;
  QHash<QString, EntityOverride> m_map;
};

// ---- D1.6 软删可回收清单（.paleo/recycle_bin.json） ------------------------
// { "removed": [ { "assetId": …, "type": …, "displayName": …,
//                  "removedAt": ISO, "reason": … } ] }
struct RecycleEntry
{
  QString assetId;
  QString type;
  QString displayName;
  QDateTime removedAt;
  QString reason;

  QVariantMap toVariant() const
  {
    QVariantMap m;
    m.insert(QStringLiteral("assetId"), assetId);
    m.insert(QStringLiteral("type"), type);
    m.insert(QStringLiteral("displayName"), displayName);
    m.insert(QStringLiteral("removedAt"), removedAt.toString(Qt::ISODate));
    m.insert(QStringLiteral("reason"), reason);
    return m;
  }
  static RecycleEntry fromVariant(const QVariantMap &m)
  {
    RecycleEntry e;
    e.assetId = m.value(QStringLiteral("assetId")).toString();
    e.type = m.value(QStringLiteral("type")).toString();
    e.displayName = m.value(QStringLiteral("displayName")).toString();
    e.removedAt = QDateTime::fromString(
        m.value(QStringLiteral("removedAt")).toString(), Qt::ISODate);
    e.reason = m.value(QStringLiteral("reason")).toString();
    return e;
  }
};

class RecycleBin
{
public:
  void load(DataCatalog *cat)
  {
    m_cat = cat;
    m_entries.clear();
    const QJsonObject root =
        loadSidecarObject(cat, QStringLiteral("recycle_bin.json"));
    for (const auto &v : root.value(QStringLiteral("removed")).toArray())
    {
      const RecycleEntry e =
          RecycleEntry::fromVariant(v.toObject().toVariantMap());
      if (!e.assetId.isEmpty())
        m_entries.append(e);
    }
  }
  bool save() const
  {
    QJsonArray arr;
    for (const RecycleEntry &e : m_entries)
    {
      QJsonObject o;
      const QVariantMap m = e.toVariant();
      for (auto it = m.constBegin(); it != m.constEnd(); ++it)
        o.insert(it.key(), QJsonValue::fromVariant(it.value()));
      arr.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("removed"), arr);
    return saveSidecarObject(m_cat, QStringLiteral("recycle_bin.json"), root);
  }
  QVector<RecycleEntry> entries() const { return m_entries; }
  bool isRemoved(const QString &assetId) const { return m_removed.contains(assetId); }
  void remove(const QString &assetId, const QString &type,
              const QString &displayName, const QString &reason)
  {
    if (m_removed.contains(assetId))
      return;
    RecycleEntry e;
    e.assetId = assetId;
    e.type = type;
    e.displayName = displayName;
    e.removedAt = QDateTime::currentDateTime();
    e.reason = reason;
    m_entries.append(e);
    m_removed.insert(assetId);
  }
  bool restore(const QString &assetId)
  {
    for (int i = 0; i < m_entries.size(); ++i)
      if (m_entries.at(i).assetId == assetId)
      {
        m_entries.removeAt(i);
        m_removed.remove(assetId);
        return true;
      }
    return false;
  }
  bool purge(const QString &assetId) { return restore(assetId); } // 清除条目（不可恢复路径同形）
  void clearAll()
  {
    m_entries.clear();
    m_removed.clear();
  }

private:
  DataCatalog *m_cat = nullptr;
  QVector<RecycleEntry> m_entries;
  QSet<QString> m_removed;
};

// ---- 行快照装配 ------------------------------------------------------------
// size/mtime 走 QFileInfo::stat——10k 资产 stat 是一次冷开销；调用方在刷新
// 管线里装配一次，过滤循环不再碰盘（D2.8 性能口径）。
inline AssetRowInfo buildAssetRow(DataCatalog *cat, const CatalogAsset &a,
                                  const TagStore &tags,
                                  const AssetOverrideStore &overrides,
                                  const RecycleBin &bin,
                                  const EntityOverrideStore &entityOverrides)
{
  AssetRowInfo row;
  row.assetId = a.id;
  row.displayName = a.displayName;
  row.type = a.type;
  const QString ov = overrides.overriddenType(a.id);
  row.effectiveType = ov.isEmpty() ? a.type : ov;
  row.removed = bin.isRemoved(a.id);
  row.tags = tags.tagsFor(a.id);

  const CatalogVersion v = cat->currentVersion(a.id);
  row.status = v.stage.isEmpty() ? QStringLiteral("RAW") : v.stage;
  row.fileName = v.fileName.isEmpty() ? a.displayName : v.fileName;
  row.currentVersionNo = v.versionNumber > 0 ? v.versionNumber : 1;
  row.versionCount = cat->versionsForAsset(a.id).size();
  if (!v.path.isEmpty())
  {
    // 外链 = 绝对路径直接 stat；受管 = DataCatalog::resolvedVersionPath
    // （含 symlink 祖先安全校验）拼工程目录。
    QString abs;
    if (v.managed)
    {
      const QString pd = projectDirFor(cat);
      if (!pd.isEmpty())
        abs = DataCatalog::resolvedVersionPath(pd, v);
    }
    else
      abs = v.path;
    if (!abs.isEmpty())
    {
      const QFileInfo fi(abs);
      if (fi.exists())
      {
        row.sizeBytes = fi.size();
        row.lastModified = fi.lastModified();
      }
    }
  }

  for (const EntityAssetLink &l : cat->linksForAsset(a.id))
  {
    if (l.unresolved)
      row.unresolved = true;
    if (!row.roles.contains(l.role))
      row.roles.append(l.role);
    if (!l.unresolved && !l.entityId.isEmpty())
    {
      const CatalogEntity e = cat->entityById(l.entityId);
      const QString nm = entityOverrides.displayName(e);
      const QString shown = nm.isEmpty() ? (e.name.isEmpty() ? l.entityId : e.name) : nm;
      if (!row.entityNames.contains(shown))
        row.entityNames.append(shown);
    }
  }
  row.roles.sort();
  row.entityNames.sort();
  return row;
}


// ---- D4.10 最近操作历史（会话内环形缓冲；不落盘）---------------------------
class OperationsHistory
{
public:
  static constexpr int kCapacity = 100;
  void push(const QString &entry)
  {
    m_entries.prepend(entry);
    if (m_entries.size() > kCapacity)
      m_entries.removeLast();
  }
  QStringList entries() const { return m_entries; }
  void clear() { m_entries.clear(); }

private:
  QStringList m_entries;
};

} // namespace paleo::dataops
