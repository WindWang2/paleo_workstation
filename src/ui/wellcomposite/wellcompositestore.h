// 层：视图
#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <QList>

#include "trackregistry.h"

// ui/wellcomposite/wellcompositestore — 会话与 sidecar 持久化
//
// 两级存储：
// 1. 会话级（QSettings，键 wellcomposite/<project>/<well>/...）：道序/道宽/显隐/
//    打印开关（D1.8、D1.4 宽度集合记忆）——跟随面板会话自动恢复；
// 2. 项目级 sidecar（JSON 文件，落源数据同目录 <源名>.wc.json）：深度标注钉
//    （D2.3）、深度书签（D2.6）、地层单元指派（D3.6）、曲线量程/单位覆盖层
//    （D3.12）、导出预设（D4.10）、对比模板（D5.6）。sidecar 不污染源数据。
//
// 分层说明：视图层文件 IO 助手（QtCore；io/* include 被分层护栏白名单挡死，
// 本组件承载 wellcomposite 特有的持久化面；迁移服务层记 TODOS 接缝）。

namespace WellComposite
{

// 深度标注钉（D2.3）：双击深度标尺加文字钉注
struct DepthPin
{
  double depth = 0.0;
  QString text;
  bool operator==(const DepthPin &o) const { return depth == o.depth && text == o.text; }
};

// 深度书签（D2.6）：命名深度点
struct DepthBookmark
{
  QString name;
  double depth = 0.0;
  bool operator==(const DepthBookmark &o) const { return name == o.name && depth == o.depth; }
};

// 地层单元指派（D3.6）：用户显式指派，程序不猜
struct StratAssignment
{
  QString layerName;  // 源数据层名（如 "A"/"C1" 标志层名）
  QString system;     // 系
  QString series;     // 统
  QString formation;  // 组
  QString member;     // 段
  bool operator==(const StratAssignment &o) const
  {
    return layerName == o.layerName && system == o.system && series == o.series &&
           formation == o.formation && member == o.member;
  }
};

// 导出预设（D4.10）
struct ExportPreset
{
  QString name;
  QString format;      // pdf/png/svg
  QString scaleRatio;  // "1:200"...
  QString depthMode;   // "whole"/"range"/"viewport"
  double rangeTop = 0.0;
  double rangeBottom = 0.0;
  int dpi = 300;
  bool includeLegend = true;
  bool includeHeader = true;
  QVariantMap toVariantMap() const;
  static ExportPreset fromVariantMap(const QVariantMap &map);
};

// 对比模板（D5.6）：井集 + 道集配置
struct ComparisonTemplate
{
  QString name;
  QStringList wells;
  QList<TrackSpec> tracks;
  bool linkScroll = true;
  QVariantMap toVariantMap() const;
  static ComparisonTemplate fromVariantMap(const QVariantMap &map);
};

class WellCompositeStore
{
public:
  // ---- 会话级（QSettings）----
  // D1.8 按「井+项目」存道配置；返回 false = 无记忆（首开）
  static QString sessionKey(const QString &project, const QString &well);
  static void saveSessionTracks(const QString &project, const QString &well,
                                const QList<TrackSpec> &specs);
  static QList<TrackSpec> loadSessionTracks(const QString &project, const QString &well);
  static void clearSessionTracks(const QString &project, const QString &well);

  // D1.4 道宽集合记忆（独立于道序——顺序变了宽度仍按道标题跟随）
  static void saveWidthSet(const QString &project, const QString &well,
                           const QVariantMap &titleToWidth);
  static QVariantMap loadWidthSet(const QString &project, const QString &well);

  // ---- 项目级 sidecar ----
  explicit WellCompositeStore(const QString &sourceDataPath);

  QString sidecarPath() const { return m_sidecarPath; }
  bool load();  // 读 sidecar（不存在 = 空文档，返回 true）
  bool save() const;

  // D2.3 深度标注
  QList<DepthPin> pins() const;
  void setPins(const QList<DepthPin> &pins);
  // D2.6 书签
  QList<DepthBookmark> bookmarks() const;
  void setBookmarks(const QList<DepthBookmark> &bookmarks);
  // D3.6 地层单元指派
  QList<StratAssignment> stratAssignments() const;
  void setStratAssignments(const QList<StratAssignment> &assignments);
  // D3.12 曲线量程/单位覆盖层（curveName → {min,max,log,unit,color}）
  QVariantMap curveOverrides() const;
  void setCurveOverrides(const QVariantMap &overrides);
  // D4.10 导出预设
  QList<ExportPreset> exportPresets() const;
  void setExportPresets(const QList<ExportPreset> &presets);
  // D5.6 对比模板
  QList<ComparisonTemplate> comparisonTemplates() const;
  void setComparisonTemplates(const QList<ComparisonTemplate> &templates);

  // sidecar 内嵌 JSON 段直接访问（编辑审计等扩展位）
  QJsonObject document() const { return m_doc; }
  void setDocument(const QJsonObject &doc) { m_doc = doc; }

  // 编辑审计日志（D3.11 摘要落 sidecar；完整历史在派生 XML 审计表）
  void appendAuditEntry(const QString &isoTimestamp, const QString &operation, const QString &detail);
  QJsonArray auditLog() const;

private:
  static QList<TrackSpec> specsFromJson(const QJsonArray &arr);
  static QJsonArray specsToJson(const QList<TrackSpec> &specs);

  QString m_sidecarPath;
  QJsonObject m_doc;
};

} // namespace WellComposite
