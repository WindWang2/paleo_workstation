#include "../src/ui/paleoicons.h"
#include "../src/qgis/layervocabulary.h"
#include "perfbudget_relax.h"
#include <QtTest>
#include <QAction>
#include <QByteArray>
#include <QDomDocument>
#include <QDomNode>
#include <QFile>
#include <QFileInfo>
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
#include <qgsnullsymbolrenderer.h>
#include <qgsproject.h>
#include <qgsrasterlayer.h>
#include <qgsreadwritecontext.h>
#include <qgsvectorlayer.h>

#include "../src/metadata/layermanifest.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/services/errorhub.h"
#include "../src/ui/layers/layertreepanel.h"
#include "uipolish_capture.h"

// B.17：导出必败的测试层——writeSymbology 置错误串并返回 false，复现
// 「QGIS 返回错误串被忽略」路径（真实层的 writeSymbology 基本不失败，用桩
// 把该分支钉住；QgsVectorLayer::writeSymbology 是 final，故取可覆写的
// QgsRasterLayer）。exportNamedStyle 在该分支不赋值 doc（半成品空文档），
// 旧实现会把它 Truncate 写进用户选定文件。
class FailingStyleExportLayer : public QgsRasterLayer
{
  public:
    using QgsRasterLayer::QgsRasterLayer;
    bool writeSymbology(QDomNode &, QDomDocument &, QString &errorMessage,
                        const QgsReadWriteContext &,
                        QgsMapLayer::StyleCategories) const override
    {
        errorMessage = QStringLiteral("stub symbology failure");
        return false;
    }
};

// offscreen 下对话框路径被 noFileDialogs 纪律短路——样式 IO 核心的失败
// 分支经 TestAccess 直达（layertreepanel.h 的 friend 先例）。
class LayerTreePanelStyleTestAccess
{
  public:
    static bool exportStyleTo(LayerTreePanel *panel, QgsMapLayer *layer, const QString &path,
                              QString *error)
    {
        return panel->exportStyleTo(layer, path, error);
    }
    static bool importStyleFrom(LayerTreePanel *panel, QgsMapLayer *layer, const QString &path,
                                QString *error)
    {
        return panel->importStyleFrom(layer, path, error);
    }
};

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

    // goal/ui-experience-polish：空态卡 + 层行选中态的修前/修后证据
    //（PALEO_UI_CAPTURE 未设时零开销直通）。
    void captureEvidence()
    {
      LayerTreePanel panel(QgsProject::instance(), nullptr, nullptr);
      uipolish::capturePanel(&panel, QStringLiteral("layertree_empty"));
    }

    // R1-D1：实例化失败的声明层不进树（无 indicator 可挂）——面板在树外
    // 把失败经全局 ErrorHub 说出来（warning 级）；修复前层位切换后缺源
    // 图层无声消失。
    void instantiationFailureRaisedToErrorHub()
    {
      QTemporaryDir tmp;
      QVERIFY(tmp.isValid());
      LayerManifest manifest(tmp.filePath(QStringLiteral("project.sqlite")));
      QVERIFY(manifest.open());
      QgisLayerService svc(nullptr, &manifest);
      LayerDeclaration bad = decl(QStringLiteral("facies.T1"), QStringLiteral("T1"));
      bad.title = QStringLiteral("T1 相面");
      bad.source = QStringLiteral("/nonexistent-dir-42/ghost.gpkg|layername=x");
      QVERIFY(svc.declare(bad));

      // 作用域守卫：QVERIFY 早退也保证全局指针还原（悬空 hub 会让后续
      // 用例 UAF）。
      struct HubInstall
      {
        ErrorHub hub;
        HubInstall() { ErrorHub::installGlobal(&hub); }
        ~HubInstall() { ErrorHub::installGlobal(nullptr); }
      } install;
      LayerTreePanel panel(QgsProject::instance(), nullptr, &svc);
      svc.setActiveHorizon(QStringLiteral("T1"));
      QCOMPARE(install.hub.size(), 1);
      const QVector<ErrorHub::Entry> entries = install.hub.entries();
      QVERIFY(entries.first().title.contains(QStringLiteral("T1")));
      QVERIFY(entries.first().text.contains(QStringLiteral("T1 相面")));
      QCOMPARE(entries.first().level, ErrorHub::Level::Warning);
    }

    // goal/ui-experience-polish：Delete 键直达「删除所选图层/组」（QAction
    // 挂键；非编辑态不弹框直接删）。
    void deleteKeyRemovesCurrentLayer()
    {
      LayerTreePanel panel(QgsProject::instance(), nullptr, nullptr);
      auto *act = panel.findChild<QAction *>(QStringLiteral("layerTreeRemoveSelectedAction"));
      QVERIFY(act);
      QCOMPARE(act->shortcut(), QKeySequence(Qt::Key_Delete));

      auto *vl = new QgsVectorLayer(QStringLiteral("Point"),
                                    QString::fromUtf8("临时井"), QStringLiteral("memory"));
      QVERIFY(vl->isValid());
      QgsProject::instance()->addMapLayer(vl);
      panel.treeView()->setCurrentLayer(vl);
      // 默认删除动作按「选中节点」取对象——补选区（仅 currentLayer 不够）。
      const QModelIndex idx = panel.treeView()->currentIndex();
      QVERIFY(idx.isValid());
      panel.treeView()->selectionModel()->select(
          idx, QItemSelectionModel::Select | QItemSelectionModel::Rows);
      panel.show();
      QVERIFY(QTest::qWaitForWindowExposed(&panel));
      QApplication::setActiveWindow(&panel); // offscreen 下快捷键派发需要活动窗口
      panel.treeView()->setFocus();
      QCOMPARE(QgsProject::instance()->mapLayers().size(), 1);
      QTest::keyClick(panel.treeView(), Qt::Key_Delete);
      // QGIS 默认动作语义（E3 不重造）：从树摘节点（工程图层注册表不动——
      // 那是「移除图层」另一档动作）。断言盯树模型。
      QVERIFY(panel.treeView()->layerTreeModel()->rootGroup()->children().isEmpty());
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
      QVERIFY(panel.findChild<QToolButton *>(QStringLiteral("layerTreeMoveUpButton")));
      QVERIFY(panel.findChild<QToolButton *>(QStringLiteral("layerTreeMoveDownButton")));
      QVERIFY(panel.findChild<QAction *>(QStringLiteral("layerTreeMoveToTopAction")));

      // 工具条图标化契约：六个按钮走 QToolButton::IconOnly + QGIS 主题图标
      //（图标缺失时按钮回退文本显示——契约断言的是「图标资源可解析」）。
      for (const QString &name :
           {QStringLiteral("layerTreeAddGroupButton"),
            QStringLiteral("layerTreeRemoveSelectedButton"),
            QStringLiteral("layerTreeExpandAllButton"),
            QStringLiteral("layerTreeCollapseAllButton"),
            QStringLiteral("layerTreeMoveUpButton"),
            QStringLiteral("layerTreeMoveDownButton")})
      {
        auto *btn = panel.findChild<QToolButton *>(name);
        QVERIFY2(btn, qPrintable(name));
        QCOMPARE(btn->toolButtonStyle(), Qt::ToolButtonIconOnly);
        QVERIFY2(!btn->icon().isNull(), qPrintable(name));
        QVERIFY2(!btn->toolTip().isEmpty(), qPrintable(name)); // 图标按钮必须留文字线索
      }

      auto *filterEdit = panel.findChild<QLineEdit *>(QStringLiteral("layerTreeFilterEdit"));
      QVERIFY(filterEdit);
      QVERIFY2(filterEdit->placeholderText().contains(QString::fromUtf8("筛选图层")),
               "占位文案钉死：筛选图层…");
    }

    void actionIconResourceContract()
    {
      // 方向 29：保留六按钮原契约，并覆盖所有静态命名动作 SVG。
      const QStringList resources{
        QStringLiteral("3d.svg"),
        QStringLiteral("grid.svg"),
        QStringLiteral("mActionAdd.svg"),
        QStringLiteral("mActionAddGroup.svg"),
        QStringLiteral("mActionAddMap.svg"),
        QStringLiteral("mActionAddTable.svg"),
        QStringLiteral("mActionAddWmsLayer.svg"),
        QStringLiteral("mActionAlignTop.svg"),
        QStringLiteral("mActionArrowDown.svg"),
        QStringLiteral("mActionArrowLeft.svg"),
        QStringLiteral("mActionArrowRight.svg"),
        QStringLiteral("mActionArrowUp.svg"),
        QStringLiteral("mActionAtlasSettings.svg"),
        QStringLiteral("mActionCancelEdits.svg"),
        QStringLiteral("mActionCaptureLine.svg"),
        QStringLiteral("mActionCapturePoint.svg"),
        QStringLiteral("mActionCapturePolygon.svg"),
        QStringLiteral("mActionCollapseTree.svg"),
        QStringLiteral("mActionDeleteSelected.svg"),
        QStringLiteral("mActionEditCopy.svg"),
        QStringLiteral("mActionElevationProfile.svg"),
        QStringLiteral("mActionExpandTree.svg"),
        QStringLiteral("mActionFileExit.svg"),
        QStringLiteral("mActionFileNew.svg"),
        QStringLiteral("mActionFileOpen.svg"),
        QStringLiteral("mActionFileSave.svg"),
        QStringLiteral("mActionFileSaveAs.svg"),
        QStringLiteral("mActionFilter2.svg"),
        QStringLiteral("mActionFilterTableFields.svg"),
        QStringLiteral("mActionFolder.svg"),
        QStringLiteral("mActionHistory.svg"),
        QStringLiteral("mActionIdentify.svg"),
        QStringLiteral("mActionMapSettings.svg"),
        QStringLiteral("mActionMeasure.svg"),
        QStringLiteral("mActionMeasureArea.svg"),
        QStringLiteral("mActionMoveFeature.svg"),
        QStringLiteral("mActionNewBookmark.svg"),
        QStringLiteral("mActionNewLayout.svg"),
        QStringLiteral("mActionNewPage.svg"),
        QStringLiteral("mActionOpenTable.svg"),
        QStringLiteral("mActionOptions.svg"),
        QStringLiteral("mActionPan.svg"),
        QStringLiteral("mActionRedo.svg"),
        QStringLiteral("mActionRefresh.svg"),
        QStringLiteral("mActionRemoveLayer.svg"),
        QStringLiteral("mActionReshape.svg"),
        QStringLiteral("mActionSaveAsPDF.svg"),
        QStringLiteral("mActionSaveEdits.svg"),
        QStringLiteral("mActionSaveMapAsImage.svg"),
        QStringLiteral("mActionSelectRectangle.svg"),
        QStringLiteral("mActionSharing.svg"),
        QStringLiteral("mActionSharingExport.svg"),
        QStringLiteral("mActionShowAllLayers.svg"),
        QStringLiteral("mActionStart.svg"),
        QStringLiteral("mActionUndo.svg"),
        QStringLiteral("mActionVertexTool.svg"),
        QStringLiteral("mActionZoomFullExtent.svg"),
        QStringLiteral("mActionZoomIn.svg"),
        QStringLiteral("mActionZoomOut.svg"),
        QStringLiteral("mActionZoomToSelected.svg"),
        QStringLiteral("mIconFolderHome.svg"),
        QStringLiteral("mIconFolderOpen.svg"),
        QStringLiteral("mIconLineLayer.svg"),
        QStringLiteral("mIconPointLayer.svg"),
        QStringLiteral("mIconPolygonLayer.svg"),
        QStringLiteral("mIconRasterLayer.svg"),
        QStringLiteral("mIconTopologicalEditing.svg"),
        QStringLiteral("mIconWarning.svg"),
        QStringLiteral("mLayoutItemElevationProfile.svg"),
        QStringLiteral("processingAlgorithm.svg"),
        QStringLiteral("processingModel.svg"),
        QStringLiteral("propertyicons/symbology.svg"),
      };
      for (const QString &name : resources)
      {
        const QIcon icon = PaleoIcons::qgisTheme(name);
        QVERIFY2(!icon.isNull(), qPrintable(name));
        QVERIFY2(!icon.pixmap(24, 24).isNull(), qPrintable(name));
      }
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

    // 上移/下移/置顶：序号 0 是所在组最上层（绘制盖住同组其余节点）。
    void moveUpDownAndPinToTop()
    {
      LayerTreePanel panel(QgsProject::instance(), nullptr, nullptr);
      auto *up = panel.findChild<QAction *>(QStringLiteral("layerTreeMoveUpAction"));
      auto *down = panel.findChild<QAction *>(QStringLiteral("layerTreeMoveDownAction"));
      auto *pin = panel.findChild<QAction *>(QStringLiteral("layerTreeMoveToTopAction"));
      QVERIFY(up);
      QVERIFY(down);
      QVERIFY(pin);

      auto *jia = new QgsVectorLayer(QStringLiteral("Point"),
                                     QString::fromUtf8("甲"), QStringLiteral("memory"));
      auto *yi = new QgsVectorLayer(QStringLiteral("Point"),
                                    QString::fromUtf8("乙"), QStringLiteral("memory"));
      auto *bing = new QgsVectorLayer(QStringLiteral("Point"),
                                      QString::fromUtf8("丙"), QStringLiteral("memory"));
      QVERIFY(jia->isValid() && yi->isValid() && bing->isValid());
      QgsProject::instance()->addMapLayer(jia);
      QgsProject::instance()->addMapLayer(yi);
      QgsProject::instance()->addMapLayer(bing);

      QgsLayerTreeGroup *root = panel.layerTreeModel()->rootGroup();
      const auto namesOf = [root]() {
        QStringList names;
        for (QgsLayerTreeNode *node : root->children())
        {
          if (auto *layerNode = qobject_cast<QgsLayerTreeLayer *>(node))
            names << layerNode->name();
        }
        return names;
      };
      const QStringList initial = namesOf();
      QCOMPARE(initial.size(), 3);

      const auto selectName = [&](const QString &name) -> bool {
        for (QgsLayerTreeNode *node : root->children())
        {
          auto *layerNode = qobject_cast<QgsLayerTreeLayer *>(node);
          if (!layerNode || layerNode->name() != name)
            continue;
          panel.treeView()->setCurrentNode(layerNode);
          const QModelIndex idx = panel.layerTreeModel()->node2index(layerNode);
          if (!idx.isValid())
            return false;
          panel.treeView()->selectionModel()->select(
              idx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
          return true;
        }
        return false;
      };

      const QString bottom = initial.last();
      QVERIFY(selectName(bottom));
      QVERIFY(up->isEnabled());
      QVERIFY(pin->isEnabled());
      up->trigger();
      QStringList stepped = namesOf();
      QCOMPARE(stepped.at(initial.size() - 2), bottom);
      QCOMPARE(stepped.last(), initial.at(initial.size() - 2));

      pin->trigger();
      QCOMPARE(namesOf().first(), bottom);
      QVERIFY(!up->isEnabled());
      QVERIFY(!pin->isEnabled());
      QVERIFY(down->isEnabled());
      const QStringList pinned = namesOf();
      up->trigger(); // 已在最上：动作触发也不改序
      QCOMPARE(namesOf(), pinned);

      QVERIFY(selectName(namesOf().last()));
      QVERIFY(!down->isEnabled());
      const QStringList beforeDown = namesOf();
      down->trigger();
      QCOMPARE(namesOf(), beforeDown);

      QVERIFY(selectName(namesOf().first()));
      QVERIFY(down->isEnabled());
      const QString top = namesOf().first();
      down->trigger();
      QCOMPARE(namesOf().at(1), top);
    }

    // opt-05：自定义绘制序生效时（「显示预测点位」置顶会启用它），树里
    // 挪层必须同步自定义序——否则画布叠置停在旧位（树序≠画序）。只被挪
    // 的那层按新树序插回序列表；特意置顶的非树序条目不被打平。
    void moveSyncsActiveCustomLayerOrder()
    {
      // 单例工程跨用例复用：前序用例遗留的层会混进树序断言——先清场。
      QgsProject::instance()->removeAllMapLayers();
      LayerTreePanel panel(QgsProject::instance(), nullptr, nullptr);
      auto *down = panel.findChild<QAction *>(QStringLiteral("layerTreeMoveDownAction"));
      QVERIFY(down);

      auto *pin = new QgsVectorLayer(QStringLiteral("Point"),
                                     QString::fromUtf8("置顶"), QStringLiteral("memory"));
      auto *a = new QgsVectorLayer(QStringLiteral("Point"),
                                   QString::fromUtf8("甲"), QStringLiteral("memory"));
      auto *b = new QgsVectorLayer(QStringLiteral("Point"),
                                   QString::fromUtf8("乙"), QStringLiteral("memory"));
      auto *c = new QgsVectorLayer(QStringLiteral("Point"),
                                   QString::fromUtf8("丙"), QStringLiteral("memory"));
      QVERIFY(pin->isValid() && a->isValid() && b->isValid() && c->isValid());
      QgsLayerTree *root = panel.layerTreeModel()->rootGroup();
      // 逐 addLayer 追加建序（工程绑定树根：addLayer 同时注册进
      // mapLayers），树序自上而下 = [甲, 乙, 丙, 置顶]——置顶层特意压树底，
      // 与下面的自定义序置顶形成发散。
      root->addLayer(a);
      root->addLayer(b);
      root->addLayer(c);
      root->addLayer(pin);
      const auto treeOrder = [root]() {
        QList<QgsMapLayer *> out;
        for (QgsLayerTreeNode *n : root->children())
          if (auto *ln = qobject_cast<QgsLayerTreeLayer *>(n))
            out << ln->layer();
        return out;
      };
      QCOMPARE(treeOrder(), QList<QgsMapLayer *>({a, b, c, pin}));

      // 模拟特意置顶：启用自定义序并把 pin 压到序列顶（与树序不同）。
      QList<QgsMapLayer *> custom = treeOrder();
      custom.removeAll(pin);
      custom.prepend(pin); // [置顶, 甲, 乙, 丙]
      root->setHasCustomLayerOrder(true);
      root->setCustomLayerOrder(custom);
      QCOMPARE(root->customLayerOrder(), QList<QgsMapLayer *>({pin, a, b, c}));

      // 把「甲」（树顶）下移一层 → 树序 [乙, 甲, 丙, 置顶]。
      QgsLayerTreeLayer *aNode = root->findLayer(a->id());
      panel.treeView()->setCurrentNode(aNode);
      const QModelIndex aIdx = panel.layerTreeModel()->node2index(aNode);
      QVERIFY(aIdx.isValid());
      panel.treeView()->selectionModel()->select(
          aIdx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
      QVERIFY(down->isEnabled());
      down->trigger();
      QCOMPARE(treeOrder(), QList<QgsMapLayer *>({b, a, c, pin}));

      // 自定义序：甲落到乙之下（跟随新树位），置顶 pin 原位不打平。
      const QList<QgsMapLayer *> after = root->customLayerOrder();
      QCOMPARE(after.indexOf(pin), 0);
      QVERIFY(after.indexOf(a) > after.indexOf(b));
      QVERIFY(after.indexOf(a) < after.indexOf(c));

      root->setHasCustomLayerOrder(false); // 状态还原，不留给后续用例
      QgsProject::instance()->removeAllMapLayers();
    }

    // opt-05 复审补网：拖拽 InternalMove 不经 moveCurrentNode——
    // QgsLayerTreeModel::dropMimeData 用 mime 克隆 insertChildNodes 到
    // 目标位再 removeRows 源节点；rowsInserted 排队回填须覆盖此路径。
    // 用 insertChildNode+removeChildNode 复现同一信号序列，无需真拖拽。
    void dragMoveSyncsActiveCustomLayerOrder()
    {
      QgsProject::instance()->removeAllMapLayers();
      LayerTreePanel panel(QgsProject::instance(), nullptr, nullptr);
      QVERIFY(panel.findChild<QAction *>(QStringLiteral("layerTreeMoveDownAction")));

      auto *pin = new QgsVectorLayer(QStringLiteral("Point"),
                                     QString::fromUtf8("置顶"), QStringLiteral("memory"));
      auto *a = new QgsVectorLayer(QStringLiteral("Point"),
                                   QString::fromUtf8("甲"), QStringLiteral("memory"));
      auto *b = new QgsVectorLayer(QStringLiteral("Point"),
                                   QString::fromUtf8("乙"), QStringLiteral("memory"));
      auto *c = new QgsVectorLayer(QStringLiteral("Point"),
                                   QString::fromUtf8("丙"), QStringLiteral("memory"));
      QVERIFY(pin->isValid() && a->isValid() && b->isValid() && c->isValid());
      QgsLayerTree *root = panel.layerTreeModel()->rootGroup();
      root->addLayer(a);
      root->addLayer(b);
      root->addLayer(c);
      root->addLayer(pin); // 树序 [甲, 乙, 丙, 置顶]
      const auto treeOrder = [root]() {
        QList<QgsMapLayer *> out;
        for (QgsLayerTreeNode *n : root->children())
          if (auto *ln = qobject_cast<QgsLayerTreeLayer *>(n))
            out << ln->layer();
        return out;
      };
      QCOMPARE(treeOrder(), QList<QgsMapLayer *>({a, b, c, pin}));

      QList<QgsMapLayer *> custom = treeOrder();
      custom.removeAll(pin);
      custom.prepend(pin); // [置顶, 甲, 乙, 丙]
      root->setHasCustomLayerOrder(true);
      root->setCustomLayerOrder(custom);

      // 模拟把「丙」拖到树顶：克隆节点插到 index 0，再移除原节点。
      QgsLayerTreeLayer *cNode = root->findLayer(c->id());
      QVERIFY(cNode);
      root->insertChildNode(0, cNode->clone());
      root->removeChildNode(cNode);
      QCOMPARE(treeOrder(), QList<QgsMapLayer *>({c, a, b, pin}));

      // 回填是排队执行的——让投递的 invokeMethod 跑完。
      QCoreApplication::processEvents();
      const QList<QgsMapLayer *> after = root->customLayerOrder();
      QCOMPARE(after.indexOf(pin), 0);          // 特意置顶不被打平
      QCOMPARE(after.indexOf(c), 1);            // 丙插到 pin 之下、甲乙之上
      QVERIFY(after.indexOf(a) > after.indexOf(c));
      QVERIFY(after.indexOf(b) > after.indexOf(a));

      root->setHasCustomLayerOrder(false);
      QgsProject::instance()->removeAllMapLayers();
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

    // 右键「属性表」：矢量层发 paleoLayerId（手工层回退 id）；栅格禁用并带原因。
    void attributeTableActionOpensVectorLayer()
    {
      QTemporaryDir tmp;
      QVERIFY(tmp.isValid());
      LayerManifest manifest(tmp.filePath(QStringLiteral("project.sqlite")));
      QVERIFY(manifest.open());
      QgisLayerService svc(nullptr, &manifest);
      LayerTreePanel panel(QgsProject::instance(), nullptr, &svc);
      auto *view = panel.treeView();
      auto *act = panel.findChild<QAction *>(QStringLiteral("layerTreeAttributeTableAction"));
      QVERIFY(act);
      QVERIFY2(!act->isEnabled(), "无选中图层时属性表动作应禁用");
      QVERIFY(act->toolTip().contains(QString::fromUtf8("未选中图层")));

      auto *managed = new QgsVectorLayer(QStringLiteral("LineString"),
                                         QString::fromUtf8("D62 约束线"),
                                         QStringLiteral("memory"));
      managed->setCustomProperty(QStringLiteral("paleoLayerId"),
                                 QStringLiteral("constraints.D62"));
      QgsProject::instance()->addMapLayer(managed);
      auto *manual = new QgsVectorLayer(QStringLiteral("Point"),
                                        QString::fromUtf8("手工点"), QStringLiteral("memory"));
      QgsProject::instance()->addMapLayer(manual);
      auto *raster = new QgsRasterLayer(QStringLiteral("/nonexistent/D62.tif"),
                                        QString::fromUtf8("D62 栅格"), QStringLiteral("gdal"));
      QgsProject::instance()->addMapLayer(raster);

      QSignalSpy spy(&panel, &LayerTreePanel::attributeTableRequested);
      view->setCurrentLayer(managed);
      QTRY_VERIFY(act->isEnabled());
      act->trigger();
      QCOMPARE(spy.count(), 1);
      QCOMPARE(spy.takeFirst().at(0).toString(), QStringLiteral("constraints.D62"));

      view->setCurrentLayer(manual);
      QTRY_VERIFY(act->isEnabled());
      act->trigger();
      QCOMPARE(spy.count(), 1);
      QCOMPARE(spy.takeFirst().at(0).toString(), manual->id());

      view->setCurrentLayer(raster);
      QTRY_VERIFY(!act->isEnabled());
      QVERIFY(act->toolTip().contains(QString::fromUtf8("仅矢量图层")));
      act->trigger();
      QCOMPARE(spy.count(), 0);
    }

    // ---- goal/time-depth-velocity：「转换为深度域…」意图信号 ----
    // 仅时间域层位（paleoLayerId horizon.*）可用；深度产物/手工层不给入口。
    void depthConvertActionEmitsIntent()
    {
      QTemporaryDir tmp;
      QVERIFY(tmp.isValid());
      LayerManifest manifest(tmp.filePath(QStringLiteral("project.sqlite")));
      QVERIFY(manifest.open());
      QgisLayerService svc(nullptr, &manifest);
      LayerTreePanel panel(QgsProject::instance(), nullptr, &svc);
      auto *view = panel.treeView();

      auto *act = panel.findChild<QAction *>(QStringLiteral("layerTreeDepthConvertAction"));
      QVERIFY(act);
      QVERIFY2(!act->isEnabled(), "无选中图层时深度转换动作应禁用");
      QVERIFY(!act->toolTip().isEmpty()); // DESIGN.md：禁用必须带 reason

      auto *horizon = new QgsRasterLayer(QStringLiteral("/nonexistent/D61.tif"),
                                         QString::fromUtf8("D61 时间构造图"),
                                         QStringLiteral("gdal"));
      horizon->setCustomProperty(QStringLiteral("paleoLayerId"), QStringLiteral("horizon.D61"));
      QgsProject::instance()->addMapLayer(horizon);
      auto *converted = new QgsRasterLayer(QStringLiteral("/nonexistent/DEPTH_D61.tif"),
                                           QString::fromUtf8("D61 深度域"),
                                           QStringLiteral("gdal"));
      converted->setCustomProperty(QStringLiteral("paleoLayerId"), QStringLiteral("depth.D61"));
      QgsProject::instance()->addMapLayer(converted);

      QSignalSpy spy(&panel, &LayerTreePanel::depthConversionRequested);
      view->setCurrentLayer(horizon);
      QTRY_VERIFY(act->isEnabled());
      act->trigger();
      QCOMPARE(spy.count(), 1);
      QCOMPARE(spy.takeFirst().at(0).toString(), QStringLiteral("horizon.D61"));

      view->setCurrentLayer(converted);
      QTRY_VERIFY(!act->isEnabled()); // 深度产物不可再转换
      act->trigger();
      QCOMPARE(spy.count(), 0);       // 已 takeFirst；禁用态触发不发意图
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
      auto *cons = mkLayer("约束层", PaleoLayerVocabulary::kConstraintsGroup.toUtf8().constData());
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

    // B.17：样式 .qml 导出/加载核心——QSaveFile 原子写 + write() 返回值
    // 校验 + QGIS 错误串与导入返回值如实上抛。旧实现三处全丢：导出失败
    // 仍 Truncate 写空/半成品文档、write() 返回值不查、导入失败静默。
    void styleIoCoreReportsFailuresAndWritesAtomically()
    {
      QTemporaryDir tmp;
      QVERIFY(tmp.isValid());
      LayerTreePanel panel(QgsProject::instance(), nullptr, nullptr);

      auto *vl = new QgsVectorLayer(QStringLiteral("Point?field=id:integer"),
                                    QStringLiteral("样式层"), QStringLiteral("memory"));
      QVERIFY(vl->isValid());
      QgsProject::instance()->addMapLayer(vl);

      const QString path = tmp.filePath(QStringLiteral("style.qml"));
      QString err;

      // 导出：文件落盘、非空、可解析（QSaveFile commit 后的成品）。
      QVERIFY2(LayerTreePanelStyleTestAccess::exportStyleTo(&panel, vl, path, &err),
               qPrintable(err));
      QFileInfo info(path);
      QVERIFY2(info.size() > 0, "exported qml must be non-empty");
      {
        QDomDocument doc;
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY2(doc.setContent(&f), "exported qml must parse");
      }

      // 导出失败（QGIS 侧错误串）：不写文件、不覆盖用户选定文件、错误上抛。
      // 预置哨兵内容——旧代码此刻会把空文档 Truncate 烙进去。
      FailingStyleExportLayer broken(QStringLiteral("/nonexistent/style-src.tif"),
                                     QStringLiteral("broken"));
      const QByteArray sentinel("SENTINEL-KEEP-ME");
      {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QVERIFY(f.write(sentinel) == sentinel.size());
      }
      QVERIFY(!LayerTreePanelStyleTestAccess::exportStyleTo(&panel, &broken, path, &err));
      QVERIFY2(!err.isEmpty(), "export failure must surface the QGIS error string");
      {
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), sentinel); // 用户文件原样保留
      }

      // 写入失败（目标路径不可开）：返回 false + 原因，不留半成品文件。
      const QString badPath = tmp.filePath(QStringLiteral("no_such_dir/style.qml"));
      QVERIFY(!LayerTreePanelStyleTestAccess::exportStyleTo(&panel, vl, badPath, &err));
      QVERIFY2(!err.isEmpty(), "write failure must report a reason");
      QVERIFY(!QFile::exists(badPath));

      // 成功导出覆盖既有文件（与上面的失败不覆盖对照）：哨兵被成品替换。
      QVERIFY2(LayerTreePanelStyleTestAccess::exportStyleTo(&panel, vl, path, &err),
               qPrintable(err));
      {
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(f.readAll() != sentinel);
      }

      // 加载失败（结构完好但不是 qgis 样式）：false + QGIS 错误串，
      // 且层渲染器不动（旧实现静默丢弃）。
      const QString notStyle = tmp.filePath(QStringLiteral("not-style.qml"));
      {
        QFile f(notStyle);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("<notqgis/>");
      }
      vl->setRenderer(new QgsNullSymbolRenderer());
      QCOMPARE(vl->renderer()->type(), QStringLiteral("nullSymbol"));
      QVERIFY(!LayerTreePanelStyleTestAccess::importStyleFrom(&panel, vl, notStyle, &err));
      QVERIFY2(!err.isEmpty(), "import failure must surface the QGIS error string");
      QCOMPARE(vl->renderer()->type(), QStringLiteral("nullSymbol"));

      // 加载失败（读取失败：文件不存在）。
      QVERIFY(!LayerTreePanelStyleTestAccess::importStyleFrom(
          &panel, vl, tmp.filePath(QStringLiteral("ghost.qml")), &err));
      QVERIFY2(!err.isEmpty(), "missing file must report a reason");

      // 往返：新源层（默认 singleSymbol）导出 → 第二层（null 渲染）导入 →
      // 渲染器恢复（vl 的渲染器已被上面的失败用例改成 null，不能复用）。
      QgsVectorLayer src(QStringLiteral("Point?field=id:integer"),
                         QStringLiteral("src"), QStringLiteral("memory"));
      const QString roundtripPath = tmp.filePath(QStringLiteral("roundtrip.qml"));
      QVERIFY2(LayerTreePanelStyleTestAccess::exportStyleTo(&panel, &src, roundtripPath, &err),
               qPrintable(err));
      QgsVectorLayer second(QStringLiteral("Point?field=id:integer"),
                            QStringLiteral("second"), QStringLiteral("memory"));
      second.setRenderer(new QgsNullSymbolRenderer());
      QCOMPARE(second.renderer()->type(), QStringLiteral("nullSymbol"));
      QVERIFY2(LayerTreePanelStyleTestAccess::importStyleFrom(&panel, &second, roundtripPath, &err),
               qPrintable(err));
      QCOMPARE(second.renderer()->type(), QStringLiteral("singleSymbol"));
    }

    // goal7 W2（F 域）：refreshIndicators 不得在逐节点循环里反复全表读
    // 声明清单（readAll × 节点数 × 每次刷新 × 每新增层两次信号）。
    // 探针：150 声明 + 150 层逐层挂载，量挂载总耗时与单次刷新耗时，
    // qInfo 落盘（selfcheck 口径：能看到数字）。宽门只挡灾难性回归；
    // 修复前后量级对比记 docs/progress/parallel-7/05-qgis-layers-layout.md。
    void indicatorRefreshScalesWithDeclarations()
    {
      QTemporaryDir tmp;
      QVERIFY(tmp.isValid());
      LayerManifest manifest(tmp.filePath(QStringLiteral("project.sqlite")));
      QVERIFY(manifest.open());
      QgisLayerService svc(nullptr, &manifest);
      const int N = 150;
      for (int i = 0; i < N; ++i)
        QVERIFY(svc.declare(decl(QStringLiteral("bulk.%1").arg(i),
                                 i % 2 == 0 ? QStringLiteral("T1")
                                            : QStringLiteral("T2"))));
      svc.setActiveHorizon(QStringLiteral("T1"));

      LayerTreePanel panel(QgsProject::instance(), nullptr, &svc);
      QgsLayerTree *root = panel.treeView()->layerTreeModel()->rootGroup();

      QElapsedTimer timer;
      timer.start();
      for (int i = 0; i < N; ++i)
      {
        const QString id = QStringLiteral("bulk.%1").arg(i);
        auto *layer = new QgsVectorLayer(QStringLiteral("Point?crs=EPSG:4326"),
                                         id, QStringLiteral("memory"));
        QVERIFY(layer->isValid());
        layer->setCustomProperty(QStringLiteral("paleoLayerId"), id);
        QgsProject::instance()->addMapLayer(layer);
      }
      const qint64 mountMs = timer.elapsed();

      timer.restart();
      panel.refreshIndicators();
      const qint64 refreshMs = timer.elapsed();
      qInfo("goal7 W2 probe: N=%d mountMs=%lld refreshMs=%lld", N,
            static_cast<long long>(mountMs), static_cast<long long>(refreshMs));
      QVERIFY2(mountMs < paleo::perfbudget::relaxedBudgetMs(30000),
               qPrintable(QStringLiteral("mountMs=%1").arg(mountMs)));
      QVERIFY2(refreshMs < paleo::perfbudget::relaxedBudgetMs(10000),
               qPrintable(QStringLiteral("refreshMs=%1").arg(refreshMs)));

      // 正确性守恒：激活层位 T1 的层无灰显 indicator，T2 的层有
      QgsMapLayer *lT1 = nullptr, *lT2 = nullptr;
      for (QgsMapLayer *l : QgsProject::instance()->mapLayers())
      {
        const QString pid = l->customProperty(QStringLiteral("paleoLayerId")).toString();
        if (pid == QLatin1String("bulk.0"))
          lT1 = l;
        if (pid == QLatin1String("bulk.1"))
          lT2 = l;
      }
      QVERIFY(lT1 && lT2);
      QgsLayerTreeLayer *nodeT1 = root->findLayer(lT1->id());
      QgsLayerTreeLayer *nodeT2 = root->findLayer(lT2->id());
      QVERIFY(nodeT1 && nodeT2);
      QCOMPARE(panel.treeView()->indicators(nodeT1).size(), 0);
      QCOMPARE(panel.treeView()->indicators(nodeT2).size(), 1);
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
