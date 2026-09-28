#include <QtTest>
#include <QHash>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <qgsapplication.h>
#include <qgslayertree.h>
#include <qgslayertreegroup.h>
#include <qgslayertreelayer.h>
#include <qgslayertreemodel.h>
#include <qgslayertreenode.h>
#include <qgslayoutitemmap.h>
#include <qgsmaplayer.h>
#include <qgsmapthemecollection.h>
#include <qgsprintlayout.h>
#include <qgsproject.h>
#include <qgsvectorlayer.h>

#include "../src/metadata/layermanifest.h"
#include "../src/qgis/layervocabulary.h"
#include "../src/qgis/qgislayerprofile.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprojectservice.h"

// wave/layer-platform 子任务 C：QgisLayerProfileService 契约测试——
// 页面档案表/档案应用摆树/主题生命周期/层位时序悬空修剪/布局钉主题/
// 主题随 .qgz 往返。状态只落在 QgsLayerTree + QgsMapThemeCollection，
// 服务自身不持任何可见性台账（PALEO_QGIS_PLAN §5）。

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
  d.styleRef = QStringLiteral("styles/%1.qml").arg(layerId);
  d.group = group;
  d.title = layerId;
  return d;
}

// 把（addMapLayer 落在树根的）图层节点挪进同名树组 —— instantiate() 只落树根
// 不建组，组匹配是声明驱动的，但组节点勾选态同步需要真实的组节点。
static void moveIntoGroup(QgsLayerTree *root, const QString &groupName,
                          const QString &layerId)
{
  QgsLayerTreeGroup *g = root->findGroup(groupName);
  if (!g)
    g = root->addGroup(groupName);
  if (QgsLayerTreeLayer *ln = root->findLayer(layerId))
  {
    if (root->takeChild(ln))
      g->addChildNode(ln);
  }
}

static QHash<QString, bool> visibilitySnapshot(QgsLayerTree *root)
{
  QHash<QString, bool> snap;
  const QList<QgsLayerTreeLayer *> layers = root->findLayers();
  for (QgsLayerTreeLayer *ln : layers)
  {
    if (QgsMapLayer *l = ln->layer())
      snap.insert(l->id(), ln->itemVisibilityChecked());
  }
  return snap;
}

// 单测 fixture：manifest（9 声明，canonical 七组各 1-2 层，两个层位）+
// QgisLayerService（null projectSvc → QgsProject::instance()）+ 图层树模型 +
// 两个无声明临时图层（树根 / 02_Prediction 组内各一）。
class ProfileFixture
{
  public:
    QTemporaryDir tmp;
    LayerManifest manifest;
    QgisLayerService layerSvc;
    QgsLayerTreeModel model;
    QgisLayerProfileService profile;
    QgsVectorLayer *tempAtRoot = nullptr;
    QgsVectorLayer *tempInPrediction = nullptr;

    ProfileFixture()
      : manifest(tmp.filePath(QStringLiteral("project.sqlite")))
      , layerSvc(nullptr, &manifest)
      , model(QgsProject::instance()->layerTreeRoot())
      , profile(QgsProject::instance())
    {
      QVERIFY(tmp.isValid());
      QString err;
      QVERIFY2(manifest.open(&err), qPrintable(err));
      profile.setLayerTreeModel(&model);
      profile.setLayerService(&layerSvc);

      const QVector<LayerDeclaration> decls = {
          decl(QStringLiteral("base.boundary"), QString(), QStringLiteral("01_Base")),
          decl(QStringLiteral("base.wells"), QString(), QStringLiteral("01_Base")),
          decl(QStringLiteral("pred.facies"), QStringLiteral("T1"), QStringLiteral("02_Prediction")),
          decl(QStringLiteral("con.prov"), QStringLiteral("T1"), QStringLiteral("03_Constraints")),
          decl(QStringLiteral("con.points"), QStringLiteral("T2"), QStringLiteral("03_Constraints")),
          decl(QStringLiteral("sf.thickness"), QString(), QStringLiteral("04_SingleFactor")),
          decl(QStringLiteral("pm.facies"), QString(), QStringLiteral("05_PaleoMap")),
          decl(QStringLiteral("ref.topo"), QString(), QStringLiteral("06_Reference")),
          decl(QStringLiteral("val.section"), QString(), QStringLiteral("07_Validation")),
      };
      for (const LayerDeclaration &d : decls)
        QVERIFY2(layerSvc.declare(d, &err), qPrintable(err));

      QgsLayerTree *root = QgsProject::instance()->layerTreeRoot();
      for (const LayerDeclaration &d : decls)
      {
        QgsMapLayer *l = layerSvc.instantiate(d.layerId, &err);
        QVERIFY2(l != nullptr, qPrintable(err));
        moveIntoGroup(root, d.group, l->id());
      }

      // 无声明临时图层（用户手加）：可见性必须不被档案摆树碰到。
      const QString src = fixtureGpkg() + QStringLiteral("|layername=basin");
      tempAtRoot = new QgsVectorLayer(src, QStringLiteral("temp.atRoot"), QStringLiteral("ogr"));
      tempInPrediction = new QgsVectorLayer(src, QStringLiteral("temp.inPred"), QStringLiteral("ogr"));
      QVERIFY(tempAtRoot->isValid());
      QVERIFY(tempInPrediction->isValid());
      QgsProject::instance()->addMapLayer(tempAtRoot);
      QgsProject::instance()->addMapLayer(tempInPrediction);
      moveIntoGroup(root, QStringLiteral("02_Prediction"), tempInPrediction->id());
    }

    QgsLayerTreeLayer *nodeFor(const QString &paleoLayerId)
    {
      QgsMapLayer *l = layerSvc.layer(paleoLayerId);
      if (!l)
        return nullptr;
      return QgsProject::instance()->layerTreeRoot()->findLayer(l->id());
    }

    bool layerChecked(const QString &paleoLayerId)
    {
      QgsLayerTreeLayer *ln = nodeFor(paleoLayerId);
      return ln && ln->itemVisibilityChecked();
    }

    void setLayerChecked(const QString &paleoLayerId, bool checked)
    {
      QgsLayerTreeLayer *ln = nodeFor(paleoLayerId);
      QVERIFY(ln != nullptr);
      ln->setItemVisibilityChecked(checked);
    }
};

class TestLayerPlatform : public QObject
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
      // 单例跨用例共享：清层、清树、清主题。
      QgsProject::instance()->clear();
    }

    // ---- 档案表：默认五页 + override 覆盖 ----
    void profileGroupTables()
    {
      QCOMPARE(QgisLayerProfileService::defaultProfileGroups(QStringLiteral("predict")),
               QStringList({QStringLiteral("01_Base"), QStringLiteral("02_Prediction")}));
      QCOMPARE(QgisLayerProfileService::defaultProfileGroups(QStringLiteral("constraint")),
               QStringList({QStringLiteral("01_Base"), QStringLiteral("03_Constraints"),
                            QStringLiteral("04_SingleFactor")}));
      QCOMPARE(QgisLayerProfileService::defaultProfileGroups(QStringLiteral("compose")),
               QStringList({QStringLiteral("01_Base"), QStringLiteral("03_Constraints"),
                            QStringLiteral("04_SingleFactor"), QStringLiteral("05_PaleoMap"),
                            QStringLiteral("06_Reference")}));
      QCOMPARE(QgisLayerProfileService::defaultProfileGroups(QStringLiteral("validate")),
               QStringList({QStringLiteral("01_Base"), QStringLiteral("07_Validation")}));
      QCOMPARE(QgisLayerProfileService::defaultProfileGroups(QStringLiteral("data")),
               QStringList());
      QCOMPARE(QgisLayerProfileService::defaultProfileGroups(QStringLiteral("bogus")),
               QStringList());
      QCOMPARE(QgisLayerProfileService::pageThemeName(QStringLiteral("compose")),
               QStringLiteral("page:compose"));

      QgisLayerProfileService prof(QgsProject::instance());
      QCOMPARE(prof.profileGroupsFor(QStringLiteral("validate")),
               QgisLayerProfileService::defaultProfileGroups(QStringLiteral("validate")));
      // override 覆盖默认表
      prof.setProfileGroupsOverride(QStringLiteral("validate"),
                                    {QStringLiteral("01_Base")});
      QCOMPARE(prof.profileGroupsFor(QStringLiteral("validate")),
               QStringList({QStringLiteral("01_Base")}));
      // override 扩展新页（manifest 允许扩展）
      prof.setProfileGroupsOverride(QStringLiteral("custom1"),
                                    {QStringLiteral("01_Base"), QStringLiteral("05_PaleoMap")});
      QCOMPARE(prof.profileGroupsFor(QStringLiteral("custom1")),
               QStringList({QStringLiteral("01_Base"), QStringLiteral("05_PaleoMap")}));
    }

    // ---- 主线7：档案应用/回滚——页面主题被删后 applyPageProfile 重建 ----
    void pageThemeRemovalAndRegeneration()
    {
      ProfileFixture fx;
      QVERIFY(fx.profile.applyPageProfile(QStringLiteral("compose")));
      QVERIFY(fx.profile.hasTheme(QStringLiteral("page:compose")));
      QVERIFY(fx.layerChecked(QStringLiteral("pm.facies")));

      // 用户在管理对话框删掉页面主题（回滚档案定制）
      QVERIFY(fx.profile.removeMapTheme(QStringLiteral("page:compose")));
      QVERIFY(!fx.profile.hasTheme(QStringLiteral("page:compose")));

      // 手改漂移（表外层勾上）后再应用：主题不存在 → 按档案表重新摆树建主题
      fx.setLayerChecked(QStringLiteral("val.section"), true);
      QVERIFY(fx.profile.applyPageProfile(QStringLiteral("compose")));
      QVERIFY(fx.profile.hasTheme(QStringLiteral("page:compose")));
      QVERIFY(fx.layerChecked(QStringLiteral("pm.facies")));
      QVERIFY(!fx.layerChecked(QStringLiteral("val.section"))); // 档案表语义恢复
    }

    // ---- 主线5：主题重命名（insert-then-remove 记录复制）----
    void renameThemeCopiesRecordAndRefuses()
    {
      ProfileFixture fx;
      QVERIFY(fx.profile.captureCurrentAsTheme(QStringLiteral("work")));
      QVERIFY(fx.profile.hasTheme(QStringLiteral("work")));

      QVERIFY(fx.profile.renameTheme(QStringLiteral("work"), QStringLiteral("work2")));
      QVERIFY(!fx.profile.hasTheme(QStringLiteral("work")));
      QVERIFY(fx.profile.hasTheme(QStringLiteral("work2")));
      // 改名后的主题记录完整可应用
      QVERIFY(fx.profile.applyTheme(QStringLiteral("work2")));

      // 拒绝路径：旧名不存在 / 新名已占用 / 空名 / 同名
      QVERIFY(!fx.profile.renameTheme(QStringLiteral("work"), QStringLiteral("x")));
      QVERIFY(fx.profile.captureCurrentAsTheme(QStringLiteral("occupied")));
      QVERIFY(!fx.profile.renameTheme(QStringLiteral("work2"), QStringLiteral("occupied")));
      QVERIFY(!fx.profile.renameTheme(QStringLiteral("work2"), QString()));
      QVERIFY(!fx.profile.renameTheme(QStringLiteral("work2"), QStringLiteral("work2")));
      QVERIFY(fx.profile.hasTheme(QStringLiteral("work2"))); // 拒绝不改状态
    }

    // ---- 主线1：词表单一权威（canonical 七组 + 旧名别名折算 + 组→页）----
    void vocabularySingleAuthority()
    {
      using namespace PaleoLayerVocabulary;
      QCOMPARE(canonicalGroups(),
               QStringList({QStringLiteral("01_Base"), QStringLiteral("02_Prediction"),
                            QStringLiteral("03_Constraints"), QStringLiteral("04_SingleFactor"),
                            QStringLiteral("05_PaleoMap"), QStringLiteral("06_Reference"),
                            QStringLiteral("07_Validation")}));
      QVERIFY(isCanonical(QStringLiteral("04_SingleFactor")));
      QVERIFY(!isCanonical(QStringLiteral("01_Prediction")));

      // 旧名 → canonical
      QCOMPARE(canonicalize(QStringLiteral("01_Prediction")), QStringLiteral("02_Prediction"));
      QCOMPARE(canonicalize(QStringLiteral("03_Predict")), QStringLiteral("02_Prediction"));
      QCOMPARE(canonicalize(QStringLiteral("02_Constraints")), QStringLiteral("03_Constraints"));
      QCOMPARE(canonicalize(QStringLiteral("03_Composite")), QStringLiteral("05_PaleoMap"));
      // canonical / 未知（00_Data、子组路径）原样返回
      QCOMPARE(canonicalize(QStringLiteral("02_Prediction")), QStringLiteral("02_Prediction"));
      QCOMPARE(canonicalize(QStringLiteral("00_Data")), QStringLiteral("00_Data"));
      QCOMPARE(canonicalize(QStringLiteral("04_SingleFactor/Contours")),
               QStringLiteral("04_SingleFactor/Contours"));

      // 家族 = canonical + 全部旧别名
      QCOMPARE(groupFamily(QStringLiteral("02_Prediction")),
               QStringList({QStringLiteral("02_Prediction"), QStringLiteral("01_Prediction"),
                            QStringLiteral("03_Predict")}));

      // 档案成员判定：旧名声明落在 canonical 档案表内
      const QStringList predict = profileGroupsForPage(QStringLiteral("predict"));
      QVERIFY(profileContains(predict, QStringLiteral("01_Prediction")));
      QVERIFY(profileContains(predict, QStringLiteral("02_Prediction")));
      QVERIFY(!profileContains(predict, QStringLiteral("03_Constraints")));

      // 组→页：旧名同样能跳页；01_Base/未知组带 reason
      QString reason;
      QCOMPARE(pageForGroup(QStringLiteral("03_Predict"), &reason), QStringLiteral("predict"));
      QCOMPARE(pageForGroup(QStringLiteral("03_Composite"), &reason), QStringLiteral("compose"));
      QCOMPARE(pageForGroup(QStringLiteral("02_Constraints"), &reason), QStringLiteral("constraint"));
      QCOMPARE(pageForGroup(QStringLiteral("01_Base"), &reason), QString());
      QVERIFY(!reason.isEmpty());
      QCOMPARE(pageForGroup(QStringLiteral("00_Data"), &reason), QString());
      QVERIFY(reason.contains(QStringLiteral("00_Data")));
    }

    // ---- 主线1现象级：档案应用不再隐藏旧组名产层（旧 .qgz 兼容）----
    void legacyGroupAliasKeepsOldPredictionVisible()
    {
      ProfileFixture fx;
      QString err;
      // 旧 .qgz 的声明组名是历史值（workflows.cpp 产点不改，消费面折算）
      QVERIFY(fx.layerSvc.declare(
          decl(QStringLiteral("pred.legacy"), QStringLiteral("T1"), QStringLiteral("01_Prediction")), &err));
      QVERIFY(fx.layerSvc.declare(
          decl(QStringLiteral("con.legacy"), QStringLiteral("T1"), QStringLiteral("02_Constraints")), &err));
      QVERIFY(fx.layerSvc.declare(
          decl(QStringLiteral("pm.legacy"), QStringLiteral("T1"), QStringLiteral("03_Composite")), &err));
      QgsLayerTree *root = QgsProject::instance()->layerTreeRoot();
      for (const QString id : {QStringLiteral("pred.legacy"), QStringLiteral("con.legacy"),
                               QStringLiteral("pm.legacy")})
      {
        QgsMapLayer *l = fx.layerSvc.instantiate(id, &err);
        QVERIFY2(l != nullptr, qPrintable(err));
        moveIntoGroup(root, QStringLiteral("01_Prediction"), l->id()); // 旧组名树节点
      }

      QVERIFY(fx.profile.applyPageProfile(QStringLiteral("predict")));
      QVERIFY(fx.layerChecked(QStringLiteral("pred.legacy")));  // 旧 01_Prediction 产层不再被表外隐藏
      QVERIFY(fx.layerChecked(QStringLiteral("pred.facies")));  // canonical 产层照常
      QVERIFY(!fx.layerChecked(QStringLiteral("con.legacy"))); // 折算 03_Constraints → predict 表外

      QVERIFY(fx.profile.applyPageProfile(QStringLiteral("constraint")));
      QVERIFY(fx.layerChecked(QStringLiteral("con.legacy")));  // 旧 02_Constraints → 03_Constraints 入表
      QVERIFY(!fx.layerChecked(QStringLiteral("pred.legacy")));

      QVERIFY(fx.profile.applyPageProfile(QStringLiteral("compose")));
      QVERIFY(fx.layerChecked(QStringLiteral("pm.legacy")));   // 旧 03_Composite → 05_PaleoMap 入表
    }

    // ---- 未接线防御：null project / null model / 未知 pageId ----
    void unwiredServiceDefends()
    {
      QgisLayerProfileService dead(nullptr);
      QVERIFY(!dead.applyPageProfile(QStringLiteral("compose")));
      QVERIFY(!dead.applyCurrentPageProfile());
      QVERIFY(!dead.hasTheme(QStringLiteral("page:compose")));
      QVERIFY(dead.themes().isEmpty());
      QVERIFY(!dead.captureCurrentAsTheme(QStringLiteral("work")));
      QVERIFY(!dead.applyTheme(QStringLiteral("work")));
      QVERIFY(!dead.removeMapTheme(QStringLiteral("work")));
      dead.setLayoutMapTheme(nullptr, QString()); // 判空：不崩
      QVERIFY(dead.layerTreeModel() == nullptr);
      QVERIFY(dead.layerService() == nullptr);

      // 有 project、无 model：apply 仍失败
      QTemporaryDir tmp;
      QVERIFY(tmp.isValid());
      LayerManifest manifest(tmp.filePath(QStringLiteral("project.sqlite")));
      QVERIFY(manifest.open());
      QgisLayerService layerSvc(nullptr, &manifest);
      QgisLayerProfileService prof(QgsProject::instance());
      prof.setLayerService(&layerSvc);
      QVERIFY(!prof.applyPageProfile(QStringLiteral("compose")));

      // 接好 model 后：未知 pageId → false
      QgsLayerTreeModel model(QgsProject::instance()->layerTreeRoot());
      prof.setLayerTreeModel(&model);
      QVERIFY(!prof.applyPageProfile(QStringLiteral("bogus")));
    }

    // ---- 无 layerService：无从回查声明 → 摆树不动任何可见性，档案仍可建立 ----
    void stagingWithoutLayerServiceLeavesLayersUntouched()
    {
      ProfileFixture fx;
      QgsLayerTree *root = QgsProject::instance()->layerTreeRoot();
      const QHash<QString, bool> before = visibilitySnapshot(root);
      QVERIFY(!before.isEmpty());

      QgisLayerProfileService prof(QgsProject::instance());
      QgsLayerTreeModel model(root);
      prof.setLayerTreeModel(&model); // 故意不注入 layerService
      QVERIFY(prof.applyPageProfile(QStringLiteral("compose")));
      QCOMPARE(visibilitySnapshot(root), before); // 声明驱动匹配：无回查 = 不动
      QVERIFY(prof.hasTheme(QStringLiteral("page:compose")));
    }

    // ---- 档案应用：摆树 + 建主题 + 信号参数 ----
    void applyPageProfileStagesTreeAndTheme()
    {
      ProfileFixture fx;
      QSignalSpy appliedSpy(&fx.profile, &QgisLayerProfileService::profileApplied);

      QVERIFY(fx.profile.applyPageProfile(QStringLiteral("compose")));
      QCOMPARE(fx.profile.currentPageProfile(), QStringLiteral("compose"));
      QCOMPARE(appliedSpy.size(), 1);
      QCOMPARE(appliedSpy.at(0).at(0).toString(), QStringLiteral("compose"));
      QCOMPARE(appliedSpy.at(0).at(1).toString(), QStringLiteral("page:compose"));

      QgsMapThemeCollection *collection = QgsProject::instance()->mapThemeCollection();
      QVERIFY(collection->hasMapTheme(QStringLiteral("page:compose")));
      QVERIFY(fx.profile.hasTheme(QStringLiteral("page:compose")));
      QVERIFY(fx.profile.themes().contains(QStringLiteral("page:compose")));

      // 档案表内组 → checked
      QVERIFY(fx.layerChecked(QStringLiteral("base.boundary")));
      QVERIFY(fx.layerChecked(QStringLiteral("base.wells")));
      QVERIFY(fx.layerChecked(QStringLiteral("con.prov")));
      QVERIFY(fx.layerChecked(QStringLiteral("con.points")));
      QVERIFY(fx.layerChecked(QStringLiteral("sf.thickness")));
      QVERIFY(fx.layerChecked(QStringLiteral("pm.facies")));
      QVERIFY(fx.layerChecked(QStringLiteral("ref.topo")));
      // 档案表外组 → unchecked
      QVERIFY(!fx.layerChecked(QStringLiteral("pred.facies")));
      QVERIFY(!fx.layerChecked(QStringLiteral("val.section")));

      // 无声明临时图层：可见性不动（默认 checked）
      QgsLayerTree *root = QgsProject::instance()->layerTreeRoot();
      QgsLayerTreeLayer *atRoot = root->findLayer(fx.tempAtRoot->id());
      QVERIFY(atRoot && atRoot->itemVisibilityChecked());
      QgsLayerTreeLayer *inPred = root->findLayer(fx.tempInPrediction->id());
      QVERIFY(inPred && inPred->itemVisibilityChecked());

      // 同名树组节点按组内结果同步勾选态
      QVERIFY(root->findGroup(QStringLiteral("01_Base"))->itemVisibilityChecked());
      QVERIFY(!root->findGroup(QStringLiteral("02_Prediction"))->itemVisibilityChecked());
      QVERIFY(root->findGroup(QStringLiteral("03_Constraints"))->itemVisibilityChecked());
      QVERIFY(!root->findGroup(QStringLiteral("07_Validation"))->itemVisibilityChecked());
    }

    // ---- 换页翻转 + 手改后显式重放覆盖（头注释语义）----
    void pageSwitchFlipsAndExplicitReplayOverrides()
    {
      ProfileFixture fx;
      QVERIFY(fx.profile.applyPageProfile(QStringLiteral("compose")));

      QVERIFY(fx.profile.applyPageProfile(QStringLiteral("validate")));
      QCOMPARE(fx.profile.currentPageProfile(), QStringLiteral("validate"));
      // compose 与 validate 的差异组翻转
      QVERIFY(fx.layerChecked(QStringLiteral("base.boundary")));
      QVERIFY(!fx.layerChecked(QStringLiteral("con.prov")));   // 03_Constraints 出表
      QVERIFY(!fx.layerChecked(QStringLiteral("pm.facies")));  // 05_PaleoMap 出表
      QVERIFY(fx.layerChecked(QStringLiteral("val.section"))); // 07_Validation 入表
      QVERIFY(fx.profile.hasTheme(QStringLiteral("page:validate")));

      // 手改可见性（表外层勾上、表内层取消）——档案不自动覆盖
      fx.setLayerChecked(QStringLiteral("pm.facies"), true);
      fx.setLayerChecked(QStringLiteral("base.wells"), false);
      QVERIFY(fx.layerChecked(QStringLiteral("pm.facies")));
      QVERIFY(!fx.layerChecked(QStringLiteral("base.wells")));

      // 显式再调 applyPageProfile：走 applyTheme 路径恢复主题态
      QVERIFY(fx.profile.applyPageProfile(QStringLiteral("validate")));
      QVERIFY(!fx.layerChecked(QStringLiteral("pm.facies")));
      QVERIFY(fx.layerChecked(QStringLiteral("base.wells")));
    }

    // ---- data 页：不操作画布、不发信号、重放自洽 ----
    void dataPageLeavesTreeAlone()
    {
      ProfileFixture fx;
      QgsLayerTree *root = QgsProject::instance()->layerTreeRoot();
      const QHash<QString, bool> before = visibilitySnapshot(root);
      QVERIFY(!before.isEmpty());

      QSignalSpy appliedSpy(&fx.profile, &QgisLayerProfileService::profileApplied);
      QVERIFY(fx.profile.applyPageProfile(QStringLiteral("data")));
      QCOMPARE(fx.profile.currentPageProfile(), QStringLiteral("data"));
      QCOMPARE(visibilitySnapshot(root), before);
      QVERIFY(!fx.profile.hasTheme(QStringLiteral("page:data")));
      QCOMPARE(appliedSpy.size(), 0);

      // data 重放：无操作、返回 true（语义自洽）
      QVERIFY(fx.profile.applyCurrentPageProfile());
      QCOMPARE(visibilitySnapshot(root), before);

      // 尚未应用任何页时重放 → false
      ProfileFixture fx2;
      QVERIFY(!fx2.profile.applyCurrentPageProfile());
    }

    // ---- predict 档案并入当前层位约束图层 ----
    void predictMergesActiveHorizonConstraints()
    {
      ProfileFixture fx;
      QgsLayerTree *root = QgsProject::instance()->layerTreeRoot();

      fx.layerSvc.setActiveHorizon(QStringLiteral("T1")); // 释放 T2 实例
      QVERIFY(fx.layerSvc.isInstantiated(QStringLiteral("con.prov")));
      QVERIFY(!fx.layerSvc.isInstantiated(QStringLiteral("con.points")));
      // 显式补一个 T2 约束层回树里（按需实例化语义）
      QVERIFY(fx.layerSvc.instantiate(QStringLiteral("con.points")));
      moveIntoGroup(root, QStringLiteral("03_Constraints"),
                    fx.layerSvc.layer(QStringLiteral("con.points"))->id());

      QVERIFY(fx.profile.applyPageProfile(QStringLiteral("predict")));
      QVERIFY(fx.layerChecked(QStringLiteral("base.boundary")));  // 01_Base
      QVERIFY(fx.layerChecked(QStringLiteral("pred.facies")));    // 02_Prediction
      QVERIFY(fx.layerChecked(QStringLiteral("con.prov")));       // T1 == activeHorizon → 并入
      QVERIFY(!fx.layerChecked(QStringLiteral("con.points")));    // T2 != activeHorizon
      QVERIFY(!fx.layerChecked(QStringLiteral("sf.thickness")));
      QVERIFY(!fx.layerChecked(QStringLiteral("val.section")));
      QVERIFY(fx.profile.hasTheme(QStringLiteral("page:predict")));

      // 并入的约束层要在主题记录里（可见层列表包含它）
      QgsMapLayer *conProv = fx.layerSvc.layer(QStringLiteral("con.prov"));
      QVERIFY(QgsProject::instance()->mapThemeCollection()
                  ->mapThemeVisibleLayerIds(QStringLiteral("page:predict"))
                  .contains(conProv->id()));
    }

    // ---- 主题生命周期：capture/apply/remove + mapThemesChanged 转发 ----
    void themeLifecycle()
    {
      ProfileFixture fx;
      QVERIFY(fx.profile.applyPageProfile(QStringLiteral("compose")));
      QCOMPARE(fx.profile.themes(),
               QStringList({QStringLiteral("page:compose")}));

      QSignalSpy changedSpy(&fx.profile, &QgisLayerProfileService::mapThemesChanged);
      QVERIFY(!fx.profile.captureCurrentAsTheme(QString())); // 空名失败
      QCOMPARE(changedSpy.size(), 0);
      QVERIFY(fx.profile.captureCurrentAsTheme(QStringLiteral("work")));
      QVERIFY(fx.profile.themes().contains(QStringLiteral("work")));
      QCOMPARE(changedSpy.size(), 1); // collection → service 转发

      // 手动隐藏一层 → applyTheme 恢复主题态
      fx.setLayerChecked(QStringLiteral("base.boundary"), false);
      fx.setLayerChecked(QStringLiteral("pm.facies"), false);
      QVERIFY(fx.profile.applyTheme(QStringLiteral("work")));
      QVERIFY(fx.layerChecked(QStringLiteral("base.boundary")));
      QVERIFY(fx.layerChecked(QStringLiteral("pm.facies")));

      // 覆盖保存：同名 capture 走 update，主题数不变
      fx.setLayerChecked(QStringLiteral("ref.topo"), false);
      QVERIFY(fx.profile.captureCurrentAsTheme(QStringLiteral("work")));
      QCOMPARE(fx.profile.themes().size(), 2);
      fx.setLayerChecked(QStringLiteral("ref.topo"), true);
      QVERIFY(fx.profile.applyTheme(QStringLiteral("work")));
      QVERIFY(!fx.layerChecked(QStringLiteral("ref.topo"))); // 覆盖后的记录生效

      // 移除
      QVERIFY(fx.profile.removeMapTheme(QStringLiteral("work")));
      QVERIFY(!fx.profile.themes().contains(QStringLiteral("work")));
      // insert + applyTheme(QGIS 内部会重发一次) + update + applyTheme + remove
      QCOMPARE(changedSpy.size(), 5);
      QVERIFY(!fx.profile.applyTheme(QStringLiteral("work"))); // 已无此主题
      QVERIFY(!fx.profile.removeMapTheme(QStringLiteral("work"))); // 重复移除失败
    }

    // ---- 层位时序：先换层位再重放 —— 无崩溃、无悬空记录（修剪生效）----
    void horizonSwitchPrunesDanglingRecords()
    {
      ProfileFixture fx;
      QgsMapThemeCollection *collection = QgsProject::instance()->mapThemeCollection();

      fx.layerSvc.setActiveHorizon(QStringLiteral("T1")); // T2 约束层释放
      QVERIFY(fx.profile.applyPageProfile(QStringLiteral("predict")));
      const QString t1LayerId = fx.layerSvc.layer(QStringLiteral("con.prov"))->id();
      QVERIFY(collection->mapThemeVisibleLayerIds(QStringLiteral("page:predict"))
                  .contains(t1LayerId));

      // 换层位：T1 实例（con.prov + pred.facies）被释放，T2 实例落树根
      fx.layerSvc.setActiveHorizon(QStringLiteral("T2"));
      QVERIFY(!fx.layerSvc.isInstantiated(QStringLiteral("con.prov")));
      QVERIFY(fx.layerSvc.isInstantiated(QStringLiteral("con.points")));

      // 重放当前页档案：主题里已不存在的层先剔除再应用
      QVERIFY(fx.profile.applyCurrentPageProfile());
      QCOMPARE(fx.profile.currentPageProfile(), QStringLiteral("predict"));

      const QList<QgsMapThemeCollection::MapThemeLayerRecord> recs =
          collection->mapThemeState(QStringLiteral("page:predict")).layerRecords();
      QVERIFY(!recs.isEmpty());
      for (const QgsMapThemeCollection::MapThemeLayerRecord &r : recs)
        QVERIFY2(r.layer() != nullptr, "修剪后主题记录不得含悬空图层");
      const QStringList visibleIds =
          collection->mapThemeVisibleLayerIds(QStringLiteral("page:predict"));
      QVERIFY(!visibleIds.contains(t1LayerId));
      QVERIFY(visibleIds.contains(
          fx.layerSvc.layer(QStringLiteral("base.boundary"))->id())); // 层位无关层保留

      // 树态：基础层仍可见，新层位的 T2 约束层不在 predict 主题里 → 隐藏
      QVERIFY(fx.layerChecked(QStringLiteral("base.boundary")));
      QVERIFY(!fx.layerChecked(QStringLiteral("con.points")));
    }

    // ---- 主题随 .qgz 往返（QgisProjectService 自有工程，非单例）----
    void themesSurviveQgzRoundtrip()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      const QString qgzPath = dir.filePath(QStringLiteral("proj.qgz"));
      QString workLayerId;

      {
        QgisProjectService svc;
        QVERIFY2(svc.createProject(qgzPath), qPrintable(svc.lastErrors().join(';')));

        LayerManifest manifest(dir.filePath(QStringLiteral("project.sqlite")));
        QString err;
        QVERIFY2(manifest.open(&err), qPrintable(err));
        QgisLayerService layerSvc(&svc, &manifest);
        QVERIFY2(layerSvc.declare(
                     decl(QStringLiteral("base.boundary"), QString(),
                          QStringLiteral("01_Base")), &err),
                 qPrintable(err));
        // 档案只摆可见性、不实例化图层（实例化归 QgisLayerService 层位切换管）
        QVERIFY2(layerSvc.instantiate(QStringLiteral("base.boundary"), &err) != nullptr,
                 qPrintable(err));

        QgsLayerTreeModel model(svc.project()->layerTreeRoot());
        QgisLayerProfileService prof(svc.project());
        prof.setLayerTreeModel(&model);
        prof.setLayerService(&layerSvc);

        QVERIFY(prof.applyPageProfile(QStringLiteral("compose")));
        QVERIFY(prof.captureCurrentAsTheme(QStringLiteral("work")));
        workLayerId = layerSvc.layer(QStringLiteral("base.boundary"))->id();
        QVERIFY2(svc.writeProject(), qPrintable(svc.lastErrors().join(';')));
      } // svc/prof 连同自有 QgsProject 析构 —— 持久化只在盘上

      QgisProjectService svc2;
      QVERIFY2(svc2.openProject(qgzPath), qPrintable(svc2.lastErrors().join(';')));
      QgisLayerProfileService prof2(svc2.project());
      QVERIFY(prof2.hasTheme(QStringLiteral("page:compose")));
      QVERIFY(prof2.hasTheme(QStringLiteral("work")));
      QVERIFY(prof2.themes().contains(QStringLiteral("page:compose")));
      QVERIFY(prof2.themes().contains(QStringLiteral("work")));

      // 记录经 layerId 重解析到重开工程里的图层，且可再次应用
      const QList<QgsMapThemeCollection::MapThemeLayerRecord> recs =
          svc2.project()->mapThemeCollection()
              ->mapThemeState(QStringLiteral("work")).layerRecords();
      QVERIFY(!recs.isEmpty());
      QCOMPARE(recs.at(0).layer()->id(), workLayerId);

      QgsLayerTreeModel model2(svc2.project()->layerTreeRoot());
      prof2.setLayerTreeModel(&model2);
      QVERIFY(prof2.applyTheme(QStringLiteral("work")));
      QVERIFY(svc2.project()->layerTreeRoot()
                  ->findLayer(workLayerId)->itemVisibilityChecked());
    }

    // ---- 布局地图项钉可见性主题 ----
    void layoutMapThemePinning()
    {
      QgsProject project;
      QgsPrintLayout layout(&project);
      auto *mapItem = new QgsLayoutItemMap(&layout);
      layout.addLayoutItem(mapItem);

      QgisLayerProfileService prof(&project);
      prof.setLayoutMapTheme(nullptr, QStringLiteral("page:compose")); // 判空：不崩
      QCOMPARE(mapItem->followVisibilityPreset(), false);

      prof.setLayoutMapTheme(mapItem, QStringLiteral("page:compose"));
      QCOMPARE(mapItem->followVisibilityPreset(), true);
      QCOMPARE(mapItem->followVisibilityPresetName(), QStringLiteral("page:compose"));

      // 空主题名 → 解钉
      prof.setLayoutMapTheme(mapItem, QString());
      QCOMPARE(mapItem->followVisibilityPreset(), false);
    }
};

int main(int argc, char *argv[])
{
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")), true); // distro install
  app.initQgis();
  TestLayerPlatform tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_layerplatform.moc"
