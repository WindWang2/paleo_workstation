#include <QtTest>
#include <QImage>
#include <QMimeData>

#include <qgsapplication.h>
#include <qgslayertree.h>
#include <qgslayertreelayer.h>
#include <qgslayertreegroup.h>
#include <qgslayertreemodel.h>
#include <qgslayertreenode.h>
#include <qgsvectorlayer.h>
#include <qgsproject.h>

#include "../src/qgis/qgisprojectservice.h"

// 「图层拖到树最顶 → 显示变空白」回归：Qt InternalMove 实际走
// mimeData(序列化) → dropMimeData(反序列化插入克隆) → removeRows(删源)。
// dropMimeData 里 QgsLayerTreeNode::readXml 用 QgsProject::instance()
// 全局单例解析图层引用——Paleo 经 QgisProjectService::setInstance()
// 把自有工程注册为单例后，克隆节点引用不再悬空（否则画布桥跳过 → 空白）。

class TestDragTopRender : public QObject
{
    Q_OBJECT
  private slots:
    void internalMoveGroupKeepsChildren();
    void internalMoveLayerKeepsCheck();
};

void TestDragTopRender::internalMoveGroupKeepsChildren()
{
  QgisProjectService svc; // 同应用：setInstance 已注册自有工程
  QgsProject *proj = svc.project();
  auto *layerA = new QgsVectorLayer(
      QStringLiteral("Polygon?crs=EPSG:4326&field=id:int"), QStringLiteral("childA"),
      QStringLiteral("memory"));
  auto *layerB = new QgsVectorLayer(
      QStringLiteral("Polygon?crs=EPSG:4326&field=id:int"), QStringLiteral("childB"),
      QStringLiteral("memory"));
  auto *layerC = new QgsVectorLayer(
      QStringLiteral("Point?crs=EPSG:4326&field=id:int"), QStringLiteral("other"),
      QStringLiteral("memory"));
  proj->addMapLayers({layerA, layerB, layerC}, false);
  QgsLayerTreeGroup *grp = proj->layerTreeRoot()->addGroup(QStringLiteral("grp"));
  grp->insertLayer(-1, layerA);
  grp->insertLayer(-1, layerB);
  proj->layerTreeRoot()->insertLayer(-1, layerC); // [grp(A,B), C]

  QgsLayerTreeModel model(proj->layerTreeRoot());
  model.setFlag(QgsLayerTreeModel::AllowNodeReorder);
  model.setFlag(QgsLayerTreeModel::AllowNodeChangeVisibility);

  // InternalMove 路径：grp 行 0 → 拖到行 1
  const QModelIndex src = model.index(0, 0, QModelIndex());
  QVERIFY(src.isValid());
  std::unique_ptr<QMimeData> md(model.mimeData({src}));
  QVERIFY(md);
  QVERIFY(model.dropMimeData(md.get(), Qt::MoveAction, 1, 0, QModelIndex()));
  QVERIFY(model.removeRows(0, 1, QModelIndex())); // Qt InternalMove 随后删源行 0

  QCOMPARE(proj->layerTreeRoot()->children().size(), 2);
  auto *moved = qobject_cast<QgsLayerTreeGroup *>(
      proj->layerTreeRoot()->children().at(0));
  QVERIFY(moved);
  QCOMPARE(moved->name(), QStringLiteral("grp"));
  QCOMPARE(moved->children().size(), 2); // 子项丢失 = 用户报的"变空白"
  auto *childNode = qobject_cast<QgsLayerTreeLayer *>(
      moved->children().at(0));
  QVERIFY(childNode);
  QVERIFY2(childNode->layer() == layerA,
           "子图层引用悬空（解析到了错误工程）→ 组内图层画布空白");
}

void TestDragTopRender::internalMoveLayerKeepsCheck()
{
  QgisProjectService svc;
  QgsProject *proj = svc.project();
  auto *layerA = new QgsVectorLayer(
      QStringLiteral("Point?crs=EPSG:4326&field=id:int"), QStringLiteral("A"),
      QStringLiteral("memory"));
  auto *layerB = new QgsVectorLayer(
      QStringLiteral("Point?crs=EPSG:4326&field=id:int"), QStringLiteral("B"),
      QStringLiteral("memory"));
  proj->addMapLayers({layerA, layerB}, false);
  proj->layerTreeRoot()->insertLayer(-1, layerA);
  proj->layerTreeRoot()->insertLayer(-1, layerB); // [A, B] 均勾选

  QgsLayerTreeModel model(proj->layerTreeRoot());
  model.setFlag(QgsLayerTreeModel::AllowNodeReorder);
  model.setFlag(QgsLayerTreeModel::AllowNodeChangeVisibility);

  const QModelIndex src = model.index(1, 0, QModelIndex());
  std::unique_ptr<QMimeData> md(model.mimeData({src}));
  QVERIFY(model.dropMimeData(md.get(), Qt::MoveAction, 0, 0, QModelIndex()));
  QVERIFY(model.removeRows(2, 1, QModelIndex())); // 源 B 现在在行 2

  QCOMPARE(proj->layerTreeRoot()->children().size(), 2);
  auto *node = qobject_cast<QgsLayerTreeLayer *>(
      proj->layerTreeRoot()->children().at(0));
  QVERIFY(node);
  QCOMPARE(node->name(), QStringLiteral("B"));
  QVERIFY2(node->itemVisibilityChecked(), "勾选态丢失");
  QVERIFY2(node->layer() == layerB, "图层引用丢失（解析到了错误工程）");
}

QTEST_MAIN(TestDragTopRender)
#include "tst_dragtop_render.moc"
