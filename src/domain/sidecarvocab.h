// 层：数据
// B-03/A-14（full-audit-20261020）：shapefile/栅格附属文件单一词表。
// 此前 storagegovernance（治理扫描）与 paleomapfiles（导入拷贝）两份词表
// 各自演化分叉：governance 缺 qml/qmd/.aux.xml（导入后 QGIS/GDAL 生成的
// 样式与统计文件被误报孤儿，用户确认回收即删）；paleomapfiles 缺 7 项
// dbf 族索引后缀（导入不全）。统一到本头，两侧并集消费。
#pragma once
#include <QStringList>
namespace paleo::sidecars {
// shapefile 族：OGR 打开图层缺一不可的必备件（shx/dbf/prj/cpg）+ 常见
// 空间索引（qix/sbn/sbx）+ dbf 属性索引（fbn/fbx/ain/aih/atx/ixs/mxs）
// + QGIS 样式/元数据（qml/qmd）。同目录同主名、大小写不敏感。
inline const QStringList &shpFamily()
{
  static const QStringList list{
      QStringLiteral("shx"), QStringLiteral("dbf"), QStringLiteral("prj"),
      QStringLiteral("cpg"), QStringLiteral("qix"), QStringLiteral("sbn"),
      QStringLiteral("sbx"), QStringLiteral("fbn"), QStringLiteral("fbx"),
      QStringLiteral("ain"), QStringLiteral("aih"), QStringLiteral("atx"),
      QStringLiteral("ixs"), QStringLiteral("mxs"), QStringLiteral("qml"),
      QStringLiteral("qmd")};
  return list;
}
// 栅格族：world 文件（tfw/tifw/jgw/jpgw/pgw/pngw/wld）+ 投影/样式。
inline const QStringList &rasterFamily()
{
  static const QStringList list{
      QStringLiteral("tfw"), QStringLiteral("tifw"), QStringLiteral("jgw"),
      QStringLiteral("jpgw"), QStringLiteral("pgw"), QStringLiteral("pngw"),
      QStringLiteral("wld"), QStringLiteral("prj"), QStringLiteral("qml"),
      QStringLiteral("qmd")};
  return list;
}
// 「主件全名 + 后缀串」特判（大小写不敏感）：ESRI 元数据（x.shp →
// x.shp.xml）与 GDAL 统计/直方图（x.tif → x.tif.aux.xml，QGIS 渲染即写）。
inline const QStringList &nameSuffixes()
{
  static const QStringList list{QStringLiteral("xml"), QStringLiteral("aux.xml")};
  return list;
}
} // namespace paleo::sidecars
