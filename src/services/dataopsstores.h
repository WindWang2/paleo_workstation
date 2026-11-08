// 层：数据
// services/dataopsstores — P1（data-commands 2026-11-08）：dataops 四个视图层
// sidecar 存储对象（TagStore/AssetOverrideStore/EntityOverrideStore/RecycleBin）
// 的可执行核心下沉数据层。文件契约零变化（.paleo/<name>.json 的键值结构与
// #482/#534 的门面读写原样保留），视图侧 dataopsmodel.h 经 include + 同命名
//空间获得同名类型（兼容声明，消费面不改写）。
//
// 纯 QtCore（无 QtWidgets）：持久化一律走 services/jsonsidecar.h 数据层门面。
// header-only（与 jsonsidecar.h 同例）：paleo_services target 只登记 .cpp。
#pragma once

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

#include "../catalog/datacatalog.h"
#include "jsonsidecar.h"

namespace paleo::dataops
{

// ---- D2.4 用户标签（.paleo/asset_tags.json） -------------------------------
// { "tags": { "<assetId>": ["标签", ...] } }
class TagStore
{
public:
  void load(DataCatalog *cat)
  {
    m_cat = cat;
    m_tags.clear();
    const QJsonObject root =
        paleo::dataopsSidecarLoad(cat, QStringLiteral("asset_tags.json"));
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
    return paleo::dataopsSidecarSave(m_cat, QStringLiteral("asset_tags.json"), root);
  }

  QStringList tagsFor(const QString &assetId) const
  {
    return m_tags.value(assetId);
  }
  QHash<QString, QStringList> snapshot() const { return m_tags; }
  // 整表前态恢复（P1-W4：持久化失败后内存回到可信状态）。空表 = 键移除，
  // 与 load() 重建的形态一致（空列表不落键）。
  void restoreSnapshot(const QHash<QString, QStringList> &snap)
  {
    m_tags = snap;
  }
  // 按资产精确恢复前态（撤销面：不误删该资产上不属于本命令的其它标签；
  // 空列表 = 移除键，与 load() 形态一致）。
  void setTagsExact(const QString &assetId, const QStringList &tags)
  {
    if (tags.isEmpty())
      m_tags.remove(assetId);
    else
      m_tags.insert(assetId, tags);
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
        paleo::dataopsSidecarLoad(cat, QStringLiteral("asset_overrides.json"));
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
    return paleo::dataopsSidecarSave(m_cat, QStringLiteral("asset_overrides.json"), root);
  }
  QString overriddenType(const QString &assetId) const
  {
    return m_types.value(assetId);
  }
  QHash<QString, QString> snapshot() const { return m_types; }
  void restoreSnapshot(const QHash<QString, QString> &snap) { m_types = snap; }
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
  // 值等价（EntityEditCmd 的幂等判定：改写前后同值 = NoChange）。
  bool operator==(const EntityOverride &o) const
  {
    return name == o.name && hasCoords == o.hasCoords &&
           (hasCoords ? (surfaceX == o.surfaceX && surfaceY == o.surfaceY) : true) &&
           note == o.note;
  }
  bool operator!=(const EntityOverride &o) const { return !(*this == o); }
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
        paleo::dataopsSidecarLoad(cat, QStringLiteral("entity_overrides.json"));
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
    return paleo::dataopsSidecarSave(m_cat, QStringLiteral("entity_overrides.json"), root);
  }
  EntityOverride overrideFor(const QString &entityId) const
  {
    return m_map.value(entityId);
  }
  QHash<QString, EntityOverride> snapshot() const { return m_map; }
  void restoreSnapshot(const QHash<QString, EntityOverride> &snap) { m_map = snap; }
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
    // ast-N 跨工程复用。只清 entries 会让上一工程的软删 id 继续挡住新工程。
    m_removed.clear();
    const QJsonObject root =
        paleo::dataopsSidecarLoad(cat, QStringLiteral("recycle_bin.json"));
    for (const auto &v : root.value(QStringLiteral("removed")).toArray())
    {
      const RecycleEntry e =
          RecycleEntry::fromVariant(v.toObject().toVariantMap());
      if (e.assetId.isEmpty())
        continue;
      m_entries.append(e);
      m_removed.insert(e.assetId);
    }
    // finding-125：hidden 集合是已删资产的权威来源（含已被「清空记录」清掉
    // entries 的资产）——与 entries 的 id 取并集，兼容无 hidden 字段的旧档。
    for (const auto &v : root.value(QStringLiteral("hidden")).toArray())
    {
      const QString id = v.toString();
      if (!id.isEmpty())
        m_removed.insert(id);
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
    // finding-125：「hidden」独立持久化已删资产 id 集合——清空记录后 entries 为
    // 空但软删资产仍须保持隐藏；只存 entries 会让 load 重建的 m_removed 落空，
    // 资产在重启后复活回主列表（与「不可恢复」确认文案相反）。
    QJsonArray hidden;
    for (const QString &id : m_removed)
      hidden.append(id);
    QJsonObject root;
    root.insert(QStringLiteral("removed"), arr);
    root.insert(QStringLiteral("hidden"), hidden);
    return paleo::dataopsSidecarSave(m_cat, QStringLiteral("recycle_bin.json"), root);
  }
  QVector<RecycleEntry> entries() const { return m_entries; }
  bool isRemoved(const QString &assetId) const { return m_removed.contains(assetId); }
  QSet<QString> removedIds() const { return m_removed; }
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
    // finding-125：只清可恢复的记录（entries），保留 m_removed——「清空记录」
    // 后软删资产保持隐藏（不再可恢复，但也不复活回主列表）。清掉的 id 由
    // save() 写入 hidden 字段持久化，重启后依旧隐藏。
    m_entries.clear();
  }
  // P1-W4：整表前态快照/恢复（entries + hidden 权威集合一起，保持不变量）。
  struct StateSnapshot
  {
    QVector<RecycleEntry> entries;
    QSet<QString> removed;
  };
  StateSnapshot captureState() const { return {m_entries, m_removed}; }
  void restoreState(const StateSnapshot &s)
  {
    m_entries = s.entries;
    m_removed = s.removed;
  }

private:
  DataCatalog *m_cat = nullptr;
  QVector<RecycleEntry> m_entries;
  QSet<QString> m_removed;
};

} // namespace paleo::dataops
