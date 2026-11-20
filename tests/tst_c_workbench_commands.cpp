// 层：测试壳
// C｜工作台命令资格、页面反馈与绑定生命周期（codex/paleo-c-workbench-commands-20261120）。
// 覆盖：
//   - C-W1 单份动作目录：表完整性、knownActions 派生一致性、与页内动作不相交、
//     每条动作被其声明族受理（运行可证伪：空工程上逐条分派，断言不落
//     「未被其声明处理族受理」兜底——import 的文件对话框由定时器关闭）。
//   - C-W2 统一准入：只读工程拒绝全部 Mutating、放行全部 Read；工程切换中
//     一律拒绝；必需依赖缺失给具体原因（无 bad_function_call、不静默成功）。
//   - C-W4 跨模态生命周期：删除确认框接受/拒绝两条路径；确认期间切只读 →
//     返回后按只读拒绝，不执行删除（真实域 oracle：图件仍在）。
//   - C-W5 当前页解析：currentWorkbenchPage 按可见页签解析，非工作台页为空。
//   - C-W6 镜像生命周期：重绑断旧源、checkable 完整复位、源销毁禁用可诊断、
//     来源缺失禁用、单次激活。
//   - C-W7 重复 attach 幂等：页/控制器数量不变。
//   - C-W3 局部撤销门：DataListPanel 只读门覆盖按钮/直调/快捷键；router
//     setWritable(false) 复用验证。
#include <QtTest>

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QFileDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QShortcut>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QVector>
#include <gdal.h>
#include <qgsapplication.h>
#include <qgscoordinatereferencesystem.h>
#include <qgsgui.h>
#include <qgseditorwidgetregistry.h>

#include "../src/catalog/datacatalog.h"
#include "../src/io/dataimportservice.h"
#include "../src/linkage/selectioncontext.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/qgiscanvascontroller.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisprocessingservice.h"
#include "../src/services/previewdoc.h"
#include "../src/ui/commandactionbinder.h"
#include "../src/ui/paleomainwindow.h"
#include "../src/ui/paleoribbon.h"
#include "../src/ui/edittools/undogrouprouter.h"
#include "../src/ui/pages/constraintpage.h"
#include "../src/ui/pages/datalist.h"
#include "../src/ui/pages/mappingworkbenchpage.h"
#include "../src/ui/workbench/mappingworkbenchcommandcontroller.h"
#include "../src/ui/workbenchcommandregistry.h"
#include "../src/ui/workbench/workbenchactioncatalog.h"
#include "../src/ui/workbench/workbenchadmission.h"
#include "../src/workflow/commanddispatcher.h"
#include "../src/workflow/dataopscommandservice.h"
#include "../src/workflow/mappingworkbench.h"
#include "../src/workflow/workflows.h"
#include "mockremotepredictionservice.h"

using namespace paleo::workbenchcmd;
using paleo::workbench::MappingWorkbenchCommandController;

namespace
{
// 与 tst_mappingworkbench::Fixture 同型的最小生产栈（空目录/空目录表——
// 业务调用失败但不弹模态），供准入与分派覆盖矩阵使用。
struct Fixture
{
  QTemporaryDir dir;
  DataCatalog catalog;
  QgisProjectService project;
  PaleoProjectStore store;
  LayerManifest manifest{dir.filePath("project.sqlite")};
  QgisLayerService layers{&project, &manifest};
  QgisProcessingService processing{&store};
  ConstraintWorkflow constraints{&processing, &layers};
  MappingWorkbench work{&layers, &processing, &project, &constraints};
  MockRemotePredictionService prediction;
  bool init()
  {
    work.setPredictionService(&prediction);
    if (!catalog.open(dir.path()) ||
        !project.createProject(dir.filePath("project.qgz")) || !manifest.open())
      return false;
    store.setProjectPaths(project.projectPath(), dir.filePath("project.gpkg"),
                          dir.filePath("project.sqlite"));
    constraints.setStore(&store);
    constraints.setCatalog(&catalog, dir.path());
    work.bindCatalog(&catalog, dir.path());
    return true;
  }
};

// 模态关闭器：进嵌套事件循环后按角色关闭最上层目标对话框，并记录是否见到。
struct ModalCloser
{
  bool sawDialog = false;
  bool accept = false;
  void arm()
  {
    QTimer::singleShot(0, [this] { closeOne(); });
  }
  void closeOne()
  {
    QWidget *target = nullptr;
    for (QWidget *w : QApplication::topLevelWidgets())
      if ((qobject_cast<QFileDialog *>(w) || qobject_cast<QMessageBox *>(w)) &&
          w->isVisible())
        target = w;
    if (!target)
      return; // 没弹框（如空工程删除早退）——sawDialog 保持 false
    sawDialog = true;
    if (auto *file = qobject_cast<QFileDialog *>(target))
    {
      file->reject(); // 文件选择统一走取消路径（路径空 = 用户取消）
      return;
    }
    auto *box = qobject_cast<QMessageBox *>(target);
    const auto buttons = box->buttons();
    QAbstractButton *choice = nullptr;
    for (auto *b : buttons)
    {
      const auto role = box->buttonRole(b);
      if (accept ? role == QMessageBox::AcceptRole : role == QMessageBox::RejectRole)
        choice = b;
    }
    if (choice)
      choice->click();
    else
      box->reject();
  }
};

class PageFeedback
{
  public:
    explicit PageFeedback(MappingWorkbenchPage *page) : m_page(page) {}
    QString message() const
    {
      if (!m_page)
        return QString();
      if (auto *label = m_page->findChild<QLabel *>(QStringLiteral("workbenchMessage")))
        return label->text();
      return QString();
    }
    void clear()
    {
      if (m_page)
        m_page->showMessage(QString());
    }
  private:
    QPointer<MappingWorkbenchPage> m_page;
};
} // namespace

class TestCWorkbenchCommands : public QObject
{
  Q_OBJECT
  struct WindowFixture
  {
    Fixture f;
    QgisCanvasController canvasCtl;
    SelectionContext selection;
    std::unique_ptr<PaleoMainWindow> window;
    bool init()
    {
      if (!f.init())
        return false;
      selection.setActiveHorizon(QStringLiteral("D61"));
      window = std::make_unique<PaleoMainWindow>(&canvasCtl, &f.project, &f.layers,
                                                  nullptr, &selection);
      window->attachWorkbench(&f.work);
      return !window->findChildren<MappingWorkbenchPage *>().isEmpty();
    }
    MappingWorkbenchCommandController *controllerFor(MappingWorkbenchPage *page) const
    {
      return page->findChild<MappingWorkbenchCommandController *>(
          QString(), Qt::FindDirectChildrenOnly);
    }
    QList<MappingWorkbenchCommandController *> controllers() const
    {
      QList<MappingWorkbenchCommandController *> out;
      for (auto *page : window->findChildren<MappingWorkbenchPage *>())
        if (auto *c = controllerFor(page))
          out.append(c);
      return out;
    }
  };

  private slots:
    // ---- C-W1：单份目录 ----
    void catalogIsCompleteAndDerived()
    {
      QVERIFY2(catalogIntegrity().isEmpty(),
               qPrintable(catalogIntegrity().join('\n')));
      QVERIFY(catalog().size() >= 40);
      // knownActions 与目录一一对应（登记表由目录派生，不再平行维护）。
      QSet<QString> ids;
      for (const auto &spec : catalog())
        ids.insert(QString::fromLatin1(spec.id));
      QCOMPARE(ids, knownActions());
      // 页内动作不经 commandRequested 外发，不得混进目录。
      for (const auto &local : pageLocalActions())
        QVERIFY2(!ids.contains(local), qPrintable(local));
      // 副作用与结果策略的枚举域内取值（防未初始化枚举）。
      for (const auto &spec : catalog())
      {
        QVERIFY(spec.effect == Effect::Read || spec.effect == Effect::Mutating);
        QVERIFY(spec.outcome == Outcome::Standard || spec.outcome == Outcome::Queued ||
                spec.outcome == Outcome::Detailed || spec.outcome == Outcome::Removal ||
                spec.outcome == Outcome::FlowOwned);
        QVERIFY(spec.family == Family::EditFlow || spec.family == Family::Query ||
                spec.family == Family::Edit || spec.family == Family::Lifecycle);
      }
      // 查询族 ≠ 只读：会产结果的查询族动作必须记 Mutating（族名不推断策略）。
      QVERIFY(find(QStringLiteral("predict"))->effect == Effect::Mutating);
      QVERIFY(find(QStringLiteral("recompute"))->effect == Effect::Mutating);
      QVERIFY(find(QStringLiteral("cancel"))->effect == Effect::Read);
      QVERIFY(find(QStringLiteral("show"))->effect == Effect::Read);
      QVERIFY(find(QStringLiteral("exportVector"))->effect == Effect::Read);
    }

    void everyActionIsClaimedByItsDeclaredFamily()
    {
      WindowFixture w;
      QVERIFY(w.init());
      auto *page = w.window->findChildren<MappingWorkbenchPage *>().first();
      auto *handler = w.controllerFor(page);
      QVERIFY(handler);
      PageFeedback feedback(page);
      const auto run = [&feedback, handler](const QString &action) {
        feedback.clear();
        handler->handleCommand(action, {});
        return feedback.message();
      };
      for (const auto &spec : catalog())
      {
        const QString action = QString::fromLatin1(spec.id);
        // 逐条挂模态关闭器：import 必弹文件对话框；其余动作在空工程上不应
        // 弹框（若弹出则被取消——用户取消分支，同样证明族已受理）。
        ModalCloser closer;
        closer.arm();
        const QString message = run(action);
        if (action == QLatin1String("import"))
        {
          QVERIFY2(closer.sawDialog, "import 应弹出文件选择对话框（声明族已受理）");
          QVERIFY(message.isEmpty()); // 用户取消：不发成功也不发失败文案
          continue;
        }
        QVERIFY2(!closer.sawDialog,
                 qPrintable(action + " 不应弹出模态对话框: " + message));
        QVERIFY2(!message.contains(QStringLiteral("未被其声明处理族受理")),
                 qPrintable(action + " -> " + message));
        QVERIFY2(!message.contains(QStringLiteral("未知操作")),
                 qPrintable(action + " -> " + message));
      }
      // 未知动作仍然显式失败（H1 回归保持）。
      const QString unknown = run(QStringLiteral("noSuchWorkbenchAction"));
      QVERIFY(unknown.contains(QStringLiteral("未知操作")));
    }

    void drawCommandClaimsWhenConstraintPagePresent()
    {
      // WindowFixture 未接 attachWorkflows（约束页缺席），draw 在那里被依赖位
      // 拒绝——这里补一个带真实 ConstraintPage 的最小上下文，验证 draw 的
      // 声明族（Lifecycle）确实受理它并给出绘制指引文案。
      Fixture f;
      QVERIFY(f.init());
      QWidget host;
      QStatusBar status;
      MappingWorkbenchPage page(QStringLiteral("constraint"), &f.work, &host);
      ConstraintPage constraint(&f.constraints, &host);
      MappingWorkbenchCommandController::Context ctx;
      ctx.workbench = &f.work;
      ctx.page = &page;
      ctx.dialogParent = &host;
      ctx.statusBar = &status;
      ctx.constraint = &constraint;
      MappingWorkbenchCommandController controller(ctx, &page);
      PageFeedback feedback(&page);
      controller.handleCommand(QStringLiteral("draw"), {});
      QVERIFY2(feedback.message().contains(QStringLiteral("绘制约束线")),
               qPrintable(feedback.message()));
    }

    // ---- C-W2：统一准入 ----
    void mutatingActionsRefusedWhenProjectReadOnly()
    {
      WindowFixture w;
      QVERIFY(w.init());
      auto *page = w.window->findChildren<MappingWorkbenchPage *>().first();
      auto *handler = w.controllerFor(page);
      PageFeedback feedback(page);
      w.window->setProjectReadOnly(true);
      for (const auto &spec : catalog())
      {
        feedback.clear();
        handler->handleCommand(QString::fromLatin1(spec.id), {});
        const QString message = feedback.message();
        if (spec.effect == Effect::Mutating)
          QVERIFY2(message.contains(QStringLiteral("只读")),
                   qPrintable(QString("%1 -> %2").arg(spec.id, message)));
        else
          QVERIFY2(!message.contains(QStringLiteral("只读")),
                   qPrintable(QString("%1 -> %2").arg(spec.id, message)));
      }
      w.window->setProjectReadOnly(false);
      // 恢复可写后按真实资格重判（不被自己的拒绝态锁死）。
      feedback.clear();
      handler->handleCommand(QStringLiteral("labels"), {});
      QVERIFY(!feedback.message().contains(QStringLiteral("只读")));
    }

    void missingDependenciesRefuseWithNamedReason()
    {
      // 最小上下文：基础四件在场，其余依赖/回调全部缺席——依赖位声明的动作
      // 必须给出点名原因并拒绝（无 bad_function_call、无 NPE、不伪成功）；
      // 无依赖声明的动作走业务路径（空工程上失败，但不是准入拒绝）。
      Fixture f;
      QVERIFY(f.init());
      QWidget host;
      QStatusBar status;
      MappingWorkbenchPage page(QStringLiteral("predict"), &f.work, &host);
      MappingWorkbenchCommandController::Context ctx;
      ctx.workbench = &f.work;
      ctx.page = &page;
      ctx.dialogParent = &host;
      ctx.statusBar = &status;
      MappingWorkbenchCommandController controller(ctx, &page);
      PageFeedback feedback(&page);
      for (const auto &spec : catalog())
      {
        feedback.clear();
        // 无依赖声明的动作会走到业务分支：import 先弹文件对话框——逐条挂
        // 关闭器（只在真有对话框时起作用），取消路径即「用户取消」分支。
        ModalCloser closer;
        closer.arm();
        controller.handleCommand(QString::fromLatin1(spec.id), {});
        const QString message = feedback.message();
        if (spec.caps != kNone)
        {
          QVERIFY2(message.contains(QStringLiteral("依赖不可用")),
                   qPrintable(QString("%1 -> %2").arg(spec.id, message)));
          // 原因点名具体依赖（不是泛化「不可用」）。
          const unsigned missing =
              paleo::workbench::WorkbenchAdmission::missingCaps(spec, ctx);
          QVERIFY(missing == spec.caps);
          const QString named = missingCapsReason(missing);
          QVERIFY2(!named.isEmpty(), spec.id);
        }
        else
        {
          QVERIFY2(!message.contains(QStringLiteral("依赖不可用")),
                   qPrintable(QString("%1 -> %2").arg(spec.id, message)));
        }
      }
    }

    void switchingProjectRefusesEverything()
    {
      Fixture f;
      QVERIFY(f.init());
      QWidget host;
      QStatusBar status;
      MappingWorkbenchPage page(QStringLiteral("compose"), &f.work, &host);
      MappingWorkbenchCommandController::Context ctx;
      ctx.workbench = &f.work;
      ctx.page = &page;
      ctx.dialogParent = &host;
      ctx.statusBar = &status;
      ctx.projectSwitching = [] { return true; };
      MappingWorkbenchCommandController controller(ctx, &page);
      PageFeedback feedback(&page);
      for (const auto &spec : catalog())
      {
        feedback.clear();
        controller.handleCommand(QString::fromLatin1(spec.id), {});
        QVERIFY2(feedback.message().contains(QStringLiteral("正在打开或切换")),
                 qPrintable(QString("%1 -> %2").arg(spec.id, feedback.message())));
      }
    }

    // ---- C-W4：跨模态生命周期（删除确认）----
    void deleteConfirmRejectedKeepsProduct()
    {
      WindowFixture w;
      QVERIFY(w.init());
      QString error;
      const auto draft = w.f.work.createBlankMap(QStringLiteral("D61"), &error);
      QVERIFY2(!draft.isEmpty(), qPrintable(error));
      auto *page = w.window->findChildren<MappingWorkbenchPage *>().first();
      auto *handler = w.controllerFor(page);
      PageFeedback feedback(page);
      // 创建是异步收尾：等删除产品清单就绪（有界轮询），排空「已生成图件」
      // 类排队消息后再进入删除路径。
      for (int i = 0; i < 100 && w.f.work.removalProducts(draft, nullptr).isEmpty(); ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
      QCoreApplication::processEvents();
      feedback.clear();
      ModalCloser closer;
      closer.accept = false; // 用户点「取消」
      closer.arm();
      handler->handleCommand(QStringLiteral("delete"), {{QStringLiteral("layer"), draft}});
      QVERIFY2(closer.sawDialog, "空产品外的真实删除应弹确认框");
      QVERIFY(feedback.message().isEmpty()); // 取消：不发成功文案
      QVERIFY(!w.f.work.declaration(draft).layerId.isEmpty()); // 图件仍在（真实域 oracle）
    }

    void deleteRefusedWhenReadOnlyFlipsDuringConfirm()
    {
      WindowFixture w;
      QVERIFY(w.init());
      QString error;
      const auto draft = w.f.work.createBlankMap(QStringLiteral("D61"), &error);
      QVERIFY2(!draft.isEmpty(), qPrintable(error));
      auto *page = w.window->findChildren<MappingWorkbenchPage *>().first();
      auto *handler = w.controllerFor(page);
      PageFeedback feedback(page);
      for (int i = 0; i < 100 && w.f.work.removalProducts(draft, nullptr).isEmpty(); ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
      QCoreApplication::processEvents();
      feedback.clear();
      // 确认框打开期间切只读：先注册翻只读的定时器，再注册关闭器（接受键）——
      // 两个 0ms 定时器按注册顺序触发，点击前工程已只读。
      ModalCloser closer;
      closer.accept = true;
      QTimer::singleShot(0, [&w] { w.window->setProjectReadOnly(true); });
      closer.arm();
      handler->handleCommand(QStringLiteral("delete"), {{QStringLiteral("layer"), draft}});
      QVERIFY(closer.sawDialog);
      QVERIFY2(feedback.message().contains(QStringLiteral("只读")),
               qPrintable(feedback.message()));
      QVERIFY(!w.f.work.declaration(draft).layerId.isEmpty()); // 未删除
      w.window->setProjectReadOnly(false);
    }

    // ---- C-W5：当前页解析 ----
    void currentWorkbenchPageFollowsVisibleTab()
    {
      WindowFixture w;
      QVERIFY(w.init());
      const auto pages = w.window->findChildren<MappingWorkbenchPage *>();
      QCOMPARE(pages.size(), 3);
      w.window->showPage(QStringLiteral("constraint"));
      auto *constraintPage = w.window->currentWorkbenchPage();
      QVERIFY(constraintPage);
      QCOMPARE(constraintPage->objectName(), QStringLiteral("mappingWorkbench.constraint"));
      w.window->showPage(QStringLiteral("compose"));
      auto *composePage = w.window->currentWorkbenchPage();
      QVERIFY(composePage);
      QCOMPARE(composePage->objectName(), QStringLiteral("mappingWorkbench.compose"));
      QVERIFY(composePage != constraintPage);
      // 非工作台页：无工作台上下文，解析为空（反馈走宿主状态栏/拒绝）。
      w.window->showPage(QStringLiteral("data"));
      QVERIFY(!w.window->currentWorkbenchPage());
    }

    // ---- C-W6：镜像生命周期 ----
    void mirrorRebindDropsOldSourceAndResetsCheckable()
    {
      QWidget host;
      auto *a = new QPushButton(QStringLiteral("A"), &host);
      a->setCheckable(true);
      a->setChecked(true);
      auto *b = new QPushButton(QStringLiteral("B"), &host);
      b->setCheckable(true);
      auto *c = new QPushButton(QStringLiteral("C"), &host); // 普通（不可勾选）
      auto *action = new QAction(QStringLiteral("mirror"), &host);
      PaleoRibbon::mirror(action, a, true, false);
      QVERIFY(action->isCheckable());
      QVERIFY(action->isChecked());
      // A(可勾选) → B(可勾选)：旧源 toggled 不再影响动作。
      PaleoRibbon::mirror(action, b, true, false);
      b->setChecked(true);
      a->setChecked(false);
      QVERIFY(action->isChecked()); // 只跟当前源 B
      // B(可勾选) → C(普通)：checkable 完整复位。
      PaleoRibbon::mirror(action, c, true, false);
      QVERIFY(!action->isCheckable());
      QVERIFY(!action->isChecked());
      QVERIFY(action->isEnabled()); // 普通源照常可用
      b->setChecked(false);          // 旧源继续变化：无影响（已断连）
      QVERIFY(!action->isCheckable());
    }

    void mirrorSourceDestroyedDisablesWithReason()
    {
      QWidget host;
      auto *action = new QAction(QStringLiteral("mirror"), &host);
      {
        auto *button = new QPushButton(QStringLiteral("src"), &host);
        PaleoRibbon::mirror(action, button, true, false);
        QVERIFY(action->isEnabled());
        delete button;
      }
      QVERIFY(!action->isEnabled());
      QVERIFY2(!action->toolTip().isEmpty(), "源销毁后 tooltip 带可诊断原因");
      QVERIFY(action->toolTip().contains(QStringLiteral("销毁")));
    }

    void mirrorMissingSourceDisablesDiagnosably()
    {
      QWidget host;
      auto *action = new QAction(QStringLiteral("mirror"), &host);
      action->setEnabled(true);
      PaleoRibbon::mirror(action, nullptr, true, false); // 源缺失（按钮未建/已毁）
      QVERIFY(!action->isEnabled());
      QVERIFY(action->toolTip().contains(QStringLiteral("不可用")));
    }

    void mirrorDispatchActivatesExactlyOnce()
    {
      paleo::command::CommandDispatcher d;
      CommandActionBinder binder(&d);
      QWidget host;
      int clicks = 0;
      auto *button = new QPushButton(QStringLiteral("src"), &host);
      QObject::connect(button, &QPushButton::clicked, [&clicks] { ++clicks; });
      auto *action = new QAction(QStringLiteral("mirror"), &host);
      PaleoRibbon::mirror(action, button, false, false);
      binder.bindMirror(action, QStringLiteral("constraint.breakLine"));
      QVERIFY(d.trigger(QStringLiteral("constraint.breakLine"), paleo::command::CommandContext{}, {}));
      QCOMPARE(clicks, 1); // dispatcher 唯一激活：恰好一次
      action->trigger();   // ribbon 点击同一条路径（不双触发）
      QCOMPARE(clicks, 2);
    }

    // ---- C-W7：重复 attach 幂等 ----
    void repeatedAttachKeepsSingleAssembly()
    {
      WindowFixture w;
      QVERIFY(w.init());
      const int pages = w.window->findChildren<MappingWorkbenchPage *>().size();
      const int controllers = w.controllers().size();
      const int tabs = w.window->findChildren<QTabWidget *>(
                           QStringLiteral("workbenchTabs.predict")).size() +
                       w.window->findChildren<QTabWidget *>(
                           QStringLiteral("workbenchTabs.constraint")).size() +
                       w.window->findChildren<QTabWidget *>(
                           QStringLiteral("workbenchTabs.compose")).size();
      QVERIFY(pages > 0);
      QCOMPARE(controllers, pages); // 每页恰一个控制器
      QCOMPARE(tabs, 3);
      w.window->attachWorkbench(&w.f.work); // 幂等：不重复包装页面/叠信号
      QCOMPARE(w.window->findChildren<MappingWorkbenchPage *>().size(), pages);
      QCOMPARE(w.controllers().size(), controllers);
    }

    // ---- C-W3：局部撤销门 ----
    void dataListUndoGateCoversButtonsDirectCallsAndShortcut()
    {
      QTemporaryDir dir;
      QVERIFY(dir.isValid());
      DataImportService importer;
      importer.setProjectDir(dir.path());
      PreviewDocService doc(&importer);
      DataListPanel panel;
      panel.setDocService(&doc);
      panel.resize(640, 740);
      panel.show();
      // 先刷新一次让 loadStoresForCatalog 完成目录会话登记（首次路径变化会
      // 清命令栈——生产里用户操作前刷新早已发生）。
      panel.refreshAssetTable();
      // 造一条可撤销历史：把 undo/redo 计数的哑命令推进面板栈。
      static int undone = 0, redone = 0;
      undone = redone = 0;
      class DumbOp : public paleo::dataops::DataOpCommand
      {
        public:
          bool undo() override { ++undone; return true; }
          bool redo() override { ++redone; return true; }
          QString text() const override { return QStringLiteral("哑操作"); }
      };
      panel.pushCommand(new DumbOp());
      undone = redone = 0; // push 已执行一次 redo——归零后再测各入口
      QVERIFY(panel.opStack() && panel.opStack()->canUndo());
      auto *undoBtn = panel.findChild<QPushButton *>(QStringLiteral("dataUndoButton"));
      QVERIFY(undoBtn && undoBtn->isEnabled());
      // 只读门：按钮置灰带原因。
      panel.setUndoReadOnly(true);
      QVERIFY(!undoBtn->isEnabled());
      QVERIFY(undoBtn->toolTip().contains(QStringLiteral("只读")));
      QSignalSpy status(&panel, &DataListPanel::statusMessage);
      // 程序直调：拒绝且不改栈。
      panel.undoOp();
      QCOMPARE(undone, 0);
      QCOMPARE(status.size(), 1);
      QVERIFY(status.first().first().toString().contains(QStringLiteral("只读")));
      // 快捷键（data.list.undo = Ctrl+Z，ShortcutCatalog 键序）：同一条门。
      // 面板取焦点让 WidgetWithChildrenShortcut 语境的匹配可达。
      panel.setFocusPolicy(Qt::StrongFocus);
      panel.setFocusPolicy(Qt::StrongFocus);
      panel.activateWindow();
      QVERIFY(QTest::qWaitForWindowActive(&panel));
      panel.setFocus();
      status.clear();
      QTest::keyClick(&panel, Qt::Key_Z, Qt::ControlModifier);
      QCOMPARE(undone, 0);
      QCOMPARE(status.size(), 1);
      QVERIFY(status.first().first().toString().contains(QStringLiteral("只读")));
      // redo 对称。
      panel.redoOp();
      QCOMPARE(redone, 0);
      // 恢复可写：历史仍在，按原契约可用。
      panel.setUndoReadOnly(false);
      QVERIFY(undoBtn->isEnabled());
      panel.undoOp();
      QCOMPARE(undone, 1);
    }

    void undoRouterWritableGateReused()
    {
      // 复用验证：router setWritable(false) → step 拒绝且不消费栈。
      PaleoUndoGroupRouter router;
      QUndoStack stack;
      QWidget scope;
      scope.show();
      PaleoUndoTarget target;
      target.describe = [&stack](bool undo) {
        return PaleoUndoStep{undo ? stack.canUndo() : stack.canRedo(),
                             undo ? stack.undoText() : stack.redoText()};
      };
      target.step = [&stack](bool undo) {
        if (undo)
          stack.undo();
        else
          stack.redo();
        return true;
      };
      router.registerScope(&scope, nullptr, target);
      stack.push(new QUndoCommand(QStringLiteral("x")));
      router.handleFocusChanged(&scope);
      QCOMPARE(stack.index(), 1); // push(x) 已使 index=1（尚未撤销）
      QVERIFY(router.step(true));
      QCOMPARE(stack.index(), 0);
      stack.push(new QUndoCommand(QStringLiteral("y"))); // index=1
      router.setWritable(false);
      QVERIFY(!router.step(true)); // 只读：不消费栈
      QCOMPARE(stack.index(), 1);  // 栈位置未变（真实域 oracle）
      router.setWritable(true);
      QVERIFY(router.step(true));  // 恢复可写：按原契约继续可用
      QCOMPARE(stack.index(), 0);
    }
};

int main(int argc, char **argv)
{
  QTemporaryDir profile;
  if (!profile.isValid())
    return 1;
  QgsApplication app(argc, argv, true, profile.path());
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH"), true);
  QgsApplication::init(profile.path());
  QCoreApplication::setOrganizationName(QStringLiteral("paleo-tests"));
  QCoreApplication::setApplicationName(QStringLiteral("tst_c_workbench_commands"));
  QString databaseError;
  if (!QgsApplication::createDatabase(&databaseError))
  {
    qCritical().noquote() << databaseError;
    return 1;
  }
  app.initQgis();
  QgsCoordinateReferenceSystem::invalidateCache(true);
  QgsGui::editorWidgetRegistry()->initEditors();
  GDALAllRegister();
  TestCWorkbenchCommands test;
  const int result = QTest::qExec(&test, argc, argv);
  QgsApplication::exitQgis();
  return result;
}

#include "tst_c_workbench_commands.moc"
