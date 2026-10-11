// 层：测试
// facies-ux 4：工作副本属性修订自动保存（落盘、几何留缓冲、撤销、编辑锁）。
#include <QtTest>
#include <QTemporaryDir>
#include <qgsapplication.h>
#include <qgscoordinatetransformcontext.h>
#include <qgsfeature.h>
#include <qgsgeometry.h>
#include <qgsvectorfilewriter.h>
#include <qgsvectorlayer.h>
#include <qgsvectorlayereditbuffer.h>
#include <memory>

#include "../src/qgis/draftautosave.h"

namespace {
int storedCode(const QString &path, qint64 fid) {
  QgsVectorLayer fresh(path, "fresh", "ogr");
  return fresh.getFeature(fid).attribute("facies_code").toInt();
}
} // namespace

class TestDraftAutosave : public QObject {
  Q_OBJECT
  QTemporaryDir m_dir;
  QString m_path;

private slots:
  void init() {
    QgsVectorLayer mem("Polygon?field=facies_code:integer&field=facies_name:string", "m", "memory");
    QgsFeature a(mem.fields()), b(mem.fields());
    a.setAttributes({1, "A"});
    a.setGeometry(QgsGeometry::fromWkt("POLYGON((0 0,1 0,1 1,0 1,0 0))"));
    b.setAttributes({2, "B"});
    b.setGeometry(QgsGeometry::fromWkt("POLYGON((1 0,2 0,2 1,1 1,1 0))"));
    QgsFeatureList list{a, b};
    QVERIFY(mem.dataProvider()->addFeatures(list));
    m_path = m_dir.filePath(QStringLiteral("draft-%1.gpkg").arg(QTest::currentTestFunction()));
    QgsVectorFileWriter::SaveVectorOptions options;
    options.driverName = "GPKG";
    options.layerName = "features";
    QString err;
    QCOMPARE(QgsVectorFileWriter::writeAsVectorFormatV3(&mem, m_path, QgsCoordinateTransformContext(),
                                                        options, &err),
             QgsVectorFileWriter::NoError);
    m_path += "|layername=features";
  }
  void attributeEditIsSavedAndUndoable() {
    QgsVectorLayer layer(m_path, "draft", "ogr");
    QVERIFY(layer.isValid());
    auto *autosave = DraftAutosave::attach(&layer);
    QCOMPARE(DraftAutosave::attach(&layer), autosave);
    QSignalSpy saved(autosave, &DraftAutosave::saved);
    QgsFeature first;
    QVERIFY(layer.getFeatures().nextFeature(first));
    const qint64 fid = first.id();
    // 未处于编辑态：不越权开启编辑。
    QVERIFY(!autosave->flush());
    QVERIFY(layer.startEditing());
    layer.beginEditCommand("change");
    QVERIFY(layer.changeAttributeValue(fid, layer.fields().indexOf("facies_code"), 3));
    layer.endEditCommand();
    QTRY_COMPARE(saved.count(), 1);
    QVERIFY(layer.isEditable());
    QVERIFY(!layer.editBuffer()->isModified());
    QCOMPARE(storedCode(m_path, fid), 3);
    QVERIFY(autosave->canUndo());
    QString error;
    QVERIFY2(autosave->undo(&error), qPrintable(error));
    QCOMPARE(storedCode(m_path, fid), 1);
    QVERIFY(!autosave->canUndo());
    QVERIFY(layer.isEditable());
    QVERIFY(!autosave->undo(&error));
    QVERIFY(!error.isEmpty());
    layer.rollBack();
  }
  void geometryEditStaysInBufferWithUndo() {
    QgsVectorLayer layer(m_path, "draft", "ogr");
    auto *autosave = DraftAutosave::attach(&layer);
    QSignalSpy saved(autosave, &DraftAutosave::saved);
    QgsFeature first;
    QVERIFY(layer.getFeatures().nextFeature(first));
    QVERIFY(layer.startEditing());
    layer.beginEditCommand("move");
    QgsGeometry moved = QgsGeometry::fromWkt("POLYGON((0 0,3 0,3 3,0 3,0 0))");
    QVERIFY(layer.changeGeometry(first.id(), moved));
    layer.endEditCommand();
    QTest::qWait(20);
    QCOMPARE(saved.count(), 0);
    QVERIFY(layer.editBuffer()->isModified());
    QVERIFY(layer.undoStack()->canUndo());
    // 几何未提交时改属性：整批仍留缓冲（不拆分提交）。
    layer.beginEditCommand("change");
    QVERIFY(layer.changeAttributeValue(first.id(), layer.fields().indexOf("facies_code"), 2));
    layer.endEditCommand();
    QTest::qWait(20);
    QCOMPARE(saved.count(), 0);
    QCOMPARE(storedCode(m_path, first.id()), 1);
    layer.rollBack();
  }
  void stopEditingClearsHistory() {
    QgsVectorLayer layer(m_path, "draft", "ogr");
    auto *autosave = DraftAutosave::attach(&layer);
    QgsFeature first;
    QVERIFY(layer.getFeatures().nextFeature(first));
    QVERIFY(layer.startEditing());
    QVERIFY(layer.changeAttributeValue(first.id(), layer.fields().indexOf("facies_code"), 2));
    QTRY_VERIFY(autosave->canUndo());
    QVERIFY(layer.commitChanges());
    QVERIFY(!autosave->canUndo());
    QCOMPARE(storedCode(m_path, first.id()), 2);
  }
};

int main(int argc, char *argv[]) {
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")), true);
  app.initQgis();
  TestDraftAutosave tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgsApplication::exitQgis();
  return rc;
}
#include "tst_draftautosave.moc"
