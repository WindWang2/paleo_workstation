// 层：测试壳
#include <QtTest>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QToolButton>
#include <QTemporaryDir>
#include <QTimer>

#include "../src/qgis/coordinateeditor.h"
#include "../src/qgis/coordinatemaptool.h"
#include "../src/qgis/coordinatetoolsdialog.h"
#include "../src/qgis/crsselector.h"
#include "../src/qgis/mapextentwidget.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/services/previewdoc.h"
#include "../src/ui/dialogs/provisionalregistrationdialog.h"
#include "helpers/visualcapture.h"

#include <qgscoordinateoperationwidget.h>
#include <qgsextentwidget.h>
#include <qgsfeature.h>
#include <qgsgeometry.h>
#include <qgsmapcanvas.h>
#include <qgsmapsettings.h>
#include <qgsprojectionselectiondialog.h>
#include <qgsprojectionselectionwidget.h>
#include <qgsvectordataprovider.h>
#include <qgsvectorlayer.h>

class TestCoordinateInputs : public QObject
{
  Q_OBJECT
private slots:
  void crsSelectorRoundTrip();
  void nativeCrsDialogSelectsCrs();
  void parseFormatsAndBounds();
  void transformKnownPoint();
  void editorTransformsAndRejectsUnknown();
  void toolsConvertAndCopy();
  void toolsUseSelectedDatumOperation();
  void mapToolPicksAndCancels();
  void extentUsesActualMapAndLayer();
  void temporaryAffineDialogPreservesParameters();
  void narrowPanelsAndVisuals();
};

void TestCoordinateInputs::crsSelectorRoundTrip()
{
  PaleoCrsSelector selector(true);
  QVERIFY(selector.setDefinition(QString()));
  QCOMPARE(selector.definition(), QString());
  QString error;
  QVERIFY(selector.validate(&error));
  QVERIFY(selector.setDefinition(QStringLiteral("EPSG:32649")));
  QCOMPARE(selector.definition(), QStringLiteral("EPSG:32649"));
  QVERIFY(selector.validate(&error, true));
  QVERIFY(selector.setDefinition(QStringLiteral("EPSG:4326")));
  QVERIFY(!selector.validate(&error, true));
  QVERIFY(!selector.setDefinition(QStringLiteral("C:/private/source.wkt")));
  QVERIFY(!selector.crs().isValid());
  QVERIFY(!selector.validate(&error));
  QVERIFY(selector.setDefinition(QStringLiteral("EPSG:32649")));
  const auto wkt = selector.crs().toWkt();
  QVERIFY(selector.setDefinition(wkt));
  QCOMPARE(selector.crs(), QgsCoordinateReferenceSystem(QStringLiteral("EPSG:32649")));
  selector.clear();
  QCOMPARE(selector.definition(), QString());
}

void TestCoordinateInputs::nativeCrsDialogSelectsCrs()
{
  PaleoCrsSelector selector(true);
  selector.setDefinition(QStringLiteral("EPSG:4326"));
  selector.widget()->show();
  bool opened = false;
  QTimer::singleShot(0, [&] {
    QgsCrsSelectionWidget *panel = nullptr;
    for (auto *widget : QApplication::topLevelWidgets())
    {
      panel = qobject_cast<QgsCrsSelectionWidget *>(widget);
      if (!panel) panel = widget->findChild<QgsCrsSelectionWidget *>();
      if (panel) break;
    }
    opened = panel != nullptr;
    if (!opened)
    {
      for (auto *widget : QApplication::topLevelWidgets())
        if (auto *dialog = qobject_cast<QDialog *>(widget)) dialog->reject();
      return;
    }
    panel->setCrs(QgsCoordinateReferenceSystem(QStringLiteral("EPSG:32649")));
    auto *dialog = qobject_cast<QDialog *>(panel->window());
    if (dialog)
    {
      const bool saved = paleo::tests::captureVisual(dialog, QStringLiteral("crs-native-selector"), QSize(840, 640));
      dialog->accept();
      QVERIFY(saved);
    }
    else panel->close();
  });
  selector.nativeWidget()->selectCrs();
  QTRY_VERIFY_WITH_TIMEOUT(opened, 3000);
  QTRY_COMPARE_WITH_TIMEOUT(selector.definition(), QStringLiteral("EPSG:32649"), 3000);
}

void TestCoordinateInputs::parseFormatsAndBounds()
{
  QgsPointXY point;
  QString error;
  const QgsCoordinateReferenceSystem geographic(QStringLiteral("EPSG:4326"));
  const QgsCoordinateReferenceSystem projected(QStringLiteral("EPSG:32649"));
  QVERIFY(paleo::coordinates::parsePoint(QStringLiteral("108.037366E"), QStringLiteral("36.086627N"),
                                         geographic, paleo::coordinates::OrdinateFormat::Decimal,
                                         &point, &error));
  QCOMPARE(point.x(), 108.037366);
  QCOMPARE(point.y(), 36.086627);
  QVERIFY(paleo::coordinates::parsePoint(QStringLiteral("108°02′14.5176″E"), QStringLiteral("36°05′11.8572″N"),
                                         geographic, paleo::coordinates::OrdinateFormat::DegreesMinutesSeconds,
                                         &point, &error));
  QVERIFY(std::abs(point.x() - 108.037366) < 0.000001);
  QVERIFY(std::abs(point.y() - 36.086627) < 0.000001);
  QVERIFY(!paleo::coordinates::parsePoint(QStringLiteral("108"), QStringLiteral("91"),
                                          geographic, paleo::coordinates::OrdinateFormat::Decimal,
                                          &point, &error));
  QVERIFY(error.contains(QStringLiteral("纬度")));
  QVERIFY(!paleo::coordinates::parsePoint(QStringLiteral("not-a-number"), QStringLiteral("36"),
                                          geographic, paleo::coordinates::OrdinateFormat::Decimal,
                                          &point, &error));
  QVERIFY(paleo::coordinates::parsePoint(QStringLiteral("300000.25"), QStringLiteral("3997621.5"),
                                         projected, paleo::coordinates::OrdinateFormat::Decimal,
                                         &point, &error));
  QCOMPARE(point.x(), 300000.25);
  QCOMPARE(point.y(), 3997621.5);
  const auto formatted = paleo::coordinates::formatPoint(QgsPointXY(108.037366, 36.086627), geographic,
                                                         paleo::coordinates::OrdinateFormat::DegreesMinutesSeconds, 2);
  QVERIFY(formatted.contains(QStringLiteral("E")));
  QVERIFY(formatted.contains(QStringLiteral("N")));
  QVERIFY(formatted.contains(QStringLiteral("°")));
  const QString decimalX = paleo::coordinates::formatOrdinate(108.037366, true, geographic,
                                                              paleo::coordinates::OrdinateFormat::Decimal, 9);
  const QString decimalY = paleo::coordinates::formatOrdinate(36.086627, false, geographic,
                                                              paleo::coordinates::OrdinateFormat::Decimal, 9);
  QVERIFY2(paleo::coordinates::parsePoint(decimalX, decimalY, geographic,
                                          paleo::coordinates::OrdinateFormat::Decimal, &point, &error),
           qPrintable(decimalX + " / " + decimalY + " / " + error));
  QVERIFY(std::abs(point.x() - 108.037366) < 0.000001);
  QVERIFY(std::abs(point.y() - 36.086627) < 0.000001);
  QVERIFY(!paleo::coordinates::parsePoint(QStringLiteral("0"), QStringLiteral("0"), {},
                                          paleo::coordinates::OrdinateFormat::Decimal, &point, &error));
}

void TestCoordinateInputs::transformKnownPoint()
{
  const QgsCoordinateReferenceSystem wgs(QStringLiteral("EPSG:4326"));
  const QgsCoordinateReferenceSystem utm(QStringLiteral("EPSG:32649"));
  QgsPointXY result;
  QString error;
  QVERIFY2(paleo::coordinates::transformPoint(QgsPointXY(108.037366, 36.086627), wgs, utm, {},
                                              &result, &error), qPrintable(error));
  QVERIFY(std::abs(result.x() - 233240.46) < 0.02);
  QVERIFY(std::abs(result.y() - 3997621.08) < 0.02);
  QVERIFY(paleo::coordinates::transformPoint(result, utm, wgs, {}, &result, &error));
  QVERIFY(std::abs(result.x() - 108.037366) < 0.000001);
  QVERIFY(std::abs(result.y() - 36.086627) < 0.000001);
  QVERIFY(!paleo::coordinates::transformPoint(result, {}, wgs, {}, &result, &error));
  QVERIFY(!paleo::coordinates::transformPoint(QgsPointXY(1e300, 1e300), wgs, utm, {},
                                              &result, &error));
}

void TestCoordinateInputs::editorTransformsAndRejectsUnknown()
{
  QgsMapCanvas canvas;
  canvas.setDestinationCrs(QgsCoordinateReferenceSystem(QStringLiteral("EPSG:32649")));
  canvas.mapSettings().setTransformContext(QgsCoordinateTransformContext());
  PaleoCoordinateEditor editor;
  editor.setMapCanvas(&canvas);
  editor.setSourceCrs(QgsCoordinateReferenceSystem(QStringLiteral("EPSG:4326")));
  editor.setTargetCrs(QgsCoordinateReferenceSystem(QStringLiteral("EPSG:32649")));
  editor.setPoint(QgsPointXY(108.037366, 36.086627));
  QgsPointXY point;
  QString error;
  QVERIFY2(editor.targetPoint(&point, &error), qPrintable(error));
  QVERIFY(std::abs(point.x() - 233240.46) < 0.02);
  QVERIFY(editor.showMapPoint(point));
  QVERIFY(editor.sourcePoint(&point, &error));
  QVERIFY(std::abs(point.x() - 108.037366) < 0.000001);
  editor.xEdit()->setText(QStringLiteral("181"));
  QVERIFY(!editor.validatePoint());
  QVERIFY(!editor.sourceSelector()->setDefinition(QStringLiteral("not-a-crs")));
  QVERIFY(!editor.targetPoint(&point, &error));
  QVERIFY(!error.isEmpty());
}

void TestCoordinateInputs::toolsConvertAndCopy()
{
  QgsMapCanvas canvas;
  canvas.setDestinationCrs(QgsCoordinateReferenceSystem(QStringLiteral("EPSG:32649")));
  PaleoCoordinateToolsDialog dialog(&canvas);
  dialog.dialog()->show();
  dialog.editor()->setSourceCrs(QgsCoordinateReferenceSystem(QStringLiteral("EPSG:4326")));
  dialog.setSourcePoint(QgsPointXY(108.037366, 36.086627));
  dialog.dialog()->findChild<QToolButton *>(QStringLiteral("coordinateOperationToggle"))->click();
  QVERIFY(dialog.dialog()->findChild<QgsCoordinateOperationWidget *>()->isVisible());
  dialog.convertButton()->click();
  QgsPointXY point;
  QVERIFY2(dialog.convertedPoint(&point), qPrintable(dialog.resultText()));
  QVERIFY(std::abs(point.x() - 233240.46) < 0.02);
  dialog.dialog()->findChild<QPushButton *>(QStringLiteral("coordinateCopyResultButton"))->click();
  QCOMPARE(QApplication::clipboard()->text(), dialog.resultText());
  dialog.dialog()->findChild<QPushButton *>(QStringLiteral("coordinateSwapButton"))->click();
  QCOMPARE(dialog.editor()->sourceSelector()->definition(), QStringLiteral("EPSG:32649"));
  QCOMPARE(dialog.targetSelector()->definition(), QStringLiteral("EPSG:4326"));
  dialog.convertButton()->click();
  QVERIFY(dialog.convertedPoint(&point));
  QVERIFY(std::abs(point.x() - 108.037366) < 0.000001);
  QVERIFY(std::abs(point.y() - 36.086627) < 0.000001);
  dialog.editor()->xEdit()->setText(QStringLiteral("invalid"));
  dialog.convertButton()->click();
  QVERIFY(!dialog.convertedPoint(&point));
  QVERIFY(dialog.dialog()->findChild<QLabel *>(QStringLiteral("coordinateToolsError"))->isVisible());
}

void TestCoordinateInputs::toolsUseSelectedDatumOperation()
{
  QgsMapCanvas canvas;
  canvas.setDestinationCrs(QgsCoordinateReferenceSystem(QStringLiteral("EPSG:32649")));
  PaleoCoordinateToolsDialog dialog(&canvas);
  dialog.dialog()->show();
  dialog.editor()->setSourceCrs(QgsCoordinateReferenceSystem(QStringLiteral("EPSG:4326")));
  dialog.dialog()->findChild<QToolButton *>(QStringLiteral("coordinateOperationToggle"))->click();
  auto *operation = dialog.dialog()->findChild<QgsCoordinateOperationWidget *>();
  QVERIFY(operation->hasSelection());
  auto selected = operation->defaultOperation();
  if (!selected.proj.isEmpty())
  {
    selected.allowFallback = false;
    operation->setSelectedOperation(selected);
    QVERIFY(!operation->selectedOperation().allowFallback);
  }
  dialog.setSourcePoint(QgsPointXY(108.037366, 36.086627));
  dialog.convertButton()->click();
  QgsPointXY point;
  QVERIFY(dialog.convertedPoint(&point));
  QVERIFY(std::abs(point.x() - 233240.46) < 0.02);
  QVERIFY(std::abs(point.y() - 3997621.08) < 0.02);
  QVERIFY(paleo::tests::captureVisual(dialog.dialog(), QStringLiteral("coordinate-tools"), QSize(680, 640)));
}

void TestCoordinateInputs::mapToolPicksAndCancels()
{
  QgsMapCanvas canvas;
  canvas.resize(320, 240);
  canvas.setDestinationCrs(QgsCoordinateReferenceSystem(QStringLiteral("EPSG:32649")));
  canvas.setExtent(QgsRectangle(0, 0, 100, 100));
  canvas.show();
  QTest::qWait(50);
  auto *tool = new PaleoCoordinateMapTool(&canvas);
  QSignalSpy picked(tool, &PaleoCoordinateMapTool::pointPicked);
  canvas.setMapTool(tool->nativeTool());
  QTest::mouseClick(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(160, 120));
  QTRY_COMPARE_WITH_TIMEOUT(picked.count(), 1, 2000);
  const auto point = picked.first().first().value<QgsPointXY>();
  QVERIFY(std::abs(point.x() - 50.0) < 0.5);
  QVERIFY(std::abs(point.y() - 50.0) < 0.5);
  QTRY_VERIFY_WITH_TIMEOUT(canvas.mapTool() == nullptr, 1000);
  auto *cancelTool = new PaleoCoordinateMapTool(&canvas);
  QSignalSpy cancelled(cancelTool, &PaleoCoordinateMapTool::cancelled);
  canvas.setMapTool(cancelTool->nativeTool());
  canvas.viewport()->setFocus();
  QTest::keyClick(canvas.viewport(), Qt::Key_Escape);
  QTRY_COMPARE_WITH_TIMEOUT(cancelled.count(), 1, 2000);
  auto *rightTool = new PaleoCoordinateMapTool(&canvas);
  QSignalSpy rightCancelled(rightTool, &PaleoCoordinateMapTool::cancelled);
  canvas.setMapTool(rightTool->nativeTool());
  QTest::mouseClick(canvas.viewport(), Qt::RightButton, Qt::NoModifier, QPoint(160, 120));
  QTRY_COMPARE_WITH_TIMEOUT(rightCancelled.count(), 1, 2000);
  QTRY_VERIFY_WITH_TIMEOUT(canvas.mapTool() == nullptr, 1000);
  QCoreApplication::processEvents();
}

void TestCoordinateInputs::extentUsesActualMapAndLayer()
{
  QgsMapCanvas canvas;
  canvas.resize(640, 480);
  const QgsCoordinateReferenceSystem crs(QStringLiteral("EPSG:32649"));
  canvas.setDestinationCrs(crs);
  auto *layer = new QgsVectorLayer(QStringLiteral("LineString?crs=EPSG:32649"),
                                   QStringLiteral("实际图层"), QStringLiteral("memory"));
  QVERIFY(layer->isValid());
  QgsFeature feature;
  feature.setGeometry(QgsGeometry::fromPolylineXY({QgsPointXY(500, 600), QgsPointXY(1500, 1800)}));
  QVERIFY(layer->dataProvider()->addFeature(feature));
  layer->updateExtents();
  canvas.setLayers({layer});
  canvas.setExtent(QgsRectangle(100, 200, 2100, 2200));
  PaleoMapExtentWidget extent;
  extent.setCanvas(&canvas);
  QCOMPARE(extent.area().xMin, canvas.extent().xMinimum());
  QCOMPARE(extent.area().yMax, canvas.extent().yMaximum());
  auto *layers = extent.widget()->findChild<QComboBox *>(QStringLiteral("mapbookExtentLayer"));
  QCOMPARE(layers->count(), 2);
  layers->setCurrentIndex(1);
  emit layers->activated(1);
  QCOMPARE(extent.area().xMin, 500.0);
  QCOMPARE(extent.area().xMax, 1500.0);
  QCOMPARE(extent.area().yMin, 600.0);
  QCOMPARE(extent.area().yMax, 1800.0);
  extent.setArea({100, 200, 1100, 1200});
  QCOMPARE(extent.area().xMax, 1100.0);
  extent.setArea({});
  QVERIFY(!extent.area().valid());
  QVERIFY(extent.nativeWidget()->isValid() == false);
}

void TestCoordinateInputs::temporaryAffineDialogPreservesParameters()
{
  QTemporaryDir dir;
  const QString path = dir.filePath(QStringLiteral("area.geojson"));
  QFile file(path);
  QVERIFY(file.open(QIODevice::WriteOnly));
  const QByteArray json = R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":{},"geometry":{"type":"Polygon","coordinates":[[[108,36],[108.001,36],[108.001,36.001],[108,36.001],[108,36]]]}}]})";
  QCOMPARE(file.write(json), qint64(json.size()));
  file.close();
  PreviewDocService document(nullptr);
  QVariantMap parameters;
  QString error;
  QTimer::singleShot(0, [] {
    QDialog *found = nullptr;
    for (auto *widget : QApplication::topLevelWidgets())
      if (widget->objectName() == QStringLiteral("provisionalRegistrationDialog"))
        found = qobject_cast<QDialog *>(widget);
    if (!found)
    {
      for (auto *widget : QApplication::topLevelWidgets())
        if (auto *dialog = qobject_cast<QDialog *>(widget)) dialog->reject();
      return;
    }
    QVERIFY(paleo::tests::captureVisual(found, QStringLiteral("temporary-affine-registration"), QSize(620, 620)));
    found->accept();
  });
  QVERIFY2(PaleoProvisionalRegistration::prompt(nullptr, &document, path, &parameters, &error),
           qPrintable(error));
  QCOMPARE(parameters.value(QStringLiteral("tx")).toDouble(), 0.0);
  QCOMPARE(parameters.value(QStringLiteral("ty")).toDouble(), 0.0);
  QCOMPARE(parameters.value(QStringLiteral("sx")).toDouble(), 1.0);
  QCOMPARE(parameters.value(QStringLiteral("sy")).toDouble(), 1.0);
  QCOMPARE(parameters.value(QStringLiteral("rotDeg")).toDouble(), 0.0);
  QVERIFY(!PaleoProvisionalRegistration::prompt(nullptr, &document, dir.filePath("missing.geojson"),
                                                 &parameters, &error));
  QVERIFY(error.contains(QStringLiteral("读不出坐标范围")));
}

void TestCoordinateInputs::narrowPanelsAndVisuals()
{
  PaleoCoordinateEditor editor;
  editor.setSourceCrs(QgsCoordinateReferenceSystem(QStringLiteral("EPSG:4326")));
  editor.setPoint(QgsPointXY(108.037366, 36.086627));
  QScrollArea scroll;
  scroll.setWidgetResizable(true);
  scroll.setWidget(editor.widget());
  scroll.resize(280, 520);
  scroll.show();
  QTest::qWait(50);
  QCOMPARE(scroll.horizontalScrollBar()->maximum(), 0);
  QVERIFY(paleo::tests::captureVisual(editor.widget(), QStringLiteral("coordinate-editor"), QSize(520, 360)));
  scroll.hide();
}

int main(int argc, char *argv[])
{
  if (qgetenv("QT_QPA_PLATFORM").isEmpty()) qputenv("QT_QPA_PLATFORM", "offscreen");
  if (!QgisRuntime::initialize()) qFatal("QgisRuntime::initialize failed");
  TestCoordinateInputs test;
  const int result = QTest::qExec(&test, argc, argv);
  QgisRuntime::shutdown();
  return result;
}

#include "tst_coordinateinputs.moc"
