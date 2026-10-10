#include <QtTest>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUndoStack>

#include "../src/app/appcontext.h"
#include "../src/ui/paleomainwindow.h"
#include "../src/ui/edittools/editingtoolbar.h"
#include "../src/ui/edittools/vertexeditortools.h" // PaleoVertexTool（QgsVertexTool 是 app-only）
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgiscanvascontroller.h"
#include "../src/qgis/qgisstyleservice.h" // C2：applyFaciesBoundaryStyle 断言面
#include "../src/metadata/layermanifest.h"
#include "../src/workflow/workflows.h"

#include <gdal.h>
#include <cpl_conv.h>

#include <qgscategorizedsymbolrenderer.h>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsfeaturerequest.h>
#include <qgsmapcanvas.h>
#include <qgsmaplayer.h>
#include <qgsproject.h>
#include <qgsvectorlayer.h>

// m2/mapping-pages（任务 C）真实栈：智能编图页两条硬路径——
//   1. CompositionWorkflow::saveFaciesAttributes 把相属性写进图层 edit buffer
//      （beginEditCommand/changeAttributeValue/endEditCommand，真实栈断言
//      getFeature 读回 + undo 复原）；
//   2. 矢量化成功（faciesPolygonsReady）后壳侧自动进入相界编辑态：
//      编辑条选层 + 开始编辑 + 画布装 PaleoVertexTool；页→壳→工作流的
//      相属性保存链端到端。
// 栈模式：AppContext 组装根 + PaleoMainWindow（tst_ui 模式）。
class TestComposeWorkflow : public QObject
{
  Q_OBJECT
  public:
    explicit TestComposeWorkflow(AppContext *ctx, QObject *parent = nullptr)
      : QObject(parent), m_ctx(ctx) {}

  private:
    AppContext *m_ctx;
    PaleoMainWindow *m_win = nullptr;

    static LayerDeclaration decl(const QString &layerId, const QString &horizon,
                                 const QString &type, const QString &source,
                                 const QString &group)
    {
      LayerDeclaration d;
      d.layerId = layerId;
      d.horizon = horizon;
      d.type = type;
      d.source = source;
      d.group = group;
      return d;
    }

    // Write a w x h Float32 GTiff (same pattern as tst_workflows.cpp).
    static QString makeRaster(const QString &path, int w, int h, const QVector<float> &px)
    {
      GDALDriverH drv = GDALGetDriverByName("GTiff");
      GDALDatasetH ds = GDALCreate(drv, path.toUtf8().constData(), w, h, 1, GDT_Float32, nullptr);
      if (!ds)
        return QString();
      const double gt[6] = {0.0, 1.0, 0.0, static_cast<double>(h), 0.0, -1.0};
      GDALSetGeoTransform(ds, const_cast<double *>(gt));
      GDALRasterBandH band = GDALGetRasterBand(ds, 1);
      GDALSetRasterNoDataValue(band, -9999.0);
      const CPLErr err = GDALRasterIO(band, GF_Write, 0, 0, w, h,
                                      const_cast<float *>(px.constData()), w, h,
                                      GDT_Float32, 0, 0);
      GDALClose(ds);
      return err == CE_None ? path : QString();
    }

  private slots:
    void initTestCase()
    {
      QVERIFY2(m_ctx->ready(), "AppContext failed to bring up QgisRuntime");
      m_win = new PaleoMainWindow(m_ctx->canvasCtl(), m_ctx->projectSvc(),
                                  m_ctx->layerSvc(), m_ctx->toolSvc(), m_ctx->selection());
      m_win->attachWorkflows(m_ctx->predictionWf(), m_ctx->constraintWf(),
                             m_ctx->compositionWf(), m_ctx->validationWf(),
                             m_ctx->importSvc(), m_ctx->seismicLink(),
                             m_ctx->processingSvc(), m_ctx->store(),
                             m_ctx->editingSvc(), m_ctx->layoutSvc(),
                             m_ctx->taskSvc());
    }

    void cleanupTestCase()
    {
      delete m_win;
      m_win = nullptr;
    }

    // C2（wave/deepen-perf）：相界地质语义类型单类型（断层切割）垂直片——
    // boundary_kind 字段补建 + 值进 edit buffer + 渲染器切到 boundary_kind
    // 分类（断层红粗边类目存在）。无字段的层不接管渲染器。
    void boundaryKindWritesAndStyles()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      QVERIFY2(m_ctx->projectSvc()->createProject(dir.filePath(QStringLiteral("b.qgz"))),
               "createProject failed");
      const QVector<float> px = {1, 1, 2, 2};
      const QString path =
          makeRaster(dir.filePath(QStringLiteral("coded_b.tif")), 2, 2, px);
      QVERIFY(!path.isEmpty());
      QString err;
      QVERIFY2(m_ctx->layerSvc()->declare(
                   decl(QStringLiteral("composite.T1"), QStringLiteral("T1"),
                        QStringLiteral("raster"), path, QStringLiteral("03_Composite")),
                   &err),
               qPrintable(err));
      QVERIFY2(m_ctx->compositionWf()->deriveFaciesPolygons(
                   QStringLiteral("T1"), QStringLiteral("composite.T1"), QVariantMap(), &err),
               qPrintable(err));
      auto *vl = qobject_cast<QgsVectorLayer *>(
          m_ctx->layerSvc()->instantiate(QStringLiteral("facies.T1")));
      QVERIFY2(vl, "facies.T1 did not instantiate as a vector layer");

      // 未写 boundary_kind 前：字段不存在 → applyFaciesBoundaryStyle 不接管。
      QVERIFY(vl->fields().lookupField(QStringLiteral("boundary_kind")) < 0);
      QgisStyleService::applyFaciesBoundaryStyle(vl);
      QVERIFY(vl->renderer()->type() != QStringLiteral("categorizedSymbol"));

      QgsFeature f;
      QgsFeatureIterator it = vl->getFeatures();
      QVERIFY(it.nextFeature(f));
      vl->select(f.id());
      QVERIFY2(m_ctx->compositionWf()->saveFaciesAttributes(
                   QStringLiteral("facies.T1"),
                   QVariantMap{{QStringLiteral("boundary_kind"),
                                QStringLiteral("fault_cut")}},
                   &err),
               qPrintable(err));

      // 字段补建 + 值落 edit buffer。
      QVERIFY(vl->fields().lookupField(QStringLiteral("boundary_kind")) >= 0);
      QgsFeature got;
      QVERIFY(vl->getFeatures(QgsFeatureRequest(f.id())).nextFeature(got));
      QCOMPARE(got.attribute(QStringLiteral("boundary_kind")).toString(),
               QStringLiteral("fault_cut"));

      // 符号映射：渲染器切到 boundary_kind 分类，断层切割类目在册。
      QCOMPARE(vl->renderer()->type(), QStringLiteral("categorizedSymbol"));
      auto *cat = static_cast<QgsCategorizedSymbolRenderer *>(vl->renderer());
      QCOMPARE(cat->classAttribute(), QStringLiteral("boundary_kind"));
      bool hasFault = false;
      for (const QgsRendererCategory &c : cat->categories())
        if (c.value().toString() == QStringLiteral("fault_cut"))
          hasFault = true;
      QVERIFY2(hasFault, "fault_cut category present");

      // 会话卫生：undo 后取消编辑会话（同 z3 口径）。
      while (vl->undoStack()->canUndo())
        vl->undoStack()->undo();
      if (auto *tb = m_win->findChild<PaleoEditingToolbar *>(QStringLiteral("editingToolbar")))
        tb->cancelEditing();
    }

    // 相属性回写：选中要素的三字段（facies_code 已有 / facies_type+comment
    // 补建）写进 edit buffer——getFeature 读回即新值（edit buffer 生效），
    // undo 后 facies_code 复原（beginEditCommand 命令组语义）。无选中 → 拒绝。
    void saveFaciesAttributesWritesThroughEditBuffer()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      QVERIFY2(m_ctx->projectSvc()->createProject(dir.filePath(QStringLiteral("p.qgz"))),
               "createProject failed"); // projectOpened → manifest/catalog 重绑
      const QVector<float> px = {1, 1, 2, 2};
      const QString path =
          makeRaster(dir.filePath(QStringLiteral("coded.tif")), 2, 2, px);
      QVERIFY(!path.isEmpty());
      QString err;
      QVERIFY2(m_ctx->layerSvc()->declare(
                   decl(QStringLiteral("composite.T1"), QStringLiteral("T1"),
                        QStringLiteral("raster"), path, QStringLiteral("03_Composite")),
                   &err),
               qPrintable(err));

      QVERIFY2(m_ctx->compositionWf()->deriveFaciesPolygons(
                   QStringLiteral("T1"), QStringLiteral("composite.T1"), QVariantMap(), &err),
               qPrintable(err));
      auto *vl = qobject_cast<QgsVectorLayer *>(
          m_ctx->layerSvc()->instantiate(QStringLiteral("facies.T1")));
      QVERIFY2(vl, "facies.T1 did not instantiate as a vector layer");

      // 无选中要素 → 拒绝并说明（不静默写全表）。
      QVERIFY(!m_ctx->compositionWf()->saveFaciesAttributes(
          QStringLiteral("facies.T1"),
          QVariantMap{{QStringLiteral("facies_code"), 3}}, &err));
      QVERIFY2(err.contains(QStringLiteral("选中")), qPrintable(err));

      QgsFeature f;
      QgsFeatureIterator it = vl->getFeatures();
      QVERIFY(it.nextFeature(f));
      const int origCode = f.attribute(QStringLiteral("facies_code")).toInt();
      vl->select(f.id());

      QVariantMap attrs;
      attrs.insert(QStringLiteral("facies_code"), 3);
      attrs.insert(QStringLiteral("facies_type"), QStringLiteral("辫状河三角洲"));
      attrs.insert(QStringLiteral("comment"), QStringLiteral("相界备注"));
      QVERIFY2(m_ctx->compositionWf()->saveFaciesAttributes(
                   QStringLiteral("facies.T1"), attrs, &err),
               qPrintable(err));

      // 读回：edit buffer 生效（无需 commit，getFeature 走 buffer 叠加）。
      QgsFeature got;
      QVERIFY(vl->getFeatures(QgsFeatureRequest(f.id())).nextFeature(got));
      QCOMPARE(got.attribute(QStringLiteral("facies_code")).toInt(), 3);
      QCOMPARE(got.attribute(QStringLiteral("facies_type")).toString(),
               QStringLiteral("辫状河三角洲"));
      QCOMPARE(got.attribute(QStringLiteral("comment")).toString(),
               QStringLiteral("相界备注"));

      // undo：命令组复原（facies_code 回原值——beginEditCommand 语义直证）。
      QVERIFY(vl->undoStack()->canUndo());
      while (vl->undoStack()->canUndo())
        vl->undoStack()->undo();
      QgsFeature undone;
      QVERIFY(vl->getFeatures(QgsFeatureRequest(f.id())).nextFeature(undone));
      QCOMPARE(undone.attribute(QStringLiteral("facies_code")).toInt(), origCode);

      // 会话卫生：自动编辑态开的编辑会话在用例结束时收掉（公共 API）——
      // 否则下个用例 createProject 清工程时编辑条内部悬空（既有 z3 缺口，
      // 见报告）。断言已做完，直接取消（undo 后 buffer 已空）。
      if (auto *tb = m_win->findChild<PaleoEditingToolbar *>(QStringLiteral("editingToolbar")))
        tb->cancelEditing();
    }

    // 矢量化成功 → 壳自动进入相界编辑态：层被选中并处于编辑会话、画布装
    // PaleoVertexTool（不是 QgsVertexTool——app-only）、页面相属性区开闸；
    // 页→壳→工作流保存链端到端写值。
    void polygonizeAutoEntersFaciesEditModeAndSavesAttrs()
    {
      m_win->showPage(QStringLiteral("compose"));
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      QVERIFY2(m_ctx->projectSvc()->createProject(dir.filePath(QStringLiteral("q.qgz"))),
               "createProject failed");
      const QVector<float> px = {1, 1, 1, 2};
      const QString path =
          makeRaster(dir.filePath(QStringLiteral("coded2.tif")), 2, 2, px);
      QVERIFY(!path.isEmpty());
      QString err;
      QVERIFY2(m_ctx->layerSvc()->declare(
                   decl(QStringLiteral("composite.T2"), QStringLiteral("T2"),
                        QStringLiteral("raster"), path, QStringLiteral("03_Composite")),
                   &err),
               qPrintable(err));

      QVERIFY2(m_ctx->compositionWf()->deriveFaciesPolygons(
                   QStringLiteral("T2"), QStringLiteral("composite.T2"), QVariantMap(), &err),
               qPrintable(err));

      // 壳自动编辑态（faciesPolygonsReady 的直接连接同步跑完）。
      auto *vl = qobject_cast<QgsVectorLayer *>(
          m_ctx->layerSvc()->layer(QStringLiteral("facies.T2")));
      QVERIFY2(vl, "shell did not materialize facies.T2");
      QVERIFY2(vl->isEditable(), "facies layer did not enter an edit session");
      auto *editTb = m_win->findChild<PaleoEditingToolbar *>(QStringLiteral("editingToolbar"));
      QVERIFY(editTb);
      QVERIFY(editTb->isEditing());
      QCOMPARE(editTb->currentLayer(), vl);
      QVERIFY2(qobject_cast<PaleoVertexTool *>(m_ctx->canvasCtl()->canvas()->mapTool()),
               "canvas tool is not PaleoVertexTool");

      // 页面相属性区：目标已指名、保存开闸。
      auto *save = m_win->findChild<QPushButton *>(QStringLiteral("faciesAttrSaveButton"));
      auto *target = m_win->findChild<QLabel *>(QStringLiteral("faciesTargetLabel"));
      QVERIFY(save && target);
      QVERIFY(save->isEnabled());
      QVERIFY2(target->text().contains(QStringLiteral("facies.T2")),
               qPrintable(target->text()));

      // 端到端：选中要素 → 页面三字段 → 保存按钮 → 图层上读回。
      QgsFeature f;
      QgsFeatureIterator it = vl->getFeatures();
      QVERIFY(it.nextFeature(f));
      vl->select(f.id());
      m_win->findChild<QLineEdit *>(QStringLiteral("faciesCodeEdit"))
          ->setText(QStringLiteral("3"));
      m_win->findChild<QLineEdit *>(QStringLiteral("faciesTypeEdit"))
          ->setText(QStringLiteral("辫状河三角洲"));
      m_win->findChild<QLineEdit *>(QStringLiteral("faciesCommentEdit"))
          ->setText(QStringLiteral("壳链端到端"));
      save->click();

      QgsFeature got;
      QVERIFY(vl->getFeatures(QgsFeatureRequest(f.id())).nextFeature(got));
      QCOMPARE(got.attribute(QStringLiteral("facies_code")).toInt(), 3);
      QCOMPARE(got.attribute(QStringLiteral("facies_type")).toString(),
               QStringLiteral("辫状河三角洲"));
      QCOMPARE(got.attribute(QStringLiteral("comment")).toString(),
               QStringLiteral("壳链端到端"));

      // 会话卫生：同上——编辑会话用例内收掉，避免 teardown 悬空。
      if (auto *tb = m_win->findChild<PaleoEditingToolbar *>(QStringLiteral("editingToolbar")))
        tb->cancelEditing();
    }

    // 方向 39：相界类型编辑语义门禁——真实栈（polygonize 两相带代码 1/2
    // 邻接）。整合接触切两侧被拒（原因含冻结词面与两侧相代码证据）；拒绝
    // 时零副作用（不建字段）；尖灭可过；带宽随非相变类型提交被拒；相变+
    // 带宽整单通过并落 edit buffer。
    void boundaryEditGateRejectsConformableCut()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      QVERIFY2(m_ctx->projectSvc()->createProject(dir.filePath(QStringLiteral("gate.qgz"))),
               "createProject failed");
      // 4×4 上下两半：默认 MIN_CELLS=4 下每类 8 像元才不被小图斑聚合吃掉。
      const QVector<float> px = {1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2};
      const QString path =
          makeRaster(dir.filePath(QStringLiteral("coded_g.tif")), 4, 4, px);
      QVERIFY(!path.isEmpty());
      QString err;
      QVERIFY2(m_ctx->layerSvc()->declare(
                   decl(QStringLiteral("composite.T1"), QStringLiteral("T1"),
                        QStringLiteral("raster"), path, QStringLiteral("03_Composite")),
                   &err),
               qPrintable(err));
      QVERIFY2(m_ctx->compositionWf()->deriveFaciesPolygons(
                   QStringLiteral("T1"), QStringLiteral("composite.T1"), QVariantMap(), &err),
               qPrintable(err));
      auto *vl = qobject_cast<QgsVectorLayer *>(
          m_ctx->layerSvc()->instantiate(QStringLiteral("facies.T1")));
      QVERIFY2(vl, "facies.T1 did not instantiate");

      // 选中相代码 1 的要素（与相代码 2 的要素共享边）。
      QgsFeature first;
      QgsFeatureIterator it = vl->getFeatures();
      QVERIFY(it.nextFeature(first));
      vl->select(first.id());

      // 整合接触切两侧 → 拒绝 + 原因文案（含词面与两侧相代码）。
      QVERIFY(!m_ctx->compositionWf()->saveFaciesAttributes(
          QStringLiteral("facies.T1"),
          QVariantMap{{QStringLiteral("boundary_kind"), QStringLiteral("conformable")}},
          &err));
      QVERIFY2(err.contains(QStringLiteral("整合接触")), qPrintable(err));
      QVERIFY2(err.contains(QStringLiteral("相变")), qPrintable(err));
      QVERIFY2(err.contains('1') && err.contains('2'), qPrintable(err)); // 两侧相代码证据
      QVERIFY2(err.contains(QString::number(first.id())), qPrintable(err)); // 定位到要素

      // 拒绝零副作用：boundary_kind 字段仍未建（门禁先于字段补建）。
      QVERIFY(vl->fields().lookupField(QStringLiteral("boundary_kind")) < 0);

      // 尖灭可过（无两侧约束）；属性落 edit buffer。
      QVERIFY2(m_ctx->compositionWf()->saveFaciesAttributes(
                   QStringLiteral("facies.T1"),
                   QVariantMap{{QStringLiteral("boundary_kind"), QStringLiteral("pinchout")}},
                   &err),
               qPrintable(err));
      {
        QgsFeature got;
        QVERIFY(vl->getFeatures(QgsFeatureRequest(first.id())).nextFeature(got));
        QCOMPARE(got.attribute(QStringLiteral("boundary_kind")).toString(),
                 QStringLiteral("pinchout"));
      }

      // 带宽随尖灭（非相变）提交 → 拒绝并说明。
      QVERIFY(!m_ctx->compositionWf()->saveFaciesAttributes(
          QStringLiteral("facies.T1"),
          QVariantMap{{QStringLiteral("transition_width"), 500.0}}, &err));
      QVERIFY2(err.contains(QStringLiteral("渐变带仅相变边界可携带")), qPrintable(err));

      // 相变 + 带宽整单通过：两字段落 edit buffer，渲染器重挂（渐变带字段
      // 存在 → 相变类目符号三层）。
      QVERIFY2(m_ctx->compositionWf()->saveFaciesAttributes(
                   QStringLiteral("facies.T1"),
                   QVariantMap{{QStringLiteral("boundary_kind"), QStringLiteral("facies_change")},
                               {QStringLiteral("transition_width"), 500.0}},
                   &err),
               qPrintable(err));
      {
        QgsFeature got;
        QVERIFY(vl->getFeatures(QgsFeatureRequest(first.id())).nextFeature(got));
        QCOMPARE(got.attribute(QStringLiteral("boundary_kind")).toString(),
                 QStringLiteral("facies_change"));
        QCOMPARE(got.attribute(QStringLiteral("transition_width")).toDouble(), 500.0);
        auto *cat = static_cast<QgsCategorizedSymbolRenderer *>(vl->renderer());
        QCOMPARE(cat->classAttribute(), QStringLiteral("boundary_kind"));
        for (const QgsRendererCategory &c : cat->categories())
          if (c.value() == QVariant(QStringLiteral("facies_change")))
            QCOMPARE(c.symbol()->symbolLayerCount(), 2); // SimpleFill(点线描边)/渐变带层
      }

      // 换类型不携带宽键 → 过（旧带宽惰性保留，不锁死类型切换；带域门禁
      // 只判本次提交的带宽提案）。尖灭两侧无门禁，此处可换。
      QVERIFY2(m_ctx->compositionWf()->saveFaciesAttributes(
                   QStringLiteral("facies.T1"),
                   QVariantMap{{QStringLiteral("boundary_kind"), QStringLiteral("pinchout")}},
                   &err),
               qPrintable(err));
      {
        QgsFeature got;
        QVERIFY(vl->getFeatures(QgsFeatureRequest(first.id())).nextFeature(got));
        QCOMPARE(got.attribute(QStringLiteral("boundary_kind")).toString(),
                 QStringLiteral("pinchout"));
        QCOMPARE(got.attribute(QStringLiteral("transition_width")).toDouble(), 500.0); // 惰性保留
      }

      while (vl->undoStack()->canUndo())
        vl->undoStack()->undo();
      if (auto *tb = m_win->findChild<PaleoEditingToolbar *>(QStringLiteral("editingToolbar")))
        tb->cancelEditing();
    }

    // 方向 39：边界核查端到端——runFaciesBoundaryQa（faciesqa 引擎只接不
    // 重写）按 boundary_kind 出核查项，报告有名有因。相变无渐变范围 →
    // transition_band_missing；绕过门禁手改的整合接触切两侧（模拟外部
    // 手编数据）→ conformable_cuts_facies。
    void boundaryQaReportsByKind()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      QVERIFY2(m_ctx->projectSvc()->createProject(dir.filePath(QStringLiteral("qa.qgz"))),
               "createProject failed");
      // 4×4 上下两半：默认 MIN_CELLS=4 下每类 8 像元才不被小图斑聚合吃掉。
      const QVector<float> px = {1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2};
      const QString path =
          makeRaster(dir.filePath(QStringLiteral("coded_q.tif")), 4, 4, px);
      QVERIFY(!path.isEmpty());
      QString err;
      QVERIFY2(m_ctx->layerSvc()->declare(
                   decl(QStringLiteral("composite.T1"), QStringLiteral("T1"),
                        QStringLiteral("raster"), path, QStringLiteral("03_Composite")),
                   &err),
               qPrintable(err));
      QVERIFY2(m_ctx->compositionWf()->deriveFaciesPolygons(
                   QStringLiteral("T1"), QStringLiteral("composite.T1"), QVariantMap(), &err),
               qPrintable(err));
      auto *vl = qobject_cast<QgsVectorLayer *>(
          m_ctx->layerSvc()->instantiate(QStringLiteral("facies.T1")));
      QVERIFY2(vl, "facies.T1 did not instantiate");

      QgsFeature first;
      QgsFeatureIterator it = vl->getFeatures();
      QVERIFY(it.nextFeature(first));
      vl->select(first.id());

      // 相变（无渐变带宽）→ 核查项：transition_band_missing 有名有因。
      QVERIFY2(m_ctx->compositionWf()->saveFaciesAttributes(
                   QStringLiteral("facies.T1"),
                   QVariantMap{{QStringLiteral("boundary_kind"), QStringLiteral("facies_change")}},
                   &err),
               qPrintable(err));
      QVariantList report = m_ctx->compositionWf()->runFaciesBoundaryQa(
          QStringLiteral("facies.T1"), &err);
      QVERIFY2(err.isEmpty(), qPrintable(err));
      QCOMPARE(report.size(), 1);
      {
        const QVariantMap row = report.first().toMap();
        QCOMPARE(row.value(QStringLiteral("name")).toString(),
                 QStringLiteral("transition_band_missing"));
        QVERIFY2(row.value(QStringLiteral("reason")).toString().contains(
                     QStringLiteral("相变带无渐变范围")),
                 qPrintable(row.value(QStringLiteral("reason")).toString()));
        QVERIFY(!row.value(QStringLiteral("regionIds")).toStringList().isEmpty());
      }

      // 绕过门禁手改整合接触（外部手编数据路径）→ conformable_cuts_facies。
      const int kindIdx = vl->fields().lookupField(QStringLiteral("boundary_kind"));
      QVERIFY(kindIdx >= 0);
      vl->beginEditCommand(QStringLiteral("手改整合接触"));
      vl->changeAttributeValue(first.id(), kindIdx, QStringLiteral("conformable"));
      vl->endEditCommand();
      report = m_ctx->compositionWf()->runFaciesBoundaryQa(QStringLiteral("facies.T1"), &err);
      QVERIFY2(err.isEmpty(), qPrintable(err));
      QCOMPARE(report.size(), 1);
      {
        const QVariantMap row = report.first().toMap();
        QCOMPARE(row.value(QStringLiteral("name")).toString(),
                 QStringLiteral("conformable_cuts_facies"));
        QVERIFY2(row.value(QStringLiteral("reason")).toString().contains(
                     QStringLiteral("整合接触边界切两侧相")),
                 qPrintable(row.value(QStringLiteral("reason")).toString()));
        QCOMPARE(row.value(QStringLiteral("regionIds")).toStringList().size(), 2);
      }

      // 修复态（相变补带宽）零核查项：回到相变 + 带 500 → transition 缺失
      // 消失，且两侧不再声明整合 → 无项。
      vl->beginEditCommand(QStringLiteral("修复相变带"));
      vl->changeAttributeValue(first.id(), kindIdx, QStringLiteral("facies_change"));
      vl->endEditCommand();
      QVERIFY2(m_ctx->compositionWf()->saveFaciesAttributes(
                   QStringLiteral("facies.T1"),
                   QVariantMap{{QStringLiteral("transition_width"), 500.0}}, &err),
               qPrintable(err)); // 门禁读现值 facies_change → 带宽可过
      report = m_ctx->compositionWf()->runFaciesBoundaryQa(QStringLiteral("facies.T1"), &err);
      QVERIFY2(err.isEmpty(), qPrintable(err));
      QCOMPARE(report.size(), 0);

      while (vl->undoStack()->canUndo())
        vl->undoStack()->undo();
      if (auto *tb = m_win->findChild<PaleoEditingToolbar *>(QStringLiteral("editingToolbar")))
        tb->cancelEditing();
    }

    // 方向 39 Oracle 1：着色 round-trip——赋类 → 渲染器按类型分类 →
    // 提交 + 工程存取（write/close/open + 重实例化）后数据与样式不回退。
    void boundaryStyleSurvivesProjectRoundTrip()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      const QString qgz = dir.filePath(QStringLiteral("rt.qgz"));
      QVERIFY2(m_ctx->projectSvc()->createProject(qgz), "createProject failed");
      const QVector<float> px = {1, 1, 2, 2};
      const QString path =
          makeRaster(dir.filePath(QStringLiteral("coded_r.tif")), 2, 2, px);
      QVERIFY(!path.isEmpty());
      QString err;
      QVERIFY2(m_ctx->layerSvc()->declare(
                   decl(QStringLiteral("composite.T1"), QStringLiteral("T1"),
                        QStringLiteral("raster"), path, QStringLiteral("03_Composite")),
                   &err),
               qPrintable(err));
      QVERIFY2(m_ctx->compositionWf()->deriveFaciesPolygons(
                   QStringLiteral("T1"), QStringLiteral("composite.T1"), QVariantMap(), &err),
               qPrintable(err));
      auto *vl = qobject_cast<QgsVectorLayer *>(
          m_ctx->layerSvc()->instantiate(QStringLiteral("facies.T1")));
      QVERIFY2(vl, "facies.T1 did not instantiate");

      QgsFeature first;
      QgsFeatureIterator it = vl->getFeatures();
      QVERIFY(it.nextFeature(first));
      vl->select(first.id());
      QVERIFY2(m_ctx->compositionWf()->saveFaciesAttributes(
                   QStringLiteral("facies.T1"),
                   QVariantMap{{QStringLiteral("boundary_kind"), QStringLiteral("fault_cut")}},
                   &err),
               qPrintable(err));
      QCOMPARE(vl->renderer()->type(), QStringLiteral("categorizedSymbol"));

      // 提交编辑（只读派生件由 saveFaciesAttributes 自愈铺可写工作副本）。
      vl = qobject_cast<QgsVectorLayer *>(
          m_ctx->layerSvc()->instantiate(QStringLiteral("facies.T1")));
      QVERIFY(vl && vl->isEditable());
      QVERIFY2(vl->commitChanges(), "commitChanges failed");
      vl->removeSelection();
      // 会话卫生：先收编辑态（同其它用例口径——复位 vertex tool/编辑条），
      // 再走工程存取；tst_rehydrate 先例 = write → open（read 自带 clear）。
      if (auto *tb = m_win->findChild<PaleoEditingToolbar *>(QStringLiteral("editingToolbar")))
        tb->cancelEditing();

      // 工程存取：write → open → 重实例化（instantiate 从源新建，
      // 样式靠 layerInstantiated 钩子重建）。
      QVERIFY2(m_ctx->projectSvc()->writeProject(), "writeProject failed");
      QVERIFY2(m_ctx->projectSvc()->openProject(qgz), "openProject failed");
      vl = qobject_cast<QgsVectorLayer *>(
          m_ctx->layerSvc()->instantiate(QStringLiteral("facies.T1")));
      QVERIFY2(vl, "facies.T1 did not re-instantiate after reopen");

      QCOMPARE(vl->renderer()->type(), QStringLiteral("categorizedSymbol"));
      auto *cat = static_cast<QgsCategorizedSymbolRenderer *>(vl->renderer());
      QCOMPARE(cat->classAttribute(), QStringLiteral("boundary_kind"));
      bool hasFault = false;
      for (const QgsRendererCategory &c : cat->categories())
        if (c.value() == QVariant(QStringLiteral("fault_cut")))
          hasFault = true;
      QVERIFY2(hasFault, "fault_cut category survives reopen");
      QgsFeature got;
      QVERIFY(vl->getFeatures(QgsFeatureRequest(first.id())).nextFeature(got));
      QCOMPARE(got.attribute(QStringLiteral("boundary_kind")).toString(),
               QStringLiteral("fault_cut")); // 数据经提交往返仍在
    }

    // 方向 39：页面可用面——下拉四类冻结词面（Oracle 3 的 UI 侧）+ 渐变带
    // 宽 spin 仅相变启用（禁用带 reason tooltip）+ 边界核查按钮在册。
    void boundaryUiAffordances()
    {
      m_win->showPage(QStringLiteral("compose"));
      auto *combo = m_win->findChild<QComboBox *>(QStringLiteral("faciesBoundaryKindCombo"));
      auto *spin = m_win->findChild<QDoubleSpinBox *>(QStringLiteral("faciesTransitionWidthSpin"));
      auto *qaButton = m_win->findChild<QPushButton *>(QStringLiteral("faciesBoundaryQaButton"));
      QVERIFY(combo && spin && qaButton);

      // 下拉词面：「无（未分类）」+ 冻结词面四类（与 boundarysemantics.h
      // titleFor 逐字一致）。
      QStringList items;
      for (int i = 0; i < combo->count(); ++i)
        items << combo->itemText(i);
      const QStringList expected = { QStringLiteral("无（未分类）"),
                                     QStringLiteral("整合接触"), QStringLiteral("尖灭"),
                                     QStringLiteral("相变"), QStringLiteral("断层切割") };
      QCOMPARE(items, expected);
      // data 侧 = 稳定 id。
      QCOMPARE(combo->itemData(1).toString(), QStringLiteral("conformable"));
      QCOMPARE(combo->itemData(2).toString(), QStringLiteral("pinchout"));
      QCOMPARE(combo->itemData(3).toString(), QStringLiteral("facies_change"));
      QCOMPARE(combo->itemData(4).toString(), QStringLiteral("fault_cut"));

      // spin 使能随类型：默认（无）禁用 + reason tooltip；相变启用。
      QVERIFY(!spin->isEnabled());
      QVERIFY(!spin->toolTip().isEmpty());
      combo->setCurrentIndex(3); // 相变
      QVERIFY(spin->isEnabled());
      combo->setCurrentIndex(1); // 整合接触
      QVERIFY(!spin->isEnabled());
    }
};

int main(int argc, char *argv[])
{
  if (qgetenv("QT_QPA_PLATFORM").isEmpty())
    qputenv("QT_QPA_PLATFORM", "offscreen");
  AppContext ctx(QStringLiteral("/usr"));
  if (!ctx.ready())
    qFatal("AppContext failed to initialize the QGIS runtime");
  TestComposeWorkflow tc(&ctx);
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_composeworkflow.moc"
