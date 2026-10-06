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
#include <QLabel>

#include "../src/services/errorhub.h"
#include "../src/ui/notifications/errorhistorydock.h"
#include "../src/ui/notifications/errorhistorymodel.h"
#include "../src/ui/notifications/notificationmanager.h"
#include "../src/ui/paleodockmanager.h"

using namespace paleo::services;
using namespace paleo::ui;

class TestErrorHubChallengerM4 : public QObject
{
  Q_OBJECT

private slots:
  void initTestCase()
  {
    QCOMPARE(QGuiApplication::platformName(), QStringLiteral("offscreen"));
  }

  void init()
  {
    if (ErrorHub::instance()) {
      ErrorHub::instance()->clear();
      ErrorHub::instance()->setMaxCapacity(ErrorHub::kDefaultMaxHistory);
      ErrorHub::instance()->setDedupWindowSecs(ErrorHub::kDefaultDedupWindowSecs);
    }
    NotificationManager::setConfirmHookForTesting(nullptr);
    NotificationManager::setOffscreenAutoAnswer(true);
    if (auto *cb = QGuiApplication::clipboard()) {
      cb->clear();
    }
  }

  void cleanup()
  {
    if (ErrorHub::instance()) {
      ErrorHub::instance()->clear();
    }
    NotificationManager::setConfirmHookForTesting(nullptr);
    NotificationManager::setOffscreenAutoAnswer(true);
    if (auto *cb = QGuiApplication::clipboard()) {
      cb->clear();
    }
  }

  // =========================================================================
  // 1. "Copy Selected" Safeguards (0 rows, cleared selection, invalid index)
  // =========================================================================
  void testCopySelectedZeroRowsSafeguard()
  {
    QClipboard *clipboard = QGuiApplication::clipboard();
    clipboard->setText(QStringLiteral("PRESERVED_CLIPBOARD_TEXT"));

    ErrorHistoryDock dock;
    dock.show();
    QCoreApplication::processEvents();

    auto *copySelBtn = dock.findChild<QToolButton *>(QStringLiteral("copySelectedButton"));
    QVERIFY(copySelBtn != nullptr);
    // 初始空表：按钮应被禁用
    QCOMPARE(copySelBtn->isEnabled(), false);

    // 直接调用 copySelectedToClipboard，剪贴板原有内容不应被覆盖为垃圾
    dock.copySelectedToClipboard();
    QCOMPARE(clipboard->text(), QStringLiteral("PRESERVED_CLIPBOARD_TEXT"));

    // 填充数据后，清除选择
    ErrorHub::instance()->reportError(ErrorDomain::General, QStringLiteral("E1"), QStringLiteral("D1"));
    QCoreApplication::processEvents();

    auto *selModel = dock.tableView()->selectionModel();
    QVERIFY(selModel != nullptr);
    selModel->clearSelection();
    selModel->clearCurrentIndex();
    QCOMPARE(selModel->hasSelection(), false);
    QCOMPARE(selModel->selectedRows().isEmpty(), true);
    QCOMPARE(copySelBtn->isEnabled(), false);

    // 调用 copySelectedToClipboard，无任何选中与当前焦点时仍应保留原剪贴板
    dock.copySelectedToClipboard();
    QCOMPARE(clipboard->text(), QStringLiteral("PRESERVED_CLIPBOARD_TEXT"));
  }

  // =========================================================================
  // 2. "Copy Selected" Discontiguous Selections
  // =========================================================================
  void testCopySelectedDiscontiguousSelections()
  {
    QClipboard *clipboard = QGuiApplication::clipboard();
    clipboard->clear();

    ErrorHub::instance()->setDedupWindowSecs(0);
    for (int i = 0; i < 5; ++i) {
      ErrorHub::instance()->reportError(
        ErrorDomain::General,
        QStringLiteral("Msg_%1").arg(i),
        QStringLiteral("Detail_%1").arg(i),
        QStringLiteral("key_%1").arg(i));
    }
    QCoreApplication::processEvents();

    ErrorHistoryDock dock;
    dock.show();
    QCoreApplication::processEvents();

    auto *table = dock.tableView();
    auto *selModel = table->selectionModel();
    auto *proxy = dock.proxyModel();
    QCOMPARE(proxy->rowCount(), 5);

    // 选择非连续行: 第 0 行 和 第 2 行 和 第 4 行 (不选 1 和 3)
    selModel->clearSelection();
    selModel->select(proxy->index(0, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
    selModel->select(proxy->index(2, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
    selModel->select(proxy->index(4, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);

    auto selectedRows = selModel->selectedRows();
    QCOMPARE(selectedRows.size(), 3);

    auto *copySelBtn = dock.findChild<QToolButton *>(QStringLiteral("copySelectedButton"));
    QVERIFY(copySelBtn != nullptr);
    QCOMPARE(copySelBtn->isEnabled(), true);

    QString exp0 = proxy->data(proxy->index(0, ErrorHistoryModel::ColTitle)).toString();
    QString exp2 = proxy->data(proxy->index(2, ErrorHistoryModel::ColTitle)).toString();
    QString exp4 = proxy->data(proxy->index(4, ErrorHistoryModel::ColTitle)).toString();
    QString unsel1 = proxy->data(proxy->index(1, ErrorHistoryModel::ColTitle)).toString();
    QString unsel3 = proxy->data(proxy->index(3, ErrorHistoryModel::ColTitle)).toString();

    dock.copySelectedToClipboard();
    QString text = clipboard->text();
    QVERIFY(!text.isEmpty());

    // 验证包含选中的三行内容
    QVERIFY(text.contains(exp0));
    QVERIFY(text.contains(exp2));
    QVERIFY(text.contains(exp4));

    // 验证严格不包含未选中的行
    QVERIFY(!text.contains(unsel1));
    QVERIFY(!text.contains(unsel3));

    // 验证分隔符格式: 3条记录应该有 2 个 "\n---\n"
    int sepCount = text.count(QStringLiteral("\n---\n"));
    QCOMPARE(sepCount, 2);
  }

  // =========================================================================
  // 3. "Copy All" on Empty vs Populated vs Filtered History
  // =========================================================================
  void testCopyAllEmptyVsPopulatedAndFiltered()
  {
    QClipboard *clipboard = QGuiApplication::clipboard();
    clipboard->setText(QStringLiteral("CLIPBOARD_CANARY"));

    ErrorHistoryDock dock;
    dock.show();
    QCoreApplication::processEvents();

    // A. 空历史测试: copyAll 不应覆盖剪贴板
    dock.copyAllToClipboard();
    QCOMPARE(clipboard->text(), QStringLiteral("CLIPBOARD_CANARY"));

    // B. 填充 3 条数据
    ErrorHub::instance()->setDedupWindowSecs(0);
    ErrorHub::instance()->reportInfo(ErrorDomain::Seismic, QStringLiteral("Seismic Info"));
    ErrorHub::instance()->reportWarning(ErrorDomain::Well, QStringLiteral("Well Warn"));
    ErrorHub::instance()->reportError(ErrorDomain::IO, QStringLiteral("IO Err"));
    QCoreApplication::processEvents();

    dock.copyAllToClipboard();
    QString copiedAll = clipboard->text();
    QVERIFY(copiedAll.contains(QStringLiteral("Seismic Info")));
    QVERIFY(copiedAll.contains(QStringLiteral("Well Warn")));
    QVERIFY(copiedAll.contains(QStringLiteral("IO Err")));
    QCOMPARE(copiedAll.count(QStringLiteral("\n---\n")), 2);

    // C. 过滤后复制全部：应仅复制当前过滤后可见的行
    clipboard->clear();
    dock.setDomainFilter(ErrorDomain::Well);
    QCOMPARE(dock.proxyModel()->rowCount(), 1);

    dock.copyAllToClipboard();
    QString copiedFiltered = clipboard->text();
    QVERIFY(copiedFiltered.contains(QStringLiteral("Well Warn")));
    QVERIFY(!copiedFiltered.contains(QStringLiteral("Seismic Info")));
    QVERIFY(!copiedFiltered.contains(QStringLiteral("IO Err")));
    QCOMPARE(copiedFiltered.count(QStringLiteral("\n---\n")), 0);
  }

  // =========================================================================
  // 4. Multi-line, Unicode, Chinese, and Special Characters Robustness
  // =========================================================================
  void testComplexMessagesUnicodeAndSpecialChars()
  {
    QClipboard *clipboard = QGuiApplication::clipboard();
    clipboard->clear();

    QString multilineMsg = QStringLiteral("【地震工区解释】层位追踪失败！\n原因: 遇到断层错断 (断距 > 120m)\n建议: 检查断层多边形闭合性。");
    QString multilineDetails = QStringLiteral("堆栈详情:\n  at SeismicTracker::traceHorizon(line=412)\n  at GridInterpolation::run()\n参数: {\"grid\": \"CGG_3D_SURVEY\", \"smooth\": 0.85}");
    QString specialChars = QStringLiteral("<xml attr=\"test\">&amp; 'quotes' \"double\" \t tabs and \\backslashes\\ --- dashes");

    ErrorHub::instance()->reportError(
      ErrorDomain::Seismic,
      multilineMsg,
      multilineDetails,
      QStringLiteral("key.multiline.seismic"));

    ErrorHub::instance()->reportWarning(
      ErrorDomain::Project,
      specialChars,
      QStringLiteral("特殊符号详情"),
      QStringLiteral("key.special.chars"));
    QCoreApplication::processEvents();

    ErrorHistoryDock dock;
    dock.show();
    QCoreApplication::processEvents();

    QCOMPARE(dock.model()->rowCount(), 2);

    // 验证 Model 的 DisplayRole 与 ToolTipRole 正确提取
    QModelIndex idx0Title = dock.model()->index(0, ErrorHistoryModel::ColTitle);
    QCOMPARE(dock.model()->data(idx0Title, Qt::DisplayRole).toString(), multilineMsg);

    QModelIndex idx0Msg = dock.model()->index(0, ErrorHistoryModel::ColMessage);
    QCOMPARE(dock.model()->data(idx0Msg, Qt::DisplayRole).toString(), multilineDetails);

    QVariant tip = dock.model()->data(idx0Title, Qt::ToolTipRole);
    QVERIFY(tip.toString().contains(QStringLiteral("【地震工区解释】")));
    QVERIFY(tip.toString().contains(QStringLiteral("堆栈详情:")));

    // 验证特殊字符
    QModelIndex idx1Title = dock.model()->index(1, ErrorHistoryModel::ColTitle);
    QCOMPARE(dock.model()->data(idx1Title, Qt::DisplayRole).toString(), specialChars);

    // 复制全部并验证剪贴板无乱码
    dock.copyAllToClipboard();
    QString copied = clipboard->text();
    QVERIFY(copied.contains(multilineMsg));
    QVERIFY(copied.contains(multilineDetails));
    QVERIFY(copied.contains(specialChars));
  }

  // =========================================================================
  // 5. Clear History: Two-way Synchronization
  // =========================================================================
  void testClearHistoryTwoWaySyncComprehensive()
  {
    // A. 填充 10 条
    for (int i = 0; i < 10; ++i) {
      ErrorHub::instance()->reportError(ErrorDomain::General, QStringLiteral("Err %1").arg(i));
    }
    QCoreApplication::processEvents();

    ErrorHistoryDock dock;
    dock.show();
    QCoreApplication::processEvents();

    auto *statusLabel = dock.findChild<QLabel *>(QStringLiteral("errorHistoryStatusLabel"));
    QVERIFY(statusLabel != nullptr);
    QCOMPARE(dock.model()->rowCount(), 10);
    QCOMPARE(statusLabel->text(), QStringLiteral("共 10 条记录"));

    // B. UI 触发清空
    dock.clearHistory();
    QCoreApplication::processEvents();

    QCOMPARE(ErrorHub::instance()->count(), 0);
    QCOMPARE(dock.model()->rowCount(), 0);
    QCOMPARE(dock.proxyModel()->rowCount(), 0);
    QCOMPARE(statusLabel->text(), QStringLiteral("共 0 条记录"));

    // C. 空状态下再次触发清空：无异常、无越界
    dock.clearHistory();
    QCoreApplication::processEvents();
    QCOMPARE(dock.model()->rowCount(), 0);

    // D. 外部通过 ErrorHub 直接清空与新增：反向同步检查
    ErrorHub::instance()->reportInfo(ErrorDomain::Well, QStringLiteral("New Well Error"));
    QCoreApplication::processEvents();
    QCOMPARE(dock.model()->rowCount(), 1);
    QCOMPARE(statusLabel->text(), QStringLiteral("共 1 条记录"));

    ErrorHub::instance()->clear();
    QCoreApplication::processEvents();
    QCOMPARE(dock.model()->rowCount(), 0);
    QCOMPARE(statusLabel->text(), QStringLiteral("共 0 条记录"));
  }

  // =========================================================================
  // 6. Filtering by Domain, Level, and Text with Complex Matches
  // =========================================================================
  void testFilteringMatrixComplexAndEmptyMatches()
  {
    ErrorHub::instance()->setDedupWindowSecs(0);

    // 注入矩阵数据:
    // 1: Domain=Seismic, Level=Info, Msg="Seismic Survey Loaded"
    // 2: Domain=Seismic, Level=Warning, Msg="Seismic Trace Clipping detected"
    // 3: Domain=Seismic, Level=Error, Msg="Seismic Grid Interpolation Error"
    // 4: Domain=Well, Level=Warning, Msg="Well Trajectory Deviation Warning"
    // 5: Domain=Well, Level=Error, Msg="Well Log LAS parsing Error"
    // 6: Domain=IO, Level=Critical, Msg="Disk IO Fatal Read Failure"
    // 7: Domain=AI, Level=Error, Msg="FaultNet Model Prediction Timeout"
    ErrorHub::instance()->reportInfo(ErrorDomain::Seismic, QStringLiteral("Seismic Survey Loaded"));
    ErrorHub::instance()->reportWarning(ErrorDomain::Seismic, QStringLiteral("Seismic Trace Clipping detected"));
    ErrorHub::instance()->reportError(ErrorDomain::Seismic, QStringLiteral("Seismic Grid Interpolation Error"));
    ErrorHub::instance()->reportWarning(ErrorDomain::Well, QStringLiteral("Well Trajectory Deviation Warning"));
    ErrorHub::instance()->reportError(ErrorDomain::Well, QStringLiteral("Well Log LAS parsing Error"));
    ErrorHub::instance()->reportCritical(ErrorDomain::IO, QStringLiteral("Disk IO Fatal Read Failure"));
    ErrorHub::instance()->reportError(ErrorDomain::AI, QStringLiteral("FaultNet Model Prediction Timeout"));
    QCoreApplication::processEvents();

    ErrorHistoryDock dock;
    dock.show();
    QCoreApplication::processEvents();

    auto *proxy = dock.proxyModel();
    auto *statusLabel = dock.findChild<QLabel *>(QStringLiteral("errorHistoryStatusLabel"));
    QVERIFY(statusLabel != nullptr);
    QCOMPARE(proxy->rowCount(), 7);

    // A. 来源域过滤
    dock.setDomainFilter(ErrorDomain::Seismic);
    QCOMPARE(proxy->rowCount(), 3);
    QCOMPARE(statusLabel->text(), QStringLiteral("显示 3 / 共 7 条记录"));

    // 重置领域
    dock.setDomainFilter(QStringLiteral("全部领域"));
    QCOMPARE(proxy->rowCount(), 7);

    // 不存在的领域
    dock.setDomainFilter(QStringLiteral("NonExistentDomain"));
    QCOMPARE(proxy->rowCount(), 0);
    QCOMPARE(statusLabel->text(), QStringLiteral("显示 0 / 共 7 条记录"));

    dock.setDomainFilter(QString());
    QCOMPARE(proxy->rowCount(), 7);

    // B. 级别过滤模式
    // WarningAndAbove: Warning(2) + Error(3) + Critical(1) = 6 (排除 Info 1)
    dock.setLevelFilterMode(ErrorHistoryFilterProxyModel::LevelFilterMode::WarningAndAbove);
    QCOMPARE(proxy->rowCount(), 6);

    // ErrorAndAbove: Error(3) + Critical(1) = 4
    dock.setLevelFilterMode(ErrorHistoryFilterProxyModel::LevelFilterMode::ErrorAndAbove);
    QCOMPARE(proxy->rowCount(), 4);

    // CriticalOnly: 1
    dock.setLevelFilterMode(ErrorHistoryFilterProxyModel::LevelFilterMode::CriticalOnly);
    QCOMPARE(proxy->rowCount(), 1);

    dock.setLevelFilterMode(ErrorHistoryFilterProxyModel::LevelFilterMode::All);
    QCOMPARE(proxy->rowCount(), 7);

    // C. 文本模糊搜索 (不区分大小写)
    dock.setSearchText(QStringLiteral("error"));
    // 包含 "error" 字符串的条目: 3 条 Error 中的 2 条 (Seismic Grid..., Well Log LAS...)
    QCOMPARE(proxy->rowCount(), 2);

    // 特殊字符搜索 (如包含 "LAS" 或 "[" 无正则崩溃)
    dock.setSearchText(QStringLiteral("LAS"));
    QCOMPARE(proxy->rowCount(), 1);

    // 正则特殊字符搜索，验证直接作为普通子串匹配
    dock.setSearchText(QStringLiteral("[regex.*+?]"));
    QCOMPARE(proxy->rowCount(), 0);

    // 清空搜索框
    dock.setSearchText(QString());
    QCOMPARE(proxy->rowCount(), 7);

    // D. 组合过滤: Domain=Well + Level=WarningOnly + Text="Trajectory"
    dock.setDomainFilter(ErrorDomain::Well);
    dock.setLevelFilterMode(ErrorHistoryFilterProxyModel::LevelFilterMode::WarningOnly);
    dock.setSearchText(QStringLiteral("Trajectory"));
    QCOMPARE(proxy->rowCount(), 1);

    // 动态新增一条符合条件的错误
    ErrorHub::instance()->reportWarning(ErrorDomain::Well, QStringLiteral("New Well Trajectory Warning"));
    QCoreApplication::processEvents();
    QCOMPARE(proxy->rowCount(), 2);
    QCOMPARE(statusLabel->text(), QStringLiteral("显示 2 / 共 8 条记录"));
  }

  // =========================================================================
  // 7. Dock Toggle View Action Cycle
  // =========================================================================
  void testDockToggleViewActionCycle()
  {
    QMainWindow mainWindow;
    PaleoDockManager dockManager(&mainWindow, QStringLiteral("test/toggle_dock"));
    auto *dock = new ErrorHistoryDock(&mainWindow);

    mainWindow.addDockWidget(Qt::BottomDockWidgetArea, dock);
    dockManager.addDock(Qt::BottomDockWidgetArea, dock);

    QAction *toggleAct = dock->toggleViewAction();
    QVERIFY(toggleAct != nullptr);
    QVERIFY(toggleAct->isCheckable());

    // 循环切换 10 次显示/隐藏，验证 checkable 状态与 widget 隐藏状态保持绝对同步
    for (int cycle = 0; cycle < 10; ++cycle) {
      // 切换为可见
      toggleAct->trigger();
      QCoreApplication::processEvents();
      QVERIFY(!dock->isHidden());
      QCOMPARE(toggleAct->isChecked(), true);

      // 切换为隐藏
      toggleAct->trigger();
      QCoreApplication::processEvents();
      QVERIFY(dock->isHidden());
      QCOMPARE(toggleAct->isChecked(), false);
    }
  }

  // =========================================================================
  // 8. Dynamic Eviction Stability under Active Filter and Selection
  // =========================================================================
  void testActiveEvictionUnderFilterAndSelection()
  {
    ErrorHub::instance()->setMaxCapacity(20);
    ErrorHub::instance()->setDedupWindowSecs(0);

    ErrorHistoryDock dock;
    dock.show();
    dock.setDomainFilter(ErrorDomain::Seismic);
    QCoreApplication::processEvents();

    // 注入 50 条消息 (Seismic 与 Well 混合)，触发多次 FIFO 逐出
    for (int i = 0; i < 50; ++i) {
      if (i % 2 == 0) {
        ErrorHub::instance()->reportError(ErrorDomain::Seismic, QStringLiteral("Seismic %1").arg(i));
      } else {
        ErrorHub::instance()->reportError(ErrorDomain::Well, QStringLiteral("Well %1").arg(i));
      }
      if (i == 10) {
        // 在逐出过程中选择表格第 0 行
        if (dock.proxyModel()->rowCount() > 0) {
          dock.tableView()->selectRow(0);
        }
      }
      QCoreApplication::processEvents();
    }

    QCOMPARE(dock.model()->rowCount(), 20);
    // 代理模型中的行数不应超过 20
    QVERIFY(dock.proxyModel()->rowCount() <= 20);
    // 并且所有保留在代理模型中的项必须是 Seismic
    for (int r = 0; r < dock.proxyModel()->rowCount(); ++r) {
      QCOMPARE(dock.proxyModel()->data(dock.proxyModel()->index(r, ErrorHistoryModel::ColDomain)).toString(),
               ErrorDomain::Seismic);
    }
  }
};

int main(int argc, char *argv[])
{
  if (qgetenv("QT_QPA_PLATFORM").isEmpty()) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  QApplication app(argc, argv);
  TestErrorHubChallengerM4 tc;
  return QTest::qExec(&tc, argc, argv);
}

#include "tst_errorhub_challenger_m4.moc"
