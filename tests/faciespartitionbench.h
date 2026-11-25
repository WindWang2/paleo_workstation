#pragma once
// Optional benchmark in tst_faciespartition, sharing its native capture fixture.
namespace FaciesPartitionBench {
bool populate(Fixture &f, int count, QString *error) {
  using namespace MappingFeatureIdentity;
  auto *provider = f.layer.dataProvider();
  if (!provider->addAttributes({QgsField(UidField, QMetaType::Type::QString),
      QgsField(ParentsField, QMetaType::Type::QString), QgsField(RootsField, QMetaType::Type::QString),
      QgsField(ReviewField, QMetaType::Type::Bool)})) return false;
  f.layer.updateFields();
  QgsFeatureList features;
  features.reserve(count);
  for (int i = 0; i < count; ++i) {
    const int x = (i % 100) * 10, y = (i / 100) * 10;
    QgsFeature feature(f.layer.fields());
    auto geometry = QgsGeometry::fromRect(QgsRectangle(x, y, x + 10, y + 10));
    geometry.convertToMultiType();
    feature.setGeometry(geometry);
    feature.setAttribute("facies_code", 1 + i % 2);
    feature.setAttribute("facies_evidence", "[]");
    feature.setAttribute("paleo_interpretation", "{}");
    const auto uid = QStringLiteral("00000000-0000-4000-8000-%1").arg(i + 1, 12, 16, QLatin1Char('0'));
    feature.setAttribute(UidField, uid);
    feature.setAttribute(ParentsField, "[]");
    feature.setAttribute(RootsField, QStringLiteral("[\"%1\"]").arg(uid));
    features << feature;
  }
  if (!provider->addFeatures(features) || !provider->createSpatialIndex()) return false;
  f.layer.updateExtents();
  const auto footprint = QgsGeometry::fromRect(f.layer.extent()).asWkb().toBase64();
  return FaciesPartitionEditor::install(&f.layer, classification(), QString::fromLatin1(footprint), error) &&
      Observer::attach(&f.layer, error) && f.layer.startEditing();
}

void sample(Fixture &f, Capture &capture, int count, const QString &phase, QJsonArray &samples) {
  capture.canvas.refresh();
  QSignalSpy settled(&capture.canvas, &QgsMapCanvas::mapCanvasRefreshed);
  QVERIFY2(settled.wait(60000), "The starting map did not finish drawing.");
  for (const auto &p : {QPoint(52,52), QPoint(58,52), QPoint(58,58), QPoint(52,58)})
    capture.click(p.x(), p.y());
  QCOMPARE(capture.tool.captureCurve()->numPoints(), 4);
  const auto *source = capture.tool.captureCurve();
  auto polygon = std::make_unique<QgsPolygon>();
  polygon->setExteriorRing(source->curveToLine());
  const QgsGeometry stroke(polygon.release());
  double frameMs = -1, inputMs = -1, renderStart = -1;
  QElapsedTimer elapsed;
  const auto ms = [&] { return elapsed.nsecsElapsed() / 1000000.; };
  QObject scope;
  QObject::connect(&capture.canvas, &QgsMapCanvas::renderStarting, &scope, [&] { renderStart = ms(); });
  QObject::connect(&capture.canvas, &QgsMapCanvas::mapCanvasRefreshed, &scope, [&] {
    if (renderStart >= 0 && frameMs < 0) frameMs = ms();
  });
  elapsed.start();
  QTimer::singleShot(0, &scope, [&] {
    QgsMapMouseEvent move(&capture.canvas, QEvent::MouseMove, QPoint(60, 60), Qt::NoButton, Qt::NoButton);
    capture.tool.canvasMoveEvent(&move);
    inputMs = ms();
  });
  capture.click(52, 58, Qt::RightButton);
  const double synchronousMs = ms();
  QCOMPARE(f.layer.undoStack()->index(), 1);
  QTRY_VERIFY_WITH_TIMEOUT(frameMs >= 0 && inputMs >= 0, 60000);
  const double renderMs = frameMs - renderStart;
  QVERIFY(f.layer.selectedFeatures().first().attribute("facies_code").toInt() == 3);
  f.layer.undoStack()->undo();
  QCOMPARE(f.layer.featureCount(), count);
  QString error;
  elapsed.restart();
  QVERIFY2(FaciesPartitionEditor::validate(&f.layer, &error), qPrintable(error));
  const double validationMs = ms();
  // Isolated geometry kernel on the same restored square. These two diagnostics
  // are independent measurements, not additive slices of the mouse event.
  const auto face = QgsGeometry::fromRect(QgsRectangle(50, 50, 60, 60));
  elapsed.restart();
  const auto kept = FaciesPartitionEditor::clippedPaint(&f.layer, stroke);
  const auto rest = face.difference(kept);
  const double geometryMs = ms();
  QVERIFY(qAbs(rest.area() + kept.area() - face.area()) < 1e-6);
  samples.append(QJsonObject{{"faces", count}, {"phase", phase}, {"first_frame_ms", frameMs},
      {"next_input_ms", inputMs}, {"render_ms", renderMs}, {"right_click_ms", synchronousMs},
      {"single_validation_ms", validationMs}, {"geometry_kernel_ms", geometryMs}});
}

void run() {
  const int rounds = qEnvironmentVariableIntValue("PALEO_FACIES_BENCH_ROUNDS");
  QVERIFY2(rounds >= 1 && rounds <= 100, "Set PALEO_FACIES_BENCH_ROUNDS to 1..100.");
  QgsApplication::setMaxThreads(1);
  QJsonArray samples;
  for (const auto count : {1000, 10000, 50000}) {
    for (int round = 0; round < rounds; ++round) {
      Fixture f; QString error;
      QVERIFY2(populate(f, count, &error), qPrintable(error));
      Capture capture(f.layer, FaciesPartitionTool::Paint);
      capture.canvas.setParallelRenderingEnabled(false);
      capture.canvas.setCachingEnabled(false);
      capture.canvas.show();
      sample(f, capture, count, "cold", samples);
      if (QTest::currentTestFailed()) return;
      sample(f, capture, count, "warm", samples);
      if (QTest::currentTestFailed()) return;
      // Persist each completed pair so a long run still yields usable raw data.
      QSaveFile output(qEnvironmentVariable("PALEO_FACIES_BENCH_JSON"));
      QVERIFY(output.open(QIODevice::WriteOnly));
      const auto json = QJsonDocument(samples).toJson();
      QCOMPARE(output.write(json), json.size());
      QVERIFY(output.commit());
    }
  }
}
}
