// 层：视图
// 辅助资料导航分支（参考相图/参考资料）——从 datalist_tree.cpp 的
// buildNavTreeSpec 析出（file_budget/ui_size 只缩不涨纪律：#566 净增后
// 回收，构建逻辑零变更）。
#include "datalist_navspec_internal.h"
#include "../../catalog/datacatalog.h"

DataListPanel::NavNodeSpec DataListPanel::buildAuxiliaryNavSpec(const QList<CatalogAsset> &auxAssets) const
{
  NavNodeSpec auxRoot;
  auxRoot.text0 = tr("辅助资料 (%1)").arg(auxAssets.size());
  auxRoot.text1 = tr("参考相图 / 文档 / 图片");
  auxRoot.type = QStringLiteral("category");
  auxRoot.sub = QStringLiteral("auxiliary");
  auxRoot.icon = QStringLiteral("mActionFolder.svg");
  // 有文档/参考图时保持展开。导入的 pptx 落在「参考资料」下，
  // 顶级组收拢时文件名不会出现在视野里。
  auxRoot.expanded = !auxAssets.isEmpty();

  NavNodeSpec faciesBranch;
  faciesBranch.text0 = tr("参考相图");
  faciesBranch.type = QStringLiteral("category");
  faciesBranch.icon = QStringLiteral("mIconPolygonLayer.svg");
  faciesBranch.expanded = true;

  NavNodeSpec docBranch;
  docBranch.text0 = tr("参考资料");
  docBranch.type = QStringLiteral("category");
  docBranch.icon = QStringLiteral("mActionFolder.svg");
  docBranch.expanded = true;

  for (const CatalogAsset &a : auxAssets)
  {
    const bool isGeo = a.displayName.endsWith(QLatin1String(".geojson"), Qt::CaseInsensitive) || a.type == QLatin1String("boundary");
    NavNodeSpec it;
    it.text0 = a.displayName;
    it.id = a.id;
    it.type = QStringLiteral("auxiliary");
    it.icon = isGeo ? QStringLiteral("mIconPolygonLayer.svg") : QStringLiteral("mActionOpenTable.svg");
    if (isGeo)
      it.text1 = tr("GeoJSON 矢量相图");
    else if (a.displayName.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive))
      it.text1 = tr("PDF 文档");
    else if (a.displayName.endsWith(QLatin1String(".pptx"), Qt::CaseInsensitive))
      it.text1 = tr("PPT 演示文稿");
    else if (a.displayName.endsWith(QLatin1String(".png"), Qt::CaseInsensitive))
      it.text1 = tr("图像");
    else if (a.displayName.endsWith(QLatin1String(".xml"), Qt::CaseInsensitive))
      it.text1 = tr("XML 数据表");
    else
      it.text1 = a.type;
    if (isGeo)
      faciesBranch.children.append(it);
    else
      docBranch.children.append(it);
  }
  auxRoot.children.append(faciesBranch);
  auxRoot.children.append(docBranch);
  return auxRoot;
}
