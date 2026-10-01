#include <QtTest>
#include <QComboBox>
#include <QLabel>
#include <QTemporaryDir>

#include <qgsmapcanvas.h>
#include <qgsvectorlayer.h>
#include <qgsattributetableview.h>
#include <qgsfeature.h>
#include <qgsgeometry.h>
#include <qgspointxy.h>

#include "../src/ui/attributetablepanel.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/qgis/qgiseditingservice.h"
#include "../src/metadata/paleoprojectstore.h"
#include "uipolish_capture.h"

#include <qgsattributetablefiltermodel.h>
#include <QSignalSpy>
#include <QToolButton>
#include <QDebug>

// AttributeTablePanel: provider-resolved vector layer → native
// QgsAttributeTableView chain (cache + model + filter) shows rows.
class TestAttrPanel : public QObject
{
  Q_OBJECT
private slots:

  void showsLayerRows()
  {
    QgsMapCanvas canvas;
    auto *vl = new QgsVectorLayer(
        QStringLiteral("Point?crs=EPSG:4326&field=name:string&field=z:double"),
        QStringLiteral("wells"), QStringLiteral("memory"));
    QVERIFY(vl->isValid());
    QgsFeature f(vl->fields());
    f.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(1, 2)));
    f.setAttribute(QStringLiteral("name"), QStringLiteral("W1"));
    f.setAttribute(QStringLiteral("z"), 12.5);
    QList<QgsFeature> feats{f};
    QVERIFY(vl->dataProvider()->addFeatures(feats));
    vl->updateExtents();

    AttributeTablePanel panel(
        &canvas, [vl](const QString &id) -> QgsVectorLayer * {
          return id == QLatin1String("wells") ? vl : nullptr;
        });
    panel.setLayerIds({QStringLiteral("wells"), QStringLiteral("constraints.T1")});
    panel.showLayer(QStringLiteral("wells"));

    QCOMPARE(panel.currentLayerId(), QStringLiteral("wells"));
    auto *view = panel.findChild<QgsAttributeTableView *>(QStringLiteral("attrView"));
    QVERIFY(view->model());
    QCOMPARE(view->model()->rowCount(), 1);

    // Unknown layer id → hint visible, model cleared.
    panel.showLayer(QStringLiteral("nope"));
    QVERIFY(panel.findChild<QLabel *>(QStringLiteral("attrEmptyHint"))->isVisibleTo(&panel));
    delete vl;
  }

  void nullProviderSafe()
  {
    QgsMapCanvas canvas;
    AttributeTablePanel panel(&canvas, {});
    panel.showLayer(QStringLiteral("x")); // must not crash
  }

  // goal/ui-experience-polish：属性表行 + 空态提示的修前/修后截图证据。
  void captureEvidence()
  {
    QgsMapCanvas canvas;
    auto *vl = new QgsVectorLayer(
        QStringLiteral("Point?crs=EPSG:4326&field=name:string&field=z:double"),
        QStringLiteral("wells"), QStringLiteral("memory"));
    QVERIFY(vl->isValid());
    QgsFeature f(vl->fields());
    f.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(1, 2)));
    f.setAttribute(QStringLiteral("name"), QStringLiteral("W1"));
    f.setAttribute(QStringLiteral("z"), 12.5);
    QList<QgsFeature> feats{f}; // addFeatures 收非常量左值引用——花括号临时量不可绑定
    QVERIFY(vl->dataProvider()->addFeatures(feats));
    AttributeTablePanel panel(
        &canvas, [vl](const QString &id) -> QgsVectorLayer * {
          return id == QLatin1String("wells") ? vl : nullptr;
        });
    panel.setLayerIds({QStringLiteral("wells")});
    panel.showLayer(QStringLiteral("wells"));
    uipolish::capturePanel(&panel, QStringLiteral("attrpanel"));
    delete vl;
  }

  // ---- mapping 主线4：属性表编辑入管线 ----

  void editingSessionPipelineThroughService()
  {
    QgsMapCanvas canvas;
    auto *vl = new QgsVectorLayer(
        QStringLiteral("Point?crs=EPSG:4326&field=name:string"),
        QStringLiteral("wells"), QStringLiteral("memory"));
    QVERIFY(vl->isValid());
    QgsFeature f(vl->fields());
    f.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(1, 2)));
    f.setAttribute(QStringLiteral("name"), QStringLiteral("W1"));
    QList<QgsFeature> feats{f};
    QVERIFY(vl->dataProvider()->addFeatures(feats));

    // 服务在场：begin → markLayerBusy("edit")；commit → 单写者队列 + markLayerFree
    //（与 MapVersionController::saveVersion 同一 commitEdit 语义）。
    PaleoProjectStore store;
    QgisEditingService svc(&store);
    AttributeTablePanel panel(
        &canvas, [vl](const QString &id) -> QgsVectorLayer * {
          return id == QLatin1String("wells") ? vl : nullptr;
        });
    panel.setEditingService(&svc);
    panel.setLayerIds({QStringLiteral("wells")});
    panel.showLayer(QStringLiteral("wells"));
    QSignalSpy startedSpy(&panel, &AttributeTablePanel::editingStarted);
    QSignalSpy stoppedSpy(&panel, &AttributeTablePanel::editingStopped);

    QVERIFY(panel.beginEditing());
    QCOMPARE(startedSpy.count(), 1);
    QVERIFY(vl->isEditable());
    QVERIFY(store.layerBusy(vl->id()));

    // 会话内改动走原生 edit command（单步 undo）——值改动与 undo 栈一致性。
    vl->beginEditCommand(QStringLiteral("Attribute changed"));
    const QgsFeatureId fid = *vl->allFeatureIds().constBegin();
    QVERIFY(vl->changeAttributeValue(fid, 0, QStringLiteral("W9")));
    vl->endEditCommand();
    QCOMPARE(vl->undoStack()->count(), 1);
    QCOMPARE(vl->getFeature(fid).attribute(0).toString(), QStringLiteral("W9"));

    QVERIFY(panel.saveEditing());
    QCOMPARE(stoppedSpy.count(), 1);
    QCOMPARE(stoppedSpy.at(0).at(1).toBool(), true);
    QVERIFY(!vl->isEditable());
    QVERIFY(!store.layerBusy(vl->id())); // markLayerFree 时机：提交即释放
    QCOMPARE(vl->undoStack()->count(), 0); // commit 清栈（版本边界）
    QCOMPARE(vl->getFeature(fid).attribute(0).toString(), QStringLiteral("W9"));

    delete vl;
  }

  void editingPipelineWithoutServiceFallsBack()
  {
    QgsMapCanvas canvas;
    auto *vl = new QgsVectorLayer(
        QStringLiteral("Point?crs=EPSG:4326&field=name:string"),
        QStringLiteral("wells"), QStringLiteral("memory"));
    QVERIFY(vl->isValid());
    QgsFeature f(vl->fields());
    f.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(1, 2)));
    f.setAttribute(QStringLiteral("name"), QStringLiteral("W1"));
    QList<QgsFeature> feats{f};
    QVERIFY(vl->dataProvider()->addFeatures(feats));

    AttributeTablePanel panel(
        &canvas, [vl](const QString &id) -> QgsVectorLayer * {
          return id == QLatin1String("wells") ? vl : nullptr;
        });
    panel.setLayerIds({QStringLiteral("wells")});
    panel.showLayer(QStringLiteral("wells"));
    QSignalSpy stoppedSpy(&panel, &AttributeTablePanel::editingStopped);

    // 无服务降级：直连 startEditing/rollBack（无 busy 标记，同工具条降级）。
    QVERIFY(panel.beginEditing());
    QVERIFY(vl->isEditable());
    const QgsFeatureId fid = *vl->allFeatureIds().constBegin();
    vl->beginEditCommand(QStringLiteral("Attribute changed"));
    QVERIFY(vl->changeAttributeValue(fid, 0, QStringLiteral("XX")));
    vl->endEditCommand();

    QVERIFY(panel.cancelEditing());
    QCOMPARE(stoppedSpy.count(), 1);
    QCOMPARE(stoppedSpy.at(0).at(1).toBool(), false);
    QVERIFY(!vl->isEditable());
    QCOMPARE(vl->getFeature(fid).attribute(0).toString(), QStringLiteral("W1")); // 回滚还原

    // 按钮态跟随：保存/放弃禁用（无会话），编辑可用。
    auto *save = panel.findChild<QToolButton *>(QStringLiteral("attrEditSaveButton"));
    auto *cancel = panel.findChild<QToolButton *>(QStringLiteral("attrEditCancelButton"));
    auto *start = panel.findChild<QToolButton *>(QStringLiteral("attrEditStartButton"));
    QVERIFY(save && cancel && start);
    QVERIFY(!save->isEnabled());
    QVERIFY(!cancel->isEnabled());
    QVERIFY(start->isEnabled());

    delete vl;
  }

  void cellEditLandsInUndoStack()
  {
    QgsMapCanvas canvas;
    auto *vl = new QgsVectorLayer(
        QStringLiteral("Point?crs=EPSG:4326&field=name:string"),
        QStringLiteral("wells"), QStringLiteral("memory"));
    QVERIFY(vl->isValid());
    QgsFeature f(vl->fields());
    f.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(1, 2)));
    f.setAttribute(QStringLiteral("name"), QStringLiteral("W1"));
    QList<QgsFeature> feats{f};
    QVERIFY(vl->dataProvider()->addFeatures(feats));

    AttributeTablePanel panel(
        &canvas, [vl](const QString &id) -> QgsVectorLayer * {
          return id == QLatin1String("wells") ? vl : nullptr;
        });
    panel.setLayerIds({QStringLiteral("wells")});
    panel.showLayer(QStringLiteral("wells"));
    const QgsFeatureId fid = *vl->allFeatureIds().constBegin();

    // 会话门：面板未开会话前图层不可写（一切写路径都进不了 edit buffer）。
    QVERIFY(!panel.isEditing());
    QVERIFY(panel.beginEditing());

    // 原生单元格提交路径 = QgsAttributeTableDelegate::setModelData：
    // beginEditCommand("Attribute changed") + changeAttributeValue +
    // endEditCommand（libqgis_gui 反汇编证实，QGIS 4.2 模型 setData 不直写
    // 图层）。headless 无编辑器部件，按同一路径直写：
    vl->beginEditCommand(QStringLiteral("Attribute changed"));
    QVERIFY(vl->changeAttributeValue(fid, 0, QStringLiteral("W7")));
    vl->endEditCommand();
    QCOMPARE(vl->undoStack()->count(), 1); // 单步 undo 组

    // 面板模型链（cache→model→filter）看见编辑缓冲里的新值。
    auto *view = panel.findChild<QgsAttributeTableView *>(QStringLiteral("attrView"));
    QVERIFY(view != nullptr && view->model() != nullptr);
    auto *filter = qobject_cast<QgsAttributeTableFilterModel *>(view->model());
    QVERIFY(filter != nullptr);
    QTest::qWait(50); // 模型经信号链刷新（跨信号转发的排队余量）
    QCOMPARE(filter->data(filter->index(0, 0), Qt::DisplayRole).toString(),
             QStringLiteral("W7"));

    // 单步 undo 还原旧值，模型跟随；重做后面板提交进版本管线。
    vl->undoStack()->undo();
    QTest::qWait(50);
    QCOMPARE(filter->data(filter->index(0, 0), Qt::DisplayRole).toString(),
             QStringLiteral("W1"));
    vl->undoStack()->redo();
    QVERIFY(panel.saveEditing());
    QVERIFY(!vl->isEditable());
    QCOMPARE(vl->getFeature(fid).attribute(0).toString(), QStringLiteral("W7"));

    delete vl;
  }
};

int main(int argc, char *argv[])
{
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
    qFatal("QgisRuntime::initialize failed");
  TestAttrPanel tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_attrpanel.moc"
