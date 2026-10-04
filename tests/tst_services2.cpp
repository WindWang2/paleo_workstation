#include <QtTest>
#include <QTemporaryDir>
#include <QSignalSpy>

#include <qgsapplication.h>
#include <qgscategorizedsymbolrenderer.h>
#include <qgsexpression.h>
#include <qgsexpressioncontext.h>
#include <qgsgeometry.h>
#include <qgslayout.h>
#include <qgslayoutitemlabel.h>
#include <qgslayoutmanager.h>
#include <qgslayoutpagecollection.h>
#include <qgsfillsymbollayer.h>
#include <qgslinesymbollayer.h>
#include <qgsmarkersymbol.h>
#include <qgsnativealgorithms.h>
#include <qgsprocessingregistry.h>
#include <qgsproject.h>
#include <qgsrendercontext.h>
#include <qgsrenderer.h>
#include <qgssinglesymbolrenderer.h>
#include <qgsvectorlayer.h>
#include <qgsvectorlayerlabeling.h>

#include "../src/algorithms/paleoalgorithms.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/qgiseditingservice.h"
#include "../src/qgis/qgislayoutservice.h"
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/qgis/qgisstyleservice.h"

// Wave-2 services acceptance:
//  - editing sessions mark the layer busy in PaleoProjectStore ("edit" task,
//    "editing in progress" reason) and free it on commit AND on rollback/failure;
//  - processing runs a registry algorithm synchronously and remaps unspecified
//    destination params into a QTemporaryDir (§41.2 temp-then-merge contract);
//  - style service applies stylesRoot/<ref>.qml via loadNamedStyle;
//  - layout service drives QgsLayoutManager CRUD + PDF export.
class TestServices2 : public QObject
{
  Q_OBJECT

private slots:
  void initTestCase()
  {
    QVERIFY(QgsApplication::instance() != nullptr);
    // initQgis() does not auto-register providers in this build — the native
    // provider is added here so "native:buffer" exists regardless of which
    // other test binaries ran first (each test binary is its own process).
    QgsProcessingRegistry *reg = QgsApplication::processingRegistry();
    if (!reg->algorithmById(QStringLiteral("native:buffer")))
      QVERIFY(reg->addProvider(new QgsNativeAlgorithms(reg)));
    QVERIFY(reg->algorithmById(QStringLiteral("native:buffer")) != nullptr);
  }

  // (a) begin -> busy("edit","editing in progress"); commit via store queue ->
  // feature persisted, layer freed, signals fired
  void editingCommitMarksAndFrees()
  {
    PaleoProjectStore store;
    QgisEditingService svc(&store);
    QSignalSpy startedSpy(&svc, &QgisEditingService::editStarted);
    QSignalSpy committedSpy(&svc, &QgisEditingService::editCommitted);

    QgsVectorLayer layer(QStringLiteral("Point?crs=EPSG:4326"), QStringLiteral("pts"), QStringLiteral("memory"));
    QVERIFY(layer.isValid());
    QVERIFY(!store.layerBusy(layer.id()));

    QString err;
    QVERIFY2(svc.beginEdit(&layer, &err), qPrintable(err));
    QVERIFY(svc.isEditing(&layer));

    QString reason;
    QVERIFY(store.layerBusy(layer.id(), &reason));
    QCOMPARE(reason, QStringLiteral("edit — editing in progress")); // "taskId — reason" contract
    QCOMPARE(startedSpy.count(), 1);
    QCOMPARE(startedSpy.at(0).at(0).toString(), layer.id());

    // edits happen against the layer's edit buffer
    QgsFeature f(layer.fields());
    f.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(1.0, 2.0)));
    QVERIFY(layer.addFeature(f));

    QVERIFY2(svc.commitEdit(&layer, &err), qPrintable(err));
    QVERIFY(!svc.isEditing(&layer));      // commitChanges(stopEditing=true)
    QVERIFY(!store.layerBusy(layer.id())); // freed on success
    QCOMPARE(committedSpy.count(), 1);
    QCOMPARE(committedSpy.at(0).at(0).toString(), layer.id());
    QCOMPARE(layer.featureCount(), 1);    // edit buffer flushed to the provider
  }

  // (b) rollback discards the buffer and frees the layer
  void editingRollbackFrees()
  {
    PaleoProjectStore store;
    QgisEditingService svc(&store);
    QSignalSpy rolledBackSpy(&svc, &QgisEditingService::editRolledBack);

    QgsVectorLayer layer(QStringLiteral("Point?crs=EPSG:4326"), QStringLiteral("pts"), QStringLiteral("memory"));
    QVERIFY(layer.isValid());

    QVERIFY(svc.beginEdit(&layer));
    QVERIFY(store.layerBusy(layer.id()));

    QgsFeature f(layer.fields());
    f.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(3.0, 4.0)));
    QVERIFY(layer.addFeature(f));

    QVERIFY(svc.rollbackEdit(&layer));
    QVERIFY(!svc.isEditing(&layer));
    QVERIFY(!store.layerBusy(layer.id()));
    QCOMPARE(rolledBackSpy.count(), 1);
    QCOMPARE(layer.featureCount(), 0); // buffer discarded
  }

  // (c) commit 失败的 busy 语义（合并后精化版，见 qgiseditingservice.cpp
  // commitEdit「orphanEdit」）：真失败保留活编辑会话供重试/回滚；无编辑态
  // 的孤儿 edit 标记即清；他人 owner（processing 等）永不被提交错误释放。
  // 正常会话「回滚必释放」由 editingRollbackFrees 覆盖。
  void editingCommitFailureFreesOrphanEditMarkOnly()
  {
    PaleoProjectStore store;
    QgisEditingService svc(&store);

    QgsVectorLayer layer(QStringLiteral("Point?crs=EPSG:4326"), QStringLiteral("pts"), QStringLiteral("memory"));
    QVERIFY(layer.isValid());
    // simulate a session whose busy mark outlived the edit state
    store.markLayerBusy(layer.id(), QStringLiteral("edit"), QStringLiteral("editing in progress"));

    QString err;
    QVERIFY(!svc.commitEdit(&layer, &err)); // commitChanges on non-editable layer -> false
    QVERIFY(!err.isEmpty());
    QVERIFY(!store.layerBusy(layer.id())); // 孤儿 edit 标记即清（无会话可重试）
    store.markLayerBusy(layer.id(), QStringLiteral("processing"), QStringLiteral("processing in progress"));
    QVERIFY(!svc.commitEdit(&layer, &err));
    QVERIFY(store.layerBusy(layer.id())); // 提交错误不能释放别的任务所有者。
    store.markLayerFree(layer.id());

    // null layer is a clean error, not a crash
    err.clear();
    QVERIFY(!svc.beginEdit(nullptr, &err));
    QVERIFY(!err.isEmpty());
  }

  // (d) run() resolves a registry algorithm, remaps the missing OUTPUT into the
  // service temp dir, and the result map points at a real, loadable layer file
  void processingRunNativeAlgLandsTempOutput()
  {
    PaleoProjectStore store;
    QgisProcessingService svc(&store);

    QgsVectorLayer layer(QStringLiteral("Point?crs=EPSG:4326&field=id:integer"), QStringLiteral("pts"), QStringLiteral("memory"));
    QVERIFY(layer.isValid());
    QgsFeature f(layer.fields());
    f.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(10.0, 20.0)));
    QVERIFY(layer.dataProvider()->addFeature(f));
    layer.updateExtents();
    QCOMPARE(layer.featureCount(), 1);

    QVariantMap params;
    params.insert(QStringLiteral("INPUT"), QVariant::fromValue(&layer));
    params.insert(QStringLiteral("DISTANCE"), 10.0);
    // no OUTPUT on purpose: the service must remap destination params itself

    QString err;
    const QVariantMap out = svc.run(QStringLiteral("native:buffer"), params, &err);
    QVERIFY2(!out.isEmpty(), qPrintable(err));

    const QString outPath = out.value(QStringLiteral("OUTPUT")).toString();
    QVERIFY2(!outPath.isEmpty(), "expected OUTPUT key in algorithm results");
    QVERIFY2(QFile::exists(outPath), qPrintable(outPath)); // landed on disk

    QgsVectorLayer buffered(outPath, QStringLiteral("buffered"), QStringLiteral("ogr"));
    QVERIFY2(buffered.isValid(), qPrintable(buffered.error().message()));
    QVERIFY(buffered.featureCount() >= 1);
  }

  // (e) unknown algorithm id -> empty map + error, no crash
  void processingRunUnknownAlgFailsCleanly()
  {
    PaleoProjectStore store;
    QgisProcessingService svc(&store);
    QString err;
    const QVariantMap out = svc.run(QStringLiteral("nope:does_not_exist"), QVariantMap(), &err);
    QVERIFY(out.isEmpty());
    QVERIFY(!err.isEmpty());
  }

  // (f) paleoAlgorithmIds returns only paleo namespace ids from the registry.
  // Tolerant both ways: 0 entries while paleoalgorithms.cpp is unlinked, >0
  // after integration — but never a non-paleo id.
  void paleoAlgorithmIdsFiltered()
  {
    PaleoProjectStore store;
    QgisProcessingService svc(&store);
    const QStringList ids = svc.paleoAlgorithmIds();
    QVERIFY(!ids.contains(QStringLiteral("native:buffer")));
    for (const QString &id : ids)
      QVERIFY2(id.startsWith(QStringLiteral("paleo")), qPrintable(id));
  }

  // (g) applyStyle loads stylesRoot/<ref>.qml; availableStyles lists basenames
  void styleApplyFromStylesRoot()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    // hand-written minimal QGIS 4.x .qml: single red marker symbol
    const QString qml = QStringLiteral(
      "<!DOCTYPE qgis PUBLIC 'http://mrcc.com/qgis.dtd' 'SYSTEM'>\n"
      "<qgis version=\"4.2.2\" styleCategories=\"Symbology\">\n"
      "  <renderer-v2 type=\"singleSymbol\" enableorderby=\"0\" forceraster=\"0\" referencescale=\"-1\" symbollevels=\"0\">\n"
      "    <symbols>\n"
      "      <symbol alpha=\"1\" clip_to_extent=\"1\" force_rhr=\"0\" frame_rate=\"10\" is_animated=\"0\" name=\"0\" type=\"marker\">\n"
      "        <layer class=\"SimpleMarker\" enabled=\"1\" id=\"{00000000-0000-0000-0000-000000000001}\" locked=\"0\" pass=\"0\">\n"
      "          <Option type=\"Map\">\n"
      "            <Option name=\"name\" type=\"QString\" value=\"circle\"/>\n"
      "            <Option name=\"color\" type=\"QString\" value=\"200,30,30,255,rgb:0.7843137,0.1176471,0.1176471,1\"/>\n"
      "            <Option name=\"size\" type=\"QString\" value=\"4\"/>\n"
      "            <Option name=\"size_unit\" type=\"QString\" value=\"MM\"/>\n"
      "          </Option>\n"
      "        </layer>\n"
      "      </symbol>\n"
      "    </symbols>\n"
      "    <rotation/>\n"
      "    <sizescale/>\n"
      "  </renderer-v2>\n"
      "</qgis>\n");
    QFile qmlFile(tmp.filePath(QStringLiteral("redpoint.qml")));
    QVERIFY(qmlFile.open(QIODevice::WriteOnly));
    QCOMPARE(qmlFile.write(qml.toUtf8()), qint64(qml.toUtf8().size()));
    qmlFile.close();
    // a non-style file must not appear in availableStyles
    QFile other(tmp.filePath(QStringLiteral("notes.txt")));
    QVERIFY(other.open(QIODevice::WriteOnly));
    other.write("not a style");
    other.close();

    QgisStyleService svc;
    svc.setStylesRoot(tmp.path());
    QCOMPARE(svc.availableStyles(), QStringList({QStringLiteral("redpoint")}));

    QgsVectorLayer layer(QStringLiteral("Point?crs=EPSG:4326"), QStringLiteral("pts"), QStringLiteral("memory"));
    QVERIFY(layer.isValid());
    QVERIFY(layer.renderer() != nullptr);

    QString err;
    QVERIFY2(svc.applyStyle(&layer, QStringLiteral("redpoint"), &err), qPrintable(err));
    QCOMPARE(layer.renderer()->type(), QStringLiteral("singleSymbol"));

    // missing ref -> false + error
    QVERIFY(!svc.applyStyle(&layer, QStringLiteral("ghost"), &err));
    QVERIFY(!err.isEmpty());
    // null layer -> false + error
    QVERIFY(!svc.applyStyle(nullptr, QStringLiteral("redpoint"), &err));
    QVERIFY(!err.isEmpty());
  }

  // 测井点：标记保持统一圆点；有沉积相字段时文字标注带相名。
  void wellPointLabelShowsFacies()
  {
    QgsVectorLayer layer(
        QStringLiteral("Point?field=name:string&field=facies_label:string&field=microfacies:string&crs=EPSG:4326"),
        QStringLiteral("wells"), QStringLiteral("memory"));
    QVERIFY(layer.isValid());
    QgsFeature feature(layer.fields());
    feature.setAttribute(QStringLiteral("name"), QStringLiteral("A1"));
    feature.setAttribute(QStringLiteral("facies_label"), QString::fromUtf8("水下分流河道"));
    feature.setAttribute(QStringLiteral("microfacies"), QString::fromUtf8("河口坝"));
    feature.setGeometry(QgsGeometry::fromWkt(QStringLiteral("POINT(1 2)")));
    QVERIFY(layer.dataProvider()->addFeature(feature));

    QgisStyleService::applyWellLayerStyle(&layer);
    auto *single = dynamic_cast<QgsSingleSymbolRenderer *>(layer.renderer());
    QVERIFY(single);
    QVERIFY(single->symbol());
    QCOMPARE(single->symbol()->color().name().toUpper(), QStringLiteral("#24303E"));
    QVERIFY(layer.labelsEnabled());
    QVERIFY(layer.labeling());
    QVERIFY(layer.labeling()->settings().isExpression);
    QgsExpression expr(layer.labeling()->settings().fieldName);
    QVERIFY2(!expr.hasParserError(), qPrintable(expr.parserErrorString()));
    QgsExpressionContext ctx;
    ctx.setFields(layer.fields());
    ctx.setFeature(feature);
    QCOMPARE(expr.evaluate(&ctx).toString(), QString::fromUtf8("A1\n水下分流河道"));
    feature.setAttribute(QStringLiteral("facies_label"), QString());
    ctx.setFeature(feature);
    QCOMPARE(expr.evaluate(&ctx).toString(), QString::fromUtf8("A1\n河口坝"));

    QgsVectorLayer plain(QStringLiteral("Point?field=name:string&crs=EPSG:4326"),
                         QStringLiteral("plain"), QStringLiteral("memory"));
    QVERIFY(plain.isValid());
    QgisStyleService::applyWellLayerStyle(&plain);
    QVERIFY(plain.labeling());
    QVERIFY(!plain.labeling()->settings().isExpression);
    QCOMPARE(plain.labeling()->settings().fieldName, QStringLiteral("name"));
    auto *plainSymbol = dynamic_cast<QgsSingleSymbolRenderer *>(plain.renderer());
    QVERIFY(plainSymbol);
    QCOMPARE(plainSymbol->symbol()->color().name().toUpper(), QStringLiteral("#24303E"));
  }

  // C3（wave/deepen-perf）：井类别符号（表 K.1 十二类 + 方向 31 开发区块
  // 四类 = 16 类）——数据字段驱动分类渲染；无类别字段回落通用「探井」
  //（单符号）；词表 + 归一化（中文词面/同义词/未知原样）+ 比例尺缩放
  //（@map_scale 数据定义尺寸）。
  void wellCategoryStyleDataDriven()
  {
    QCOMPARE(QgisStyleService::wellCategoryDefinitions().size(), 16);
    const QVariantMap first = QgisStyleService::wellCategoryDefinitions().first().toMap();
    QCOMPARE(first.value(QStringLiteral("id")).toString(), QStringLiteral("wildcat"));
    QCOMPARE(first.value(QStringLiteral("title")).toString(), QStringLiteral("预探井"));
    const QVariantMap last = QgisStyleService::wellCategoryDefinitions().last().toMap();
    QCOMPARE(last.value(QStringLiteral("id")).toString(), QStringLiteral("water_prod"));

    // 归一化：规范 id / 中文词面 / 常见同义词 → id；未知原样；空 → 空。
    QCOMPARE(QgisStyleService::normalizeWellCategory(QStringLiteral("wildcat")),
             QStringLiteral("wildcat"));
    QCOMPARE(QgisStyleService::normalizeWellCategory(QStringLiteral("预探井")),
             QStringLiteral("wildcat"));
    QCOMPARE(QgisStyleService::normalizeWellCategory(QStringLiteral("工业气流")),
             QStringLiteral("gas_flow"));
    QCOMPARE(QgisStyleService::normalizeWellCategory(QStringLiteral(" 评价井 ")),
             QStringLiteral("appraisal"));
    QCOMPARE(QgisStyleService::normalizeWellCategory(QStringLiteral("注水井")),
             QStringLiteral("water_injection"));
    QCOMPARE(QgisStyleService::normalizeWellCategory(QStringLiteral("开发井")),
             QStringLiteral("oil_prod"));
    QCOMPARE(QgisStyleService::normalizeWellCategory(QStringLiteral("神秘井")),
             QStringLiteral("神秘井"));
    QVERIFY(QgisStyleService::normalizeWellCategory(QString()).isEmpty());

    // 无字段 / 空字段名 → 单符号通用「探井」（外细环+实心盘两层）。
    QgsVectorLayer noField(QStringLiteral("Point?crs=EPSG:4326&field=z:double"),
                           QStringLiteral("w1"), QStringLiteral("memory"));
    QVERIFY(noField.isValid());
    QgisStyleService::applyWellCategoryStyle(&noField, QString());
    QCOMPARE(noField.renderer()->type(), QStringLiteral("singleSymbol"));
    auto *single = static_cast<QgsSingleSymbolRenderer *>(noField.renderer());
    QCOMPARE(single->symbol()->symbolLayerCount(), 2);

    // 方向 31：比例尺缩放——@map_scale 数据定义尺寸挂全类；两个比例尺档
    // 位求值 6mm/3mm（构造层各自基准 6mm，setDataDefinedSize 语义=整符号
    // 按值重设尺寸，多层按比例同步）。
    auto *singleMarker = static_cast<QgsMarkerSymbol *>(single->symbol());
    const QString ddExpr = singleMarker->dataDefinedSize().asExpression();
    QVERIFY(!ddExpr.isEmpty());
    QVERIFY(ddExpr.contains(QStringLiteral("@map_scale")));
    for (const auto &scaleAndSize : {QPair<double, double>{250000.0, 6.0},
                                     QPair<double, double>{2500000.0, 3.0}})
    {
      QgsExpressionContext ctx;
      auto *scope = new QgsExpressionContextScope();
      scope->setVariable(QStringLiteral("map_scale"), scaleAndSize.first);
      ctx.appendScope(scope);
      QgsExpression expr(ddExpr);
      QVERIFY(expr.isValid());
      QCOMPARE(expr.evaluate(&ctx).toDouble(), scaleAndSize.second);
    }

    // 有类别字段 → 分类渲染：id 与中文词面双桶；wildcat（双环）、
    // gas_flow（红环+斜线）与 oil_prod（开发区块：环+盘+白点）层数可区分。
    QgsVectorLayer withField(
        QStringLiteral("Point?crs=EPSG:4326&field=well_class:string"),
        QStringLiteral("w2"), QStringLiteral("memory"));
    QVERIFY(withField.isValid());
    QgisStyleService::applyWellCategoryStyle(&withField, QStringLiteral("well_class"));
    QCOMPARE(withField.renderer()->type(), QStringLiteral("categorizedSymbol"));
    auto *cat = static_cast<QgsCategorizedSymbolRenderer *>(withField.renderer());
    QCOMPARE(cat->classAttribute(), QStringLiteral("well_class"));
    QSet<QString> values;
    int wildcatLayers = -1, gasFlowLayers = -1, oilProdLayers = -1;
    for (const QgsRendererCategory &c : cat->categories())
    {
      if (c.value().isValid() && !c.value().toString().isEmpty())
        values.insert(c.value().toString());
      if (c.value() == QVariant(QStringLiteral("wildcat")))
        wildcatLayers = c.symbol()->symbolLayerCount();
      if (c.value() == QVariant(QStringLiteral("gas_flow")))
        gasFlowLayers = c.symbol()->symbolLayerCount();
      if (c.value() == QVariant(QStringLiteral("oil_prod")))
        oilProdLayers = c.symbol()->symbolLayerCount();
    }
    QVERIFY(values.contains(QStringLiteral("wildcat")));
    QVERIFY(values.contains(QStringLiteral("预探井"))); // 中文词面同桶命中
    QVERIFY(values.contains(QStringLiteral("abandoned")));
    QVERIFY(values.contains(QStringLiteral("water_injection")));
    QCOMPARE(wildcatLayers, 2); // 双环
    QCOMPARE(gasFlowLayers, 2); // 红环 + 斜线
    QCOMPARE(oilProdLayers, 3); // 墨环 + 绿盘 + 中心白点
    // 字段名不存在 → 同无字段路径（不抛、单符号）。
    QgsVectorLayer ghost(QStringLiteral("Point?crs=EPSG:4326"), QStringLiteral("w3"),
                         QStringLiteral("memory"));
    QgisStyleService::applyWellCategoryStyle(&ghost, QStringLiteral("nope"));
    QCOMPARE(ghost.renderer()->type(), QStringLiteral("singleSymbol"));
  }

  // C2（wave/deepen-perf）：相界语义符号——boundary_kind 分类渲染；无字段
  // 的面层不接管；非面层不接管。
  void faciesBoundaryStyleOnlyWithKindField()
  {
    QgsVectorLayer plain(QStringLiteral("Polygon?crs=EPSG:4326"), QStringLiteral("p1"),
                         QStringLiteral("memory"));
    QVERIFY(plain.isValid());
    const QgsFeatureRenderer *before = plain.renderer();
    QgisStyleService::applyFaciesBoundaryStyle(&plain);
    QCOMPARE(plain.renderer(), before); // 无字段 → 渲染器不动

    QgsVectorLayer withKind(QStringLiteral("Polygon?crs=EPSG:4326&field=boundary_kind:string"),
                            QStringLiteral("p2"), QStringLiteral("memory"));
    QVERIFY(withKind.isValid());
    QgisStyleService::applyFaciesBoundaryStyle(&withKind);
    QCOMPARE(withKind.renderer()->type(), QStringLiteral("categorizedSymbol"));
    auto *cat = static_cast<QgsCategorizedSymbolRenderer *>(withKind.renderer());
    QCOMPARE(cat->classAttribute(), QStringLiteral("boundary_kind"));
    bool hasFault = false, hasEmpty = false;
    for (const QgsRendererCategory &c : cat->categories())
    {
      if (c.value().toString() == QStringLiteral("fault_cut"))
        hasFault = true;
      if (c.value().type() == QVariant::String && c.value().toString().isEmpty())
        hasEmpty = true; // 空串（未标类型）落常规相界
    }
    QVERIFY(hasFault);
    QVERIFY(hasEmpty);

    QgsVectorLayer points(QStringLiteral("Point?crs=EPSG:4326&field=boundary_kind:string"),
                          QStringLiteral("pt"), QStringLiteral("memory"));
    const QgsFeatureRenderer *beforePts = points.renderer();
    QgisStyleService::applyFaciesBoundaryStyle(&points);
    QCOMPARE(points.renderer(), beforePts); // 非面层不接管
  }

  // 方向 39：四类目全开 + 三类线型（实/虚/点，复用方向 31 相界线型调性）
  // + 相变渐变带符号层（data-defined 绑 transition_width，仅字段存在时挂）
  // + 表外值/空值落中性桶（诚实：不猜类）。
  void faciesBoundaryStyleFourKindsLineTypesAndBand()
  {
    // helper：类目符号的描边笔型（createSimple 的面符号 = 单 SimpleFill 层，
    // 描边由填充层自绘——strokeStyle 即线型）。
    const auto outlinePenOf = [](QgsSymbol *sym) -> Qt::PenStyle {
      if (!sym || sym->symbolLayerCount() < 1)
        return Qt::NoPen;
      auto *fill = dynamic_cast<QgsSimpleFillSymbolLayer *>(sym->symbolLayer(0));
      return fill ? fill->strokeStyle() : Qt::NoPen;
    };

    QgsVectorLayer layer(QStringLiteral(
                             "Polygon?crs=EPSG:4326&field=boundary_kind:string"),
                         QStringLiteral("bk"), QStringLiteral("memory"));
    QVERIFY(layer.isValid());
    QgisStyleService::applyFaciesBoundaryStyle(&layer);
    auto *cat = static_cast<QgsCategorizedSymbolRenderer *>(layer.renderer());

    // 四类目按冻结词面命名（UI/属性/样式三处词面一致的面）；空串与
    // catch-all 两个中性桶同标「常规相界」（共 6 类目）。
    const QStringList expectedTitles = { QStringLiteral("整合接触"), QStringLiteral("尖灭"),
                                         QStringLiteral("相变"), QStringLiteral("断层切割"),
                                         QStringLiteral("常规相界"),
                                         QStringLiteral("常规相界") };
    QStringList titles;
    for (const QgsRendererCategory &c : cat->categories())
      titles << c.label();
    QCOMPARE(titles, expectedTitles);

    // 线型：整合=实线、尖灭=虚线、相变=点线（渐变带字段缺失 → 无带层）。
    const auto categoryFor = [cat](const QString &value) -> const QgsRendererCategory * {
      for (const QgsRendererCategory &c : cat->categories())
        if (c.value() == QVariant(value))
          return &c;
      return nullptr;
    };
    const QgsRendererCategory *conformable = categoryFor(QStringLiteral("conformable"));
    const QgsRendererCategory *pinchout = categoryFor(QStringLiteral("pinchout"));
    const QgsRendererCategory *change = categoryFor(QStringLiteral("facies_change"));
    QVERIFY(conformable && pinchout && change);
    QCOMPARE(outlinePenOf(conformable->symbol()), Qt::SolidLine);
    QCOMPARE(outlinePenOf(pinchout->symbol()), Qt::DashLine);
    QCOMPARE(outlinePenOf(change->symbol()), Qt::DotLine);
    QCOMPARE(change->symbol()->symbolLayerCount(), 1); // 无 transition_width 字段 → 不挂带层

    // 带 transition_width 字段：相变符号两层（SimpleFill 点线描边 + 带层），带层
    // data-defined 宽度+线型绑 transition_width，宽度单位 = 地图单位。
    QgsVectorLayer banded(QStringLiteral(
                              "Polygon?crs=EPSG:4326&field=boundary_kind:string"
                              "&field=transition_width:double"),
                          QStringLiteral("bkb"), QStringLiteral("memory"));
    QVERIFY(banded.isValid());
    QgisStyleService::applyFaciesBoundaryStyle(&banded);
    auto *catB = static_cast<QgsCategorizedSymbolRenderer *>(banded.renderer());
    const QgsRendererCategory *changeB = [catB]() {
      for (const QgsRendererCategory &c : catB->categories())
        if (c.value() == QVariant(QStringLiteral("facies_change")))
          return &c;
      return static_cast<const QgsRendererCategory *>(nullptr);
    }();
    QVERIFY(changeB);
    QCOMPARE(changeB->symbol()->symbolLayerCount(), 2);
    auto *band = dynamic_cast<QgsSimpleLineSymbolLayer *>(changeB->symbol()->symbolLayer(1));
    QVERIFY2(band, "band layer at index 1");
    QCOMPARE(band->widthUnit(), Qgis::RenderUnit::MapUnits);
    QVERIFY(band->dataDefinedProperties().hasProperty(
        QgsSymbolLayer::Property::StrokeWidth));
    QVERIFY(band->dataDefinedProperties().hasProperty(
        QgsSymbolLayer::Property::StrokeStyle));
    QVERIFY(band->dataDefinedProperties()
                .property(QgsSymbolLayer::Property::StrokeWidth)
                .asExpression()
                .contains(QStringLiteral("transition_width")));

    // 表外值与空值 → 中性桶（catch-all/空串类目各承接，不猜类不 crash）。
    QgsFeature weird;
    weird.setGeometry(QgsGeometry::fromWkt(
        QStringLiteral("POLYGON((30 0, 31 0, 31 1, 30 1, 30 0))")));
    QgsAttributes weirdAttrs;
    weirdAttrs << QVariant(QStringLiteral("erosion"));
    weird.setAttributes(weirdAttrs);
    QVERIFY(layer.dataProvider()->addFeature(weird));
    QgsFeature unmarked;
    unmarked.setGeometry(QgsGeometry::fromWkt(
        QStringLiteral("POLYGON((40 0, 41 0, 41 1, 40 1, 40 0))")));
    QgsAttributes emptyAttrs;
    emptyAttrs << QVariant(QString());
    unmarked.setAttributes(emptyAttrs);
    QVERIFY(layer.dataProvider()->addFeature(unmarked));
    QgsSymbol *catchAll = nullptr;
    for (const QgsRendererCategory &c : cat->categories())
      if (!c.value().isValid())
        catchAll = c.symbol();
    QVERIFY(catchAll);
    QgsSymbol *neutralEmpty = categoryFor(QString())->symbol();
    QVERIFY(neutralEmpty);

    QgsFeature gotWeird, gotEmpty;
    QVERIFY(layer.getFeatures(QgsFeatureRequest(weird.id())).nextFeature(gotWeird));
    QVERIFY(layer.getFeatures(QgsFeatureRequest(unmarked.id())).nextFeature(gotEmpty));
    QgsRenderContext context;
    cat->startRender(context, layer.fields());
    QgsSymbol *forWeird = cat->symbolForFeature(gotWeird, context);
    QgsSymbol *forEmpty = cat->symbolForFeature(gotEmpty, context);
    cat->stopRender(context);
    QVERIFY2(forWeird, "out-of-vocab value resolves to the catch-all symbol");
    QVERIFY2(forEmpty, "empty value resolves to a symbol");
    // startRender 可能克隆符号——按视觉属性对表（实线 + 中性灰描边同
    // 「常规相界」类目，不猜类）。
    const auto neutralLike = [](QgsSymbol *sym) {
      auto *fill =
          sym ? dynamic_cast<QgsSimpleFillSymbolLayer *>(sym->symbolLayer(0)) : nullptr;
      return fill && fill->strokeStyle() == Qt::SolidLine &&
             fill->strokeColor() == QColor(QStringLiteral("#5D6E80"));
    };
    QVERIFY(neutralLike(catchAll));
    QVERIFY(neutralLike(neutralEmpty));
    QVERIFY(neutralLike(forWeird));
    QVERIFY(neutralLike(forEmpty));
  }

  // (h) layout lifecycle: create (duplicate rejected) -> pdf export -> remove
  void layoutLifecycleAndPdfExport()
  {
    QgsProject project;
    QgisLayoutService svc(&project);
    QSignalSpy addedSpy(&svc, &QgisLayoutService::layoutAdded);
    QSignalSpy removedSpy(&svc, &QgisLayoutService::layoutRemoved);

    QVERIFY(svc.layoutNames().isEmpty());

    QString err;
    QgsLayout *layout = svc.createLayout(QStringLiteral("Map 1"), &err);
    QVERIFY2(layout != nullptr, qPrintable(err));
    QCOMPARE(svc.layoutNames(), QStringList({QStringLiteral("Map 1")}));
    QCOMPARE(svc.layout(QStringLiteral("Map 1")), layout);
    QCOMPARE(addedSpy.count(), 1);
    QCOMPARE(addedSpy.at(0).at(0).toString(), QStringLiteral("Map 1"));

    // duplicate name rejected with error
    QVERIFY(svc.createLayout(QStringLiteral("Map 1"), &err) == nullptr);
    QVERIFY(!err.isEmpty());

    // give the page real content so the exporter has something to render
    if (layout->pageCollection()->pageCount() == 0)
      layout->pageCollection()->extendByNewPage();
    auto *label = new QgsLayoutItemLabel(layout);
    label->setText(QStringLiteral("paleo layout test"));
    label->attemptSetSceneRect(QRectF(10, 10, 80, 20));
    layout->addLayoutItem(label);

    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString pdfPath = tmp.filePath(QStringLiteral("map1.pdf"));
    err.clear();
    QVERIFY2(svc.exportPdf(QStringLiteral("Map 1"), pdfPath, &err), qPrintable(err));
    QVERIFY(QFile::exists(pdfPath));
    QVERIFY(QFileInfo(pdfPath).size() > 0); // non-empty PDF

    // export of unknown layout -> false + error
    QVERIFY(!svc.exportPdf(QStringLiteral("ghost"), pdfPath, &err));
    QVERIFY(!err.isEmpty());

    QVERIFY(svc.removeLayout(QStringLiteral("Map 1")));
    QCOMPARE(removedSpy.count(), 1);
    QCOMPARE(removedSpy.at(0).at(0).toString(), QStringLiteral("Map 1"));
    QVERIFY(svc.layoutNames().isEmpty());
    QVERIFY(svc.layout(QStringLiteral("Map 1")) == nullptr);
    QVERIFY(!svc.removeLayout(QStringLiteral("Map 1"))); // second remove fails
  }
};

int main(int argc, char *argv[])
{
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")), true); // distro install
  app.initQgis();
  TestServices2 tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_services2.moc"
