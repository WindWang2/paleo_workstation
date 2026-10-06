#include <QSignalSpy>
#include <QTest>
#include <QItemSelectionModel>
#include <QScrollBar>
#include <tuple>

#include "ui/pages/dataopsviews.h"
#include "ui/attributetablepanel.h"

using namespace paleo::dataops;

class TestDataopsIncremental : public QObject
{
  Q_OBJECT

private slots:
  void updateSingleRowEmitsDataChangedNoReset();
  void updateRowsBatchContiguousRanges();
  void updateCellAndSetDataSupport();
  void viewPreservesSelectionAndScrollOnUpdate();
  void attributeTablePanelIncrementalContract();
  void mutationDemonstration_resetOccursIfChannelBypassed();
};

static QVector<AssetRowInfo> makeTestRows(int count)
{
  QVector<AssetRowInfo> rows;
  rows.reserve(count);
  for (int i = 0; i < count; ++i)
  {
    AssetRowInfo r;
    r.assetId = QStringLiteral("asset-%1").arg(i);
    r.displayName = QStringLiteral("Asset %1").arg(i);
    r.type = QStringLiteral("well_log");
    r.effectiveType = QStringLiteral("well_log");
    r.tags = QStringList{QStringLiteral("tag1")};
    r.sizeBytes = 1024 * (i + 1);
    r.currentVersionNo = 1;
    r.lastModified = QDateTime::currentDateTime();
    rows.append(r);
  }
  return rows;
}

void TestDataopsIncremental::updateSingleRowEmitsDataChangedNoReset()
{
  FlatAssetModel model;
  const QVector<AssetRowInfo> rows = makeTestRows(10);
  model.setRows(rows);

  QSignalSpy spyDataChanged(&model, &QAbstractItemModel::dataChanged);
  QSignalSpy spyModelReset(&model, &QAbstractItemModel::modelReset);

  QVERIFY(spyDataChanged.isValid());
  QVERIFY(spyModelReset.isValid());

  // 增量更新行 3
  AssetRowInfo rowData = rows.at(3);
  rowData.effectiveType = QStringLiteral("horizon");
  rowData.displayName = QStringLiteral("Updated Horizon");

  const bool ok = model.updateRow(3, rowData);
  QVERIFY(ok);

  // 断言：发射了 dataChanged，且 modelReset 严格为 0
  QCOMPARE(spyModelReset.count(), 0);
  QCOMPARE(spyDataChanged.count(), 1);

  const QList<QVariant> args = spyDataChanged.takeFirst();
  const QModelIndex topLeft = args.at(0).toModelIndex();
  const QModelIndex bottomRight = args.at(1).toModelIndex();

  QCOMPARE(topLeft.row(), 3);
  QCOMPARE(topLeft.column(), 0);
  QCOMPARE(bottomRight.row(), 3);
  QCOMPARE(bottomRight.column(), FlatAssetModel::ColCount - 1);

  // 验证模型内数据已更新
  QCOMPARE(model.data(model.index(3, FlatAssetModel::ColType), Qt::DisplayRole).toString(),
           QStringLiteral("horizon"));
  QCOMPARE(model.data(model.index(3, FlatAssetModel::ColName), Qt::DisplayRole).toString(),
           QStringLiteral("Updated Horizon"));

  // 边界值：非法行拒绝
  QVERIFY(!model.updateRow(-1, rowData));
  QVERIFY(!model.updateRow(999, rowData));
}

void TestDataopsIncremental::updateRowsBatchContiguousRanges()
{
  FlatAssetModel model;
  const QVector<AssetRowInfo> rows = makeTestRows(20);
  model.setRows(rows);

  QSignalSpy spyDataChanged(&model, &QAbstractItemModel::dataChanged);
  QSignalSpy spyModelReset(&model, &QAbstractItemModel::modelReset);

  // 准备两组更新：一组连续区间 [2, 3, 4]，一组离散单点 [8]
  QVector<AssetRowInfo> updates;
  for (int r : {2, 3, 4, 8})
  {
    AssetRowInfo u = rows.at(r);
    u.effectiveType = QStringLiteral("boundary");
    updates.append(u);
  }

  const int updatedCount = model.updateRows(updates);
  QCOMPARE(updatedCount, 4);

  // 断言：绝无 modelReset，聚合成 2 个连续区间发射
  QCOMPARE(spyModelReset.count(), 0);
  QCOMPARE(spyDataChanged.count(), 2);

  // 区间 1: 行 2 到 4
  const QList<QVariant> range1 = spyDataChanged.at(0);
  QCOMPARE(range1.at(0).toModelIndex().row(), 2);
  QCOMPARE(range1.at(1).toModelIndex().row(), 4);

  // 区间 2: 行 8 到 8
  const QList<QVariant> range2 = spyDataChanged.at(1);
  QCOMPARE(range2.at(0).toModelIndex().row(), 8);
  QCOMPARE(range2.at(1).toModelIndex().row(), 8);

  // 未受影响的行不受干扰
  QCOMPARE(model.data(model.index(0, FlatAssetModel::ColType), Qt::DisplayRole).toString(),
           QStringLiteral("well_log"));
  QCOMPARE(model.data(model.index(2, FlatAssetModel::ColType), Qt::DisplayRole).toString(),
           QStringLiteral("boundary"));
  QCOMPARE(model.data(model.index(8, FlatAssetModel::ColType), Qt::DisplayRole).toString(),
           QStringLiteral("boundary"));
}

void TestDataopsIncremental::updateCellAndSetDataSupport()
{
  FlatAssetModel model;
  const QVector<AssetRowInfo> rows = makeTestRows(5);
  model.setRows(rows);

  QSignalSpy spyDataChanged(&model, &QAbstractItemModel::dataChanged);
  QSignalSpy spyModelReset(&model, &QAbstractItemModel::modelReset);

  // 1. 直接单元格更新
  const bool okCell = model.updateCell(1, FlatAssetModel::ColName, QStringLiteral("DirectName"));
  QVERIFY(okCell);
  QCOMPARE(spyModelReset.count(), 0);
  QCOMPARE(spyDataChanged.count(), 1);
  QCOMPARE(model.data(model.index(1, FlatAssetModel::ColName), Qt::DisplayRole).toString(),
           QStringLiteral("DirectName"));

  // 2. QAbstractItemModel 标准 setData 接口更新
  const QModelIndex typeIdx = model.index(1, FlatAssetModel::ColType);
  QVERIFY(model.flags(typeIdx).testFlag(Qt::ItemIsEditable));

  const bool okSetData = model.setData(typeIdx, QStringLiteral("seismic"), Qt::EditRole);
  QVERIFY(okSetData);
  QCOMPARE(spyModelReset.count(), 0);
  QCOMPARE(spyDataChanged.count(), 2);
  QCOMPARE(model.data(typeIdx, Qt::DisplayRole).toString(), QStringLiteral("seismic"));

  // 3. 批量单元格更新
  QVector<std::tuple<int, int, QVariant>> cellBatch;
  cellBatch.append({0, FlatAssetModel::ColName, QStringLiteral("Batch0")});
  cellBatch.append({1, FlatAssetModel::ColTags, QStringList{QStringLiteral("newTag")}});

  const int applied = model.updateCells(cellBatch);
  QCOMPARE(applied, 2);
  QCOMPARE(spyModelReset.count(), 0);
}

void TestDataopsIncremental::viewPreservesSelectionAndScrollOnUpdate()
{
  AssetVirtualView view;
  const QVector<AssetRowInfo> rows = makeTestRows(100);
  view.flatModel()->setRows(rows);

  // 选中行 5、6、7
  auto *selModel = view.selectionModel();
  QVERIFY(selModel != nullptr);

  QItemSelection selection;
  selection.select(view.flatModel()->index(5, 0),
                   view.flatModel()->index(7, FlatAssetModel::ColCount - 1));
  selModel->select(selection, QItemSelectionModel::Select | QItemSelectionModel::Rows);

  const QItemSelection origSelection = selModel->selection();
  QVERIFY(!origSelection.isEmpty());

  // 滚动视图
  view.verticalScrollBar()->setValue(25);
  const int origScroll = view.verticalScrollBar()->value();

  QSignalSpy spyDataChanged(view.flatModel(), &QAbstractItemModel::dataChanged);
  QSignalSpy spyModelReset(view.flatModel(), &QAbstractItemModel::modelReset);

  // 增量更新正在选中的第 6 行和未选中的第 10 行
  QVector<AssetRowInfo> updates;
  AssetRowInfo u6 = rows.at(6);
  u6.effectiveType = QStringLiteral("horizon");
  updates.append(u6);

  AssetRowInfo u10 = rows.at(10);
  u10.effectiveType = QStringLiteral("horizon");
  updates.append(u10);

  const int cnt = view.updateRows(updates);
  QCOMPARE(cnt, 2);

  // 断言：零 reset，发射了 dataChanged
  QCOMPARE(spyModelReset.count(), 0);
  QVERIFY(spyDataChanged.count() > 0);

  // 断言核心不变量：选区完全保留
  QCOMPARE(selModel->selection(), origSelection);
  // 断言核心不变量：滚动条位置完全保留
  QCOMPARE(view.verticalScrollBar()->value(), origScroll);
}

void TestDataopsIncremental::attributeTablePanelIncrementalContract()
{
  AttributeTablePanel panel(nullptr, [](const QString &) { return nullptr; });

  // 未加载图层时安全拒绝
  QVERIFY(!panel.updateCell(0, 0, QStringLiteral("test")));
  QVector<std::tuple<int, int, QVariant>> batch;
  batch.append({0, 0, QStringLiteral("v")});
  QCOMPARE(panel.updateCells(batch), 0);
  QVERIFY(panel.attributeModel() == nullptr);
  QVERIFY(panel.filterModel() == nullptr);
}

void TestDataopsIncremental::mutationDemonstration_resetOccursIfChannelBypassed()
{
  // 变异示范：如果走旧式全量灌入（setRows）或绕过增量通道直接 resetModel，
  // 选区将被清空、滚动被置零，且产生 modelReset 信号被断言直接抓住。
  AssetVirtualView view;
  const QVector<AssetRowInfo> rows = makeTestRows(20);
  view.flatModel()->setRows(rows);

  auto *selModel = view.selectionModel();
  QItemSelection selection;
  selection.select(view.flatModel()->index(2, 0),
                   view.flatModel()->index(3, FlatAssetModel::ColCount - 1));
  selModel->select(selection, QItemSelectionModel::Select | QItemSelectionModel::Rows);
  QVERIFY(!selModel->selection().isEmpty());

  QSignalSpy spyReset(view.flatModel(), &QAbstractItemModel::modelReset);

  // 模拟全量 reset 路径（变异路径）
  view.flatModel()->setRows(rows);

  // 断言抓住：旧路径产生了 modelReset 并且选区丢失
  QCOMPARE(spyReset.count(), 1);
  QVERIFY(selModel->selection().isEmpty());
}

QTEST_MAIN(TestDataopsIncremental)
#include "tst_dataops_incremental.moc"
