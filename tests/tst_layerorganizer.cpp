#include <QtTest>
#include <QMimeData>
#include <QTemporaryDir>

#include <qgsapplication.h>
#include <qgslayertree.h>
#include <qgslayertreegroup.h>
#include <qgslayertreelayer.h>
#include <qgslayertreemodel.h>
#include <qgslayertreenode.h>
#include <qgsmaplayer.h>
#include <qgsproject.h>
#include <qgsvectorlayer.h>

#include "../src/metadata/layermanifest.h"
#include "../src/qgis/qgislayerorganizer.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprojectservice.h"

// 图层树布局器（QgisLayerOrganizer）契约测试：
//   · 00_Data 共享层（测区范围/井位）置顶树根；
//   · 层位声明先建带 paleoHorizon 凭据的占位组，图层实例化收编进组；
//   · 层位组带内按 mappingHorizons 序，组内按 canonical 组秩（07→00）；
//   · 层位组之下才是共享平铺产层（05_PaleoMap 等）；无声明手动层不受管理；
//   · 归位 = clone+insert+remove——图层引用/勾选态/自定义属性全保留；
//   · 用户拖拽（InternalMove）不被接管——手动摆位保持用户选择。
//
// 状态只落在 QgsLayerTree + QgsProject——服务自身不持树位台账。

static QString fixtureGpkg()
{
#ifdef FIXTURE_GPKG
  return QStringLiteral(FIXTURE_GPKG);
#else
  const QString testsDir = QFileInfo(QString::fromUtf8(__FILE__)).absolutePath();
  return QDir(testsDir).absoluteFilePath(QStringLiteral("../testdata/fixture.gpkg"));
#endif
}

static LayerDeclaration decl(const QString &layerId, const QString &horizon,
                             const QString &group)
{
  LayerDeclaration d;
  d.layerId = layerId;
  d.horizon = horizon;
  d.type = QStringLiteral("vector");
  d.source = fixtureGpkg() + QStringLiteral("|layername=basin");
  d.group = group;
  d.title = layerId;
  return d;
}

struct OrganizerFixture
{
  QTemporaryDir tmp;
  LayerManifest manifest;
  QgisProjectService projectSvc; // setInstance 注册自有工程——clone 解析引用靠它
  QgisLayerService layerSvc;
  QgisLayerOrganizer organizer;

  OrganizerFixture()
    : manifest(tmp.filePath(QStringLiteral("project.sqlite")))
    , layerSvc(&projectSvc, &manifest)
    , organizer(&projectSvc, &layerSvc)
  {
    QString err;
    if ( !tmp.isValid() || !manifest.open( &err ) )
      qWarning( "fixture setup failed: %s", qPrintable( err ) );
  }

  QgsProject *project() { return projectSvc.project(); }
  QgsLayerTree *root() { return projectSvc.project()->layerTreeRoot(); }

  QgsMapLayer *instantiate(const QString &layerId)
  {
    QString err;
    QgsMapLayer *layer = layerSvc.instantiate(layerId, &err);
    if ( !layer )
      qWarning( "fixture instantiate(%s): %s", qPrintable( layerId ),
                qPrintable( err ) );
    return layer;
  }
};

class TestLayerOrganizer : public QObject
{
  Q_OBJECT
  private slots:
    void sharedDataPinsTop();
    void horizonGroupsOrderedByMappingSequence();
    void insideGroupCanonicalRank();
    void declaredHorizonKeepsPlaceholder();
    void staleGroupPruned();
    void reparentPreservesCheckAndReference();
    void userDragNotReorganized();
    void unmanagedLayerUntouched();
};

void TestLayerOrganizer::sharedDataPinsTop()
{
  OrganizerFixture fx;
  QString err;
  // 乱序声明/实例化——布局须收敛到同一终态。
  QVERIFY2(fx.layerSvc.declare(decl(QStringLiteral("sf.flat"), QString(),
                                    QStringLiteral("04_SingleFactor")), &err),
           qPrintable(err));
  QVERIFY2(fx.layerSvc.declare(decl(QStringLiteral("hz.pred"), QStringLiteral("D61"),
                                    QStringLiteral("02_Prediction")), &err),
           qPrintable(err));
  QVERIFY2(fx.layerSvc.declare(decl(QStringLiteral("wells"), QString(),
                                    QStringLiteral("00_Data")), &err),
           qPrintable(err));
  QVERIFY2(fx.layerSvc.declare(decl(QStringLiteral("boundary.map"), QString(),
                                    QStringLiteral("00_Data")), &err),
           qPrintable(err));
  fx.instantiate(QStringLiteral("sf.flat"));
  fx.instantiate(QStringLiteral("hz.pred"));
  fx.instantiate(QStringLiteral("wells"));
  fx.instantiate(QStringLiteral("boundary.map"));

  const QList<QgsLayerTreeNode *> kids = fx.root()->children();
  QCOMPARE(kids.size(), 4);
  // 置顶区：两个 00_Data 层在最上（先后顺序按上图序，稳定即可）。
  auto *topA = qobject_cast<QgsLayerTreeLayer *>(kids.at(0));
  auto *topB = qobject_cast<QgsLayerTreeLayer *>(kids.at(1));
  QVERIFY(topA && topB);
  const QStringList pinned = { topA->layerId(), topB->layerId() };
  const QString wellsId = fx.layerSvc.layer(QStringLiteral("wells"))->id();
  const QString boundaryId = fx.layerSvc.layer(QStringLiteral("boundary.map"))->id();
  QVERIFY(pinned.contains(wellsId));
  QVERIFY(pinned.contains(boundaryId));
  // 层位组在共享平铺产层之上。
  auto *grp = qobject_cast<QgsLayerTreeGroup *>(kids.at(2));
  QVERIFY(grp);
  QCOMPARE(grp->customProperty(QStringLiteral("paleoHorizon")).toString(),
           QStringLiteral("D61"));
  QVERIFY(grp->itemVisibilityChecked()); // 占位/实组默认勾选——不遮挡子层
  auto *hzNode = qobject_cast<QgsLayerTreeLayer *>(grp->children().constFirst());
  QVERIFY(hzNode);
  QCOMPARE(hzNode->layer(),
           fx.layerSvc.layer(QStringLiteral("hz.pred"))); // 同一图层引用
  auto *flat = qobject_cast<QgsLayerTreeLayer *>(kids.at(3));
  QVERIFY(flat);
  QCOMPARE(flat->layer(), fx.layerSvc.layer(QStringLiteral("sf.flat")));
}

void TestLayerOrganizer::horizonGroupsOrderedByMappingSequence()
{
  OrganizerFixture fx;
  QString err;
  // 逆序声明：D71 在前——组带内仍按 mappingHorizons 序（D61 浅于 D71）。
  QVERIFY2(fx.layerSvc.declare(decl(QStringLiteral("h.deep"), QStringLiteral("D71"),
                                    QStringLiteral("05_PaleoMap")), &err),
           qPrintable(err));
  QVERIFY2(fx.layerSvc.declare(decl(QStringLiteral("h.shallow"), QStringLiteral("D61"),
                                    QStringLiteral("05_PaleoMap")), &err),
           qPrintable(err));
  fx.instantiate(QStringLiteral("h.deep"));
  fx.instantiate(QStringLiteral("h.shallow"));

  const QList<QgsLayerTreeNode *> kids = fx.root()->children();
  QCOMPARE(kids.size(), 2);
  QCOMPARE(kids.at(0)->customProperty(QStringLiteral("paleoHorizon")).toString(),
           QStringLiteral("D61"));
  QCOMPARE(kids.at(1)->customProperty(QStringLiteral("paleoHorizon")).toString(),
           QStringLiteral("D71"));
}

void TestLayerOrganizer::insideGroupCanonicalRank()
{
  OrganizerFixture fx;
  QString err;
  // 同层位三声明：05_PaleoMap 在上（rank 小），02_Prediction 居中，
  // 00_Data 垫底（层位底图栅格承接位）。
  QVERIFY2(fx.layerSvc.declare(decl(QStringLiteral("d61.data"), QStringLiteral("D61"),
                                    QStringLiteral("00_Data")), &err),
           qPrintable(err));
  QVERIFY2(fx.layerSvc.declare(decl(QStringLiteral("d61.pred"), QStringLiteral("D61"),
                                    QStringLiteral("02_Prediction")), &err),
           qPrintable(err));
  QVERIFY2(fx.layerSvc.declare(decl(QStringLiteral("d61.map"), QStringLiteral("D61"),
                                    QStringLiteral("05_PaleoMap")), &err),
           qPrintable(err));
  fx.instantiate(QStringLiteral("d61.data"));
  fx.instantiate(QStringLiteral("d61.pred"));
  fx.instantiate(QStringLiteral("d61.map"));

  QgsLayerTreeGroup *grp = nullptr;
  for (QgsLayerTreeNode *n : fx.root()->children())
    if (n->customProperty(QStringLiteral("paleoHorizon")).toString()
        == QLatin1String("D61"))
      grp = qobject_cast<QgsLayerTreeGroup *>(n);
  QVERIFY(grp);
  QCOMPARE(grp->children().size(), 3);
  const QStringList order = {
      qobject_cast<QgsLayerTreeLayer *>(grp->children().at(0))->layer()
          ->customProperty(QStringLiteral("paleoLayerId")).toString(),
      qobject_cast<QgsLayerTreeLayer *>(grp->children().at(1))->layer()
          ->customProperty(QStringLiteral("paleoLayerId")).toString(),
      qobject_cast<QgsLayerTreeLayer *>(grp->children().at(2))->layer()
          ->customProperty(QStringLiteral("paleoLayerId")).toString(),
  };
  QCOMPARE(order, (QStringList{QStringLiteral("d61.map"),
                               QStringLiteral("d61.pred"),
                               QStringLiteral("d61.data")}));
}

void TestLayerOrganizer::declaredHorizonKeepsPlaceholder()
{
  OrganizerFixture fx;
  QString err;
  // 层位声明了但尚未实例化（§37 懒加载）——组节点先在树里留名。
  QVERIFY2(fx.layerSvc.declare(decl(QStringLiteral("h.late"), QStringLiteral("D62"),
                                    QStringLiteral("02_Prediction")), &err),
           qPrintable(err));
  QgsLayerTreeGroup *grp = nullptr;
  for (QgsLayerTreeNode *n : fx.root()->children())
    if (n->customProperty(QStringLiteral("paleoHorizon")).toString()
        == QLatin1String("D62"))
      grp = qobject_cast<QgsLayerTreeGroup *>(n);
  QVERIFY2(grp, "层位声明应先建占位组");
  QCOMPARE(grp->children().size(), 0);

  // 后实例化 → 收编进既有占位组。
  QgsMapLayer *layer = fx.instantiate(QStringLiteral("h.late"));
  QVERIFY(layer);
  QCOMPARE(grp->children().size(), 1);
  auto *node = qobject_cast<QgsLayerTreeLayer *>(grp->children().constFirst());
  QVERIFY(node);
  QCOMPARE(node->layer(), layer);
}

void TestLayerOrganizer::staleGroupPruned()
{
  OrganizerFixture fx;
  QString err;
  QVERIFY2(fx.layerSvc.declare(decl(QStringLiteral("h.tmp"), QStringLiteral("D63"),
                                    QStringLiteral("02_Prediction")), &err),
           qPrintable(err));
  fx.organizer.reorganize();
  int groups = 0;
  for (QgsLayerTreeNode *n : fx.root()->children())
    if (n->customProperty(QStringLiteral("paleoHorizon")).toString()
        == QLatin1String("D63"))
      ++groups;
  QCOMPARE(groups, 1);

  // 声明撤销且无子节点 → 占位组剪掉（有声明的空组才留名）。
  QVERIFY2(fx.manifest.remove(QStringLiteral("h.tmp"), &err), qPrintable(err));
  fx.organizer.reorganize();
  for (QgsLayerTreeNode *n : fx.root()->children())
    QVERIFY(n->customProperty(QStringLiteral("paleoHorizon")).toString()
            != QLatin1String("D63"));
}

void TestLayerOrganizer::reparentPreservesCheckAndReference()
{
  OrganizerFixture fx;
  QString err;
  QVERIFY2(fx.layerSvc.declare(decl(QStringLiteral("wells"), QString(),
                                    QStringLiteral("00_Data")), &err),
           qPrintable(err));
  QVERIFY2(fx.layerSvc.declare(decl(QStringLiteral("hz.a"), QStringLiteral("D61"),
                                    QStringLiteral("05_PaleoMap")), &err),
           qPrintable(err));
  QgsMapLayer *wells = fx.instantiate(QStringLiteral("wells"));
  QgsMapLayer *hz = fx.instantiate(QStringLiteral("hz.a"));
  QVERIFY(wells && hz);

  // 把共享层节点手动塞进层位组（模拟旧工程散乱树位）+ 取消勾选 +
  // 自定义属性 → 规整须归位到树顶且状态全保留。
  QgsLayerTreeLayer *wellsNode = fx.root()->findLayer(wells->id());
  QVERIFY(wellsNode);
  QgsLayerTreeGroup *grp = nullptr;
  for (QgsLayerTreeNode *n : fx.root()->children())
    if (n->customProperty(QStringLiteral("paleoHorizon")).toString()
        == QLatin1String("D61"))
      grp = qobject_cast<QgsLayerTreeGroup *>(n);
  QVERIFY(grp);
  if (fx.root()->takeChild(wellsNode))
    grp->addChildNode(wellsNode);
  QCOMPARE(grp->children().size(), 2);
  wellsNode->setItemVisibilityChecked(false);
  wellsNode->setCustomProperty(QStringLiteral("userTag"),
                               QStringLiteral("keep-me"));

  fx.organizer.reorganize();

  wellsNode = fx.root()->findLayer(wells->id()); // clone 后是新节点对象
  QVERIFY(wellsNode);
  QCOMPARE(wellsNode->layer(), wells); // 图层引用不变
  QCOMPARE(wellsNode->parent(), static_cast<QgsLayerTreeNode *>(fx.root()));
  QCOMPARE(wellsNode->itemVisibilityChecked(), false);
  QCOMPARE(wellsNode->customProperty(QStringLiteral("userTag")).toString(),
           QStringLiteral("keep-me"));
  QCOMPARE(fx.root()->children().constFirst(), // 归位到树顶
           static_cast<QgsLayerTreeNode *>(wellsNode));
}

void TestLayerOrganizer::userDragNotReorganized()
{
  OrganizerFixture fx;
  QString err;
  QVERIFY2(fx.layerSvc.declare(decl(QStringLiteral("hz.a"), QStringLiteral("D61"),
                                    QStringLiteral("05_PaleoMap")), &err),
           qPrintable(err));
  QVERIFY2(fx.layerSvc.declare(decl(QStringLiteral("hz.b"), QStringLiteral("D61"),
                                    QStringLiteral("02_Prediction")), &err),
           qPrintable(err));
  QgsMapLayer *a = fx.instantiate(QStringLiteral("hz.a"));
  QgsMapLayer *b = fx.instantiate(QStringLiteral("hz.b"));
  QVERIFY(a && b);
  QgsLayerTreeGroup *grp = nullptr;
  for (QgsLayerTreeNode *n : fx.root()->children())
    if (n->customProperty(QStringLiteral("paleoHorizon")).toString()
        == QLatin1String("D61"))
      grp = qobject_cast<QgsLayerTreeGroup *>(n);
  QVERIFY(grp);
  QCOMPARE(grp->children().size(), 2); // [hz.a(05), hz.b(02)]

  // InternalMove：组内 hz.b 拖到 hz.a 之上——mimeData 克隆路径不触发
  // legendLayersAdded，布局器不纠正用户摆位。
  QgsLayerTreeModel model(fx.root());
  model.setFlag(QgsLayerTreeModel::AllowNodeReorder);
  const QModelIndex grpIdx = model.node2index(grp);
  const QModelIndex src = model.index(1, 0, grpIdx);
  QVERIFY(src.isValid());
  std::unique_ptr<QMimeData> md(model.mimeData({src}));
  QVERIFY(md);
  QVERIFY(model.dropMimeData(md.get(), Qt::MoveAction, 0, 0, grpIdx));
  // 源行在插入克隆后挪到下标 2——InternalMove 删的是新下标。
  QVERIFY(model.removeRows(2, 1, grpIdx));

  QCOMPARE(grp->children().size(), 2);
  auto *top = qobject_cast<QgsLayerTreeLayer *>(grp->children().at(0));
  QVERIFY(top);
  QCOMPARE(top->layer(), b); // 用户摆位生效且引用不悬空
}

void TestLayerOrganizer::unmanagedLayerUntouched()
{
  OrganizerFixture fx;
  QString err;
  QVERIFY2(fx.layerSvc.declare(decl(QStringLiteral("wells"), QString(),
                                    QStringLiteral("00_Data")), &err),
           qPrintable(err));
  fx.instantiate(QStringLiteral("wells"));

  // 用户手加的图层（无 paleoLayerId/无声明）——布局器不动它。
  auto *manual = new QgsVectorLayer(
      fixtureGpkg() + QStringLiteral("|layername=basin"),
      QStringLiteral("manual"), QStringLiteral("ogr"));
  QVERIFY(manual->isValid());
  fx.project()->addMapLayer(manual); // 落树根顶部
  QCOMPARE(fx.root()->children().constFirst()->name(),
           QStringLiteral("manual"));

  // 规整后手动层仍在树顶：Unmanaged 节点对摆放透明——不被移动也不被
  // 挤位，受管层只对受管层排相对位。
  fx.organizer.reorganize();
  QCOMPARE(fx.root()->children().constFirst()->name(),
           QStringLiteral("manual"));
  QCOMPARE(fx.root()->children().size(), 2);
}

int main(int argc, char *argv[])
{
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")), true);
  app.initQgis();
  TestLayerOrganizer tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_layerorganizer.moc"
