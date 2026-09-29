// 层：视图
#pragma once

#include <QVariantMap>

#include <functional>
#include <memory>

#include "wellcompositetrack.h"

// ui/wellcomposite/trackregistry — D1.1/D1.2 道类型注册表与道规格（TrackSpec）
//
// 模型/视图分离的落点：
//   TrackSpec   = 可序列化的道数据模型（类型 id + 标题 + 宽度 + 可见性 + 类型参数），
//                 持久化（QSettings/sidecar）与道增删改均以 spec 为凭；
//   WellTrack   = 渲染器（paintHeader/paintBody），由注册表工厂从 spec 构建。
// 面板不再直接 new 具体道类——道增删改只动 spec 列表，再增量同步到画布，
// 面板与画布不重建（D1.1）。
//
// 新增道类型零侵入（D1.2）：TrackRegistry::registerType 登记一条 Entry
// （typeId/显示名/缺省宽/工厂/捕获器）即可被「新建道」菜单、配置对话框、
// 持久化恢复、CSV 导出等全部下游能力发现；无需改任何 switch。

namespace WellComposite
{

// 道规格（数据模型侧；QVariantMap 可往返序列化）
struct TrackSpec
{
  QString typeId;   // 注册表类型 id：depth/text/formation/litho/core/image/curve/symbol/strat/facies/gr/discrete
  QString title;
  qreal width = 100.0;
  bool visible = true;
  bool printIncluded = true; // D4.8 打印/导出开关
  QVariantMap params;        // 类型特定参数（见各注册项注释）

  bool isNull() const { return typeId.isEmpty(); }

  QVariantMap toVariantMap() const;
  static TrackSpec fromVariantMap(const QVariantMap &map);

  // 曲线道便捷参数（typeId == curve/gr/discrete）
  QStringList curveNames() const { return params.value(QStringLiteral("curves")).toStringList(); }
  void setCurveNames(const QStringList &names) { params.insert(QStringLiteral("curves"), names); }
  // 每曲线覆盖：{name: {min,max,log,unit,color}}（D1.7/D3.12 覆盖层）
  QVariantMap curveOverrides() const { return params.value(QStringLiteral("curveOverrides")).toMap(); }
  void setCurveOverrides(const QVariantMap &ov) { params.insert(QStringLiteral("curveOverrides"), ov); }
  bool showGrid() const { return params.value(QStringLiteral("showGrid"), true).toBool(); }
  int gridDensity() const { return params.value(QStringLiteral("gridDensity"), 2).toInt(); }
};

// 道类型注册项
struct TrackRegistryEntry
{
  QString typeId;
  QString displayName;          // 「新建道」菜单/对话框显示名（中文源串）
  qreal defaultWidth = 100.0;
  bool userCreatable = true;    // depth 标尺道等不可由用户新建
  std::function<std::shared_ptr<WellTrack>(const TrackSpec &)> create;
  std::function<TrackSpec(const std::shared_ptr<WellTrack> &)> capture;
};

class TrackRegistry
{
public:
  static TrackRegistry &instance();

  // 注册（重复 typeId 覆盖旧项——测试可替换工厂；内置注册见 ensureBuiltins）
  void registerType(const TrackRegistryEntry &entry);
  bool contains(const QString &typeId) const;
  const TrackRegistryEntry *entry(const QString &typeId) const;

  QStringList typeIds() const;
  QStringList userCreatableTypeIds() const;
  QString displayName(const QString &typeId) const;

  // spec → 渲染器；未知类型返回空（调用方判定）
  std::shared_ptr<WellTrack> createTrack(const TrackSpec &spec) const;
  // 渲染器 → spec（捕获当前标题/宽度/可见性/类型参数）
  TrackSpec captureSpec(const std::shared_ptr<WellTrack> &track) const;

  // 旧枚举映射（兼容既有 TrackType 消费方）
  static QString typeIdForEnum(TrackType type);
  static TrackType enumForTypeId(const QString &typeId);

private:
  TrackRegistry();
  void ensureBuiltins();

  QMap<QString, TrackRegistryEntry> m_entries;
};

// 曲线数据 ↔ 覆盖参数应用（curve/gr/discrete 道工厂与编辑器共用）：
// 按 name 在 available 里查找曲线，应用 overrides（量程/对数/单位/色），返回装配后的曲线。
CurveData applyCurveOverride(const CurveData &src, const QVariantMap &overrideMap);

} // namespace WellComposite
