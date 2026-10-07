#include <QtTest>
#include <QSignalSpy>
#include <QApplication>
#include <QGuiApplication>
#include <QClipboard>
#include <QMainWindow>
#include <QTableView>
#include <QComboBox>
#include <QLineEdit>
#include <QToolButton>
#include <QAction>
#include <QMenu>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "../src/services/errorhub.h"
#include "../src/ui/notifications/errorhistorydock.h"
#include "../src/ui/notifications/errorhistorymodel.h"
#include "../src/ui/notifications/notificationmanager.h"
#include "../src/ui/paleodockmanager.h"

using namespace paleo::services;
using namespace paleo::ui;

class TestErrorHistoryDock : public QObject
{
  Q_OBJECT

private slots:
  void initTestCase()
  {
    QCOMPARE(QGuiApplication::platformName(), QStringLiteral("offscreen"));
  }

  void init()
  {
    if (paleo::services::ErrorHub::instance()) {
      paleo::services::ErrorHub::instance()->clear();
      paleo::services::ErrorHub::instance()->setMaxCapacity(paleo::services::ErrorHub::kDefaultMaxHistory);
      paleo::services::ErrorHub::instance()->setDedupWindowSecs(paleo::services::ErrorHub::kDefaultDedupWindowSecs);
    }
    NotificationManager::setConfirmHookForTesting(nullptr);
    NotificationManager::setOffscreenAutoAnswer(true);
  }

  void cleanup()
  {
    if (paleo::services::ErrorHub::instance()) {
      paleo::services::ErrorHub::instance()->clear();
    }
    NotificationManager::setConfirmHookForTesting(nullptr);
    NotificationManager::setOffscreenAutoAnswer(true);
  }

  // Test Case 1: Model initial state against empty paleo::services::ErrorHub
  void testModelInitialStateEmpty()
  {
    ErrorHistoryModel model;
    QCOMPARE(model.rowCount(), 0);
    QVERIFY(model.columnCount() >= 5);
    QCOMPARE(model.columnCount(), ErrorHistoryModel::ColumnCount);

    for (int col = 0; col < model.columnCount(); ++col) {
      QVERIFY(!model.headerData(col, Qt::Horizontal, Qt::DisplayRole).toString().isEmpty());
    }

    QVERIFY(!model.index(0, 0).isValid());
    QVERIFY(!model.data(model.index(0, 0), Qt::DisplayRole).isValid());
  }

  // Test Case 2: Model updates dynamically when paleo::services::ErrorHub emits signals
  void testModelDynamicUpdatesAndAggregation()
  {
    ErrorHistoryModel model;
    QSignalSpy rowsInsertedSpy(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy dataChangedSpy(&model, &QAbstractItemModel::dataChanged);

    paleo::services::ErrorHub::instance()->reportError(
      ErrorDomain::Seismic,
      QStringLiteral("SEGY测网解析失败"),
      QStringLiteral("文件格式不合法: /data/survey.sgy"),
      QStringLiteral("key.segy.bad"));
    QCoreApplication::processEvents();

    QCOMPARE(rowsInsertedSpy.count(), 1);
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0, ErrorHistoryModel::ColDomain)).toString(), ErrorDomain::Seismic);
    QCOMPARE(model.data(model.index(0, ErrorHistoryModel::ColTitle)).toString(), QStringLiteral("SEGY测网解析失败"));
    QCOMPARE(model.data(model.index(0, ErrorHistoryModel::ColCount)).toInt(), 1);

    // 60s 去重聚合命中
    paleo::services::ErrorHub::instance()->reportError(
      ErrorDomain::Seismic,
      QStringLiteral("SEGY测网解析失败"),
      QStringLiteral("重试失败"),
      QStringLiteral("key.segy.bad"));
    QCoreApplication::processEvents();

    QCOMPARE(rowsInsertedSpy.count(), 1); // 不新增行
    QVERIFY(dataChangedSpy.count() >= 1);
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0, ErrorHistoryModel::ColCount)).toInt(), 2);

    // 新增独立警告
    paleo::services::ErrorHub::instance()->reportWarning(
      ErrorDomain::Well,
      QStringLiteral("井斜角异常"),
      QStringLiteral("井名 W-01"),
      QStringLiteral("key.well.dev"));
    QCoreApplication::processEvents();

    QCOMPARE(rowsInsertedSpy.count(), 2);
    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(model.data(model.index(1, ErrorHistoryModel::ColDomain)).toString(), ErrorDomain::Well);
  }

  // Test Case 3: Ring buffer FIFO eviction stability (500 -> 600 items)
  void testFifoEvictionStabilityAndBoundary()
  {
    paleo::services::ErrorHub::instance()->clear();
    paleo::services::ErrorHub::instance()->setMaxCapacity(500);
    paleo::services::ErrorHub::instance()->setDedupWindowSecs(0); // 禁用聚合，确保每条独立
    ErrorHistoryModel model;
    QTableView tableView;
    tableView.setModel(&model);

    // 填满 500 条
    for (int i = 1; i <= 500; ++i) {
      paleo::services::ErrorHub::instance()->reportError(
        ErrorDomain::General,
        QStringLiteral("Error_%1").arg(i),
        QStringLiteral("Detail_%1").arg(i),
        QStringLiteral("key_%1").arg(i));
    }
    QCoreApplication::processEvents();

    QCOMPARE(model.rowCount(), 500);
    QCOMPARE(model.data(model.index(0, ErrorHistoryModel::ColTitle)).toString(), QStringLiteral("Error_1"));
    QCOMPARE(model.data(model.index(499, ErrorHistoryModel::ColTitle)).toString(), QStringLiteral("Error_500"));

    // 连续再注入 100 条 (501 -> 600)
    for (int i = 501; i <= 600; ++i) {
      paleo::services::ErrorHub::instance()->reportWarning(
        ErrorDomain::IO,
        QStringLiteral("Error_%1").arg(i),
        QStringLiteral("Detail_%1").arg(i),
        QStringLiteral("key_%1").arg(i));
    }
    QCoreApplication::processEvents();

    QCOMPARE(model.rowCount(), 500);
    // FIFO 逐出前 100 条后，第 0 行应为原 101，第 499 行应为 600
    QCOMPARE(model.data(model.index(0, ErrorHistoryModel::ColTitle)).toString(), QStringLiteral("Error_101"));
    QCOMPARE(model.data(model.index(499, ErrorHistoryModel::ColTitle)).toString(), QStringLiteral("Error_600"));

    // 遍历所有 500 行及各列，验证无越界崩溃
    for (int r = 0; r < 500; ++r) {
      for (int c = 0; c < model.columnCount(); ++c) {
        QVariant val = model.data(model.index(r, c), Qt::DisplayRole);
        QVERIFY(val.isValid());
      }
    }
  }

  // Test Case 4: Filtering by Domain and Level
  void testFilteringByDomainAndLevel()
  {
    paleo::services::ErrorHub::instance()->clear();
    paleo::services::ErrorHub::instance()->setDedupWindowSecs(0);

    // 预填充:
    // 2 Info (General, Seismic)
    // 3 Warning (General, Seismic, Well)
    // 4 Error (Seismic, Well, IO, Catalog)
    // 1 Critical (Project)
    // 共 10 条
    paleo::services::ErrorHub::instance()->reportInfo(ErrorDomain::General, QStringLiteral("Info 1"));
    paleo::services::ErrorHub::instance()->reportInfo(ErrorDomain::Seismic, QStringLiteral("Info 2"));

    paleo::services::ErrorHub::instance()->reportWarning(ErrorDomain::General, QStringLiteral("Warn 1"));
    paleo::services::ErrorHub::instance()->reportWarning(ErrorDomain::Seismic, QStringLiteral("Warn 2"));
    paleo::services::ErrorHub::instance()->reportWarning(ErrorDomain::Well, QStringLiteral("Warn 3"));

    paleo::services::ErrorHub::instance()->reportError(ErrorDomain::Seismic, QStringLiteral("Err 1"));
    paleo::services::ErrorHub::instance()->reportError(ErrorDomain::Well, QStringLiteral("Err 2"));
    paleo::services::ErrorHub::instance()->reportError(ErrorDomain::IO, QStringLiteral("Err 3"));
    paleo::services::ErrorHub::instance()->reportError(ErrorDomain::Catalog, QStringLiteral("Err 4"));

    paleo::services::ErrorHub::instance()->reportCritical(ErrorDomain::Project, QStringLiteral("Crit 1"));
    QCoreApplication::processEvents();

    ErrorHistoryDock dock;
    auto *proxy = dock.proxyModel();
    QCOMPARE(proxy->rowCount(), 10);

    // 按 Level = Error 过滤
    dock.setLevelFilter(ErrorLevel::Error);
    QCOMPARE(proxy->rowCount(), 4);
    for (int r = 0; r < proxy->rowCount(); ++r) {
      QCOMPARE(proxy->data(proxy->index(r, ErrorHistoryModel::ColLevel), ErrorHistoryModel::LevelRole).value<ErrorLevel>(),
               ErrorLevel::Error);
    }

    // 按 Level = Warning 过滤
    dock.setLevelFilter(ErrorLevel::Warning);
    QCOMPARE(proxy->rowCount(), 3);

    // 按 Domain = Seismic 过滤（级别重置为全部）
    dock.setLevelFilterMode(ErrorHistoryFilterProxyModel::LevelFilterMode::All);
    dock.setDomainFilter(ErrorDomain::Seismic);
    QCOMPARE(proxy->rowCount(), 3); // 1 Info + 1 Warning + 1 Error
    for (int r = 0; r < proxy->rowCount(); ++r) {
      QCOMPARE(proxy->data(proxy->index(r, ErrorHistoryModel::ColDomain)).toString(), ErrorDomain::Seismic);
    }

    // 组合过滤: Level == Error AND Domain == Seismic
    dock.setLevelFilter(ErrorLevel::Error);
    QCOMPARE(proxy->rowCount(), 1);
    QCOMPARE(proxy->data(proxy->index(0, ErrorHistoryModel::ColDomain)).toString(), ErrorDomain::Seismic);
    QCOMPARE(proxy->data(proxy->index(0, ErrorHistoryModel::ColLevel), ErrorHistoryModel::LevelRole).value<ErrorLevel>(),
             ErrorLevel::Error);

    // 恢复全部
    dock.setDomainFilter(QString());
    dock.setLevelFilterMode(ErrorHistoryFilterProxyModel::LevelFilterMode::All);
    QCOMPARE(proxy->rowCount(), 10);
  }

  // Test Case 5: Clipboard copy functionality (selected vs all)
  void testClipboardCopyFormattedString()
  {
    QClipboard *clipboard = QGuiApplication::clipboard();
    clipboard->clear();

    paleo::services::ErrorHub::instance()->reportError(ErrorDomain::Seismic, QStringLiteral("地震测网解析失败"), QStringLiteral("path=/data/1.sgy"), QStringLiteral("copy.1"));
    paleo::services::ErrorHub::instance()->reportWarning(ErrorDomain::Well, QStringLiteral("分层标定超限"), QStringLiteral("well=W-02"), QStringLiteral("copy.2"));
    QCoreApplication::processEvents();

    ErrorHistoryDock dock;
    dock.show();
    QCoreApplication::processEvents();

    // 复制选中第一行
    dock.tableView()->selectRow(0);
    dock.copySelectedToClipboard();
    QString copied = clipboard->text();
    QVERIFY(!copied.isEmpty());
    QVERIFY(copied.contains(QStringLiteral("地震测网解析失败")));
    QVERIFY(copied.contains(QStringLiteral("Seismic")));
    QVERIFY(!copied.contains(QStringLiteral("分层标定超限")));

    // 复制全部
    clipboard->clear();
    dock.copyAllToClipboard();
    QString allCopied = clipboard->text();
    QVERIFY(allCopied.contains(QStringLiteral("地震测网解析失败")));
    QVERIFY(allCopied.contains(QStringLiteral("分层标定超限")));
    QVERIFY(allCopied.contains(QStringLiteral("Well")));
  }

  // Test Case 6: Clear history button clears both paleo::services::ErrorHub and UI model
  void testClearHistoryTwoWaySync()
  {
    for (int i = 0; i < 15; ++i) {
      paleo::services::ErrorHub::instance()->reportInfo(ErrorDomain::General, QStringLiteral("Msg %1").arg(i), QString(), QStringLiteral("key.%1").arg(i));
    }
    QCoreApplication::processEvents();
    QCOMPARE(paleo::services::ErrorHub::instance()->count(), 15);

    ErrorHistoryDock dock;
    QCOMPARE(dock.tableView()->model()->rowCount(), 15);

    auto *clearBtn = dock.findChild<QToolButton *>(QStringLiteral("clearHistoryButton"));
    QVERIFY(clearBtn != nullptr);
    clearBtn->click();
    QCoreApplication::processEvents();

    QCOMPARE(paleo::services::ErrorHub::instance()->count(), 0);
    QVERIFY(paleo::services::ErrorHub::instance()->history().isEmpty());
    QCOMPARE(dock.tableView()->model()->rowCount(), 0);

    // 清空后再次写入
    paleo::services::ErrorHub::instance()->reportInfo(ErrorDomain::General, QStringLiteral("清空后新消息"), QString(), QStringLiteral("new.1"));
    QCoreApplication::processEvents();
    QCOMPARE(paleo::services::ErrorHub::instance()->count(), 1);
    QCOMPARE(dock.tableView()->model()->rowCount(), 1);
  }

  // Test Case 7: Dock creation and View menu action toggle state
  void testDockCreationAndViewMenuActionToggle()
  {
    QMainWindow mainWindow;
    PaleoDockManager dockManager(&mainWindow, QStringLiteral("test/history_dock"));
    auto *dock = new ErrorHistoryDock(&mainWindow);

    QCOMPARE(dock->objectName(), QStringLiteral("errorHistoryDock"));
    QVERIFY(dock->windowTitle().contains(QStringLiteral("错误")) || dock->windowTitle().contains(QStringLiteral("Error")));

    mainWindow.addDockWidget(Qt::BottomDockWidgetArea, dock);
    dockManager.addDock(Qt::BottomDockWidgetArea, dock);

    QVERIFY(dock->isHidden() || !dock->isVisible());

    QAction *act = dock->toggleViewAction();
    QVERIFY(act != nullptr);
    QVERIFY(act->isCheckable());
    QCOMPARE(act->isChecked(), false);

    // 通过 action 显示
    act->trigger();
    QCoreApplication::processEvents();
    QVERIFY(!dock->isHidden());
    QCOMPARE(act->isChecked(), true);

    // 通过 action 隐藏
    act->trigger();
    QCoreApplication::processEvents();
    QVERIFY(dock->isHidden());
    QCOMPARE(act->isChecked(), false);

    // 检查 PaleoDockManager 生成的菜单中包含该 action
    QScopedPointer<QMenu> menu(dockManager.createMenu(&mainWindow));
    QVERIFY(menu->actions().contains(act));
  }

  // Robustness Edge Case: 活跃过滤条件下的 FIFO 逐出稳定性
  void testFilteringDuringActiveFifoEviction()
  {
    paleo::services::ErrorHub::instance()->clear();
    paleo::services::ErrorHub::instance()->setMaxCapacity(50);
    paleo::services::ErrorHub::instance()->setDedupWindowSecs(0);

    ErrorHistoryDock dock;
    dock.setDomainFilter(ErrorDomain::Seismic);

    // 注入交替 domain 的消息，触发 FIFO 逐出
    for (int i = 0; i < 120; ++i) {
      QString domain = (i % 2 == 0) ? ErrorDomain::Seismic : ErrorDomain::Well;
      paleo::services::ErrorHub::instance()->reportInfo(domain, QStringLiteral("Evict_%1").arg(i), QString(), QString::number(i));
      if (i % 10 == 0) {
        QCoreApplication::processEvents();
      }
    }
    QCoreApplication::processEvents();

    QCOMPARE(dock.model()->rowCount(), 50);
    // 代理模型行数应 <= 50，且全部为 Seismic
    QVERIFY(dock.proxyModel()->rowCount() <= 50);
    for (int r = 0; r < dock.proxyModel()->rowCount(); ++r) {
      QCOMPARE(dock.proxyModel()->data(dock.proxyModel()->index(r, ErrorHistoryModel::ColDomain)).toString(),
               ErrorDomain::Seismic);
    }
  }

  // Robustness Edge Case: 空选择安全守护
  void testCopyOnEmptySelectionSafeguard()
  {
    QClipboard *clipboard = QGuiApplication::clipboard();
    clipboard->clear();

    ErrorHistoryDock dock;
    dock.copySelectedToClipboard();
    QVERIFY(clipboard->text().isEmpty());
  }

  // Challenger 1: 500-item circular buffer FIFO eviction stress (2,000 errors sequentially)
  void testAdversarialSequentialFifoHeavyLoad2000Items()
  {
    paleo::services::ErrorHub::instance()->clear();
    paleo::services::ErrorHub::instance()->setMaxCapacity(500);
    paleo::services::ErrorHub::instance()->setDedupWindowSecs(0);

    ErrorHistoryModel model;
    QTableView tableView;
    tableView.setModel(&model);

    // Push 2,000 unique errors sequentially
    for (int i = 1; i <= 2000; ++i) {
      paleo::services::ErrorHub::instance()->reportError(
        ErrorDomain::General,
        QStringLiteral("HeavyErr_%1").arg(i),
        QStringLiteral("HeavyDetail_%1").arg(i),
        QStringLiteral("key_heavy_%1").arg(i));

      if (i % 50 == 0) {
        QCoreApplication::processEvents();
      }

      if (i == 500) {
        QCoreApplication::processEvents();
        QCOMPARE(model.rowCount(), 500);
        QCOMPARE(model.data(model.index(0, ErrorHistoryModel::ColTitle)).toString(), QStringLiteral("HeavyErr_1"));
        QCOMPARE(model.data(model.index(499, ErrorHistoryModel::ColTitle)).toString(), QStringLiteral("HeavyErr_500"));
      } else if (i == 501) {
        QCoreApplication::processEvents();
        QCOMPARE(model.rowCount(), 500);
        QCOMPARE(model.data(model.index(0, ErrorHistoryModel::ColTitle)).toString(), QStringLiteral("HeavyErr_2"));
        QCOMPARE(model.data(model.index(499, ErrorHistoryModel::ColTitle)).toString(), QStringLiteral("HeavyErr_501"));
      } else if (i == 1000) {
        QCoreApplication::processEvents();
        QCOMPARE(model.rowCount(), 500);
        QCOMPARE(model.data(model.index(0, ErrorHistoryModel::ColTitle)).toString(), QStringLiteral("HeavyErr_501"));
        QCOMPARE(model.data(model.index(499, ErrorHistoryModel::ColTitle)).toString(), QStringLiteral("HeavyErr_1000"));
      }
    }
    QCoreApplication::processEvents();

    // Final checks at 2000
    QCOMPARE(model.rowCount(), 500);
    QCOMPARE(model.data(model.index(0, ErrorHistoryModel::ColTitle)).toString(), QStringLiteral("HeavyErr_1501"));
    QCOMPARE(model.data(model.index(499, ErrorHistoryModel::ColTitle)).toString(), QStringLiteral("HeavyErr_2000"));

    // Exhaustively verify all 500 rows across all columns and custom roles
    for (int r = 0; r < 500; ++r) {
      int expectedNum = 1501 + r;
      QString expTitle = QStringLiteral("HeavyErr_%1").arg(expectedNum);
      QString expDetail = QStringLiteral("HeavyDetail_%1").arg(expectedNum);

      QCOMPARE(model.data(model.index(r, ErrorHistoryModel::ColTitle)).toString(), expTitle);
      QCOMPARE(model.data(model.index(r, ErrorHistoryModel::ColMessage)).toString(), expDetail);
      QCOMPARE(model.data(model.index(r, ErrorHistoryModel::ColDomain)).toString(), ErrorDomain::General);
      QCOMPARE(model.data(model.index(r, ErrorHistoryModel::ColLevel)).toString(), QStringLiteral("错误"));
      QCOMPARE(model.data(model.index(r, ErrorHistoryModel::ColCount)).toInt(), 1);

      // Custom roles
      QCOMPARE(model.data(model.index(r, 0), ErrorHistoryModel::DomainRole).toString(), ErrorDomain::General);
      QCOMPARE(model.data(model.index(r, 0), ErrorHistoryModel::CountRole).toInt(), 1);
      QCOMPARE(model.data(model.index(r, 0), ErrorHistoryModel::LevelRole).value<ErrorLevel>(), ErrorLevel::Error);

      const auto &entry = model.entryAt(r);
      QCOMPARE(entry.message, expTitle);
      QCOMPARE(entry.details, expDetail);
      QVERIFY(entry.id > 0);
    }

    // Out-of-bounds guards
    QVERIFY(!model.index(-1, 0).isValid());
    QVERIFY(!model.index(500, 0).isValid());
    QVERIFY(!model.index(0, -1).isValid());
    QVERIFY(!model.index(0, ErrorHistoryModel::ColumnCount).isValid());
    QVERIFY(!model.data(model.index(-1, 0)).isValid());
    QVERIFY(!model.data(model.index(500, 0)).isValid());
    QCOMPARE(model.entryAt(-1).id, 0);
    QCOMPARE(model.entryAt(500).id, 0);
  }

  // Challenger 1: Selection model and clipboard survival under heavy FIFO eviction
  void testAdversarialSelectionSurvivalAcrossMassiveEvictions()
  {
    paleo::services::ErrorHub::instance()->clear();
    paleo::services::ErrorHub::instance()->setMaxCapacity(500);
    paleo::services::ErrorHub::instance()->setDedupWindowSecs(0);

    ErrorHistoryDock dock;
    dock.show();
    QCoreApplication::processEvents();

    // Populate initial 500 items
    for (int i = 1; i <= 500; ++i) {
      paleo::services::ErrorHub::instance()->reportError(
        ErrorDomain::IO,
        QStringLiteral("InitErr_%1").arg(i),
        QStringLiteral("InitDetail_%1").arg(i),
        QStringLiteral("init_%1").arg(i));
    }
    QCoreApplication::processEvents();
    QCOMPARE(dock.model()->rowCount(), 500);

    // Select row 0 (eviction target), row 50, row 250, row 499
    auto *selModel = dock.tableView()->selectionModel();
    QVERIFY(selModel != nullptr);
    selModel->select(dock.proxyModel()->index(0, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
    selModel->select(dock.proxyModel()->index(50, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
    selModel->select(dock.proxyModel()->index(250, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
    selModel->select(dock.proxyModel()->index(499, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);

    QCOMPARE(selModel->selectedRows().size(), 4);

    // Push 1,500 new errors, triggering 1,500 evictions at row 0
    for (int i = 501; i <= 2000; ++i) {
      paleo::services::ErrorHub::instance()->reportWarning(
        ErrorDomain::Seismic,
        QStringLiteral("StormErr_%1").arg(i),
        QStringLiteral("StormDetail_%1").arg(i),
        QStringLiteral("storm_%1").arg(i));

      if (i % 100 == 0) {
        QCoreApplication::processEvents();
        // Invoke copySelectedToClipboard during active eviction stream
        dock.copySelectedToClipboard();
        dock.copyAllToClipboard();
      }
    }
    QCoreApplication::processEvents();

    QCOMPARE(dock.model()->rowCount(), 500);
    QCOMPARE(dock.proxyModel()->rowCount(), 500);

    // Now select row 0 and immediately evict it with a single error
    dock.tableView()->selectRow(0);
    QVERIFY(selModel->hasSelection());

    paleo::services::ErrorHub::instance()->reportCritical(
      ErrorDomain::System,
      QStringLiteral("EvictRow0"),
      QStringLiteral("DetailRow0"),
      QStringLiteral("key.evict.row0"));
    QCoreApplication::processEvents();

    QCOMPARE(dock.model()->rowCount(), 500);
    // Ensure clipboard copy on modified/shifted selection does not crash
    dock.copySelectedToClipboard();
    dock.copyAllToClipboard();
    QString text = QGuiApplication::clipboard()->text();
    QVERIFY(text.contains(QStringLiteral("EvictRow0")));
  }

  // Challenger 1: Active proxy filtering and dynamic sorting under continuous eviction
  void testAdversarialProxySortingAndFilteringDuringEvictionStorm()
  {
    paleo::services::ErrorHub::instance()->clear();
    paleo::services::ErrorHub::instance()->setMaxCapacity(500);
    paleo::services::ErrorHub::instance()->setDedupWindowSecs(0);

    ErrorHistoryDock dock;
    dock.show();

    // Enable sorting descending by timestamp
    dock.tableView()->sortByColumn(ErrorHistoryModel::ColTimestamp, Qt::DescendingOrder);
    dock.setDomainFilter(ErrorDomain::Seismic);
    dock.setLevelFilterMode(ErrorHistoryFilterProxyModel::LevelFilterMode::ErrorAndAbove);

    const QString domains[4] = {ErrorDomain::Seismic, ErrorDomain::Well, ErrorDomain::General, ErrorDomain::IO};
    const ErrorLevel levels[4] = {ErrorLevel::Info, ErrorLevel::Warning, ErrorLevel::Error, ErrorLevel::Critical};

    // Inject 1,200 errors with cycling domains and levels
    for (int i = 1; i <= 1200; ++i) {
      ErrorEntry e;
      e.domain = domains[i % 4];
      e.level = levels[(i / 4) % 4];
      e.message = QStringLiteral("CycleErr_%1").arg(i);
      e.details = QStringLiteral("CycleDetail_%1").arg(i);
      e.deduplicationKey = QStringLiteral("cycle_%1").arg(i);
      e.timestamp = QDateTime::currentDateTime().addMSecs(i * 10);
      paleo::services::ErrorHub::instance()->report(e);

      if (i % 60 == 0) {
        QCoreApplication::processEvents();
      }
    }
    QCoreApplication::processEvents();

    auto *proxy = dock.proxyModel();
    auto *model = dock.model();
    QCOMPARE(model->rowCount(), 500);

    // Verify all rows in proxy meet the filter criteria
    int proxyRows = proxy->rowCount();
    QVERIFY(proxyRows > 0);
    QVERIFY(proxyRows <= 500);

    for (int r = 0; r < proxyRows; ++r) {
      QModelIndex proxyIdx = proxy->index(r, 0);
      QModelIndex srcIdx = proxy->mapToSource(proxyIdx);
      QVERIFY(srcIdx.isValid());
      QVERIFY(srcIdx.row() >= 0 && srcIdx.row() < 500);

      QString dom = proxy->data(proxy->index(r, ErrorHistoryModel::ColDomain)).toString();
      QCOMPARE(dom, ErrorDomain::Seismic);

      auto lvl = proxy->data(proxy->index(r, ErrorHistoryModel::ColLevel), ErrorHistoryModel::LevelRole).value<ErrorLevel>();
      QVERIFY(lvl == ErrorLevel::Error || lvl == ErrorLevel::Critical);

      // Verify descending timestamp sort order
      if (r > 0) {
        QDateTime prevTime = proxy->data(proxy->index(r - 1, ErrorHistoryModel::ColTimestamp), ErrorHistoryModel::TimestampRole).toDateTime();
        QDateTime currTime = proxy->data(proxy->index(r, ErrorHistoryModel::ColTimestamp), ErrorHistoryModel::TimestampRole).toDateTime();
        QVERIFY(prevTime >= currTime);
      }
    }

    // Switch to ascending sort on ColCount
    dock.tableView()->sortByColumn(ErrorHistoryModel::ColCount, Qt::AscendingOrder);
    QCoreApplication::processEvents();
    QCOMPARE(proxy->rowCount(), proxyRows);

    // Reset filters
    dock.setDomainFilter(QString());
    dock.setLevelFilterMode(ErrorHistoryFilterProxyModel::LevelFilterMode::All);
    QCoreApplication::processEvents();
    QCOMPARE(proxy->rowCount(), 500);
  }

  // Challenger 1: Dynamic deduplication aggregation (repeated errors within 60s) updates in-place
  void testAdversarialDynamicDedupInPlaceAggregationNoRowDuplication()
  {
    paleo::services::ErrorHub::instance()->clear();
    paleo::services::ErrorHub::instance()->setMaxCapacity(500);
    paleo::services::ErrorHub::instance()->setDedupWindowSecs(60);

    ErrorHistoryModel model;
    QSignalSpy rowsInsertedSpy(&model, &QAbstractItemModel::rowsInserted);
    QSignalSpy rowsRemovedSpy(&model, &QAbstractItemModel::rowsRemoved);
    QSignalSpy dataChangedSpy(&model, &QAbstractItemModel::dataChanged);

    // 1. Report 5 base errors
    for (int i = 0; i < 5; ++i) {
      paleo::services::ErrorHub::instance()->reportError(
        ErrorDomain::Catalog,
        QStringLiteral("BaseError_%1").arg(i),
        QStringLiteral("Detail_%1").arg(i),
        QStringLiteral("key.base.%1").arg(i));
    }
    QCoreApplication::processEvents();

    QCOMPARE(model.rowCount(), 5);
    QCOMPARE(rowsInsertedSpy.count(), 5);
    QCOMPARE(rowsRemovedSpy.count(), 0);

    rowsInsertedSpy.clear();
    rowsRemovedSpy.clear();
    dataChangedSpy.clear();

    // 2. Hammer entry #2 with 200 repeated reports within 60s window
    for (int rep = 1; rep <= 200; ++rep) {
      paleo::services::ErrorHub::instance()->reportError(
        ErrorDomain::Catalog,
        QStringLiteral("BaseError_2"),
        QStringLiteral("RepeatAppend_%1").arg(rep),
        QStringLiteral("key.base.2"));

      if (rep % 50 == 0) {
        QCoreApplication::processEvents();
      }
    }
    QCoreApplication::processEvents();

    // In-place updates: exactly 0 insertions, exactly 0 removals
    QCOMPARE(rowsInsertedSpy.count(), 0);
    QCOMPARE(rowsRemovedSpy.count(), 0);
    QCOMPARE(model.rowCount(), 5);
    QCOMPARE(dataChangedSpy.count(), 200);

    // Entry 2 is at row 2 with count = 201
    QCOMPARE(model.data(model.index(2, ErrorHistoryModel::ColCount)).toInt(), 201);
    const auto &entry2 = model.entryAt(2);
    QCOMPARE(entry2.aggregationCount, 201);
    QCOMPARE(entry2.message, QStringLiteral("BaseError_2"));

    // Verify other rows are untouched
    for (int r = 0; r < 5; ++r) {
      if (r == 2) continue;
      QCOMPARE(model.data(model.index(r, ErrorHistoryModel::ColCount)).toInt(), 1);
      QCOMPARE(model.data(model.index(r, ErrorHistoryModel::ColTitle)).toString(), QStringLiteral("BaseError_%1").arg(r));
    }
  }

  // Challenger 1: Interleaving dynamic deduplication and FIFO eviction boundaries
  void testAdversarialInterleavedDedupAndFifoEviction()
  {
    paleo::services::ErrorHub::instance()->clear();
    paleo::services::ErrorHub::instance()->setMaxCapacity(500);
    paleo::services::ErrorHub::instance()->setDedupWindowSecs(60);

    ErrorHistoryModel model;

    // Fill 490 unique items
    for (int i = 1; i <= 490; ++i) {
      paleo::services::ErrorHub::instance()->reportInfo(
        ErrorDomain::General,
        QStringLiteral("Pre_%1").arg(i),
        QString(),
        QStringLiteral("pre_%1").arg(i));
    }

    // Item 491: target key
    paleo::services::ErrorHub::instance()->reportError(
      ErrorDomain::Well,
      QStringLiteral("TargetPinned"),
      QStringLiteral("OriginalDetail"),
      QStringLiteral("key.target.pinned"));

    // Fill remaining 9 items up to 500
    for (int i = 1; i <= 9; ++i) {
      paleo::services::ErrorHub::instance()->reportInfo(
        ErrorDomain::General,
        QStringLiteral("Post_%1").arg(i),
        QString(),
        QStringLiteral("post_%1").arg(i));
    }
    QCoreApplication::processEvents();
    QCOMPARE(model.rowCount(), 500);
    QCOMPARE(model.data(model.index(490, ErrorHistoryModel::ColTitle)).toString(), QStringLiteral("TargetPinned"));

    // Aggregate target 20 times (remains at row 490, count increases)
    for (int i = 0; i < 20; ++i) {
      paleo::services::ErrorHub::instance()->reportError(
        ErrorDomain::Well,
        QStringLiteral("TargetPinned"),
        QStringLiteral("More"),
        QStringLiteral("key.target.pinned"));
    }
    QCoreApplication::processEvents();
    QCOMPARE(model.rowCount(), 500);
    QCOMPARE(model.data(model.index(490, ErrorHistoryModel::ColCount)).toInt(), 21);

    // Push 100 new unique items -> items 1..100 evicted, target shifts to row 390
    for (int i = 1; i <= 100; ++i) {
      paleo::services::ErrorHub::instance()->reportWarning(
        ErrorDomain::IO,
        QStringLiteral("Shift_%1").arg(i),
        QString(),
        QStringLiteral("shift_%1").arg(i));
    }
    QCoreApplication::processEvents();
    QCOMPARE(model.rowCount(), 500);
    QCOMPARE(model.data(model.index(390, ErrorHistoryModel::ColTitle)).toString(), QStringLiteral("TargetPinned"));
    QCOMPARE(model.data(model.index(390, ErrorHistoryModel::ColCount)).toInt(), 21);

    // Aggregate again while shifted: should update in-place at row 390
    paleo::services::ErrorHub::instance()->reportError(
      ErrorDomain::Well,
      QStringLiteral("TargetPinned"),
      QString(),
      QStringLiteral("key.target.pinned"));
    QCoreApplication::processEvents();
    QCOMPARE(model.data(model.index(390, ErrorHistoryModel::ColCount)).toInt(), 22);

    // Push 450 more items -> target at row 390 is eventually evicted
    for (int i = 1; i <= 450; ++i) {
      paleo::services::ErrorHub::instance()->reportInfo(
        ErrorDomain::System,
        QStringLiteral("EvictAll_%1").arg(i),
        QString(),
        QStringLiteral("evict_all_%1").arg(i));
    }
    QCoreApplication::processEvents();
    QCOMPARE(model.rowCount(), 500);

    // Target has been evicted. Reporting it again now should create a brand new entry (count 1)
    paleo::services::ErrorHub::instance()->reportError(
      ErrorDomain::Well,
      QStringLiteral("TargetPinned"),
      QStringLiteral("Reborn"),
      QStringLiteral("key.target.pinned"));
    QCoreApplication::processEvents();

    QCOMPARE(model.rowCount(), 500);
    QCOMPARE(model.data(model.index(499, ErrorHistoryModel::ColTitle)).toString(), QStringLiteral("TargetPinned"));
    QCOMPARE(model.data(model.index(499, ErrorHistoryModel::ColCount)).toInt(), 1);
  }

  // Challenger 1: Multi-threaded concurrency stress (4 worker threads reporting 2,000 errors)
  void testAdversarialConcurrentMultiThreadedStress()
  {
    paleo::services::ErrorHub::instance()->clear();
    paleo::services::ErrorHub::instance()->setMaxCapacity(500);
    paleo::services::ErrorHub::instance()->setDedupWindowSecs(60);

    ErrorHistoryDock dock;
    dock.show();
    QCoreApplication::processEvents();

    std::atomic<bool> startFlag{false};
    std::atomic<int> completedWorkers{0};
    const int kThreads = 4;
    const int kItemsPerThread = 500; // total 2,000 errors

    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
      workers.emplace_back([t, &startFlag, &completedWorkers, kItemsPerThread]() {
        while (!startFlag.load()) {
          std::this_thread::yield();
        }

        for (int i = 0; i < kItemsPerThread; ++i) {
          if (t < 2) {
            // Threads 0 & 1: report to 5 shared keys (concurrent aggregation)
            int keyId = i % 5;
            paleo::services::ErrorHub::instance()->reportWarning(
              ErrorDomain::AI,
              QStringLiteral("SharedWarn_%1").arg(keyId),
              QStringLiteral("Thread_%1_iter_%2").arg(t).arg(i),
              QStringLiteral("shared.key.%1").arg(keyId));
          } else {
            // Threads 2 & 3: report unique keys (concurrent FIFO eviction)
            paleo::services::ErrorHub::instance()->reportError(
              ErrorDomain::Gridding,
              QStringLiteral("UniqueErr_t%1_i%2").arg(t).arg(i),
              QStringLiteral("Detail_t%1_i%2").arg(t).arg(i),
              QStringLiteral("unique.key.t%1.i%2").arg(t).arg(i));
          }
        }
        completedWorkers.fetch_add(1);
      });
    }

    // Release threads
    startFlag.store(true);

    // Concurrently pump main thread event loop and query dock/model
    while (completedWorkers.load() < kThreads) {
      QCoreApplication::processEvents();
      int rows = dock.model()->rowCount();
      QVERIFY(rows >= 0 && rows <= 500);
      if (rows > 0) {
        // Sample access to columns
        QVariant v = dock.model()->data(dock.model()->index(0, ErrorHistoryModel::ColTitle));
        Q_UNUSED(v);
        dock.copySelectedToClipboard();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    for (auto &w : workers) {
      if (w.joinable()) {
        w.join();
      }
    }

    // Drain remaining events
    QCoreApplication::processEvents();

    // Hub and model capped strictly at 500
    QCOMPARE(dock.model()->rowCount(), 500);
    QCOMPARE(dock.proxyModel()->rowCount(), 500);

    // Verify all 500 rows are fully indexable and valid
    for (int r = 0; r < 500; ++r) {
      for (int c = 0; c < ErrorHistoryModel::ColumnCount; ++c) {
        QVariant val = dock.model()->data(dock.model()->index(r, c), Qt::DisplayRole);
        QVERIFY(val.isValid());
      }
    }

    dock.copyAllToClipboard();
    QVERIFY(!QGuiApplication::clipboard()->text().isEmpty());
  }

  // Challenger 1: Rapid clearHistory during heavy traffic does not deadlock or corrupt state
  void testAdversarialRapidClearDuringEvictionAndAggregation()
  {
    paleo::services::ErrorHub::instance()->clear();
    paleo::services::ErrorHub::instance()->setMaxCapacity(500);
    paleo::services::ErrorHub::instance()->setDedupWindowSecs(60);

    ErrorHistoryDock dock;
    dock.show();

    // Pump 300 items
    for (int i = 0; i < 300; ++i) {
      paleo::services::ErrorHub::instance()->reportInfo(
        ErrorDomain::General,
        QStringLiteral("PreClear_%1").arg(i),
        QString(),
        QStringLiteral("pre_clear_%1").arg(i));
    }
    QCoreApplication::processEvents();
    QCOMPARE(dock.model()->rowCount(), 300);

    // Clear via dock action
    dock.clearHistory();
    QCoreApplication::processEvents();

    QCOMPARE(dock.model()->rowCount(), 0);
    QCOMPARE(dock.proxyModel()->rowCount(), 0);
    QCOMPARE(paleo::services::ErrorHub::instance()->count(), 0);

    // Subsequent traffic after clear
    for (int i = 0; i < 600; ++i) {
      paleo::services::ErrorHub::instance()->reportError(
        ErrorDomain::Project,
        QStringLiteral("PostClear_%1").arg(i),
        QString(),
        QStringLiteral("post_clear_%1").arg(i));
    }
    QCoreApplication::processEvents();

    QCOMPARE(dock.model()->rowCount(), 500);
    QCOMPARE(dock.model()->data(dock.model()->index(0, ErrorHistoryModel::ColTitle)).toString(),
             QStringLiteral("PostClear_100"));
    QCOMPARE(dock.model()->data(dock.model()->index(499, ErrorHistoryModel::ColTitle)).toString(),
             QStringLiteral("PostClear_599"));
  }
};

int main(int argc, char *argv[])
{
  if (qgetenv("QT_QPA_PLATFORM").isEmpty()) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  QApplication app(argc, argv);
  TestErrorHistoryDock tc;
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_error_history_dock.moc"
