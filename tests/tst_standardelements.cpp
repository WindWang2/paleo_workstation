#include <QtTest>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QMap>
#include <QStringList>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <qgslayoutmanager.h>

#include "../src/qgis/standardelements.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/qgis/facieshierarchyrenderer.h"
#include "../src/qgis/faciesmaplegend.h"
#include "../src/qgis/mappingartifactwriter.h"
#include "../src/domain/faciescatalog.h"
#include "../src/domain/facieshierarchy.h"
#include <qgsrenderer.h>
#include <qgsmaplayerlegend.h>
#include <qgslayertree.h>
#include <qgslayertreemodellegendnode.h>
#include <qgsvectorlayer.h>
#include <qgsvectordataprovider.h>
#include <qgsrasterlayer.h>
#include <qgspalettedrasterrenderer.h>
#include <qgslayoutexporter.h>
#include <qgsgeometry.h>

#include <qgslayout.h>
#include <qgslayoutitem.h>
#include <qgslayoutitemlabel.h>
#include <qgslayoutitemlegend.h>
#include <qgslayoutitemmap.h>
#include <qgslayoutitemmapgrid.h>
#include <qgslayoutitemmapoverview.h>
#include <qgslayoutitempage.h>
#include <qgslayoutitempicture.h>
#include <qgslayoutitemscalebar.h>
#include <qgslayoutpagecollection.h>
#include <qgslayoutundostack.h>
#include <qgsprintlayout.h>
#include <qgsproject.h>
#include <qgsreadwritecontext.h>

// 方向 25 M1/M2/M3 核心：标准图件元素工厂 + 三类内容模板骨架。
// 纪律同 tst_layoutdesigner_full：QgsProject 堆分配并故意泄漏（QGIS 4.2.2
// 栈上析构有 QgsProjectStyleSettings 崩溃路径）；版面本身逐用例管理。
class TestStandardElements : public QObject
{
  Q_OBJECT

  private:
    QgsPrintLayout *makeLayout()
    {
      auto *project = new QgsProject(); // leaked on purpose (see class comment)
      auto *layout = new QgsPrintLayout( project );
      layout->initializeDefaults();
      return layout;
    }

    static int countItems( QgsPrintLayout *layout, const QString &id )
    {
      QList<QgsLayoutItem *> items;
      layout->layoutItems( items );
      int n = 0;
      for ( QgsLayoutItem *item : items )
        if ( item->id() == id )
          ++n;
      return n;
    }

    static QgsLayoutItem *findItem( QgsPrintLayout *layout, const QString &id )
    {
      QList<QgsLayoutItem *> items;
      layout->layoutItems( items );
      for ( QgsLayoutItem *item : items )
        if ( item->id() == id )
          return item;
      return nullptr;
    }

  private slots:
    void figureKindVocabulary()
    {
      const QList<PaleoStandardElements::FigureKind> kinds = {
        PaleoStandardElements::FigureKind::WellPosition,
        PaleoStandardElements::FigureKind::SingleFactor,
        PaleoStandardElements::FigureKind::Facies,
      };
      QStringList keys;
      for ( auto kind : kinds )
      {
        const QString key = PaleoStandardElements::figureKindKey( kind );
        QVERIFY( !key.isEmpty() );
        QVERIFY( !keys.contains( key ) );
        keys << key;

        PaleoStandardElements::FigureKind back;
        QVERIFY( PaleoStandardElements::figureKindFromKey( key, &back ) );
        QCOMPARE( back, kind );

        QVERIFY( !PaleoStandardElements::figureKindTitle( kind ).isEmpty() );
      }
      QCOMPARE( keys, QStringList( { QStringLiteral( "well_position" ),
                                     QStringLiteral( "single_factor" ),
                                     QStringLiteral( "facies" ) } ) );
      PaleoStandardElements::FigureKind dummy;
      QVERIFY( !PaleoStandardElements::figureKindFromKey( QStringLiteral( "nope" ), &dummy ) );
      QCOMPARE( PaleoStandardElements::figureKindTitle( PaleoStandardElements::FigureKind::Facies ),
                QStringLiteral( "沉积相图" ) );
    }

    void applyPageSetupResizesPage()
    {
      std::unique_ptr<QgsPrintLayout> layout( makeLayout() );

      PaleoStandardElements::PageSetup a3l;
      a3l.sizeName = QStringLiteral( "A3" );
      a3l.landscape = true;
      QVERIFY( PaleoStandardElements::applyPageSetup( layout.get(), a3l ) );
      QgsLayoutSize paper = layout->pageCollection()->page( 0 )->pageSize();
      QCOMPARE( paper.units(), Qgis::LayoutUnit::Millimeters );
      QVERIFY( qAbs( paper.width() - 420.0 ) < 0.5 );
      QVERIFY( qAbs( paper.height() - 297.0 ) < 0.5 );

      PaleoStandardElements::PageSetup a4p;
      a4p.sizeName = QStringLiteral( "A4" );
      a4p.landscape = false;
      QVERIFY( PaleoStandardElements::applyPageSetup( layout.get(), a4p ) );
      paper = layout->pageCollection()->page( 0 )->pageSize();
      QVERIFY( qAbs( paper.width() - 210.0 ) < 0.5 );
      QVERIFY( qAbs( paper.height() - 297.0 ) < 0.5 );

      QVERIFY( !PaleoStandardElements::applyPageSetup( nullptr, a4p ) );
    }

    void legendContainsOnlyPresentAndEnabledClasses()
    {
      QgsVectorLayer layer("Polygon?field=facies_code:integer&crs=EPSG:3857", "facies", "memory");
      const auto schema = FaciesCatalog::defaults();
      QVariantList allCodes;
      for (const auto &entry : schema) allCodes << entry.toMap().value("code");
      QgsFeatureList features;
      for (const int code : {1, 5}) {
        QgsFeature feature(layer.fields()); feature.setAttribute("facies_code", code);
        feature.setGeometry(QgsGeometry::fromRect(QgsRectangle(code * 100, 0, code * 100 + 100, 100)));
        features << feature;
      }
      QVERIFY(layer.dataProvider()->addFeatures(features)); layer.updateExtents();
      FaciesHierarchyRenderer::apply(&layer, schema, "micro_facies");
      const auto catalogue = FaciesHierarchy::legend(schema, "micro_facies", allCodes);
      auto present = FaciesMapLegend::presentEntries(&layer, catalogue);
      QCOMPARE(present.size(), 2);
      QCOMPARE(present[0].toMap().value("code").toInt(), 1);
      QCOMPARE(present[1].toMap().value("code").toInt(), 5);
      const auto actual = FaciesMapLegend::symbolEntries(&layer,
          layer.customProperty("paleo/faciesDisplayLegend").toList());
      QCOMPARE(present[1].toMap().value("legend_icon").value<QIcon>().pixmap(32, 16).toImage(),
               actual[1].toMap().value("legend_icon").value<QIcon>().pixmap(32, 16).toImage());
      layer.renderer()->checkLegendSymbolItem(present[1].toMap().value("key").toString(), false);
      QCOMPARE(FaciesMapLegend::presentEntries(&layer, catalogue).size(), 1);
      QgsFeatureIds ids;
      for (const auto &feature : features) ids.insert(feature.id());
      QVERIFY(layer.dataProvider()->deleteFeatures(ids));
      QVERIFY(FaciesMapLegend::presentEntries(&layer, catalogue).isEmpty());
    }
    void rasterLegendContainsOnlyValuesPresentInTheRaster()
    {
      QTemporaryDir directory;
      QString error;
      const auto path = directory.filePath("present-classes.tif");
      QVERIFY(MappingArtifactWriter::raster(path, {1, 5}, 2, 1, QRectF(0, 0, 200, 100), &error));
      QgsRasterLayer layer(path, "facies"); QVERIFY(layer.isValid());
      const auto schema = FaciesCatalog::defaults();
      const auto actual = FaciesHierarchyRenderer::apply(&layer, schema, "micro_facies");
      QVariantList allCodes;
      for (const auto &entry : schema) allCodes << entry.toMap().value("code");
      const auto catalogue = FaciesHierarchy::legend(schema, "micro_facies", allCodes);
      const auto present = FaciesMapLegend::presentEntries(&layer, catalogue);
      QCOMPARE(present.size(), 2);
      QCOMPARE(present[0].toMap().value("code").toInt(), 1);
      QCOMPARE(present[1].toMap().value("code").toInt(), 5);
      QCOMPARE(FaciesMapLegend::presentEntries(&layer, actual).size(), 2);
    }
    void hierarchicalFaciesLegendFollowsRendererAndLayout()
    {
      auto *layout = makeLayout();
      auto *manager = layout->project()->layoutManager();
      QVERIFY(manager->addLayout(layout));
      const auto cleanup = qScopeGuard([manager, layout] { manager->removeLayout(layout); });
      auto *layer = new QgsVectorLayer("Polygon?field=facies_code:integer&crs=EPSG:3857", "微相结果 · v1", "memory");
      const auto schema = FaciesCatalog::defaults();
      QgsFeatureList features;
      for (int i = 0; i < schema.size(); ++i) {
        QgsFeature feature(layer->fields());
        feature.setAttribute("facies_code", schema[i].toMap().value("code"));
        feature.setGeometry(QgsGeometry::fromRect(QgsRectangle(i * 100, 0, (i + 1) * 100, 100)));
        features << feature;
      }
      QVERIFY(layer->dataProvider()->addFeatures(features));
      layer->updateExtents();
      layout->project()->addMapLayer(layer);
      QCOMPARE(layer->project(), layout->project());
      QCOMPARE(manager->layouts().size(), 1);
      auto entries = FaciesHierarchyRenderer::apply(layer, schema, "micro_facies");
      QCOMPARE(entries.size(), 7);
      const auto previews = FaciesMapLegend::symbolEntries(layer, entries);
      for (const auto &entry : previews) QVERIFY(!entry.toMap().value("legend_icon").value<QIcon>().isNull());
      auto *map = PaleoStandardElements::addMainMap(layout, {layer}, layer->extent());
      map->attemptResize(QgsLayoutSize(200, 150, Qgis::LayoutUnit::Millimeters));
      map->attemptMove(QgsLayoutPoint(10, 24, Qgis::LayoutUnit::Millimeters));
      map->zoomToExtent(layer->extent());
      auto *legend = PaleoStandardElements::addLegend(layout, map);
      QVERIFY(legend);
      auto *nodeLayer = legend->model()->rootGroup()->findLayer(layer->id());
      QVERIFY(nodeLayer);
      QTRY_COMPARE(legend->model()->layerLegendNodes(nodeLayer).size(), 10);
      auto nodes = legend->model()->layerLegendNodes(nodeLayer);
      int headings = 0, symbols = 0;
      for (auto *node : nodes) {
        if (dynamic_cast<QgsSymbolLegendNode *>(node)) {
          ++symbols;
          QVERIFY(!node->data(int(QgsLayerTreeModelLegendNode::CustomRole::ParentRuleKey)).toString().isEmpty());
        } else ++headings;
      }
      QCOMPARE(headings, 3); QCOMPARE(symbols, 7);
      QgsLayoutExporter exporter(layout);
      const auto image = exporter.renderPageToImage(0, QSize(1200, 850), 120);
      QVERIFY(!image.isNull());
      QCOMPARE(legend->model()->layerLegendNodes(nodeLayer).size(), 10); // 按地图过滤后仍保留父标题。
      const auto capture = qEnvironmentVariable("PALEO_FACIES_LEGEND_EXPORT");
      if (!capture.isEmpty()) QVERIFY(image.save(capture));
      map->zoomToExtent(QgsRectangle(10, 10, 20, 20));
      exporter.renderPageToImage(0, QSize(1200, 850), 120);
      QTRY_COMPARE(legend->model()->layerLegendNodes(nodeLayer).size(), 3); // 一个可见微相及其两级父标题。
      map->zoomToExtent(layer->extent());
      FaciesHierarchyRenderer::apply(layer, schema, "facies");
      exporter.renderPageToImage(0, QSize(1200, 850), 120);
      QTRY_COMPARE(legend->model()->layerLegendNodes(nodeLayer).size(), 1);
      QCOMPARE(legend->model()->layerLegendNodes(nodeLayer)[0]->data(Qt::DisplayRole).toString(), QStringLiteral("三角洲相"));
      FaciesHierarchyRenderer::apply(layer, schema, "sub_facies");
      exporter.renderPageToImage(0, QSize(1200, 850), 120);
      QTRY_COMPARE(legend->model()->layerLegendNodes(nodeLayer).size(), 3); // 相父节点 + 两个亚相
    }
    void rasterLegendAggregatesParentsAndUsesActualSolidColors()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      const auto schema = FaciesCatalog::defaults();
      QVector<int> cells;
      for (const auto &value : schema) cells << value.toMap().value("code").toInt();
      QString error;
      const auto path = dir.filePath("facies.tif");
      QVERIFY(MappingArtifactWriter::raster(path, cells, 7, 1, QRectF(0, 0, 700, 100), &error));
      QgsRasterLayer layer(path, "地震微相 · Mock");
      QVERIFY(layer.isValid());
      auto entries = FaciesHierarchyRenderer::apply(&layer, schema, "micro_facies");
      for (const auto &entry : entries) QVERIFY(entry.toMap().value("texture").toString().isEmpty());
      QgsLayerTree root;
      auto *nodeLayer = root.addLayer(&layer);
      auto nodes = layer.legend()->createLayerTreeModelLegendNodes(nodeLayer);
      QCOMPARE(nodes.size(), 10);
      int rasterSymbols = 0;
      for (auto *node : nodes) rasterSymbols += dynamic_cast<QgsRasterSymbolLegendNode *>(node) != nullptr;
      QCOMPARE(rasterSymbols, 7);
      qDeleteAll(nodes);
      entries = FaciesHierarchyRenderer::apply(&layer, schema, "facies");
      QCOMPARE(entries.size(), 1);
      auto *renderer = dynamic_cast<QgsPalettedRasterRenderer *>(layer.renderer());
      QVERIFY(renderer);
      QCOMPARE(renderer->classes().size(), 7); // 原始微相编码仍保留，图例按相合并
      for (const auto &item : renderer->classes()) QCOMPARE(item.color, QColor(entries[0].toMap().value("color").toString()));
      nodes = layer.legend()->createLayerTreeModelLegendNodes(nodeLayer);
      QCOMPARE(nodes.size(), 1);
      QCOMPARE(nodes[0]->data(Qt::DisplayRole).toString(), QStringLiteral("三角洲相"));
      qDeleteAll(nodes);
    }

    void factoriesLinkAndStyle()
    {
      std::unique_ptr<QgsPrintLayout> layout( makeLayout() );

      QgsLayoutItemMap *map = PaleoStandardElements::addMainMap( layout.get(), {}, QgsRectangle() );
      QVERIFY( map != nullptr );
      QCOMPARE( map->id(), QStringLiteral( "map" ) );
      QCOMPARE( map->scene(), static_cast<QGraphicsScene *>( layout.get() ) ); // 所有权在 layout
      // 装饰定位基准是地图的 scene 矩形——先给地图一个真实版面尺寸。
      map->attemptResize( QgsLayoutSize( 200, 150, Qgis::LayoutUnit::Millimeters ) );
      map->attemptMove( QgsLayoutPoint( 10, 24, Qgis::LayoutUnit::Millimeters ) );
      map->setExtent( QgsRectangle( 0, 0, 1000, 800 ) );

      QgsLayoutItemScaleBar *bar = PaleoStandardElements::addScaleBar( layout.get(), map );
      QVERIFY( bar != nullptr );
      QCOMPARE( bar->linkedMap(), map );                 // 比例联动落点
      QCOMPARE( bar->units(), Qgis::DistanceUnit::Meters );
      QCOMPARE( bar->style(), QStringLiteral( "Single Box" ) ); // 条式 + 数字

      QgsLayoutItemLegend *legend = PaleoStandardElements::addLegend( layout.get(), map );
      QVERIFY( legend != nullptr );
      QCOMPARE( legend->linkedMap(), map );
      QCOMPARE( legend->syncMode(), Qgis::LegendSyncMode::AllProjectLayers ); // 随图层树自动更新
      QVERIFY( legend->legendFilterByMapEnabled() );

      QgsLayoutItem *north = PaleoStandardElements::addNorthArrow( layout.get(), map );
      QVERIFY( north != nullptr );
      QCOMPARE( north->id(), QStringLiteral( "northArrow" ) );
      if ( auto *picture = qobject_cast<QgsLayoutItemPicture *>( north ) )
        QCOMPARE( picture->linkedMap(), map ); // 随地图旋转

      PaleoStandardElements::addCoordinateGrid( map );
      QCOMPARE( map->grids()->size(), 1 );
      QVERIFY( map->grids()->grid( 0 )->enabled() );
      QVERIFY( map->grids()->grid( 0 )->annotationEnabled() );

      // 定位约定（DESIGN.md 画布内装饰）：指北针在地图左上、比例尺在地图左下。
      const QRectF mapRect = map->mapRectToScene( map->rect() );
      QVERIFY( north->mapRectToScene( north->rect() ).center().x() < mapRect.center().x() );
      QVERIFY( north->mapRectToScene( north->rect() ).center().y() < mapRect.center().y() );
      QVERIFY( bar->mapRectToScene( bar->rect() ).center().y() > mapRect.center().y() );

      const auto block = PaleoStandardElements::addTitleBlock( layout.get(), QStringLiteral( "图名" ),
                                                               QStringLiteral( "副题" ) );
      QVERIFY( block.title && block.subtitle && block.signature );
      QCOMPARE( block.title->text(), QStringLiteral( "图名" ) );
      QCOMPARE( block.subtitle->text(), QStringLiteral( "副题" ) );
      QCOMPARE( block.title->textFormat().size(), 15.0 ); // DESIGN.md 图件标题 15pt
      QCOMPARE( block.subtitle->textFormat().size(), 9.0 );
      QCOMPARE( block.signature->textFormat().size(), 8.0 );

      // 空参防呆：nullptr 全部安全。
      QCOMPARE( PaleoStandardElements::addScaleBar( nullptr, nullptr ), nullptr );
      QCOMPARE( PaleoStandardElements::addLegend( nullptr, nullptr ), nullptr );
      QCOMPARE( PaleoStandardElements::addNorthArrow( nullptr, nullptr ), nullptr );
      PaleoStandardElements::addCoordinateGrid( nullptr ); // 不崩即可
    }

    void insetOverviewBindsMainMap()
    {
      std::unique_ptr<QgsPrintLayout> layout( makeLayout() );
      QgsLayoutItemMap *map = PaleoStandardElements::addMainMap( layout.get(), {}, QgsRectangle() );
      map->attemptResize( QgsLayoutSize( 200, 150, Qgis::LayoutUnit::Millimeters ) );
      map->setExtent( QgsRectangle( 0, 0, 1000, 800 ) ); // 定框后设范围（mapbook 顺序纪律）

      QgsLayoutItemMap *inset = PaleoStandardElements::addInsetMap( layout.get(), map );
      QVERIFY( inset != nullptr );
      QCOMPARE( inset->id(), QStringLiteral( "inset" ) );
      QCOMPARE( inset->overviews()->size(), 1 );
      QCOMPARE( inset->overviews()->overview( 0 )->linkedMap(), map ); // M3 联动落点
      QVERIFY( inset->overviews()->overview( 0 )->enabled() );
      QVERIFY( !inset->extent().isEmpty() );

      QCOMPARE( PaleoStandardElements::addInsetMap( nullptr, map ), nullptr );
      QCOMPARE( PaleoStandardElements::addInsetMap( layout.get(), nullptr ), nullptr );
    }

    void populateFigureKindsInventory()
    {
      for ( auto kind : { PaleoStandardElements::FigureKind::WellPosition,
                          PaleoStandardElements::FigureKind::SingleFactor,
                          PaleoStandardElements::FigureKind::Facies } )
      {
        std::unique_ptr<QgsPrintLayout> layout( makeLayout() );
        PaleoStandardElements::PageSetup a3p;
        a3p.sizeName = QStringLiteral( "A3" );
        a3p.landscape = false;
        QString err;
        QVERIFY2( PaleoStandardElements::populateFigureLayout( layout.get(), kind, a3p, {},
                                                               QgsRectangle(), &err ),
                  qPrintable( err ) );

        // 页面规格生效。
        const QgsLayoutSize paper = layout->pageCollection()->page( 0 )->pageSize();
        QVERIFY( qAbs( paper.width() - 297.0 ) < 0.5 );
        QVERIFY( qAbs( paper.height() - 420.0 ) < 0.5 );

        // 标准元素清单：恰好各一件（骨架唯一性）。
        for ( const char *id : { "map", "scalebar", "legend", "northArrow", "title", "subtitle",
                                 "signature" } )
        {
          const QString itemId = QString::fromLatin1( id );
          QVERIFY2( countItems( layout.get(), itemId ) == 1,
                    qPrintable( QStringLiteral( "%1: %2" )
                                    .arg( PaleoStandardElements::figureKindKey( kind ), itemId ) ) );
        }

        // 沉积相图独有插图（overview 指主图）。
        QCOMPARE( countItems( layout.get(), QStringLiteral( "inset" ) ),
                  kind == PaleoStandardElements::FigureKind::Facies ? 1 : 0 );

        // 标题=图件种类名；副题为空串占位。
        auto *title = qobject_cast<QgsLayoutItemLabel *>(
            findItem( layout.get(), QStringLiteral( "title" ) ) );
        QVERIFY( title != nullptr );
        QCOMPARE( title->text(), PaleoStandardElements::figureKindTitle( kind ) );

        // 主地图在标题块之下。
        auto *mapItem = qobject_cast<QgsLayoutItemMap *>( findItem( layout.get(), QStringLiteral( "map" ) ) );
        QVERIFY( mapItem != nullptr );
        QVERIFY( mapItem->mapRectToScene( mapItem->rect() ).top() > 20.0 );

        // 网格挂上主地图。
        QCOMPARE( mapItem->grids()->size(), 1 );
      }
    }

    void populateClearsExistingContent()
    {
      std::unique_ptr<QgsPrintLayout> layout( makeLayout() );
      // 预置杂物：两个标签 + 一段 undo 历史。
      const int undoBefore = layout->undoStack()->stack()->count();
      auto *junk = new QgsLayoutItemLabel( layout.get() );
      junk->setId( QStringLiteral( "junk" ) );
      layout->addLayoutItem( junk );
      auto *junk2 = new QgsLayoutItemLabel( layout.get() );
      junk2->setId( QStringLiteral( "junk2" ) );
      layout->addLayoutItem( junk2 );
      QVERIFY( countItems( layout.get(), QStringLiteral( "junk" ) ) == 1 );
      const int undoBaseline = layout->undoStack()->stack()->count(); // 含夹具两条 add

      QVERIFY( PaleoStandardElements::populateFigureLayout(
          layout.get(), PaleoStandardElements::FigureKind::WellPosition,
          PaleoStandardElements::PageSetup{}, {}, QgsRectangle() ) );
      QCOMPARE( countItems( layout.get(), QStringLiteral( "junk" ) ), 0 );
      QCOMPARE( countItems( layout.get(), QStringLiteral( "junk2" ) ), 0 );

      // 模板装配不进 undo 历史（清场与重建都不留命令）。
      QCOMPARE( layout->undoStack()->stack()->count(), undoBaseline );

      QVERIFY( !PaleoStandardElements::populateFigureLayout( nullptr,
                                                             PaleoStandardElements::FigureKind::Facies,
                                                             PaleoStandardElements::PageSetup{}, {},
                                                             QgsRectangle() ) );
    }

    void templateXmlRoundTrip()
    {
      // Oracle 1 的种子：populate → saveAsTemplate → 清场 → loadFromTemplate，
      // 元素树（id 集合）+ 绑定（linkedMap/syncMode/overview）+ 属性（位置/尺寸/
      // 字号）逐字段一致。
      std::unique_ptr<QgsPrintLayout> layout( makeLayout() );
      QVERIFY( PaleoStandardElements::populateFigureLayout(
          layout.get(), PaleoStandardElements::FigureKind::Facies,
          PaleoStandardElements::PageSetup{}, {}, QgsRectangle() ) );

      QTemporaryDir dir;
      const QString path = dir.filePath( QStringLiteral( "facies.qpt" ) );
      QgsReadWriteContext saveCtx;
      QVERIFY( layout->saveAsTemplate( path, saveCtx ) );
      QVERIFY( QFileInfo::exists( path ) );

      // 记录套用前状态（逐字段）。
      QStringList idsBefore;
      QMap<QString, QPointF> posBefore;
      QMap<QString, QSizeF> sizeBefore;
      QList<QgsLayoutItem *> items; // 内容项（页面项不进 round-trip 账）
      layout->layoutItems( items );
      for ( QgsLayoutItem *item : items )
      {
        if ( qobject_cast<QgsLayoutItemPage *>( item ) )
          continue;
        idsBefore << item->id();
        posBefore[item->id()] = item->mapToScene( item->rect().topLeft() );
        sizeBefore[item->id()] = QSizeF( item->rect().width(), item->rect().height() );
      }
      idsBefore.sort();

      // 清场重载（loadFromTemplate clearExisting）——页面项保留。
      for ( QgsLayoutItem *item : items )
      {
        if ( !qobject_cast<QgsLayoutItemPage *>( item ) )
          layout->removeLayoutItem( item );
      }
      QTest::qWait( 10 );

      QFile in( path );
      QVERIFY( in.open( QIODevice::ReadOnly ) );
      QDomDocument doc;
      QVERIFY( doc.setContent( &in ) );
      in.close();
      bool loadedOk = false;
      layout->loadFromTemplate( doc, QgsReadWriteContext(), true, &loadedOk );
      QVERIFY( loadedOk );

      // 元素树一致（页项不计——QGIS 会按模板页集合重建）。
      QStringList idsAfter;
      QList<QgsLayoutItem *> after;
      layout->layoutItems( after );
      for ( QgsLayoutItem *item : after )
      {
        if ( qobject_cast<QgsLayoutItemPage *>( item ) )
          continue;
        idsAfter << item->id();
      }
      idsAfter.sort();
      QCOMPARE( idsAfter, idsBefore );

      // 属性逐字段：位置/尺寸（1mm 容差——QGIS 序列化往返的浮点口径）。
      for ( QgsLayoutItem *item : after )
      {
        if ( qobject_cast<QgsLayoutItemPage *>( item ) )
          continue;
        QVERIFY2( posBefore.contains( item->id() ),
                  qPrintable( item->id() ) );
        QVERIFY2( qAbs( item->mapToScene( item->rect().topLeft() ).x() - posBefore[item->id()].x() ) < 1.0,
                  qPrintable( item->id() ) );
        QVERIFY2( qAbs( item->mapToScene( item->rect().topLeft() ).y() - posBefore[item->id()].y() ) < 1.0,
                  qPrintable( item->id() ) );
        QVERIFY2( qAbs( item->rect().width() - sizeBefore[item->id()].width() ) < 1.0,
                  qPrintable( item->id() ) );
      }

      // 绑定逐字段：比例尺/图例回指主图；插图的 overview 回指主图。
      auto *mapAfter = qobject_cast<QgsLayoutItemMap *>( findItem( layout.get(), QStringLiteral( "map" ) ) );
      auto *barAfter = qobject_cast<QgsLayoutItemScaleBar *>(
          findItem( layout.get(), QStringLiteral( "scalebar" ) ) );
      auto *legendAfter = qobject_cast<QgsLayoutItemLegend *>(
          findItem( layout.get(), QStringLiteral( "legend" ) ) );
      auto *insetAfter = qobject_cast<QgsLayoutItemMap *>(
          findItem( layout.get(), QStringLiteral( "inset" ) ) );
      QVERIFY( mapAfter && barAfter && legendAfter && insetAfter );
      QCOMPARE( barAfter->linkedMap(), mapAfter );
      QCOMPARE( legendAfter->linkedMap(), mapAfter );
      QCOMPARE( legendAfter->syncMode(), Qgis::LegendSyncMode::AllProjectLayers );
      QCOMPARE( insetAfter->overviews()->size(), 1 );
      QCOMPARE( insetAfter->overviews()->overview( 0 )->linkedMap(), mapAfter );
    }
};

int main( int argc, char *argv[] )
{
  if ( qgetenv( "QT_QPA_PLATFORM" ).isEmpty() )
    qputenv( "QT_QPA_PLATFORM", "offscreen" );
  if ( !QgisRuntime::initialize( QStringLiteral( "/usr" ) ) )
    qFatal( "QgisRuntime::initialize failed" );
  TestStandardElements tc;
  const int rc = QTest::qExec( &tc, argc, argv );
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_standardelements.moc"
