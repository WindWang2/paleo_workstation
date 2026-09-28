#include <QtTest>
#include <QComboBox>
#include <QDialog>
#include <QGuiApplication>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QToolButton>

#include <qgsapplication.h>
#include <qgslayertree.h>
#include <qgslayertreelayer.h>
#include <qgslayertreemodel.h>
#include <qgsmaplayer.h>
#include <qgsproject.h>

#include "../src/metadata/layermanifest.h"
#include "../src/qgis/qgislayerprofile.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/ui/layers/layerprofilebar.h"

// wave/layer-platform 子任务 D：LayerProfileBar 契约测试——
// 主题下拉实时刷新/页面档案指示与中文映射/程序化 setCurrentPage 不发
// themeSelected（用户激活路径才发）/保存主题 offscreen no-op/管理对话框
// 列表-应用-删除（重命名 V1 暂未支持）。bar 属视图层——只渲染 + 调
// QgisLayerProfileService，不直碰 QgsProject/QgsMapThemeCollection。

static QString fixtureGpkg()
{
#ifdef FIXTURE_GPKG
  return QStringLiteral(FIXTURE_GPKG);
#else
  const QString testsDir = QFileInfo(QString::fromUtf8(__FILE__)).absolutePath();
  return QDir(testsDir).absoluteFilePath(QStringLiteral("../testdata/fixture.gpkg"));
#endif
}

static LayerDeclaration decl(const QString &layerId, const QString &group)
{
  LayerDeclaration d;
  d.layerId = layerId;
  d.horizon = QString(); // 本测试不动层位：全部层位无关
  d.type = QStringLiteral("vector");
  d.source = fixtureGpkg() + QStringLiteral("|layername=basin");
  d.styleRef = QStringLiteral("styles/%1.qml").arg(layerId);
  d.group = group;
  d.title = layerId;
  return d;
}

// 单测 fixture（照 tst_layerplatform ProfileFixture 思路精简）：manifest +
// QgisLayerService（null projectSvc → QgsProject::instance()）+ 图层树模型 +
// 档案服务，四个声明层覆盖 compose 档案的表内/表外组。
class BarFixture
{
  public:
    QTemporaryDir tmp;
    LayerManifest manifest;
    QgisLayerService layerSvc;
    QgsLayerTreeModel model;
    QgisLayerProfileService profile;

    BarFixture()
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
          decl(QStringLiteral("base.boundary"), QStringLiteral("01_Base")),
          decl(QStringLiteral("con.prov"), QStringLiteral("03_Constraints")),
          decl(QStringLiteral("pm.facies"), QStringLiteral("05_PaleoMap")),
          decl(QStringLiteral("val.section"), QStringLiteral("07_Validation")),
      };
      for (const LayerDeclaration &d : decls)
      {
        QVERIFY2(layerSvc.declare(d, &err), qPrintable(err));
        QVERIFY2(layerSvc.instantiate(d.layerId, &err) != nullptr, qPrintable(err));
      }
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

class TestLayerProfileBar : public QObject
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

    // ---- ctor 空工程：下拉空、label 空、objectName 契约 ----
    void ctorWithEmptyProject()
    {
      QgisLayerProfileService dead(nullptr); // 空工程（未注入 QgsProject）
      LayerProfileBar bar(&dead);

      QComboBox *combo = bar.themeCombo();
      QVERIFY(combo != nullptr);
      QCOMPARE(combo->objectName(), QStringLiteral("layerThemeCombo"));
      QCOMPARE(combo->count(), 0);

      QLabel *label = bar.findChild<QLabel *>(QStringLiteral("layerPageProfileLabel"));
      QVERIFY(label != nullptr);
      QCOMPARE(label->text(), QString());
      QCOMPARE(bar.currentPage(), QString());

      QVERIFY(bar.findChild<QToolButton *>(QStringLiteral("layerSaveThemeButton")) != nullptr);
      QVERIFY(bar.findChild<QToolButton *>(QStringLiteral("layerManageThemesButton")) != nullptr);
    }

    // ---- null service：纯渲染防御，不崩 ----
    void ctorWithNullService()
    {
      LayerProfileBar bar(nullptr);
      QCOMPARE(bar.themeCombo()->count(), 0);
      bar.setCurrentPage(QStringLiteral("compose"));
      QCOMPARE(bar.currentPage(), QStringLiteral("compose"));
      QLabel *label = bar.findChild<QLabel *>(QStringLiteral("layerPageProfileLabel"));
      QVERIFY(label != nullptr);
      QCOMPARE(label->text(), QStringLiteral("页面档案：智能编图"));
    }

    // ---- 页档案指示：五页中文映射 + 未知 id 原样 + 空则空文案 ----
    void pageLabelMapping()
    {
      BarFixture fx;
      LayerProfileBar bar(&fx.profile);
      QLabel *label = bar.findChild<QLabel *>(QStringLiteral("layerPageProfileLabel"));
      QVERIFY(label != nullptr);

      const QHash<QString, QString> cn = {
          {QStringLiteral("data"), QStringLiteral("数据管理")},
          {QStringLiteral("predict"), QStringLiteral("预测编图")},
          {QStringLiteral("constraint"), QStringLiteral("单因素图")},
          {QStringLiteral("compose"), QStringLiteral("智能编图")},
          {QStringLiteral("validate"), QStringLiteral("验证")},
      };
      for (auto it = cn.cbegin(); it != cn.cend(); ++it)
      {
        bar.setCurrentPage(it.key());
        QCOMPARE(bar.currentPage(), it.key());
        QCOMPARE(label->text(), QStringLiteral("页面档案：") + it.value());
      }

      bar.setCurrentPage(QStringLiteral("bogus")); // 未知 id：原样
      QCOMPARE(label->text(), QStringLiteral("页面档案：bogus"));
      bar.setCurrentPage(QString()); // 空：空文案
      QCOMPARE(label->text(), QString());

      // 尚无 page:compose 主题时 setCurrentPage 不往下拉里造项
      QCOMPARE(bar.themeCombo()->count(), 0);
    }

    // ---- service 侧 capture → mapThemesChanged → 下拉刷新（保住选中）----
    void captureRefillsComboViaSignal()
    {
      BarFixture fx;
      LayerProfileBar bar(&fx.profile);
      QSignalSpy selSpy(&bar, &LayerProfileBar::themeSelected);
      QCOMPARE(bar.themeCombo()->count(), 0);

      QVERIFY(fx.profile.captureCurrentAsTheme(QStringLiteral("工作主题")));
      QCOMPARE(bar.themeCombo()->count(), 1);
      QCOMPARE(bar.themeCombo()->itemText(0), QStringLiteral("工作主题"));
      QCOMPARE(bar.themeCombo()->itemData(0).toString(), QStringLiteral("工作主题"));

      // 选中「工作主题」后再加主题：刷新保住当前选中（若仍在列表）
      bar.themeCombo()->setCurrentIndex(0);
      QVERIFY(fx.profile.captureCurrentAsTheme(QStringLiteral("另一个主题")));
      QCOMPARE(bar.themeCombo()->count(), 2);
      QCOMPARE(bar.themeCombo()->currentData().toString(), QStringLiteral("工作主题"));

      QCOMPARE(selSpy.size(), 0); // 全程程序化，无用户激活
    }

    void missingPageThemeClearsPreviousSelection()
    {
      BarFixture fx;
      LayerProfileBar bar(&fx.profile);
      QSignalSpy selected(&bar, &LayerProfileBar::themeSelected);
      QVERIFY(fx.profile.applyPageProfile(QStringLiteral("compose")));
      bar.setCurrentPage(QStringLiteral("compose"));
      QCOMPARE(bar.themeCombo()->currentData().toString(), QStringLiteral("page:compose"));
      bar.setCurrentPage(QStringLiteral("validate")); // no theme exists for this page yet
      QCOMPARE(bar.themeCombo()->currentIndex(), -1);
      QCOMPARE(bar.themeCombo()->count(), 1);
      QCOMPARE(selected.count(), 0);
    }

    // ---- page 主题：applyPageProfile + setCurrentPage → label/下拉选中，不发信号 ----
    void pageThemeSelectionIsSilent()
    {
      BarFixture fx;
      LayerProfileBar bar(&fx.profile);
      QSignalSpy selSpy(&bar, &LayerProfileBar::themeSelected);

      QVERIFY(fx.profile.applyPageProfile(QStringLiteral("compose")));
      // collection→service→bar 转发：下拉出现 page:compose，显示「页面·智能编图」
      QCOMPARE(bar.themeCombo()->count(), 1);
      QCOMPARE(bar.themeCombo()->itemText(0), QStringLiteral("页面·智能编图"));
      QCOMPARE(bar.themeCombo()->itemData(0).toString(), QStringLiteral("page:compose"));

      bar.setCurrentPage(QStringLiteral("compose"));
      QLabel *label = bar.findChild<QLabel *>(QStringLiteral("layerPageProfileLabel"));
      QVERIFY(label != nullptr);
      QCOMPARE(label->text(), QStringLiteral("页面档案：智能编图"));
      QCOMPARE(bar.themeCombo()->currentData().toString(), QStringLiteral("page:compose"));
      QCOMPARE(bar.themeCombo()->currentText(), QStringLiteral("页面·智能编图"));

      QCOMPARE(selSpy.size(), 0); // 程序化 setCurrentPage 不发 themeSelected
    }

    // ---- 用户选择路径：applyTheme 生效 + themeSelected 发射 ----
    void userSelectionAppliesAndEmits()
    {
      BarFixture fx;
      LayerProfileBar bar(&fx.profile);
      QSignalSpy selSpy(&bar, &LayerProfileBar::themeSelected);

      QVERIFY(fx.profile.applyPageProfile(QStringLiteral("compose")));
      // 手改可见性后定格为用户主题：pm.facies 隐藏、val.section 可见
      fx.setLayerChecked(QStringLiteral("pm.facies"), false);
      fx.setLayerChecked(QStringLiteral("val.section"), true);
      QVERIFY(fx.profile.captureCurrentAsTheme(QStringLiteral("工作主题")));

      // 程序化 setCurrentIndex 本身不发 activated；用户路径 = 激活槽
      const int idx = bar.themeCombo()->findData(QStringLiteral("page:compose"));
      QVERIFY(idx >= 0);
      bar.themeCombo()->setCurrentIndex(idx);
      QVERIFY(QMetaObject::invokeMethod(&bar, "onComboActivated", Q_ARG(int, idx)));
      QCOMPARE(selSpy.size(), 1);
      QCOMPARE(selSpy.at(0).at(0).toString(), QStringLiteral("page:compose"));

      // service->applyTheme 已作用到树：恢复 page:compose 主题态
      QVERIFY(fx.layerChecked(QStringLiteral("pm.facies")));
      QVERIFY(!fx.layerChecked(QStringLiteral("val.section")));
    }

    // ---- 管理对话框：列表=themes()、「页面·」显示名、删除（含 page:*）、应用 ----
    void manageDialogListsAppliesAndDeletes()
    {
      BarFixture fx;
      LayerProfileBar bar(&fx.profile);
      QVERIFY(fx.profile.applyPageProfile(QStringLiteral("compose")));
      QVERIFY(fx.profile.captureCurrentAsTheme(QStringLiteral("工作主题")));
      const QStringList themesBefore = fx.profile.themes();
      QCOMPARE(themesBefore.size(), 2);

      QDialog *dlg = bar.buildManageDialog(); // bar 自持（父子所有权）
      QVERIFY(dlg != nullptr);
      QCOMPARE(dlg->objectName(), QStringLiteral("layerManageThemesDialog"));

      QListWidget *list = dlg->findChild<QListWidget *>(QStringLiteral("layerManageThemeList"));
      QVERIFY(list != nullptr);
      QCOMPARE(list->count(), themesBefore.size());
      for (int i = 0; i < list->count(); ++i)
      {
        QCOMPARE(list->item(i)->data(Qt::UserRole).toString(), themesBefore.at(i));
        if (themesBefore.at(i) == QLatin1String("page:compose"))
          QCOMPARE(list->item(i)->text(), QStringLiteral("页面·智能编图"));
      }

      QPushButton *applyBtn =
          dlg->findChild<QPushButton *>(QStringLiteral("layerManageApplyThemeButton"));
      QVERIFY(applyBtn != nullptr);
      QPushButton *removeBtn =
          dlg->findChild<QPushButton *>(QStringLiteral("layerManageDeleteThemeButton"));
      QVERIFY(removeBtn != nullptr);
      // 主线5：重命名按钮在场 + page:* 约定名提示
      QVERIFY(dlg->findChild<QPushButton *>(QStringLiteral("layerManageRenameThemeButton")) != nullptr);
      QVERIFY(dlg->findChild<QLabel *>(QStringLiteral("layerManageRenameNote")) != nullptr);
      QCOMPARE(dlg->findChild<QLabel *>(QStringLiteral("layerManageRenameNote"))->text(),
               QStringLiteral("page:* 页面档案名不可改"));

      // 删除 page:compose（offscreen 免确认直接删；page:* 允许删）
      for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->data(Qt::UserRole).toString() == QLatin1String("page:compose"))
          list->setCurrentRow(i);
      removeBtn->click();
      QCOMPARE(fx.profile.themes().size(), themesBefore.size() - 1);
      QVERIFY(!fx.profile.hasTheme(QStringLiteral("page:compose")));
      QCOMPARE(list->count(), themesBefore.size() - 1); // 列表即时收敛

      // 「应用」：applyTheme + 关窗（Accepted）
      fx.setLayerChecked(QStringLiteral("pm.facies"), false); // 工作主题里它是可见的
      list->setCurrentRow(0); // 仅剩「工作主题」
      applyBtn->click();
      QCOMPARE(dlg->result(), QDialog::Accepted);
      QVERIFY(fx.layerChecked(QStringLiteral("pm.facies"))); // 主题态恢复
    }

    // ---- 主线5：重命名按钮对 page:* 约定名静默不动（offscreen 无输入通道）----
    void manageDialogRenameShieldsPageThemes()
    {
      BarFixture fx;
      LayerProfileBar bar(&fx.profile);
      QVERIFY(fx.profile.applyPageProfile(QStringLiteral("compose")));
      QVERIFY(fx.profile.captureCurrentAsTheme(QStringLiteral("work")));

      QDialog *dlg = bar.buildManageDialog();
      QListWidget *list = dlg->findChild<QListWidget *>(QStringLiteral("layerManageThemeList"));
      auto *renameBtn =
          dlg->findChild<QPushButton *>(QStringLiteral("layerManageRenameThemeButton"));
      QVERIFY(list != nullptr && renameBtn != nullptr);

      // 选中 page:compose 点重命名：offscreen 无输入框 → 静默不改名
      for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->data(Qt::UserRole).toString() == QLatin1String("page:compose"))
          list->setCurrentRow(i);
      renameBtn->click();
      QVERIFY(fx.profile.hasTheme(QStringLiteral("page:compose")));
      QCOMPARE(fx.profile.themes().size(), 2);

      // 服务面重命名语义（work → work2）与 UI 解耦直证
      QVERIFY(fx.profile.renameTheme(QStringLiteral("work"), QStringLiteral("work2")));
      QVERIFY(!fx.profile.hasTheme(QStringLiteral("work")));
      QVERIFY(fx.profile.hasTheme(QStringLiteral("work2")));
    }

    // ---- offscreen：showManageDialog 不 exec 不死（直接调用即返回）----
    void showManageDialogOffscreenReturnsImmediately()
    {
      BarFixture fx;
      LayerProfileBar bar(&fx.profile);
      QVERIFY(QGuiApplication::platformName() == QLatin1String("offscreen"));

      bar.showManageDialog(); // 到这里即证明未卡 exec

      // 主工具条「管理主题…」按钮同一通道，offscreen 点击也不死
      QToolButton *manageBtn =
          bar.findChild<QToolButton *>(QStringLiteral("layerManageThemesButton"));
      QVERIFY(manageBtn != nullptr);
      manageBtn->click();
    }

    // ---- 保存主题按钮：offscreen no-op 不弹不死、不产生主题 ----
    void saveButtonOffscreenIsNoop()
    {
      BarFixture fx;
      LayerProfileBar bar(&fx.profile);
      QToolButton *saveBtn =
          bar.findChild<QToolButton *>(QStringLiteral("layerSaveThemeButton"));
      QVERIFY(saveBtn != nullptr);

      saveBtn->click();
      QCOMPARE(fx.profile.themes().size(), 0); // 未 capture、未弹 QInputDialog
    }
};

int main(int argc, char *argv[])
{
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(QStringLiteral("/usr"), true); // distro install
  app.initQgis();
  TestLayerProfileBar tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_layerprofilebar.moc"
