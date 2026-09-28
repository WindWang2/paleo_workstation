#include <QtTest>
#include <QAction>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QToolButton>

#include <qgsapplication.h>
#include <qgslayertree.h>
#include <qgslayertreemodel.h>
#include <qgslayertreeview.h>
#include <qgslayertreeviewindicator.h>
#include <qgsmaplayer.h>
#include <qgsproject.h>
#include <qgsrasterlayer.h>
#include <qgsvectorlayer.h>

#include "../src/metadata/layermanifest.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/ui/layers/layertreepanel.h"

// Fixture path: prefer the build-provided define, else derive from this file's
// location so standalone g++ builds work too (tst_layerservice convention).
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
                             const QString &type = QStringLiteral("vector"))
{
  LayerDeclaration d;
  d.layerId = layerId;
  d.horizon = horizon;
  d.type = type;
  d.source = fixtureGpkg() + QStringLiteral("|layername=basin");
  d.styleRef = QStringLiteral("styles/%1.qml").arg(layerId);
  d.group = QStringLiteral("04_SingleFactor");
  return d;
}

// wave/layer-platform 子任务 A：LayerTreePanel（工具条/筛选/组级显隐/复制
// 图层/indicator/右键意图信号/objectName 兼容）。面板属视图层——只渲染 +
// 发意图信号（docs/UI_LAYER_PLAN.md §1）。
class TestLayerTreePanel : public QObject
{
  Q_OBJECT

  private slots:
    void initTestCase()
    {
      QVERIFY(QgsApplication::instance() != nullptr);
      QVERIFY2(QFile::exists(fixtureGpkg()),
               qPrintable(QStringLiteral("fixture missing: %1").arg(fixtureGpkg())));
    }

    void cleanup()
    {
      // Tests share the QgsProject singleton (null-projectSvc fallback).
      QgsProject::instance()->removeAllMapLayers();
      QgsLayerTree *root = QgsProject::instance()->layerTreeRoot();
      const auto kids = root->children();
      for (QgsLayerTreeNode *n : kids)
        root->removeChildNode(n);
    }

    // ---- objectName 兼容契约（接线后 tst_ui 依赖同一组名字） ----
    void objectNamesContract()
    {
      LayerTreePanel panel(QgsProject::instance(), nullptr, nullptr);

      auto *view = panel.treeView();
      QVERIFY(view);
      QCOMPARE(view->objectName(), QStringLiteral("layerTreeView"));
      QVERIFY(view->model() != nullptr);
      QVERIFY(view->layerTreeModel() != nullptr);

      auto *empty = panel.findChild<QLabel *>(QStringLiteral("layerTreeEmptyState"));
      QVERIFY(empty);
      QVERIFY(empty->text().contains(QString::fromUtf8("图层树是空的")));

      QVERIFY(panel.findChild<QToolButton *>(QStringLiteral("layerTreeAddGroupButton")));
      QVERIFY(panel.findChild<QToolButton *>(QStringLiteral("layerTreeRemoveSelectedButton")));
      QVERIFY(panel.findChild<QToolButton *>(QStringLiteral("layerTreeExpandAllButton")));
      QVERIFY(panel.findChild<QToolButton *>(QStringLiteral("layerTreeCollapseAllButton")));

      auto *filterEdit = panel.findChild<QLineEdit *>(QStringLiteral("layerTreeFilterEdit"));
      QVERIFY(filterEdit);
      QVERIFY2(filterEdit->placeholderText().contains(QString::fromUtf8("筛选图层")),
               "占位文案钉死：筛选图层…");
    }

    // ---- 空态随工程图层集显隐 ----
    void emptyStateTogglesWithProjectLayers()
    {
      LayerTreePanel panel(QgsProject::instance(), nullptr, nullptr);
      auto *empty = panel.findChild<QLabel *>(QStringLiteral("layerTreeEmptyState"));
      QVERIFY(empty);
      QVERIFY(QgsProject::instance()->mapLayers().isEmpty());
      QVERIFY(!empty->isHidden());

      auto *vl = new QgsVectorLayer(QStringLiteral("Point"),
                                    QString::fromUtf8("临时井"), QStringLiteral("memory"));
      QVERIFY(vl->isValid());
      QgsProject::instance()->addMapLayer(vl);
      QVERIFY(empty->isHidden());

      QgsProject::instance()->removeMapLayer(vl->id());
      QVERIFY(!empty->isHidden());
    }

    // ---- 新增图层的 legend 节点默认展开 ----
    void newLayersGetExpandedLegendNodes()
    {
      LayerTreePanel panel(QgsProject::instance(), nullptr, nullptr);
      auto *vl = new QgsVectorLayer(QStringLiteral("Point"),
                                    QString::fromUtf8("临时井"), QStringLiteral("memory"));
      QgsProject::instance()->addMapLayer(vl);
      QgsLayerTreeModel *model = panel.treeView()->layerTreeModel();
      QTRY_VERIFY(model->rootGroup()->findLayer(vl->id()) != nullptr);
      QVERIFY2(model->rootGroup()->findLayer(vl->id())->isExpanded(),
               "新增图层 legend 节点应展开");
    }

    // ---- 筛选：命中显示 / 未命中隐藏 / 组保留 / 空串恢复 ----
    void filterMatchesNameIdAndPaleoLayerId()
    {
      LayerTreePanel panel(QgsProject::instance(), nullptr, nullptr);
      auto *view = panel.treeView();
      QgsLayerTreeModel *model = view->layerTreeModel();
      QgsLayerTree *root = model->rootGroup();

      auto *facies = new QgsVectorLayer(QStringLiteral("Polygon"),
                                        QString::fromUtf8("相图甲"), QStringLiteral("memory"));
      auto *check = new QgsVectorLayer(QStringLiteral("Point"),
                                       QString::fromUtf8("检查乙"), QStringLiteral("memory"));
      auto *wells = new QgsVectorLayer(QStringLiteral("Point"),
                                       QString::fromUtf8("井位丙"), QStringLiteral("memory"));
      facies->setCustomProperty(QStringLiteral("paleoLayerId"), QStringLiteral("facies.T1"));
      QgsProject::instance()->addMapLayer(facies);
      QgsProject::instance()->addMapLayer(check);
      QgsProject::instance()->addMapLayer(wells);

      auto *gPred = root->addGroup(QStringLiteral("02_Prediction"));
      auto *gValid = root->addGroup(QStringLiteral("07_Validation"));
      QVERIFY(moveNodeToGroup(model, facies, gPred));
      QVERIFY(moveNodeToGroup(model, check, gValid));

      // view 内部有代理模型：node2index 必须走 view（proxy 感知）
      const auto hidden = [&view](QgsLayerTreeNode *node) {
        const QModelIndex idx = view->node2index(node);
        return view->isRowHidden(idx.row(), idx.parent());
      };

      // 空串全显（默认态）
      QVERIFY(!hidden(root->findLayer(facies->id())));
      QVERIFY(!hidden(root->findLayer(check->id())));
      QVERIFY(!hidden(root->findLayer(wells->id())));
      QVERIFY(!hidden(gPred));
      QVERIFY(!hidden(gValid));

      // 按显示名过滤：命中层显示、组保留；其余隐藏
      panel.setFilterText(QString::fromUtf8("相图"));
      QCOMPARE(panel.filterText(), QString::fromUtf8("相图"));
      QVERIFY(!hidden(root->findLayer(facies->id())));
      QVERIFY(hidden(root->findLayer(check->id())));
      QVERIFY(hidden(root->findLayer(wells->id())));
      QVERIFY(!hidden(gPred));
      QVERIFY(hidden(gValid));

      // 按 paleoLayerId 过滤
      panel.setFilterText(QStringLiteral("facies"));
      QVERIFY(!hidden(root->findLayer(facies->id())));
      QVERIFY(hidden(root->findLayer(check->id())));

      // 按 QgsMapLayer::id() 过滤（自动生成串，唯一定位）
      panel.setFilterText(wells->id().mid(0, 8));
      QVERIFY(hidden(root->findLayer(facies->id())));
      QVERIFY(!hidden(root->findLayer(wells->id())));

      // 大小写不敏感
      panel.setFilterText(QStringLiteral("FACIES"));
      QVERIFY(!hidden(root->findLayer(facies->id())));

      // 输入框 textChanged 实时驱动
      panel.findChild<QLineEdit *>(QStringLiteral("layerTreeFilterEdit"))
          ->setText(QString::fromUtf8("检查"));
      QVERIFY(hidden(root->findLayer(facies->id())));
      QVERIFY(!hidden(root->findLayer(check->id())));
      QVERIFY(!hidden(gValid));
      QVERIFY(hidden(gPred));

      // 空串恢复全显
      panel.setFilterText(QString());
      QVERIFY(!hidden(root->findLayer(facies->id())));
      QVERIFY(!hidden(root->findLayer(check->id())));
      QVERIFY(!hidden(root->findLayer(wells->id())));
      QVERIFY(!hidden(gPred));
      QVERIFY(!hidden(gValid));
    }

    // ---- 组级显隐：组节点勾选切换，子层跟随 ----
    // 注：QgsLayerTreeNode::setItemVisibilityChecked 文档钉死「仅自身」；
    // 组级级联语义是 setItemVisibilityCheckedRecursive（checkbox 交互路径），
    // 祖先链语义由 isVisible() 承载——这里把两条真语义都钉住。
    void groupVisibilityRecursesToChildren()
    {
      LayerTreePanel panel(QgsProject::instance(), nullptr, nullptr);
      QgsLayerTreeModel *model = panel.treeView()->layerTreeModel();
      QgsLayerTree *root = model->rootGroup();

      auto *vl = new QgsVectorLayer(QStringLiteral("Point"),
                                    QString::fromUtf8("临时井"), QStringLiteral("memory"));
      QgsProject::instance()->addMapLayer(vl);
      QTRY_VERIFY(root->findLayer(vl->id()) != nullptr);
      QgsLayerTreeGroup *g = root->addGroup(QStringLiteral("02_Prediction"));
      QVERIFY(moveNodeToGroup(model, vl, g));
      QgsLayerTreeNode *child = root->findLayer(vl->id());

      // AllowNodeChangeVisibility 语义：模型勾选位可用
      const QModelIndex gi = model->node2index(g);
      QVERIFY(model->flags(gi) & Qt::ItemIsUserCheckable);
      QVERIFY(model->setData(gi, Qt::Unchecked, Qt::CheckStateRole));
      QVERIFY(!g->itemVisibilityChecked());

      // 组级显隐级联：子层勾选位跟随（QGIS 递归变体）
      g->setItemVisibilityCheckedRecursive(true);
      QVERIFY(g->itemVisibilityChecked());
      QVERIFY(child->itemVisibilityChecked());
      g->setItemVisibilityCheckedRecursive(false);
      QVERIFY(!child->itemVisibilityChecked());

      // 祖先链语义：仅关组自身，子层勾选位不动、有效可见性为否
      g->setItemVisibilityCheckedRecursive(true);
      QVERIFY(child->itemVisibilityChecked() && child->isVisible());
      g->setItemVisibilityChecked(false);
      QVERIFY(child->itemVisibilityChecked());
      QVERIFY(!child->isVisible());
    }

    // ---- 复制图层：副本存在、带“副本”、paleoLayerId 清空、同组 ----
    void duplicateLayerKeepsGroupAndClearsPaleoId()
    {
      LayerTreePanel panel(QgsProject::instance(), nullptr, nullptr);
      auto *view = panel.treeView();
      QgsLayerTree *root = view->layerTreeModel()->rootGroup();

      auto *orig = new QgsVectorLayer(QStringLiteral("Polygon"),
                                      QString::fromUtf8("相图甲"), QStringLiteral("memory"));
      orig->setCustomProperty(QStringLiteral("paleoLayerId"), QStringLiteral("facies.T1"));
      QgsProject::instance()->addMapLayer(orig);
      auto *gPred = root->addGroup(QStringLiteral("02_Prediction"));
      QTRY_VERIFY(root->findLayer(orig->id()) != nullptr);
      QVERIFY(moveNodeToGroup(view->layerTreeModel(), orig, gPred));

      view->setCurrentLayer(orig);
      auto *dupAct = panel.findChild<QAction *>(QStringLiteral("layerTreeDuplicateAction"));
      QVERIFY(dupAct);
      QVERIFY(dupAct->isEnabled());
      dupAct->trigger();

      const QList<QgsMapLayer *> dups =
          QgsProject::instance()->mapLayersByName(QString::fromUtf8("相图甲 副本"));
      QCOMPARE(dups.size(), 1);
      QgsMapLayer *dup = dups.first();
      QVERIFY2(dup->customProperty(QStringLiteral("paleoLayerId")).toString().isEmpty(),
               "副本必须清空 paleoLayerId——它不再是 manifest 管辖层");
      QCOMPARE(dup->type(), orig->type());

      // 与原层同父（同组节点）
      QgsLayerTreeNode *dupNode = root->findLayer(dup->id());
      QVERIFY(dupNode);
      QCOMPARE(dupNode->parent(), static_cast<QgsLayerTreeNode *>(gPred));

      // 原层不受影响
      QVERIFY(root->findLayer(orig->id()) != nullptr);
      QCOMPARE(orig->customProperty(QStringLiteral("paleoLayerId")).toString(),
               QStringLiteral("facies.T1"));

      // 树根层复制落树根
      auto *rootLayer = new QgsVectorLayer(QStringLiteral("Point"),
                                           QString::fromUtf8("井位丙"), QStringLiteral("memory"));
      QgsProject::instance()->addMapLayer(rootLayer);
      QTRY_VERIFY(root->findLayer(rootLayer->id()) != nullptr);
      view->setCurrentLayer(rootLayer);
      dupAct->trigger();
      const QList<QgsMapLayer *> dups2 =
          QgsProject::instance()->mapLayersByName(QString::fromUtf8("井位丙 副本"));
      QCOMPARE(dups2.size(), 1);
      QCOMPARE(static_cast<QgsLayerTreeNode *>(root->findLayer(dups2.first()->id())->parent()),
               static_cast<QgsLayerTreeNode *>(root));
    }

    // ---- indicator：层位未激活灰显 ----
    void horizonMismatchShowsInactiveIndicator()
    {
      QTemporaryDir tmp;
      QVERIFY(tmp.isValid());
      LayerManifest manifest(tmp.filePath(QStringLiteral("project.sqlite")));
      QVERIFY(manifest.open());
      QgisLayerService svc(nullptr, &manifest);
      QVERIFY(svc.declare(decl(QStringLiteral("facies.T1"), QStringLiteral("T1"))));
      QVERIFY(svc.declare(decl(QStringLiteral("check.T2"), QStringLiteral("T2"))));
      svc.setActiveHorizon(QStringLiteral("T2"));
      QVERIFY(svc.instantiate(QStringLiteral("facies.T1"))); // T1 层在场而激活层位是 T2

      LayerTreePanel panel(QgsProject::instance(), nullptr, &svc);
      auto *view = panel.treeView();
      QgsLayerTree *root = view->layerTreeModel()->rootGroup();

      QgsMapLayer *t1 = svc.layer(QStringLiteral("facies.T1"));
      QgsMapLayer *t2 = svc.layer(QStringLiteral("check.T2"));
      QVERIFY(t1 && t2);
      QgsLayerTreeNode *n1 = root->findLayer(t1->id());
      QgsLayerTreeNode *n2 = root->findLayer(t2->id());
      QVERIFY(n1 && n2);

      const QList<QgsLayerTreeViewIndicator *> inds = view->indicators(n1);
      QCOMPARE(inds.size(), 1);
      QCOMPARE(inds.first()->toolTip(),
               QString::fromUtf8("该图层属于层位 T1（未激活）"));

      // 激活层位本身无灰显
      QCOMPARE(view->indicators(n2).size(), 0);

      // 切回 T1：release+instantiate 信号驱动排队刷新（setActiveHorizon 的
      // horizonReleased 发在 m_activeHorizon 落位之前），灰显清空
      svc.setActiveHorizon(QStringLiteral("T1"));
      QTRY_COMPARE(view->indicators(n1).size(), 0);
    }

    // ---- indicator：缺源警示 ----
    void invalidLayerShowsWarningIndicator()
    {
      LayerTreePanel panel(QgsProject::instance(), nullptr, nullptr);
      auto *view = panel.treeView();

      auto *bad = new QgsRasterLayer(QStringLiteral("/nonexistent/x.tif"),
                                     QString::fromUtf8("坏栅格"), QStringLiteral("gdal"));
      QVERIFY(!bad->isValid());
      QgsProject::instance()->addMapLayer(bad);
      QgsLayerTree *root = view->layerTreeModel()->rootGroup();
      QTRY_VERIFY(root->findLayer(bad->id()) != nullptr);

      const QList<QgsLayerTreeViewIndicator *> inds =
          view->indicators(root->findLayer(bad->id()));
      QCOMPARE(inds.size(), 1);
      QCOMPARE(inds.first()->toolTip(), QString::fromUtf8("图层源不可用"));
      QVERIFY(!inds.first()->icon().isNull());
    }

    // ---- 「属性…」意图信号：paleoLayerId 优先，手工层回退 id() ----
    void propertiesActionEmitsLayerId()
    {
      QTemporaryDir tmp;
      QVERIFY(tmp.isValid());
      LayerManifest manifest(tmp.filePath(QStringLiteral("project.sqlite")));
      QVERIFY(manifest.open());
      QgisLayerService svc(nullptr, &manifest);
      LayerTreePanel panel(QgsProject::instance(), nullptr, &svc);
      auto *view = panel.treeView();

      auto *act = panel.findChild<QAction *>(QStringLiteral("layerTreePropertiesAction"));
      QVERIFY(act);
      QVERIFY2(!act->isEnabled(), "无选中图层时属性动作应禁用");

      auto *managed = new QgsVectorLayer(QStringLiteral("Polygon"),
                                         QString::fromUtf8("相图甲"), QStringLiteral("memory"));
      managed->setCustomProperty(QStringLiteral("paleoLayerId"), QStringLiteral("facies.T1"));
      QgsProject::instance()->addMapLayer(managed);
      auto *manual = new QgsVectorLayer(QStringLiteral("Point"),
                                        QString::fromUtf8("井位丙"), QStringLiteral("memory"));
      QgsProject::instance()->addMapLayer(manual);

      QSignalSpy spy(&panel, &LayerTreePanel::propertiesRequested);
      view->setCurrentLayer(managed);
      QTRY_VERIFY(act->isEnabled());
      act->trigger();
      QCOMPARE(spy.count(), 1);
      QCOMPARE(spy.takeFirst().at(0).toString(), QStringLiteral("facies.T1"));

      view->setCurrentLayer(manual);
      act->trigger();
      QCOMPARE(spy.count(), 1);
      QCOMPARE(spy.takeFirst().at(0).toString(), manual->id());
    }

    // ---- 「在新页打开所属编图页」：组→页映射 + 禁用 reason ----
    void mappingPageActionResolvesGroupsAndReasons()
    {
      LayerTreePanel panel(QgsProject::instance(), nullptr, nullptr);
      auto *view = panel.treeView();
      QgsLayerTreeModel *model = view->layerTreeModel();
      QgsLayerTree *root = model->rootGroup();

      auto *act = panel.findChild<QAction *>(QStringLiteral("layerTreeOpenMappingPageAction"));
      QVERIFY(act);

      // mkLayer(name, group)：注册并按需挪组；失败返回 nullptr（QTest 宏不进
      // 非 void lambda）。
      auto mkLayer = [root, model](const char *name, const char *group) -> QgsVectorLayer * {
        auto *vl = new QgsVectorLayer(QStringLiteral("Point"),
                                      QString::fromUtf8(name), QStringLiteral("memory"));
        QgsProject::instance()->addMapLayer(vl);
        if (root->findLayer(vl->id()) == nullptr)
          return nullptr;
        if (group)
        {
          QgsLayerTreeGroup *g = root->findGroup(QString::fromUtf8(group));
          if (!g)
            g = root->addGroup(QString::fromUtf8(group));
          if (!moveNodeToGroup(model, vl, g))
            return nullptr;
        }
        return vl;
      };

      QSignalSpy spy(&panel, &LayerTreePanel::mappingPageRequested);

      auto *pred = mkLayer("预测层", "02_Prediction");
      auto *cons = mkLayer("约束层", "03_Constraints");
      auto *sing = mkLayer("单因素层", "04_SingleFactor");
      auto *map = mkLayer("编图层", "05_PaleoMap");
      auto *ref = mkLayer("参考层", "06_Reference");
      auto *val = mkLayer("验证层", "07_Validation");
      auto *base = mkLayer("底图层", "01_Base");
      auto *loose = mkLayer("散层", nullptr);
      QVERIFY(pred && cons && sing && map && ref && val && base && loose);

      // 逐组触发：断言信号携带的 pageId
      const QList<QPair<QgsMapLayer *, QString>> cases = {
          {pred, QStringLiteral("predict")},
          {cons, QStringLiteral("constraint")},
          {sing, QStringLiteral("constraint")},
          {map, QStringLiteral("compose")},
          {ref, QStringLiteral("compose")},
          {val, QStringLiteral("validate")}};
      for (const auto &c : cases)
      {
        view->setCurrentLayer(c.first);
        QVERIFY2(act->isEnabled(),
                 qPrintable(QStringLiteral("group of %1 must map").arg(c.first->name())));
        spy.clear();
        act->trigger();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.takeFirst().at(0).toString(), c.second);
      }

      // 01_Base：禁用 + reason tooltip；未编组：禁用 + reason
      view->setCurrentLayer(base);
      QVERIFY(!act->isEnabled());
      QVERIFY2(!act->toolTip().isEmpty(), "禁用动作必须给 reason（DESIGN.md）");
      view->setCurrentLayer(loose);
      QVERIFY(!act->isEnabled());
      QVERIFY2(!act->toolTip().isEmpty(), "禁用动作必须给 reason（DESIGN.md）");
    }

    // ---- 工具条：添加组/删除选中/展开/折叠 ----
    void toolbarButtonsDriveTreeActions()
    {
      LayerTreePanel panel(QgsProject::instance(), nullptr, nullptr);
      auto *view = panel.treeView();
      QgsLayerTree *root = view->layerTreeModel()->rootGroup();

      auto *addBtn = panel.findChild<QToolButton *>(QStringLiteral("layerTreeAddGroupButton"));
      auto *rmBtn = panel.findChild<QToolButton *>(QStringLiteral("layerTreeRemoveSelectedButton"));
      auto *expBtn = panel.findChild<QToolButton *>(QStringLiteral("layerTreeExpandAllButton"));
      auto *colBtn = panel.findChild<QToolButton *>(QStringLiteral("layerTreeCollapseAllButton"));
      QVERIFY(addBtn && rmBtn && expBtn && colBtn);

      addBtn->click();
      QCOMPARE(root->findGroups().size(), 1);

      auto *vl = new QgsVectorLayer(QStringLiteral("Point"),
                                    QString::fromUtf8("临时井"), QStringLiteral("memory"));
      QgsProject::instance()->addMapLayer(vl);
      QTRY_VERIFY(root->findLayer(vl->id()) != nullptr);

      colBtn->click();
      QVERIFY(!view->isExpanded(view->node2index(root->findLayer(vl->id()))));
      expBtn->click();
      QVERIFY(view->isExpanded(view->node2index(root->findLayer(vl->id()))));

      view->setCurrentLayer(vl);
      rmBtn->click();
      QVERIFY2(root->findLayer(vl->id()) == nullptr,
               "删除选中后节点应从树上移除");
    }

    // ---- 主线5：「删除选中」消歧 + 编辑会话守卫 ----
    void removalGuardRefusesEditingLayers()
    {
      LayerTreePanel panel(QgsProject::instance(), nullptr, nullptr);
      QSignalSpy refuseSpy(&panel, &LayerTreePanel::layerRemovalRefused);
      auto *view = panel.treeView();
      QgsLayerTree *root = view->layerTreeModel()->rootGroup();

      // 文案消歧：动作明确写「图层/组」，并指向要素删除的另一入口。
      auto *rmBtn = panel.findChild<QToolButton *>(QStringLiteral("layerTreeRemoveSelectedButton"));
      QVERIFY(rmBtn != nullptr && rmBtn->defaultAction() != nullptr);
      QCOMPARE(rmBtn->defaultAction()->text(), QStringLiteral("删除所选图层/组"));
      QVERIFY(rmBtn->defaultAction()->toolTip().contains(QStringLiteral("要素")));

      auto *vl = new QgsVectorLayer(QStringLiteral("Point"),
                                    QString::fromUtf8("编辑中"), QStringLiteral("memory"));
      QgsProject::instance()->addMapLayer(vl);
      QTRY_VERIFY(root->findLayer(vl->id()) != nullptr);
      view->setCurrentLayer(vl);

      // 编辑会话中：删除被拒 + 原因信号；节点保留
      vl->startEditing();
      rmBtn->click();
      QCOMPARE(refuseSpy.count(), 1);
      QVERIFY(refuseSpy.at(0).at(0).toString().contains(QStringLiteral("编辑")));
      QVERIFY(root->findLayer(vl->id()) != nullptr);

      // 收尾会话后同一按钮删除成功
      vl->rollBack();
      rmBtn->click();
      QVERIFY2(root->findLayer(vl->id()) == nullptr,
               "会话收尾后删除所选图层应生效");
    }

    // ---- 导出/加载样式：offscreen 无对话框（硬纪律烟测——不弹不死） ----
    void styleActionsNoOpUnderOffscreen()
    {
      LayerTreePanel panel(QgsProject::instance(), nullptr, nullptr);
      auto *view = panel.treeView();

      auto *vl = new QgsVectorLayer(QStringLiteral("Point"),
                                    QString::fromUtf8("临时井"), QStringLiteral("memory"));
      QgsProject::instance()->addMapLayer(vl);
      view->setCurrentLayer(vl);

      auto *exportAct = panel.findChild<QAction *>(QStringLiteral("layerTreeExportStyleAction"));
      auto *importAct = panel.findChild<QAction *>(QStringLiteral("layerTreeImportStyleAction"));
      QVERIFY(exportAct && importAct);
      QVERIFY(exportAct->isEnabled() && importAct->isEnabled());

      if (QGuiApplication::platformName() == QLatin1String("offscreen"))
      {
        exportAct->trigger(); // 硬纪律：offscreen 直接 no-op
        importAct->trigger();
        QVERIFY(vl->isValid()); // 未崩、层未被动坏
      }
    }

  private:
    // 把注册层（bridge 已落在树根）挪进目标组——takeChild/insertChildNode 是
    // QGIS 树内挪节点的标准对。
    static bool moveNodeToGroup(QgsLayerTreeModel *model, QgsMapLayer *layer,
                                QgsLayerTreeGroup *group)
    {
      if (!model || !layer || !group)
        return false;
      QgsLayerTreeLayer *node = model->rootGroup()->findLayer(layer->id());
      if (!node || !node->parent())
        return false;
      if (node->parent() == group)
        return true;
      if (!node->parent()->takeChild(node))
        return false;
      group->insertChildNode(-1, node);
      return group->findLayer(layer->id()) != nullptr;
    }
};

int main(int argc, char *argv[])
{
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")), true); // distro install
  app.initQgis();
  TestLayerTreePanel tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_layertreepanel.moc"
