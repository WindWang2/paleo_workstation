#include <QtTest>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <qgsapplication.h>
#include <qgsmapcanvas.h>
#include <qgsmaplayer.h>
#include <qgsmaplayerstylemanager.h>
#include <qgsmeshlayer.h>
#include <qgsnullsymbolrenderer.h>
#include <qgsproject.h>
#include <qgsrasterlayer.h>
#include <qgsreadwritecontext.h>
#include <qgsvectorlayer.h>

#include "../src/metadata/layermanifest.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/ui/layers/layerpropertiesdialog.h"

// Fixture path: prefer the build-provided define, else derive from this file's
// location so standalone g++ builds work too.
static QString fixtureGpkg()
{
#ifdef FIXTURE_GPKG
  return QStringLiteral(FIXTURE_GPKG);
#else
  const QString testsDir = QFileInfo(QString::fromUtf8(__FILE__)).absolutePath();
  return QDir(testsDir).absoluteFilePath(QStringLiteral("../testdata/fixture.gpkg"));
#endif
}

static LayerDeclaration decl(const QString &layerId, const QString &horizon)
{
  LayerDeclaration d;
  d.layerId = layerId;
  d.horizon = horizon;
  d.type = QStringLiteral("vector");
  d.source = fixtureGpkg() + QStringLiteral("|layername=basin");
  d.styleRef = QStringLiteral("styles/%1.qml").arg(layerId);
  d.group = QStringLiteral("04_SingleFactor");
  return d;
}

// wave/layer-platform 子任务 B：LayerPropertiesDialog —— 原生属性壳 +
// Paleo 业务页 + QgsMapLayerStyleManager 样式管理。offscreen 只构造不
// exec（QTest 可验证三类图层行为），业务页经 createBusinessPage 直取。
class TestLayerProperties : public QObject
{
  Q_OBJECT

  private slots:
    void initTestCase()
    {
      QVERIFY(QgsApplication::instance() != nullptr);
      QVERIFY2(QFile::exists(fixtureGpkg()),
               qPrintable(QStringLiteral("fixture missing: %1").arg(fixtureGpkg())));
      QVERIFY2(QGuiApplication::platformName() == QStringLiteral("offscreen"),
               "openLayerProperties 的不-exec 契约依赖 offscreen 平台");
    }

    void cleanup()
    {
      // 先冲掉 offscreen 路径 deleteLater 的原生对话框（业务页持有图层指针），
      // 再清共享 QgsProject 单例。DeferredDelete 需显式派发。
      qApp->processEvents();
      QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
      QgsProject::instance()->removeAllMapLayers();
    }

    // (a) 矢量：声明 → 实例化 → openLayerProperties offscreen 不 exec 不崩；
    // 原生壳里 Paleo 业务页字段正确；layerPropertiesApplied 不发射。
    void vectorOpenConstructsDialogWithBusinessPage()
    {
      QTemporaryDir tmp;
      QVERIFY(tmp.isValid());
      LayerManifest manifest(tmp.filePath(QStringLiteral("project.sqlite")));
      QVERIFY(manifest.open());
      QgisLayerService svc(nullptr, &manifest);
      QVERIFY(svc.declare(decl(QStringLiteral("facies.T1"), QStringLiteral("T1"))));
      QgsMapLayer *layer = svc.instantiate(QStringLiteral("facies.T1"));
      QVERIFY2(layer && layer->isValid(), "fixture vector must instantiate");

      // Deps 全空：canvas=null（QgsVectorLayerProperties 接受）+ messageBar
      // 缺省自造持有（m_ownMessageBar 路径）。
      LayerPropertiesDialog::Deps deps;
      LayerPropertiesDialog dlg(&svc, deps);
      QSignalSpy appliedSpy(&dlg, &LayerPropertiesDialog::layerPropertiesApplied);

      dlg.openLayerProperties(QStringLiteral("facies.T1"));
      QCOMPARE(appliedSpy.count(), 0); // offscreen 构造路径不发射

      QDialog *native = findNativeDialog();
      QVERIFY2(native, "offscreen must construct the native properties dialog");
      QPointer<QObject> guard(native);

      auto *idLabel = native->findChild<QLabel *>(QStringLiteral("paleoPropLayerId"));
      QVERIFY2(idLabel, "business page must be embedded in the native dialog");
      QCOMPARE(idLabel->text(), QStringLiteral("facies.T1"));
      QCOMPARE(native->findChild<QLabel *>(QStringLiteral("paleoPropGroup"))->text(),
               QStringLiteral("04_SingleFactor"));
      QCOMPARE(native->findChild<QLabel *>(QStringLiteral("paleoPropHorizon"))->text(),
               QStringLiteral("T1"));
      QCOMPARE(native->findChild<QLabel *>(QStringLiteral("paleoPropSource"))->text(),
               fixtureGpkg() + QStringLiteral("|layername=basin"));

      qApp->processEvents();
      // deleteLater 的 DeferredDelete 事件 processEvents 默认不冲——显式派发
      QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
      QVERIFY2(guard.isNull(), "offscreen-constructed dialog must be reaped");
    }

    // (b) 栅格（无 tif fixture）：坏路径 QgsRasterLayer 直接 addMapLayer +
    // paleoLayerId 自定义属性 → 解析链 b) 工程扫描命中，构造
    // QgsRasterLayerProperties 不崩（canvas 在场）。
    void rasterViaProjectScanConstructsNativeDialog()
    {
      QTemporaryDir tmp;
      QVERIFY(tmp.isValid());
      LayerManifest manifest(tmp.filePath(QStringLiteral("project.sqlite")));
      QVERIFY(manifest.open());
      QgisLayerService svc(nullptr, &manifest);

      QgsRasterLayer *broken = new QgsRasterLayer(
          QStringLiteral("/nonexistent/broken-probe.tif"), QStringLiteral("缺源栅格"));
      broken->setCustomProperty(QStringLiteral("paleoLayerId"), QStringLiteral("raster.broken"));
      QgsProject::instance()->addMapLayer(broken);
      QVERIFY(!broken->isValid());
      QVERIFY(svc.layer(QStringLiteral("raster.broken")) == nullptr); // 服务缓存没有 → 走扫描

      QgsMapCanvas canvas;
      LayerPropertiesDialog::Deps deps;
      deps.canvas = &canvas;
      LayerPropertiesDialog dlg(&svc, deps);
      QSignalSpy appliedSpy(&dlg, &LayerPropertiesDialog::layerPropertiesApplied);
      dlg.openLayerProperties(QStringLiteral("raster.broken"));
      QCOMPARE(appliedSpy.count(), 0);

      QDialog *native = findNativeDialog();
      QVERIFY2(native, "broken-source raster must still get a properties dialog");
      QPointer<QObject> guard(native);
      QVERIFY(native->findChild<QLabel *>(QStringLiteral("paleoPropLayerId")) != nullptr);
      QCOMPARE(native->findChild<QLabel *>(QStringLiteral("paleoPropLayerId"))->text(),
               QStringLiteral("raster.broken"));

      qApp->processEvents();
      QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
      QVERIFY(guard.isNull());
    }

    // (c) 缺源：isValid()==false 的图层 openLayerProperties 行为 = 可构造
    // 对话框（不崩、无断言异常）；canvas=null 路径。
    void missingSourceRasterIsValidFalseNoCrash()
    {
      QTemporaryDir tmp;
      QVERIFY(tmp.isValid());
      LayerManifest manifest(tmp.filePath(QStringLiteral("project.sqlite")));
      QVERIFY(manifest.open());
      QgisLayerService svc(nullptr, &manifest);

      QgsRasterLayer *broken = new QgsRasterLayer(
          QStringLiteral("/nonexistent/missing-source.tif"), QStringLiteral("缺源栅格2"));
      broken->setCustomProperty(QStringLiteral("paleoLayerId"), QStringLiteral("raster.missing"));
      QgsProject::instance()->addMapLayer(broken);
      QVERIFY(!broken->isValid());

      LayerPropertiesDialog::Deps deps; // canvas=null：栅格壳也接受
      LayerPropertiesDialog dlg(&svc, deps);
      dlg.openLayerProperties(QStringLiteral("raster.missing")); // 不崩即过
      QVERIFY(findNativeDialog() != nullptr);
    }

    // (d) 未知 layerId → 无操作（无对话框、不崩）；createBusinessPage → nullptr。
    void unknownLayerIdIsNoOp()
    {
      QTemporaryDir tmp;
      QVERIFY(tmp.isValid());
      LayerManifest manifest(tmp.filePath(QStringLiteral("project.sqlite")));
      QVERIFY(manifest.open());
      QgisLayerService svc(nullptr, &manifest);
      LayerPropertiesDialog dlg(&svc, LayerPropertiesDialog::Deps{});

      dlg.openLayerProperties(QStringLiteral("ghost.layer"));
      QVERIFY2(findNativeDialog() == nullptr, "unknown layerId must be a no-op");
      QVERIFY(dlg.createBusinessPage(QStringLiteral("ghost.layer")) == nullptr);
      QVERIFY(dlg.createBusinessPage(QString()) == nullptr);
    }

    // (e) mesh 等其他类型 → 无操作（不崩、无对话框）。
    void meshTypeIsNoOp()
    {
      QTemporaryDir tmp;
      QVERIFY(tmp.isValid());
      LayerManifest manifest(tmp.filePath(QStringLiteral("project.sqlite")));
      QVERIFY(manifest.open());
      QgisLayerService svc(nullptr, &manifest);

      QgsMeshLayer *mesh = new QgsMeshLayer(QStringLiteral("/nonexistent/probe.mesh"),
                                            QStringLiteral("mesh"), QStringLiteral("mdal"));
      mesh->setCustomProperty(QStringLiteral("paleoLayerId"), QStringLiteral("mesh.x"));
      QgsProject::instance()->addMapLayer(mesh);

      LayerPropertiesDialog dlg(&svc, LayerPropertiesDialog::Deps{});
      dlg.openLayerProperties(QStringLiteral("mesh.x")); // qobject_cast 双落空 → 无操作
      QVERIFY2(findNativeDialog() == nullptr, "mesh layers must be a no-op");
    }

    // (f) 业务页：objectName 齐全 + 声明字段正确；无声明（手工层走扫描）→
    // group/horizon/source 显示「—」；未关联资产 → 按钮禁用 + reason tooltip +
    // 点击不发信号；创建时间无时间戳 → 「—」。
    void businessPageFieldsAndUnlinkedAsset()
    {
      QTemporaryDir tmp;
      QVERIFY(tmp.isValid());
      LayerManifest manifest(tmp.filePath(QStringLiteral("project.sqlite")));
      QVERIFY(manifest.open());
      QgisLayerService svc(nullptr, &manifest);
      QVERIFY(svc.declare(decl(QStringLiteral("facies.T1"), QStringLiteral("T1"))));
      QVERIFY(svc.instantiate(QStringLiteral("facies.T1")));

      LayerPropertiesDialog dlg(&svc, LayerPropertiesDialog::Deps{});
      QPointer<QWidget> page(dlg.createBusinessPage(QStringLiteral("facies.T1")));
      QVERIFY(page != nullptr);

      const char *names[] = {"paleoPropLayerId",  "paleoPropGroup", "paleoPropHorizon",
                             "paleoPropAsset",    "paleoPropSource", "paleoPropCreated",
                             "paleoInspectAssetButton"};
      for (const char *n : names)
        QVERIFY2(page->findChild<QLabel *>(QString::fromLatin1(n)) ||
                     page->findChild<QPushButton *>(QString::fromLatin1(n)),
                 n);

      QCOMPARE(page->findChild<QLabel *>(QStringLiteral("paleoPropLayerId"))->text(),
               QStringLiteral("facies.T1"));
      QCOMPARE(page->findChild<QLabel *>(QStringLiteral("paleoPropGroup"))->text(),
               QStringLiteral("04_SingleFactor"));
      QCOMPARE(page->findChild<QLabel *>(QStringLiteral("paleoPropHorizon"))->text(),
               QStringLiteral("T1"));
      QCOMPARE(page->findChild<QLabel *>(QStringLiteral("paleoPropSource"))->text(),
               fixtureGpkg() + QStringLiteral("|layername=basin"));
      QCOMPARE(page->findChild<QLabel *>(QStringLiteral("paleoPropAsset"))->text(),
               QStringLiteral("未关联"));
      // 主线5：创建时间来自 instantiate() 落的 paleoCreatedAt 图层自定义属性
      //（ISO UTC；非 instantiate 产生的层保持占位「—」）。
      const QString createdText =
          page->findChild<QLabel *>(QStringLiteral("paleoPropCreated"))->text();
      QVERIFY2(createdText != QStringLiteral("—"),
               qPrintable(QStringLiteral("instantiated layer must carry paleoCreatedAt: %1").arg(createdText)));
      QVERIFY2(createdText.startsWith(QStringLiteral("20")),
               qPrintable(QStringLiteral("not an ISO timestamp: %1").arg(createdText)));

      // 未关联资产：按钮禁用 + reason tooltip（DESIGN.md 禁用带 reason），
      // 点击不发射 assetInspectionRequested（负面断言）。
      auto *btn = page->findChild<QPushButton *>(QStringLiteral("paleoInspectAssetButton"));
      QVERIFY(btn != nullptr);
      QVERIFY2(!btn->isEnabled(), "unlinked asset must disable the button");
      QVERIFY2(!btn->toolTip().isEmpty(), "disabled button must carry a reason tooltip");
      QSignalSpy inspectSpy(&dlg, &LayerPropertiesDialog::assetInspectionRequested);
      btn->click();
      QCOMPARE(inspectSpy.count(), 0);

      // 手工层（无声明，链 b）：页面可用，声明侧字段为「—」。
      QgsVectorLayer *manual = new QgsVectorLayer(QStringLiteral("Point?field=id:integer"),
                                                  QStringLiteral("手工层"), QStringLiteral("memory"));
      manual->setCustomProperty(QStringLiteral("paleoLayerId"), QStringLiteral("manual.m"));
      QgsProject::instance()->addMapLayer(manual);
      QPointer<QWidget> manualPage(dlg.createBusinessPage(QStringLiteral("manual.m")));
      QVERIFY(manualPage != nullptr);
      QCOMPARE(manualPage->findChild<QLabel *>(QStringLiteral("paleoPropLayerId"))->text(),
               QStringLiteral("manual.m"));
      QCOMPARE(manualPage->findChild<QLabel *>(QStringLiteral("paleoPropGroup"))->text(),
               QStringLiteral("—"));
      QCOMPARE(manualPage->findChild<QLabel *>(QStringLiteral("paleoPropHorizon"))->text(),
               QStringLiteral("—"));
      QCOMPARE(manualPage->findChild<QLabel *>(QStringLiteral("paleoPropSource"))->text(),
               QStringLiteral("—"));

      delete page;
      delete manualPage;
    }

    // (g) 保存预设：offscreen QInputDialog no-op → 默认名来自 manifest
    // styleRef 的 basename 去目录去 .qml（styles/facies.T1.qml → facies.T1）；
    // 无声明层 → 「预设1」；保存后下拉列出该预设。
    void savePresetUsesStyleRefDefaultName()
    {
      QTemporaryDir tmp;
      QVERIFY(tmp.isValid());
      LayerManifest manifest(tmp.filePath(QStringLiteral("project.sqlite")));
      QVERIFY(manifest.open());
      QgisLayerService svc(nullptr, &manifest);
      QVERIFY(svc.declare(decl(QStringLiteral("facies.T1"), QStringLiteral("T1"))));
      QgsMapLayer *layer = svc.instantiate(QStringLiteral("facies.T1"));
      QVERIFY(layer != nullptr);
      QgsMapLayerStyleManager *sm = layer->styleManager();
      QVERIFY(sm != nullptr);

      LayerPropertiesDialog dlg(&svc, LayerPropertiesDialog::Deps{});
      QPointer<QWidget> page(dlg.createBusinessPage(QStringLiteral("facies.T1")));
      QVERIFY(page != nullptr);
      auto *save = page->findChild<QPushButton *>(QStringLiteral("paleoSavePresetButton"));
      QVERIFY(save != nullptr);
      save->click(); // offscreen：跳过取名对话框，直接用默认名

      QVERIFY2(sm->styles().contains(QStringLiteral("facies.T1")),
               qPrintable(sm->styles().join(QLatin1Char(','))));
      auto *combo = page->findChild<QComboBox *>(QStringLiteral("paleoPresetCombo"));
      QVERIFY(combo != nullptr);
      QVERIFY(combo->findText(QStringLiteral("facies.T1")) >= 0);
      delete page;

      // 无声明手工层 → 「预设1」
      QgsVectorLayer *manual = new QgsVectorLayer(QStringLiteral("Point?field=id:integer"),
                                                  QStringLiteral("手工层"), QStringLiteral("memory"));
      manual->setCustomProperty(QStringLiteral("paleoLayerId"), QStringLiteral("manual.m"));
      QgsProject::instance()->addMapLayer(manual);
      QPointer<QWidget> mpage(dlg.createBusinessPage(QStringLiteral("manual.m")));
      QVERIFY(mpage != nullptr);
      mpage->findChild<QPushButton *>(QStringLiteral("paleoSavePresetButton"))->click();
      QVERIFY2(manual->styleManager()->styles().contains(QStringLiteral("预设1")),
               qPrintable(manual->styleManager()->styles().join(QLatin1Char(','))));
      delete mpage;
    }

    // (h) 预设恢复：addStyleFromLayer("p1") 后改 renderer → 业务页选 p1 恢复
    // → renderer type 回到预设时状态。
    void presetRestoreRoundtrip()
    {
      QTemporaryDir tmp;
      QVERIFY(tmp.isValid());
      LayerManifest manifest(tmp.filePath(QStringLiteral("project.sqlite")));
      QVERIFY(manifest.open());
      QgisLayerService svc(nullptr, &manifest);
      QVERIFY(svc.declare(decl(QStringLiteral("facies.T1"), QStringLiteral("T1"))));
      QgsMapLayer *layer = svc.instantiate(QStringLiteral("facies.T1"));
      QVERIFY(layer != nullptr);
      auto *vl = qobject_cast<QgsVectorLayer *>(layer);
      QVERIFY(vl != nullptr);
      QgsMapLayerStyleManager *sm = layer->styleManager();

      const QString beforeType = vl->renderer()->type();
      QVERIFY(sm->addStyleFromLayer(QStringLiteral("p1")));

      // 换 renderer（null symbol）再经业务页恢复 p1
      vl->setRenderer(new QgsNullSymbolRenderer());
      QCOMPARE(vl->renderer()->type(), QStringLiteral("nullSymbol"));

      LayerPropertiesDialog dlg(&svc, LayerPropertiesDialog::Deps{});
      QPointer<QWidget> page(dlg.createBusinessPage(QStringLiteral("facies.T1")));
      QVERIFY(page != nullptr);
      auto *combo = page->findChild<QComboBox *>(QStringLiteral("paleoPresetCombo"));
      QVERIFY(combo != nullptr);
      QVERIFY2(combo->findText(QStringLiteral("p1")) >= 0, "combo must list stored presets");
      combo->setCurrentText(QStringLiteral("p1"));
      page->findChild<QPushButton *>(QStringLiteral("paleoRestorePresetButton"))->click();
      QCOMPARE(vl->renderer()->type(), beforeType); // 渲染恢复
      QCOMPARE(sm->currentStyle(), QStringLiteral("p1"));
      delete page;
    }

    // (i) .qml 导出导入（按钮所调的同一 QGIS API 面）：exportNamedStyle 落
    // QTemporaryDir 文件 → 存在非空 → 第二层 importNamedStyle 后 renderer 生效。
    void qmlExportImportRoundtrip()
    {
      QTemporaryDir tmp;
      QVERIFY(tmp.isValid());
      LayerManifest manifest(tmp.filePath(QStringLiteral("project.sqlite")));
      QVERIFY(manifest.open());
      QgisLayerService svc(nullptr, &manifest);
      QVERIFY(svc.declare(decl(QStringLiteral("facies.T1"), QStringLiteral("T1"))));
      QgsMapLayer *layer = svc.instantiate(QStringLiteral("facies.T1"));
      QVERIFY(layer != nullptr);

      QDomDocument doc;
      QString styleErr;
      QgsReadWriteContext ctx;
      layer->exportNamedStyle(doc, styleErr, ctx);
      QVERIFY2(styleErr.isEmpty(), qPrintable(styleErr));

      const QString path = tmp.filePath(QStringLiteral("facies.T1.qml"));
      {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QVERIFY(f.write(doc.toByteArray()) > 0);
        f.close();
      }
      QVERIFY(QFile::exists(path));
      QVERIFY2(QFileInfo(path).size() > 0, "exported qml must be non-empty");

      // 第二层：memory 矢量（同 Polygon 几何，qml 样式要求几何匹配）换
      // null renderer → 导入后恢复 singleSymbol
      QgsVectorLayer second(QStringLiteral("Polygon?field=id:integer"),
                            QStringLiteral("second"), QStringLiteral("memory"));
      second.setRenderer(new QgsNullSymbolRenderer());
      QCOMPARE(second.renderer()->type(), QStringLiteral("nullSymbol"));

      QDomDocument imported;
      {
        QFile rf(path);
        QVERIFY(rf.open(QIODevice::ReadOnly));
        QVERIFY(imported.setContent(&rf));
      }
      QString importErr;
      QVERIFY2(second.importNamedStyle(imported, importErr), qPrintable(importErr));
      QCOMPARE(second.renderer()->type(), QStringLiteral("singleSymbol"));
    }

  private:
    // offscreen openLayerProperties 构造的原生壳（无父窗口 → top-level），
    // objectName 约定 paleoLayerPropertiesDialog；deleteLater 冲掉之前有效。
    QDialog *findNativeDialog() const
    {
      for (QWidget *w : QApplication::topLevelWidgets())
        if (w->objectName() == QLatin1String("paleoLayerPropertiesDialog"))
          return qobject_cast<QDialog *>(w);
      return nullptr;
    }
};

int main(int argc, char *argv[])
{
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(QStringLiteral("/usr"), true); // distro install
  app.initQgis();
  TestLayerProperties tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_layerproperties.moc"
