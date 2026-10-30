#include <QtTest>
#include <cmath>
#include <QElapsedTimer>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUndoStack>
#include <memory>
#include <limits>
#include <qgsrasterlayer.h>
#include <qgsapplication.h>
#include <qgsmapcanvas.h>
#include <qgsvectordataprovider.h>
#include <qgsvectorlayer.h>
#include "../src/qgis/mappingfeatureidentity.h"
#include "../src/qgis/mappingartifactwriter.h"
#include "../src/qgis/evidencemapadapter.h"
#include "../src/domain/interpretationevidence.h"

using namespace MappingFeatureIdentity;
namespace {
std::unique_ptr<QgsVectorLayer> polygons() {
  auto layer = std::make_unique<QgsVectorLayer>("Polygon?crs=EPSG:3857&field=facies_code:integer",
                                               "working", "memory");
  QgsFeatureList features;
  for (int i = 0; i < 2; ++i) {
    QgsFeature feature(layer->fields()); feature.setAttribute("facies_code", 1);
    feature.setGeometry(QgsGeometry::fromWkt(i == 0
        ? "POLYGON((0 0,10 0,10 10,0 10,0 0))"
        : "POLYGON((10 0,20 0,20 10,10 10,10 0))"));
    features << feature;
  }
  layer->dataProvider()->addFeatures(features); layer->updateExtents();
  layer->setCustomProperty("paleoLayerId", "draft.identity");
  layer->setCustomProperty("paleoVersionId", "version1");
  return layer;
}
// QGIS 4.2 accepts a mutable geometry reference. Keep fixture temporaries
// local to the test instead of adding a production compatibility wrapper.
bool changeGeometry(QgsVectorLayer *layer, QgsFeatureId fid, QgsGeometry geometry) {
  return layer->changeGeometry(fid, geometry);
}
QgsFeature first(QgsVectorLayer *layer) {
  QgsFeature feature; auto features = layer->getFeatures(); features.nextFeature(feature); return feature;
}
}
class TestFeatureIdentity : public QObject {
  Q_OBJECT
private slots:
  void snapshotGivesIdentityWithoutChangingLegacySource() {
    auto source = polygons(); QVERIFY(source->isValid()); source->setReadOnly(true);
    QTemporaryDir directory; const auto path = directory.filePath("snapshot.gpkg");
    QString error;
    QVERIFY2(MappingArtifactWriter::vectorSnapshot(source.get(), path, &error), qPrintable(error));
    QCOMPARE(source->fields().indexOf(UidField), -1);
    QVERIFY(!source->isEditable());
    QgsVectorLayer saved(path + "|layername=features", "saved", "ogr");
    QVERIFY(saved.isValid()); const auto feature = first(&saved);
    QVERIFY(!uid(feature).isEmpty());
    QCOMPARE(uid(find(&saved, uid(feature))), uid(feature));
    const auto againPath = directory.filePath("again.gpkg");
    QVERIFY(MappingArtifactWriter::vectorSnapshot(&saved, againPath, &error));
    QgsVectorLayer again(againPath + "|layername=features", "again", "ogr");
    QCOMPARE(geometryFingerprint(find(&again, uid(feature))), geometryFingerprint(feature));
    QVERIFY(!MappingArtifactWriter::vectorSnapshot(&saved, path, &error));
    QCOMPARE(uid(find(&saved, uid(feature))), uid(feature));
    QVERIFY(!ensure(source.get(), &error));
  }
  void geometryChangeKeepsUidAndUndoRestoresReview() {
    auto layer = polygons(); QString error;
    auto *observer = Observer::attach(layer.get(), &error); QVERIFY2(observer, qPrintable(error));
    QSignalSpy edits(observer, &Observer::businessEdit);
    const auto feature = first(layer.get()); const auto identity = uid(feature);
    QVERIFY(layer->startEditing()); layer->selectByIds({feature.id()});
    layer->beginEditCommand("move boundary");
    QVERIFY(changeGeometry(layer.get(), feature.id(), QgsGeometry::fromWkt("POLYGON((0 0,9 0,9 10,0 10,0 0))")));
    layer->endEditCommand();
    const auto modified = layer->getFeature(feature.id());
    QCOMPARE(uid(modified), identity);
    QVERIFY(modified.attribute(ReviewField).toBool());
    QVERIFY(geometryFingerprint(modified) != geometryFingerprint(feature));
    QCOMPARE(observer->changes().size(), 1);
    QCOMPARE(edits.count(), 1);
    QCOMPARE(edits.first().at(1).toString(), QStringLiteral("geometry"));
    layer->undoStack()->undo();
    QCOMPARE(geometryFingerprint(layer->getFeature(feature.id())), geometryFingerprint(feature));
    QVERIFY(!layer->getFeature(feature.id()).attribute(ReviewField).toBool());
    QVERIFY(observer->changes().isEmpty());
    layer->undoStack()->redo();
    QCOMPARE(uid(layer->getFeature(feature.id())), identity);
    QVERIFY(layer->getFeature(feature.id()).attribute(ReviewField).toBool());
    QCOMPARE(edits.count(), 1); // native undo/redo replays stored business edits
  }
  void destroyedCommandRestoresNativeBufferAndClosesMacro() {
    auto layer = polygons(); QString error;
    auto *observer = Observer::attach(layer.get(), &error); QVERIFY2(observer, qPrintable(error));
    const auto original = first(layer.get());
    QSignalSpy edits(observer, &Observer::businessEdit);
    QVERIFY(layer->startEditing()); layer->selectByIds({original.id()});
    const int index = layer->undoStack()->index();
    layer->beginEditCommand("cancelled native geometry");
    QVERIFY(changeGeometry(layer.get(), original.id(), QgsGeometry::fromWkt("POLYGON((0 0,3 0,3 10,0 10,0 0))")));
    // Qt inhibits native destroy's initial undo until the observer closes the
    // outer macro; discardCommand then performs the complete atomic discard.
    layer->destroyEditCommand();
    QCOMPARE(layer->undoStack()->index(), index);
    QCOMPARE(geometryFingerprint(layer->getFeature(original.id())), geometryFingerprint(original));
    QCOMPARE(edits.count(), 0); QVERIFY(observer->changes().isEmpty());
    layer->beginEditCommand("subsequent valid geometry");
    QVERIFY(changeGeometry(layer.get(), original.id(), QgsGeometry::fromWkt("POLYGON((0 0,4 0,4 10,0 10,0 0))")));
    layer->endEditCommand(); QCOMPARE(edits.count(), 1);
    layer->undoStack()->undo();
    QCOMPARE(geometryFingerprint(layer->getFeature(original.id())), geometryFingerprint(original));
  }
  void spatialHistorySurvivesNormalizationUntilAcknowledged() {
    auto layer = polygons(); QString error;
    auto *observer = Observer::attach(layer.get(), &error); QVERIFY2(observer, qPrintable(error));
    const auto original = first(layer.get());
    QVERIFY(layer->startEditing()); layer->selectByIds({original.id()});
    layer->beginEditCommand("change geometry");
    QVERIFY(changeGeometry(layer.get(), original.id(), QgsGeometry::fromWkt("POLYGON((0 0,4 0,4 10,0 10,0 0))")));
    layer->endEditCommand(); QVERIFY(layer->commitChanges());
    QCOMPARE(observer->changes().size(), 1);
    // Saving a version may normalize fields in a separate edit session. A
    // failed registration still needs the original spatial audit snapshot.
    QVERIFY(layer->startEditing()); layer->beginEditCommand("normalize attribute");
    QVERIFY(layer->changeAttributeValue(original.id(), layer->fields().indexOf("facies_code"), 2));
    layer->endEditCommand(); QVERIFY(layer->commitChanges());
    QCOMPARE(observer->changes().size(), 1);
    observer->acknowledge(); QVERIFY(observer->changes().isEmpty());
  }
  void rollbackOfLaterSessionKeepsCommittedSpatialHistory() {
    auto layer = polygons(); QString error;
    auto *observer = Observer::attach(layer.get(), &error); QVERIFY2(observer, qPrintable(error));
    const auto original = first(layer.get());
    QVERIFY(layer->startEditing()); layer->selectByIds({original.id()});
    layer->beginEditCommand("committed geometry");
    const auto committed = QgsGeometry::fromWkt("POLYGON((0 0,8 0,8 10,0 10,0 0))");
    QVERIFY(changeGeometry(layer.get(), original.id(), committed)); layer->endEditCommand();
    QVERIFY(layer->commitChanges()); QCOMPARE(observer->changes().size(), 1);
    QVERIFY(layer->startEditing()); layer->beginEditCommand("abandoned later attribute");
    QVERIFY(layer->changeAttributeValue(original.id(), layer->fields().indexOf("facies_code"), 9));
    layer->endEditCommand(); QVERIFY(layer->rollBack());
    QCOMPARE(layer->getFeature(original.id()).attribute("facies_code").toInt(), 1);
    QVERIFY(layer->getFeature(original.id()).geometry().isGeosEqual(committed));
    QCOMPARE(observer->changes().size(), 1);
    QCOMPARE(observer->changes().first().toMap().value("initial_feature").toMap().value("geometry_hash").toString(),
             geometryFingerprint(original));
  }
  void rollbackDoesNotArchiveCancelledLaterReviewHistory() {
    auto layer = polygons(); QString error;
    QVERIFY(layer->dataProvider()->addAttributes({QgsField("paleo_interpretation", QMetaType::Type::QString)}));
    layer->updateFields(); auto *observer = Observer::attach(layer.get(), &error); QVERIFY2(observer, qPrintable(error));
    const auto original = first(layer.get());
    QVERIFY(layer->startEditing()); layer->selectByIds({original.id()});
    layer->beginEditCommand("durable geometry");
    QVERIFY(changeGeometry(layer.get(), original.id(), QgsGeometry::fromWkt("POLYGON((0 0,8 0,8 10,0 10,0 0))")));
    layer->endEditCommand(); QVERIFY(layer->commitChanges());
    QVERIFY(layer->startEditing());
    layer->beginEditCommand("cancelled later review");
    QVERIFY(layer->changeAttributeValue(original.id(), layer->fields().indexOf("paleo_interpretation"), "cancelled-review"));
    layer->endEditCommand();
    layer->beginEditCommand("cancelled later geometry");
    QVERIFY(changeGeometry(layer.get(), original.id(), QgsGeometry::fromWkt("POLYGON((0 0,6 0,6 10,0 10,0 0))")));
    layer->endEditCommand(); QVERIFY(layer->rollBack());
    QCOMPARE(observer->changes().size(), 1);
    QVERIFY(observer->changes().first().toMap().value("before_interpretation").toString().isEmpty());
  }
  void nestedDestroyCancelsOnlyInnerNativeChanges() {
    auto layer = polygons(); QString error;
    auto *observer = Observer::attach(layer.get(), &error); QVERIFY2(observer, qPrintable(error));
    const auto original = first(layer.get());
    QSignalSpy edits(observer, &Observer::businessEdit);
    QVERIFY(layer->startEditing()); layer->selectByIds({original.id()});
    const auto outer = QgsGeometry::fromWkt("POLYGON((0 0,8 0,8 10,0 10,0 0))");
    layer->beginEditCommand("outer retained command");
    QVERIFY(changeGeometry(layer.get(), original.id(), outer));
    layer->beginEditCommand("inner cancelled command");
    QVERIFY(changeGeometry(layer.get(), original.id(), QgsGeometry::fromWkt("POLYGON((0 0,3 0,3 10,0 10,0 0))")));
    QVERIFY(layer->changeAttributeValue(original.id(), layer->fields().indexOf("facies_code"), 7));
    layer->destroyEditCommand();
    QVERIFY(layer->getFeature(original.id()).geometry().isGeosEqual(outer));
    QCOMPARE(layer->getFeature(original.id()).attribute("facies_code").toInt(), 1);
    QCOMPARE(edits.count(), 0);
    layer->endEditCommand(); QCOMPARE(edits.count(), 1);
    layer->undoStack()->undo();
    QCOMPARE(geometryFingerprint(layer->getFeature(original.id())), geometryFingerprint(original));
    layer->undoStack()->redo();
    QVERIFY(layer->getFeature(original.id()).geometry().isGeosEqual(outer));
    QCOMPARE(layer->getFeature(original.id()).attribute("facies_code").toInt(), 1);
    QCOMPARE(edits.count(), 1);
  }
  void nativeClassificationIsOneBusinessEditWithBeforeCode() {
    auto layer = polygons(); QString error;
    QVERIFY(layer->dataProvider()->addAttributes({QgsField("paleo_interpretation", QMetaType::Type::QString)}));
    layer->updateFields();
    auto *observer = Observer::attach(layer.get(), &error); QVERIFY2(observer, qPrintable(error));
    const auto original = first(layer.get());
    QSignalSpy edits(observer, &Observer::businessEdit);
    int beforeCode = -1;
    connect(observer, &Observer::businessEdit, layer.get(), [&](qint64 fid, const QString &) {
      beforeCode = observer->editBefore(fid).value("facies_code").toInt();
      QVERIFY(layer->changeAttributeValue(fid, layer->fields().indexOf("paleo_interpretation"), "journal marker"));
    });
    QVERIFY(layer->startEditing()); layer->selectByIds({original.id()});
    layer->beginEditCommand("native attribute table facies change");
    QVERIFY(layer->changeAttributeValue(original.id(), layer->fields().indexOf("facies_code"), 3));
    layer->endEditCommand();
    QCOMPARE(edits.count(), 1); QCOMPARE(edits.first().at(1).toString(), QString("classification"));
    QCOMPARE(beforeCode, 1);
    layer->undoStack()->undo(); QCOMPARE(layer->getFeature(original.id()).attribute("facies_code").toInt(), 1);
    QVERIFY(layer->getFeature(original.id()).attribute("paleo_interpretation").toString().isEmpty());
    layer->undoStack()->redo(); QCOMPARE(layer->getFeature(original.id()).attribute("facies_code").toInt(), 3);
    QCOMPARE(layer->getFeature(original.id()).attribute("paleo_interpretation").toString(), QString("journal marker"));
    QCOMPARE(edits.count(), 1);
  }
  void nativeSplitChildrenHaveNewIdsAndSingleUndo() {
    auto layer = polygons(); QString error;
    auto *observer = Observer::attach(layer.get(), &error); QVERIFY2(observer, qPrintable(error));
    QSignalSpy edits(observer, &Observer::businessEdit);
    const auto original = first(layer.get()); const auto parent = uid(original);
    QVERIFY(layer->startEditing()); layer->selectByIds({original.id()});
    layer->beginEditCommand("native split");
    QVERIFY(changeGeometry(layer.get(), original.id(), QgsGeometry::fromWkt("POLYGON((0 0,5 0,5 10,0 10,0 0))")));
    QgsFeature child(original);
    child.setGeometry(QgsGeometry::fromWkt("POLYGON((5 0,10 0,10 10,5 10,5 0))"));
    QVERIFY(layer->addFeature(child)); layer->endEditCommand();
    const auto retained = layer->getFeature(original.id());
    const auto split = layer->getFeature(child.id());
    QVERIFY(uid(retained) != parent); QVERIFY(uid(split) != parent);
    QVERIFY(uid(split) != uid(retained));
    QVERIFY(retained.attribute(ParentsField).toString().contains(parent));
    QVERIFY(split.attribute(ParentsField).toString().contains(parent));
    QVERIFY(retained.attribute(ReviewField).toBool()); QVERIFY(split.attribute(ReviewField).toBool());
    QCOMPARE(edits.count(), 2);
    for (const auto &edit : edits) QCOMPARE(edit.at(1).toString(), QStringLiteral("split"));
    layer->undoStack()->undo();
    QCOMPARE(layer->featureCount(), qint64(2));
    QCOMPARE(uid(layer->getFeature(original.id())), parent);
    QCOMPARE(geometryFingerprint(layer->getFeature(original.id())), geometryFingerprint(original));
    layer->undoStack()->redo();
    QCOMPARE(uid(layer->getFeature(original.id())), uid(retained));
    QCOMPARE(uid(layer->getFeature(child.id())), uid(split));
    QCOMPARE(edits.count(), 2);
  }
  void threeSplitChildrenNeverRetainOriginalUid() {
    auto layer = polygons(); QString error;
    QVERIFY2(Observer::attach(layer.get(), &error), qPrintable(error));
    const auto original = first(layer.get());
    QVERIFY(layer->startEditing()); layer->selectByIds({original.id()});
    layer->beginEditCommand("split in three");
    QVERIFY(changeGeometry(layer.get(), original.id(), QgsGeometry::fromWkt("POLYGON((0 0,3 0,3 10,0 10,0 0))")));
    QgsFeature b(original), c(original);
    b.setGeometry(QgsGeometry::fromWkt("POLYGON((3 0,6 0,6 10,3 10,3 0))"));
    c.setGeometry(QgsGeometry::fromWkt("POLYGON((6 0,10 0,10 10,6 10,6 0))"));
    QVERIFY(layer->addFeature(b)); QVERIFY(layer->addFeature(c)); layer->endEditCommand();
    QSet<QString> unique;
    for (auto fid : {original.id(), b.id(), c.id()}) {
      const auto feature = layer->getFeature(fid);
      QVERIFY(uid(feature) != uid(original)); unique.insert(uid(feature));
      QVERIFY(feature.attribute(ParentsField).toString().contains(uid(original)));
    }
    QCOMPARE(unique.size(), 3);
    layer->undoStack()->undo(); QCOMPARE(layer->featureCount(), qint64(2));
    QCOMPARE(uid(layer->getFeature(original.id())), uid(original));
  }
  void temporaryChildHistoryResolvesAfterRealGpkgCommit_data() {
    QTest::addColumn<bool>("split");
    QTest::newRow("create") << false;
    QTest::newRow("split") << true;
  }
  void temporaryChildHistoryResolvesAfterRealGpkgCommit() {
    QFETCH(bool, split);
    QTemporaryDir directory; QString error; auto source = polygons();
    const auto path = directory.filePath("working.gpkg");
    QVERIFY2(MappingArtifactWriter::vectorSnapshot(source.get(), path, &error), qPrintable(error));
    QgsVectorLayer layer(path + "|layername=features", "working", "ogr");
    QVERIFY(layer.isValid()); auto *observer = Observer::attach(&layer, &error);
    QVERIFY2(observer, qPrintable(error));
    const auto original = first(&layer);
    QVERIFY(layer.startEditing()); layer.selectByIds({original.id()});
    layer.beginEditCommand(split ? "first split" : "create new polygon");
    QgsFeature child(layer.fields()); child.setAttribute("facies_code", 1);
    if (split) {
      QVERIFY(changeGeometry(&layer, original.id(), QgsGeometry::fromWkt("POLYGON((0 0,5 0,5 10,0 10,0 0))")));
      child = original;
      // GPKG's provider primary key is not a copied business identity.
      if (layer.fields().indexOf("fid") >= 0) child.setAttribute("fid", QVariant());
    }
    child.setGeometry(QgsGeometry::fromWkt("POLYGON((5 0,10 0,10 10,5 10,5 0))"));
    QVERIFY(layer.addFeature(child)); QVERIFY(child.id() < 0); layer.endEditCommand();
    const auto temporary = layer.getFeature(child.id()); const auto childUid = uid(temporary);
    QVERIFY(!childUid.isEmpty());
    layer.selectByIds({child.id()}); layer.beginEditCommand("second command changes temporary child");
    QVERIFY(changeGeometry(&layer, child.id(), QgsGeometry::fromWkt("POLYGON((5 0,9 0,9 10,5 10,5 0))")));
    layer.endEditCommand(); const auto afterHash = geometryFingerprint(layer.getFeature(child.id()));
    QVERIFY2(layer.commitChanges(), qPrintable(layer.commitErrors().join(";")));
    const auto committed = find(&layer, childUid); QVERIFY(committed.isValid()); QVERIFY(committed.id() >= 0);
    bool found = false;
    for (const auto &value : observer->changes()) {
      const auto change = value.toMap();
      if (change.value("initial_feature").toMap().value("feature_uid").toString() != childUid) continue;
      found = true; QVERIFY(!change.value("deleted").toBool());
      QCOMPARE(change.value("before_uid").toString(), childUid);
      QCOMPARE(change.value("after_uid").toString(), childUid);
      QCOMPARE(change.value("after_fingerprint").toString(), afterHash);
      QCOMPARE(change.value("initial_feature").toMap().value("geometry_hash").toString(), geometryFingerprint(temporary));
    }
    QVERIFY(found);
    const auto snapshotPath = directory.filePath("version.gpkg");
    QVERIFY2(MappingArtifactWriter::vectorSnapshot(&layer, snapshotPath, &error), qPrintable(error));
    QgsVectorLayer reopened(snapshotPath + "|layername=features", "version", "ogr"); QVERIFY(reopened.isValid());
    QCOMPARE(geometryFingerprint(find(&reopened, childUid)), afterHash);
    QCOMPARE(find(&reopened, childUid).attribute(ParentsField), committed.attribute(ParentsField));
    observer->acknowledge(); QVERIFY(observer->changes().isEmpty());
  }
  void consecutiveRealGpkgSplitsKeepImmediateParentsAndRootLineage() {
    QTemporaryDir directory; QString error; auto source = polygons();
    const auto path = directory.filePath("split.gpkg");
    QVERIFY(MappingArtifactWriter::vectorSnapshot(source.get(), path, &error));
    QgsVectorLayer layer(path + "|layername=features", "split", "ogr");
    QVERIFY(layer.isValid()); QVERIFY2(Observer::attach(&layer, &error), qPrintable(error));
    const auto original = first(&layer); const auto rootUid = uid(original);
    QVERIFY(layer.startEditing()); layer.selectByIds({original.id()});
    layer.beginEditCommand("split root");
    QVERIFY(changeGeometry(&layer, original.id(), QgsGeometry::fromWkt("POLYGON((0 0,5 0,5 10,0 10,0 0))")));
    QgsFeature child(original); child.setAttribute("fid", QVariant());
    child.setGeometry(QgsGeometry::fromWkt("POLYGON((5 0,10 0,10 10,5 10,5 0))"));
    QVERIFY(layer.addFeature(child)); layer.endEditCommand();
    const auto directParent = layer.getFeature(child.id()); const auto parentUid = uid(directParent);
    QVERIFY(parentUid != rootUid);
    layer.selectByIds({child.id()}); layer.beginEditCommand("split child again");
    QVERIFY(changeGeometry(&layer, child.id(), QgsGeometry::fromWkt("POLYGON((5 0,7 0,7 10,5 10,5 0))")));
    QgsFeature grandchild(directParent); grandchild.setAttribute("fid", QVariant());
    grandchild.setGeometry(QgsGeometry::fromWkt("POLYGON((7 0,10 0,10 10,7 10,7 0))"));
    QVERIFY(layer.addFeature(grandchild)); layer.endEditCommand();
    const auto leftUid = uid(layer.getFeature(child.id())), rightUid = uid(layer.getFeature(grandchild.id()));
    QVERIFY(leftUid != parentUid); QVERIFY(rightUid != parentUid); QVERIFY(leftUid != rightUid);
    for (const auto fid : {child.id(), grandchild.id()}) {
      const auto feature = layer.getFeature(fid);
      QCOMPARE(QJsonDocument::fromJson(feature.attribute(ParentsField).toString().toUtf8()).toVariant().toList(), QVariantList{parentUid});
      QCOMPARE(QJsonDocument::fromJson(feature.attribute(RootsField).toString().toUtf8()).toVariant().toList(), QVariantList{rootUid});
    }
    layer.undoStack()->undo(); QCOMPARE(uid(layer.getFeature(child.id())), parentUid);
    QVERIFY(!layer.getFeature(grandchild.id()).isValid());
    layer.undoStack()->redo(); QCOMPARE(uid(layer.getFeature(child.id())), leftUid);
    QVERIFY2(layer.commitChanges(), qPrintable(layer.commitErrors().join(";")));
    const auto version = directory.filePath("split-version.gpkg");
    QVERIFY(MappingArtifactWriter::vectorSnapshot(&layer, version, &error));
    QgsVectorLayer reopened(version + "|layername=features", "version", "ogr");
    for (const auto &identity : {leftUid, rightUid}) {
      const auto feature = find(&reopened, identity); QVERIFY(feature.isValid());
      QCOMPARE(QJsonDocument::fromJson(feature.attribute(ParentsField).toString().toUtf8()).toVariant().toList(), QVariantList{parentUid});
      QCOMPARE(QJsonDocument::fromJson(feature.attribute(RootsField).toString().toUtf8()).toVariant().toList(), QVariantList{rootUid});
    }
  }
  void exactNativeMergeHasAllParentsAndDeletedSnapshot() {
    auto layer = polygons(); QString error;
    auto *observer = Observer::attach(layer.get(), &error); QVERIFY2(observer, qPrintable(error));
    auto features = layer->getFeatures(); QgsFeature a, b;
    QVERIFY(features.nextFeature(a)); QVERIFY(features.nextFeature(b)); features.close();
    QVERIFY(layer->startEditing()); layer->selectByIds({a.id(), b.id()});
    layer->beginEditCommand("native merge");
    QVERIFY(changeGeometry(layer.get(), a.id(), QgsGeometry::unaryUnion({a.geometry(), b.geometry()})));
    QVERIFY(layer->deleteFeature(b.id())); layer->endEditCommand();
    const auto merged = layer->getFeature(a.id());
    QVERIFY(uid(merged) != uid(a)); QVERIFY(uid(merged) != uid(b));
    const auto parents = QJsonDocument::fromJson(merged.attribute(ParentsField).toString().toUtf8()).toVariant().toList();
    QVERIFY(parents.contains(uid(a))); QVERIFY(parents.contains(uid(b))); QCOMPARE(parents.size(), 2);
    const auto changes = observer->changes(); QCOMPARE(changes.size(), 2);
    bool deleted = false;
    for (const auto &change : changes) deleted |= change.toMap().value("deleted").toBool();
    QVERIFY(deleted);
    layer->undoStack()->undo(); QCOMPARE(layer->featureCount(), qint64(2));
    QCOMPARE(uid(layer->getFeature(a.id())), uid(a));
    QCOMPARE(uid(layer->getFeature(b.id())), uid(b));
    layer->undoStack()->redo(); QCOMPARE(uid(layer->getFeature(a.id())), uid(merged));
  }
  void uncertainGeometryAndDeletionNeverInventMergeSupport() {
    auto layer = polygons(); QString error;
    QVERIFY(Observer::attach(layer.get(), &error));
    auto features = layer->getFeatures(); QgsFeature a, b;
    QVERIFY(features.nextFeature(a)); QVERIFY(features.nextFeature(b)); features.close();
    QVERIFY(layer->startEditing()); layer->selectByIds({a.id(), b.id()});
    layer->beginEditCommand("delete and move unrelated boundary");
    QVERIFY(changeGeometry(layer.get(), a.id(), QgsGeometry::fromWkt("POLYGON((0 0,9 0,9 9,0 9,0 0))")));
    QVERIFY(layer->deleteFeature(b.id())); layer->endEditCommand();
    const auto changed = layer->getFeature(a.id());
    QCOMPARE(uid(changed), uid(a));
    QVERIFY(changed.attribute(ReviewField).toBool());
    QVERIFY(!changed.attribute(ParentsField).toString().contains(uid(b)));
  }
  void explicitMergeApiStoresParentsAndEmitsOneBusinessEdit() {
    auto layer = polygons(); QString error;
    auto *observer = Observer::attach(layer.get(), &error); QVERIFY2(observer, qPrintable(error));
    const auto feature = first(layer.get());
    QSignalSpy edits(observer, &Observer::businessEdit);
    QVERIFY(layer->startEditing()); layer->selectByIds({feature.id()});
    QVERIFY2(setParents(layer.get(), feature.id(), {uid(feature), "historical-parent"}, &error), qPrintable(error));
    const auto merged = layer->getFeature(feature.id());
    QVERIFY(uid(merged) != uid(feature));
    QVERIFY(merged.attribute(ParentsField).toString().contains(uid(feature)));
    QVERIFY(merged.attribute(ParentsField).toString().contains("historical-parent"));
    QCOMPARE(edits.count(), 1); QCOMPARE(edits.first().at(1).toString(), QString("merge"));
    layer->undoStack()->undo(); QCOMPARE(uid(layer->getFeature(feature.id())), uid(feature));
    layer->undoStack()->redo(); QCOMPARE(uid(layer->getFeature(feature.id())), uid(merged));
    QCOMPARE(edits.count(), 1);
  }
  void duplicateAndCorruptLineageCannotResolveWrongFeature() {
    auto layer = polygons(); QString error; QVERIFY(ensure(layer.get(), &error));
    auto features = layer->getFeatures(); QgsFeature a, b;
    QVERIFY(features.nextFeature(a)); QVERIFY(features.nextFeature(b)); features.close();
    QVERIFY(layer->startEditing());
    QVERIFY(layer->changeAttributeValue(b.id(), layer->fields().indexOf(UidField), uid(a)));
    QVERIFY(!find(layer.get(), uid(a)).isValid());
    QVERIFY(ensure(layer.get(), &error));
    QVERIFY(uid(layer->getFeature(a.id())) != uid(a));
    QVERIFY(uid(layer->getFeature(b.id())) != uid(a));
    QVERIFY(layer->getFeature(a.id()).attribute(ReviewField).toBool());
    const auto corrected = uid(layer->getFeature(a.id()));
    QVERIFY(layer->changeAttributeValue(a.id(), layer->fields().indexOf(ParentsField), "broken"));
    QVERIFY(!ensure(layer.get(), &error));
    QCOMPARE(uid(layer->getFeature(a.id())), corrected);
    QVERIFY(!setParents(layer.get(), a.id(), {uid(b)}, &error));
  }
  void duplicateSnapshotForksInterpretationsAndPreservesPublishedHistory() {
    QTemporaryDir directory; QString error; auto initial = polygons();
    const auto sourcePath = directory.filePath("published.gpkg");
    QVERIFY(MappingArtifactWriter::vectorSnapshot(initial.get(), sourcePath, &error));
    QgsVectorLayer source(sourcePath + "|layername=features", "published", "ogr");
    QVERIFY(source.isValid());
    QVERIFY(source.startEditing());
    QVERIFY(source.addAttribute(QgsField("paleo_interpretation", QMetaType::Type::QString)));
    QVERIFY(source.addAttribute(QgsField("facies_evidence", QMetaType::Type::QString)));
    auto iterator = source.getFeatures(); QgsFeature a, b;
    QVERIFY(iterator.nextFeature(a)); QVERIFY(iterator.nextFeature(b)); iterator.close();
    const auto parent = uid(a);
    const QVariantMap historicalEvent{{"event_id", "historical-review"}, {"actor_id", "local:prior"},
      {"before_revision", 0}, {"after_revision", 1}, {"action", "review"},
      {"reason", "此前的人工确认"}, {"timestamp", "2026-10-13T08:00:00.000Z"}};
    const QVariantMap inherited{{"id", "parent-interpretation"}, {"assertion_id", "parent-assertion"},
      {"feature_uid", parent}, {"horizon", "D61"}, {"status", "Confirmed"}, {"revision", 1},
      {"history", QVariantList{historicalEvent}}};
    const QVariantList records{QVariantMap{{"id", "well-observation"}, {"type", "well"},
      {"role", "SUPPORTED_BY"}, {"verification", "Verified"}, {"revision", 1},
      {"feature_uid", parent}, {"horizon", "D61"}, {"text", "历史井解释"}}};
    const auto interpretationJson = paleo::evidence::encode(inherited), recordsJson = paleo::evidence::encode(records);
    for (auto fid : {a.id(), b.id()}) {
      QVERIFY(source.changeAttributeValues(fid, {{source.fields().indexOf(UidField), parent},
        {source.fields().indexOf("paleo_interpretation"), interpretationJson},
        {source.fields().indexOf("facies_evidence"), recordsJson}}));
    }
    QVERIFY(source.commitChanges()); source.setReadOnly(true);
    QFile sourceFile(sourcePath); QVERIFY(sourceFile.open(QIODevice::ReadOnly));
    const auto beforeHash = QCryptographicHash::hash(sourceFile.readAll(), QCryptographicHash::Sha256); sourceFile.close();
    const auto targetPath = directory.filePath("new-snapshot.gpkg");
    QVERIFY2(MappingArtifactWriter::vectorSnapshot(&source, targetPath, &error), qPrintable(error));
    QgsVectorLayer target(targetPath + "|layername=features", "target", "ogr"); QVERIFY(target.isValid());
    auto targets = target.getFeatures(); QgsFeature feature; QSet<QString> children, interpretations;
    while (targets.nextFeature(feature)) {
      QVariantMap interpretation; QVariantList evidence;
      QVERIFY2(paleo::evidence::decodeInterpretation(feature.attribute("paleo_interpretation").toString(), &interpretation, &error), qPrintable(error));
      QVERIFY(paleo::evidence::decodeRecords(feature.attribute("facies_evidence").toString(), &evidence, &error));
      QVERIFY(uid(feature) != parent); children.insert(uid(feature));
      QCOMPARE(interpretation.value("feature_uid").toString(), uid(feature));
      QCOMPARE(interpretation.value("status").toString(), QString("NeedsReview"));
      QVERIFY(interpretation.value("id").toString() != inherited.value("id").toString());
      QVERIFY(interpretation.value("assertion_id").toString() != inherited.value("assertion_id").toString());
      interpretations.insert(interpretation.value("id").toString());
      QCOMPARE(interpretation.value("inherited_interpretation").toMap(), inherited);
      QCOMPARE(interpretation.value("inherited_evidence").toList(), records);
      QCOMPARE(interpretation.value("history").toList().size(), 1);
      QCOMPARE(evidence.first().toMap().value("feature_uid").toString(), uid(feature));
      QVERIFY(feature.attribute(ReviewField).toBool());
    }
    QCOMPARE(children.size(), 2); QCOMPARE(interpretations.size(), 2);
    QCOMPARE(source.getFeature(a.id()).attribute("paleo_interpretation").toString(), interpretationJson);
    QCOMPARE(source.getFeature(b.id()).attribute(UidField).toString(), parent);
    QVERIFY(sourceFile.open(QIODevice::ReadOnly));
    QCOMPARE(QCryptographicHash::hash(sourceFile.readAll(), QCryptographicHash::Sha256), beforeHash);
  }
  void pendingReviewJournalSurvivesDeletionAndUndo() {
    auto layer = polygons(); QString error;
    QVERIFY(layer->dataProvider()->addAttributes({QgsField("paleo_interpretation", QMetaType::Type::QString)}));
    layer->updateFields();
    auto *observer = Observer::attach(layer.get(), &error); QVERIFY2(observer, qPrintable(error));
    const auto original = first(layer.get());
    const QVariantMap pending{{"id", "pending-review"}, {"assertion_id", "pending-assertion"},
      {"feature_uid", uid(original)}, {"status", "Disputed"}, {"revision", 1},
      {"history", QVariantList{QVariantMap{{"event_id", "unsaved-review-event"},
        {"actor_id", "local:researcher"}, {"before_revision", 0}, {"after_revision", 1},
        {"reason", "保留尚待处理的争议"}, {"timestamp", "2026-10-13T08:00:00.000Z"}}}}};
    const auto json = paleo::evidence::encode(pending);
    QVERIFY(layer->startEditing());
    layer->beginEditCommand("review without saving a map version");
    QVERIFY(layer->changeAttributeValue(original.id(), layer->fields().indexOf("paleo_interpretation"), json));
    layer->endEditCommand(); QVERIFY(observer->changes().isEmpty());
    layer->beginEditCommand("delete reviewed feature");
    QVERIFY(layer->deleteFeature(original.id())); layer->endEditCommand();
    QCOMPARE(observer->changes().size(), 1);
    const auto change = observer->changes().first().toMap();
    QCOMPARE(change.value("before_interpretation").toString(), json);
    QCOMPARE(change.value("before_uid").toString(), uid(original));
    QCOMPARE(change.value("before_facies_code").toInt(), 1);
    QVERIFY(change.value("deleted").toBool());
    layer->undoStack()->undo(); QVERIFY(observer->changes().isEmpty());
    QCOMPARE(layer->getFeature(original.id()).attribute("paleo_interpretation").toString(), json);
  }
  void highlightIsReadOnlyStableAndRestoresAcrossReload() {
    auto layer = polygons(); QString error; QVERIFY(ensure(layer.get(), &error));
    const auto feature = first(layer.get());
    auto *canvas = new QgsMapCanvas;
    canvas->setDestinationCrs(layer->crs());
    EvidenceMapAdapter adapter(canvas);
    adapter.bindLayer(layer.get());
    QSignalSpy selected(&adapter, &EvidenceMapAdapter::featuresSelected);
    QVERIFY2(adapter.showFeature(uid(feature), true, &error), qPrintable(error));
    QVERIFY(!layer->isEditable()); QVERIFY(layer->selectedFeatureIds().isEmpty());
    layer->selectByIds({feature.id()}); QCOMPARE(selected.count(), 1);
    QCOMPARE(selected.first().at(2).toStringList(), QStringList{uid(feature)});
    QTemporaryDir directory; const auto path = directory.filePath("reload.gpkg");
    QVERIFY(MappingArtifactWriter::vectorSnapshot(layer.get(), path, &error));
    adapter.bindLayer(nullptr); layer.reset();
    QgsVectorLayer reload(path + "|layername=features", "reload", "ogr");
    reload.setCustomProperty("paleoLayerId", "draft.identity");
    reload.setCustomProperty("paleoVersionId", "version1");
    adapter.bindLayer(&reload); QCoreApplication::processEvents();
    QCOMPARE(adapter.highlightedUid(), uid(feature));
    QVERIFY(adapter.showFeature(uid(feature), false, &error));
    QVERIFY(!adapter.showFeature("deleted", false, &error));
    QVERIFY(!error.isEmpty());
    // A queued restore for the old feature cannot overwrite a newer choice.
    adapter.showFeature(uid(feature), false, &error);
    reload.triggerRepaint(); adapter.clear(); QCoreApplication::processEvents();
    QVERIFY(adapter.highlightedUid().isEmpty());
    delete canvas;
    QVERIFY(!adapter.showFeature(uid(feature), false, &error));
  }
  void legacyMapSelectionDoesNotPretendFidIsStable() {
    auto layer = polygons(); QgsMapCanvas canvas; canvas.setDestinationCrs(layer->crs());
    EvidenceMapAdapter adapter(&canvas); adapter.bindLayer(layer.get());
    QSignalSpy selected(&adapter, &EvidenceMapAdapter::featuresSelected);
    QSignalSpy unavailable(&adapter, &EvidenceMapAdapter::unavailable);
    layer->selectByIds({first(layer.get()).id()});
    QCOMPARE(selected.count(), 0); QCOMPARE(unavailable.count(), 1);
    QString error; QVERIFY(adapter.showExtent(true, &error));
    QCOMPARE(layer->fields().indexOf(UidField), -1);
  }
  void rasterSourceExtentTransformsWithoutEditingOrSelectingLayers() {
    QTemporaryDir directory; const auto path = directory.filePath("source.tif");
    QString error;
    QVERIFY(MappingArtifactWriter::raster(path, {1, 2, 3, 4}, 2, 2,
                                         QRectF(0, 0, 20, 20), &error));
    QgsRasterLayer source(path, "source", "gdal"); QVERIFY(source.isValid());
    // This test fixture deliberately sets a real projection, separate from
    // the workstation's datum-free local engineering grid.
    source.setCrs(QgsCoordinateReferenceSystem("EPSG:3857"));
    QgsMapCanvas canvas; canvas.setDestinationCrs(QgsCoordinateReferenceSystem("EPSG:4326"));
    EvidenceMapAdapter adapter(&canvas);
    QVERIFY2(adapter.showLayer(&source, true, &error), qPrintable(error));
    QVERIFY(!canvas.currentLayer()); QVERIFY(!adapter.layer());
    QVERIFY(canvas.extent().width() > 0); QVERIFY(canvas.extent().width() < 1);
    // #822 口径：缺坐标系（空 CRS）回落 EPSG:32650 且可定位，不再拒收。
    source.setCrs(QgsCoordinateReferenceSystem()); error.clear();
    QVERIFY2(adapter.showLayer(&source, true, &error), qPrintable(error)); QVERIFY(error.isEmpty());
    QCOMPARE(source.crs().authid(), QString("EPSG:32650"));
    QVERIFY(!canvas.currentLayer()); QVERIFY(!adapter.layer());
    const auto fallback = canvas.extent().center(); // UTM50N 东距 0..20m ≈ 112.5°E, 赤道
    QVERIFY(canvas.extent().width() > 0); QVERIFY(fallback.x() > 110 && fallback.x() < 115);
    QVERIFY(std::abs(fallback.y()) < 1);
  }
  void pointNavigationRejectsUnavailableOrDestroyedCanvas() {
    const auto wkt = QgsCoordinateReferenceSystem("EPSG:3857").toWkt(); QString error;
    EvidenceMapAdapter missing(nullptr); QSignalSpy failures(&missing, &EvidenceMapAdapter::unavailable);
    QVERIFY(!missing.showPoint(10, 20, wkt, true, &error));
    QCOMPARE(failures.count(), 1); QVERIFY(!error.isEmpty());
    auto *canvas = new QgsMapCanvas; canvas->setDestinationCrs(QgsCoordinateReferenceSystem("EPSG:3857"));
    EvidenceMapAdapter adapter(canvas);
    QVERIFY(adapter.showPoint(10, 20, wkt, true, &error));
    QVERIFY(!canvas->currentLayer()); QVERIFY(!adapter.layer());
    delete canvas;
    QVERIFY(!adapter.showPoint(10, 20, wkt, true, &error)); QVERIFY(!error.isEmpty());
  }
  void pointNavigationTransformsCoordinatesAndRejectsBadCrs() {
    QgsMapCanvas canvas; canvas.setDestinationCrs(QgsCoordinateReferenceSystem("EPSG:3857"));
    canvas.setExtent(QgsRectangle(-1000, -1000, 1000, 1000));
    EvidenceMapAdapter adapter(&canvas); QString error;
    // 非法（非空且不可解析）坐标系仍拒收；#822 只回落「缺失」的空坐标系。
    QVERIFY(!adapter.showPoint(10, 20, "invalid crs", true, &error));
    QVERIFY(!error.isEmpty());
    error.clear(); QVERIFY(!adapter.showPoint(10, 20, "PROJCS[\"broken\"", true, &error));
    QVERIFY(!error.isEmpty());
    error.clear(); QVERIFY2(adapter.showPoint(500000, 0, QString(), true, &error), qPrintable(error));
    QVERIFY(error.isEmpty()); // 缺失 → EPSG:32650：UTM50N (500000,0) = 117°E 赤道
    const auto missingCenter = canvas.extent().center();
    QVERIFY(std::abs(missingCenter.x() - 13024380.42) < 1000); QVERIFY(std::abs(missingCenter.y()) < 1000);
    const auto wkt = QgsCoordinateReferenceSystem("EPSG:4326").toWkt();
    QVERIFY(adapter.showPoint(1, 1, wkt, true, &error));
    const auto center = canvas.extent().center();
    QVERIFY(center.x() > 100000 && center.x() < 120000);
    QVERIFY(center.y() > 100000 && center.y() < 120000);
    QVERIFY(!canvas.currentLayer()); QVERIFY(!adapter.layer());
    QVERIFY(!adapter.showPoint(std::numeric_limits<double>::infinity(), 1, wkt, true, &error));
  }
  void featureIdCacheInvalidatesForEditsRollbackAndMissingLayer() {
    auto layer = polygons(); QString error; QVERIFY(ensure(layer.get(), &error));
    auto iterator = layer->getFeatures(); QgsFeature a, b;
    QVERIFY(iterator.nextFeature(a)); QVERIFY(iterator.nextFeature(b)); iterator.close();
    QgsMapCanvas canvas; canvas.setDestinationCrs(layer->crs());
    EvidenceMapAdapter adapter(&canvas); adapter.bindLayer(layer.get());
    QCOMPARE(adapter.featureId(uid(a), &error), a.id());
    QCOMPARE(adapter.featureId(uid(a), &error), a.id());
    QCOMPARE(adapter.indexBuildCount(), 1);
    QVERIFY(layer->startEditing());
    QVERIFY(layer->changeAttributeValue(a.id(), layer->fields().indexOf(UidField), "revised-uid"));
    QVERIFY(FID_IS_NULL(adapter.featureId(uid(a), &error)));
    QCOMPARE(adapter.featureId("revised-uid", &error), a.id());
    QCOMPARE(adapter.indexBuildCount(), 2);
    QVERIFY(layer->changeAttributeValue(b.id(), layer->fields().indexOf(UidField), "revised-uid"));
    QVERIFY(FID_IS_NULL(adapter.featureId("revised-uid", &error)));
    QVERIFY(layer->rollBack());
    QCOMPARE(adapter.featureId(uid(a), &error), a.id());
    layer.reset(); QVERIFY(!adapter.layer());
    QVERIFY(FID_IS_NULL(adapter.featureId(uid(a), &error)));
  }
  void sameProviderNewVersionRefreshesNavigationIdentity() {
    auto layer = polygons(); QString error; QVERIFY(ensure(layer.get(), &error));
    const auto feature = first(layer.get());
    QgsMapCanvas canvas; canvas.setDestinationCrs(layer->crs());
    EvidenceMapAdapter adapter(&canvas); adapter.bindLayer(layer.get());
    QVERIFY(adapter.showFeature(uid(feature), false, &error));
    layer->setCustomProperty("paleoVersionId", "version2"); adapter.bindLayer(layer.get());
    QSignalSpy selection(&adapter, &EvidenceMapAdapter::featuresSelected);
    layer->selectByIds({feature.id()}); QCOMPARE(selection.count(), 1);
    QCOMPARE(selection.first().at(1).toString(), QString("version2"));
    QCOMPARE(adapter.highlightedUid(), uid(feature));
    QCoreApplication::processEvents();
    QCOMPARE(adapter.highlightedUid(), uid(feature));
  }
  void spatialSnapshotAndExactCandidatesDoNotMutateSelection() {
    auto layer = polygons(); QString error; QVERIFY(ensure(layer.get(), &error));
    const auto feature = first(layer.get()); EvidenceMapAdapter adapter(nullptr); adapter.bindLayer(layer.get());
    const auto snapshot = adapter.featureSnapshot(uid(feature), &error);
    QCOMPARE(snapshot.value("state").toString(), QString("available"));
    QCOMPARE(snapshot.value("feature_uid").toString(), uid(feature));
    QCOMPARE(snapshot.value("geometry_hash").toString(), geometryFingerprint(feature));
    QCOMPARE(snapshot.value("version_id").toString(), QString("version1"));
    QVERIFY(!snapshot.value("wkb").toByteArray().isEmpty());
    const auto target = [&](const QString &wkt) {
      return QVariantMap{{"wkb", QgsGeometry::fromWkt(wkt).asWkb()}, {"crs_wkt", layer->crs().toWkt()}};
    };
    const auto local = adapter.spatialCandidates(target("POINT(1 1)"));
    QCOMPARE(local.value("state").toString(), QString("intersects"));
    QCOMPARE(local.value("features").toList().size(), 1);
    const auto candidate = local.value("features").toList().first().toMap();
    QCOMPARE(candidate.value("feature_uid").toString(), uid(feature));
    QCOMPARE(candidate.value("identity_state").toString(), QString("stable"));
    QCOMPARE(candidate.value("association_basis").toString(), QString("exact_geometry_intersection"));
    QVERIFY(!candidate.contains("role")); QVERIFY(!candidate.contains("wkb"));
    QCOMPARE(adapter.spatialCandidates(target("POINT(100 100)"), uid(feature)).value("state").toString(), QString("disjoint"));
    QCOMPARE(adapter.spatialCandidates(target("POINT(1 1)"), "missing").value("state").toString(), QString("missing"));
    QCOMPARE(adapter.featureSnapshot("missing").value("state").toString(), QString("missing"));
    QCOMPARE(adapter.indexBuildCount(), 1);
    QVERIFY(!layer->isEditable()); QVERIFY(layer->selectedFeatureIds().isEmpty()); QVERIFY(adapter.highlightedUid().isEmpty());
    // Bounds overlap is deliberately insufficient: the triangle excludes (9,9).
    QVERIFY(layer->startEditing());
    QVERIFY(changeGeometry(layer.get(), feature.id(), QgsGeometry::fromWkt("POLYGON((0 0,10 0,0 10,0 0))")));
    QVERIFY(layer->commitChanges());
    QCOMPARE(adapter.spatialCandidates(target("POINT(9 9)"), uid(feature)).value("state").toString(), QString("disjoint"));
    QCOMPARE(adapter.spatialCandidates(target("POINT(1 1)"), uid(feature)).value("state").toString(), QString("intersects"));
    auto features = layer->getFeatures(); QgsFeature a, b;
    QVERIFY(features.nextFeature(a)); QVERIFY(features.nextFeature(b)); features.close();
    QVERIFY(layer->startEditing()); QVERIFY(layer->changeAttributeValue(b.id(), layer->fields().indexOf(UidField), uid(a)));
    QCOMPARE(adapter.featureSnapshot(uid(a)).value("state").toString(), QString("ambiguous"));
    QCOMPARE(adapter.spatialCandidates(target("POINT(1 1)"), uid(a)).value("state").toString(), QString("ambiguous"));
    QVERIFY(layer->rollBack());
  }
  void spatialCandidatesTransformCrsAndFailHonestlyForUnknownScope() {
    QgsVectorLayer layer("Point?crs=EPSG:3857&field=paleo_feature_uid:string", "projected", "memory");
    QgsFeature feature(layer.fields()); feature.setAttribute(UidField, "projected-point");
    feature.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(111319.490793, 111325.142866)));
    QgsFeatureList rows{feature}; QVERIFY(layer.dataProvider()->addFeatures(rows));
    EvidenceMapAdapter adapter(nullptr); adapter.bindLayer(&layer);
    QVariantMap target{{"wkb", QgsGeometry::fromWkt("POLYGON((0.999 0.999,1.001 0.999,1.001 1.001,0.999 1.001,0.999 0.999))").asWkb()},
      {"crs_wkt", QgsCoordinateReferenceSystem("EPSG:4326").toWkt()}};
    QCOMPARE(adapter.spatialCandidates(target, "projected-point").value("state").toString(), QString("intersects"));
    target.insert("crs_wkt", "invalid crs"); QString error;
    QCOMPARE(adapter.spatialCandidates(target, {}, 200, &error).value("state").toString(), QString("unknown")); QVERIFY(!error.isEmpty());
    target.insert("crs_wkt", layer.crs().toWkt()); target.insert("wkb", QByteArray("invalid wkb"));
    QCOMPARE(adapter.spatialCandidates(target).value("state").toString(), QString("unknown"));
    EvidenceMapAdapter absent(nullptr);
    QCOMPARE(absent.spatialCandidates(target).value("state").toString(), QString("unknown"));
    auto legacy = polygons(); adapter.bindLayer(legacy.get());
    target.insert("wkb", QgsGeometry::fromWkt("POINT(1 1)").asWkb());
    QCOMPARE(adapter.spatialCandidates(target, "fictional-fid").value("state").toString(), QString("missing"));
    const auto legacyRow = adapter.spatialCandidates(target).value("features").toList().first().toMap();
    QCOMPARE(legacyRow.value("identity_state").toString(), QString("legacy_missing"));
    // #822 口径：来源图层缺坐标系回落 EPSG:32650 并给出确定判定（3857 的 (1,1) 远离 UTM50N 原点附近要素）。
    legacy->setCrs(QgsCoordinateReferenceSystem());
    QCOMPARE(adapter.spatialCandidates(target).value("state").toString(), QString("disjoint"));
    QCOMPARE(legacy->crs().authid(), QString("EPSG:32650"));
    target.insert("crs_wkt", "invalid crs"); // 目标侧非法坐标系仍判 unknown
    QCOMPARE(adapter.spatialCandidates(target).value("state").toString(), QString("unknown"));
  }
  void spatialCandidatesAreBoundedAndRasterScopeIsExtentOnly() {
    QgsVectorLayer layer("Point?crs=EPSG:3857&field=paleo_feature_uid:string", "bounded", "memory");
    QgsFeatureList rows;
    for (int n = 0; n < 250; ++n) {
      QgsFeature feature(layer.fields()); feature.setAttribute(UidField, QString("point-%1").arg(n));
      feature.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(n, 1))); rows << feature;
    }
    QVERIFY(layer.dataProvider()->addFeatures(rows)); layer.updateExtents();
    EvidenceMapAdapter adapter(nullptr); adapter.bindLayer(&layer);
    const QVariantMap target{{"wkb", QgsGeometry::fromWkt("POLYGON((-1 0,300 0,300 20,-1 20,-1 0))").asWkb()},
      {"crs_wkt", layer.crs().toWkt()}};
    const auto limited = adapter.spatialCandidates(target, {}, 10);
    QCOMPARE(limited.value("features").toList().size(), 10); QVERIFY(limited.value("truncated").toBool());
    const auto capped = adapter.spatialCandidates(target, {}, 100000);
    QCOMPARE(capped.value("features").toList().size(), 200); QVERIFY(capped.value("truncated").toBool());
    QCOMPARE(adapter.indexBuildCount(), 1); QVERIFY(!layer.isEditable());
    QTemporaryDir directory; const auto path = directory.filePath("extent.tif"); QString error;
    QVERIFY(MappingArtifactWriter::raster(path, {1, 2, 3, 4}, 2, 2, QRectF(0, 0, 20, 20), &error));
    QgsRasterLayer raster(path, "raster", "gdal"); QVERIFY(raster.isValid()); raster.setCrs(layer.crs());
    const auto extent = adapter.spatialCandidates(&raster, target);
    QCOMPARE(extent.value("state").toString(), QString("intersects"));
    QCOMPARE(extent.value("scope").toString(), QString("raster_extent"));
    QVERIFY(extent.value("features").toList().isEmpty());
    QVERIFY(!extent.contains("role"));
    QCOMPARE(adapter.spatialCandidates(&raster, target, "pixel-as-uid").value("state").toString(), QString("missing"));
    QCOMPARE(adapter.layer(), &layer); // pure source query preserves the primary binding
  }
  void editingLargeLayerDoesNotRebuildIdentityIndexPerCommand() {
    QgsVectorLayer layer("Point?crs=EPSG:3857&field=paleo_feature_uid:string&field=facies_code:integer", "large-working", "memory");
    QVERIFY(layer.dataProvider()->addAttributes({QgsField(ParentsField, QMetaType::Type::QString),
                                                QgsField(RootsField, QMetaType::Type::QString),
                                                QgsField(ReviewField, QMetaType::Type::Bool)}));
    layer.updateFields(); QgsFeatureList features; features.reserve(100000);
    for (int n = 0; n < 100000; ++n) {
      QgsFeature feature(layer.fields()); feature.setAttribute(UidField, QString("feature-%1").arg(n));
      feature.setAttribute("facies_code", 1); feature.setAttribute(ParentsField, "[]"); feature.setAttribute(ReviewField, false);
      feature.setAttribute(RootsField, QString("[\"feature-%1\"]").arg(n));
      feature.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(n, 0))); features << feature;
    }
    QVERIFY(layer.dataProvider()->addFeatures(features)); features.clear();
    QString error; auto *observer = Observer::attach(&layer, &error); QVERIFY2(observer, qPrintable(error));
    const auto feature = first(&layer); QVERIFY(layer.startEditing()); layer.selectByIds({feature.id()});
    const int beforeBuilds = observer->identityIndexBuildCount(); QElapsedTimer timer; timer.start();
    for (int n = 0; n < 20; ++n) {
      layer.beginEditCommand("drag point");
      QVERIFY(changeGeometry(&layer, feature.id(), QgsGeometry::fromPointXY(QgsPointXY(n + 1, 7))));
      layer.endEditCommand();
    }
    qInfo() << "identity edit scale 100000, 20 commands ms" << timer.elapsed();
    QCOMPARE(observer->identityIndexBuildCount(), beforeBuilds);
    QCOMPARE(observer->changes().size(), 1);
  }
  void scaleUsesOneIndexForRapidSelections_data() {
    QTest::addColumn<int>("count"); QTest::newRow("10000") << 10000; QTest::newRow("100000") << 100000;
  }
  void scaleUsesOneIndexForRapidSelections() {
    QFETCH(int, count);
    QgsVectorLayer layer("Point?crs=EPSG:3857&field=paleo_feature_uid:string", "scale", "memory");
    QgsFeatureList features; features.reserve(count);
    for (int i = 0; i < count; ++i) {
      QgsFeature feature(layer.fields()); feature.setAttribute(UidField, QString("feature-%1").arg(i));
      feature.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(i, i % 100)));
      features << feature;
    }
    QVERIFY(layer.dataProvider()->addFeatures(features)); layer.updateExtents(); features.clear();
    QgsMapCanvas canvas; canvas.setDestinationCrs(layer.crs());
    EvidenceMapAdapter adapter(&canvas); adapter.bindLayer(&layer);
    QElapsedTimer elapsed; elapsed.start(); QString error;
    for (int i = 0; i < 100; ++i)
    {
      const auto identity = QString("feature-%1").arg((i * 997) % count);
      QVERIFY(!FID_IS_NULL(adapter.featureId(identity, &error)));
      QVERIFY2(adapter.showFeature(identity, false, &error), qPrintable(error));
    }
    qInfo() << "identity scale" << count << "100 selections ms" << elapsed.elapsed();
    QCOMPARE(adapter.indexBuildCount(), 1);
    QVERIFY(!layer.isEditable()); QVERIFY(layer.selectedFeatureIds().isEmpty());
  }
};
int main(int argc, char **argv) {
  QgsApplication application(argc, argv, false);
  application.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")), true);
  application.initQgis();
  TestFeatureIdentity test; const int result = QTest::qExec(&test, argc, argv);
  QgsApplication::exitQgis(); return result;
}
#include "tst_featureidentity.moc"
