// 层：视图
#include "datalist_navspec_internal.h"
#include "../../catalog/datacatalog.h"
#include "../../catalog/catalogroles.h"
#include "../../services/previewdoc.h"

void DataListPanel::describeWellAssetNode(NavNodeSpec &node, const CatalogAsset &asset,
                                         const EntityAssetLink &link) const
{
  const QString role = catalogRoleDisplay(link.role);
  QString detail;
  if (link.role == QLatin1String("well_log")) {
    detail = asset.displayName;
    node.icon = QStringLiteral("mIconLineLayer.svg");
  } else if (link.role == QLatin1String("tops") || link.role == QLatin1String("time_depth")) {
    detail = asset.displayName;
    node.icon = QStringLiteral("mActionOpenTable.svg");
  } else if (link.role == QLatin1String("well_head")) {
    detail = asset.displayName;
    node.icon = QStringLiteral("mIconPointLayer.svg");
  } else {
    node.icon = QStringLiteral("mActionOpenTable.svg");
  }
  if (link.role == QLatin1String("well_log") && !link.unresolved) {
    const QString kind = link.isPrimary ? tr("主文件") : tr("成员");
    // 旧工程不补扫文件：缺少入库摘要时保留原显示，避免树刷新触发整文件解析。
    node.text0 = tr("%1 · %2 (%3)").arg(asset.displayName, kind, role);
    node.text1 = tr("%1 · %2").arg(kind, asset.displayName);
    if (m_doc && m_doc->catalog()) {
      const auto version = m_doc->catalog()->currentVersion(asset.id);
      const QStringList curves = version.extra.value(QStringLiteral("logCurves")).toStringList();
      if (!curves.isEmpty()) {
        node.text0 = tr("%1 · 测井数据 · %2条曲线").arg(asset.displayName).arg(curves.size());
        node.text1 = curves.join(QStringLiteral(", "));
        const QString source = version.fileName.isEmpty() ? asset.displayName : version.fileName;
        node.tooltip0 = tr("来源：%1\n版本：v%2\n%3")
            .arg(source).arg(version.versionNumber).arg(kind);
        const QString summary = version.extra.value(QStringLiteral("logSummary")).toString();
        if (!summary.isEmpty()) node.tooltip0 += QLatin1Char('\n') + summary;
      }
    }
  } else {
    node.text0 = QStringLiteral("%1 (%2)").arg(asset.displayName, role);
    node.text1 = detail;
  }
  if (link.unresolved) {
    node.warning = true;
    node.text1 = tr("未决关联");
  }
}
