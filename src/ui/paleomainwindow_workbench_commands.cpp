// 层：视图
// paleomainwindow_workbench_commands — attachWorkbench 命令分发链（#473）：
// MappingWorkbenchPage::commandRequested 的动作分支表，逐字平移自
// paleomainwindow_workbench.cpp 原 lambda 体（仅整体去一层缩进），行为守恒。
// 参数即原 lambda 捕获面（this 之外全部显式化）；私有成员访问经
// paleomainwindow.h 的成员函数声明（同 W4 拆分段先例）。
// #538：runWorkbenchCommand 原 397 行单体（file-budget 闸 >200 行）按动作族
// 拆为三个文件内自由函数（查询族/编辑族/生命周期族）+ 入口的参数解析/分派/
// 公共消息逻辑。私有成员与私有嵌套回调类型（WorkbenchShowFn 等在
// paleomainwindow.h 私有段，自由函数无法命名）经 WorkbenchCmdDeps 显式传入
// （回调类型展开为等价的 std::function 特化）——#538 只许改三个 workbench
// .cpp，不在头文件加成员声明。行为守恒：分支内 early-return 经
// WbCmdStep::Return 上传入口，公共消息逻辑保持单份。
#include "paleomainwindow.h"
#include "workbenchcommandregistry.h"

#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/qgislayerservice.h"
#include "../services/errorhub.h"
#include "../services/paleotaskservice.h" // QPointer<PaleoTask>::running 需完整类型
#include "../services/previewdoc.h"
#include "../workflow/constraintimport.h" // ConstraintImportFailure 机器可判定原因码
#include "../workflow/mappingworkbench.h"
#include "../domain/facieshierarchy.h" // #754：pinFaciesLevel 文案携带层级名
#include "datapreview/datapreviewtabs.h"
#include "notifications/paleonotify.h"
#include "pages/constraintpage.h"
#include "pages/datapage.h"
#include "pages/mappingreferencepanel.h"
#include "pages/mappingworkbenchpage.h"
#include "edittools/editingtoolbar.h"  // 命令分支用 PaleoEditingToolbar 完整类型
#include "pages/wellpredictionpanel.h"

#include <QApplication>
#include <QDialog>
#include <QFileDialog>
#include <QHash>
#include <QInputDialog>
#include <QPointer>
#include <QStatusBar>
#include <QStringList>
#include <QVariantMap>
#include <algorithm>
#include <functional>
#include <memory>
#include <qgsmapcanvas.h>
#include <qgsmaplayer.h>
#include <qgsvectorlayer.h>

namespace {
// review-fix M5：工作台命令失败除页内/状态栏 10s 提示外，同步入账 ErrorHub
// 错误历史（historyOnly：已就地呈现，不再弹卡），用户移开视线后仍可在
// 状态栏错误胶囊里找回。未装配全局 hub（单测）时只剩 qWarning。
void reportWorkbenchFailure(const QString &action, const QString &message)
{
  if (ErrorHub *hub = ErrorHub::global())
    hub->raise(ErrorHub::Level::Warning, QStringLiteral("workbench"),
               PaleoMainWindow::tr("工作台操作失败：%1").arg(action), message,
               QString(), false, true);
  else
    qWarning("workbench command '%s' failed: %s", qPrintable(action),
             qPrintable(message));
}
} // namespace

namespace {

// 同步长操作的等待光标 RAII（构造即设、析构即还；异常路径不卡在等待态，R2-07②）。
struct WaitCursorGuard
{
  WaitCursorGuard() { QApplication::setOverrideCursor(Qt::WaitCursor); }
  ~WaitCursorGuard() { QApplication::restoreOverrideCursor(); }
  WaitCursorGuard(const WaitCursorGuard &) = delete;
  WaitCursorGuard &operator=(const WaitCursorGuard &) = delete;
};

// 分支结局：Skip=本族不认领该 action（交给下一族）；Handled=认领且落入入口
// 的公共消息逻辑；Return=分支已自行收尾（原分支内 early-return，直接返回，
// 不走消息逻辑）。
enum class WbCmdStep { Skip, Handled, Return };

// 命令分发链的显式依赖面：私有成员指针 + 私有段回调类型的 std::function
// 等价展开；action/horizon/layer/inputs/params 是入口从 signal payload 解出
// 的每调用值，原样透传（不落成员）。
struct WorkbenchCmdDeps {
  PaleoMainWindow *self = nullptr;
  MappingWorkbench *workbench = nullptr;
  MappingWorkbenchPage *page = nullptr;
  PaleoEditingToolbar *edit = nullptr;
  ConstraintPage *constraint = nullptr;
  WellPredictionPanel *wellPanel = nullptr;
  QDockWidget *wellDock = nullptr;
  QgisCanvasController *canvasCtl = nullptr;
  QgisLayerService *layerSvc = nullptr;
  DataPreviewTabs *previewTabs = nullptr;
  PreviewDocService *previewDoc = nullptr;
  QPointer<PaleoTask> factorTask;
  QVariantMap *lastImpact = nullptr;  // G09 影响分析报告（受控重算入口用）
  std::function<void(const QString &)> legend;
  std::function<bool(const QString &, QString *)> show;
  std::function<bool(const QString &, QString *)> compare;
  std::function<void(const QString &)> openWells;
  std::function<bool(const QString &)> beginEdit;
  std::shared_ptr<QHash<QString, QString>> shown;
};

// 查询族：predict / wellattributes / wellformations / cancel / show / graph /
// catalog / reverseQuery / impactAnalysis / compareRecompute / recompute。
// statusDetail：分支产出的量化详情成功文案（像元数、报告路径、逐条 reason）。
// 分支只填文本不自行显示，由入口统一张贴（20s）——此前分支当场 showMessage
// 后，入口尾部又以 10s 通用文案「操作完成，图件与版本已更新。」覆盖状态栏，
// 量化结果用户实际不可见。
WbCmdStep runWorkbenchQueryFamily(const WorkbenchCmdDeps &d,
                                  const QString &action, const QString &h,
                                  const QString &id,
                                  const QStringList &inputs,
                                  const QVariantMap &p, bool &ok,
                                  QString &error, QString &statusDetail) {
  if (action == "predict")
    ok = d.workbench->predict(h, p.value("kind").toString(), inputs,
                              &error, p.value("horizon_file").toString());
  else if (action == "wellattributes") {
    const auto layer = d.workbench->openWellAttributes(&error);
    ok = !layer.isEmpty();
    if (ok) d.self->showAttributeTable(layer);
  }
  else if (action == "wellformations") {
    const auto layer = d.workbench->openWellFormations(&error);
    ok = !layer.isEmpty();
    if (ok) d.self->showAttributeTable(layer);
  }
  else if (action == "cancel")
    d.workbench->cancelPrediction();
  else if (action == "show")
    ok = d.show(id, &error);
  else if (action == "graph") {
    const auto version = d.workbench->versionForLayer(id);
    if (version.id.isEmpty()) { ok = false; error = PaleoMainWindow::tr("所选图件尚未登记到数据管理"); }
    else d.self->openDataGraph(version.assetId, version.id);
  }
  else if (action == "catalog") {
    const auto version = d.workbench->versionForLayer(id);
    auto *dataPage = d.self->findChild<DataPage *>();
    if (!dataPage || version.id.isEmpty()) {
      ok = false;
      error = PaleoMainWindow::tr("所选图件尚未登记到数据管理");
    } else {
      d.self->showPage("data");
      if (d.previewTabs)
        d.previewTabs->openVersion(version.id);
      dataPage->focusVersion(version.assetId, version.id);
    }
  } else if (action == "reverseQuery") {
    QVariantMap meta;
    const auto rows = d.workbench->evidenceReverseQuery(
        p.value("reverse_kind").toString(),
        p.value("reverse_argument").toString(), &error, &meta);
    ok = error.isEmpty();
    d.page->setReverseResults(rows, ok ? QString() : error, meta);
    error.clear();
  } else if (action == "impactAnalysis") {
    // X13：版本对选择在功能层（同资产上一版 = 变化前），视图只传图件。
    // 重放为同步长操作（≤64 产品 × 2 × 400 万像元）：等待光标 +
    // 状态栏提示（DESIGN.md Motion「>1s 显示进度条」的同步路径
    // 最小满足；异步任务化在 full-audit 台账记为剩余工作）。
    // 光标 RAII：分析抛异常（bad_alloc 族）也不卡死在等待态（R2-07②）。
    d.self->statusBar()->showMessage(PaleoMainWindow::tr("正在分析影响（重放冻结输入）…"), 0);
    const WaitCursorGuard waitCursor;
    *d.lastImpact = d.workbench->analyzeLayerVersionImpact(id, h, &error);
    ok = !d.lastImpact->isEmpty();
    d.page->setImpactAvailable(ok);
    if (ok) {
      const auto determined = d.lastImpact->value("determined").toList();
      qint64 changedCells = 0;
      for (const auto &value : determined)
        changedCells += value.toMap().value("changed_cells").toLongLong();
      const auto reportPath =
          d.lastImpact->value("report_path").toString();
      statusDetail =
          reportPath.isEmpty()
              ? PaleoMainWindow::tr("影响分析完成：直接依赖 %1 项，潜在影响 %2 项，可确定"
                   "变化 %3 像元（报告未能保存到工程目录）")
                    .arg(d.lastImpact->value("direct_products").toList().size())
                    .arg(d.lastImpact->value("potential_products").toList().size())
                    .arg(changedCells)
              : PaleoMainWindow::tr("影响分析完成：直接依赖 %1 项，潜在影响 %2 项，可确定变化 "
                   "%3 像元（详见 %4）")
                    .arg(d.lastImpact->value("direct_products").toList().size())
                    .arg(d.lastImpact->value("potential_products").toList().size())
                    .arg(changedCells)
                    .arg(reportPath);
    }
  } else if (action == "compareRecompute") {
    // X13：受控重算成果 vs 其替代的旧版本（像元级 diff + 报告落盘）。
    const auto report =
        d.workbench->compareWithRecomputeSource(id, &error);
    ok = !report.isEmpty();
    if (ok) {
      auto matrix = report.value("code_matrix").toList();
      std::sort(matrix.begin(), matrix.end(),
                [](const QVariant &a, const QVariant &b) {
                  return a.toMap().value("cells").toLongLong() >
                         b.toMap().value("cells").toLongLong();
                });
      QStringList top;
      for (const auto &value : matrix.mid(0, 3))
        top << PaleoMainWindow::tr("%1：%2 像元")
                   .arg(value.toMap().value("transition").toString())
                   .arg(value.toMap().value("cells").toLongLong());
      statusDetail =
          PaleoMainWindow::tr("重算对比：变化 %1/%2 像元；主要转移 %3（详见 %4）")
              .arg(report.value("cells_changed").toLongLong())
              .arg(report.value("total_cells").toLongLong())
              .arg(top.isEmpty() ? PaleoMainWindow::tr("无") : top.join("；"))
              .arg(report.value("report_path").toString());
    }
  } else if (action == "recompute") {
    if (d.lastImpact->isEmpty()) {
      ok = false;
      error = PaleoMainWindow::tr("请先运行影响分析，再按其结果受控重算");
    } else {
      const auto plan = d.workbench->recomputePlan(*d.lastImpact, &error);
      ok = !plan.isEmpty() && plan.value("status").toString() != QStringLiteral("empty");
      if (ok) {
        const WaitCursorGuard waitCursor;
        const auto execution = d.workbench->executeRecompute(plan, &error);
        // 执行整体失败（工程未开/报告过期）返回空图：不得伪报「已启动 0/0 项」。
        if (execution.isEmpty()) {
          ok = false;
          if (error.isEmpty())
            error = PaleoMainWindow::tr("受控重算未能执行，请检查工程状态后重试");
        } else {
          const auto results = execution.value("results").toList();
          int started = 0;
          QStringList notes;
          for (const auto &value : results) {
            const auto row = value.toMap();
            if (row.value("ok").toBool())
              ++started;
            // 如实带上逐条结论（跳过/过期/已完成的 reason 都在结果行）。
            const auto note = row.value("reason").toString().isEmpty()
                                  ? row.value("error").toString()
                                  : row.value("reason").toString();
            if (!note.isEmpty() && notes.size() < 3)
              notes << note;
          }
          statusDetail =
              PaleoMainWindow::tr("受控重算已启动 %1/%2 项：%3新版本由任务中心生成，历史版本"
                 "与已确认解释保持不变")
                  .arg(started)
                  .arg(results.size())
                  .arg(notes.isEmpty() ? QString()
                                       : notes.join("；") + QStringLiteral("。"));
        }
      } else if (error.isEmpty())
        error = PaleoMainWindow::tr("影响报告中没有可重算的综合编图成果");
    }
  } else
    return WbCmdStep::Skip;
  return WbCmdStep::Handled;
}

// 编辑族：labels / welltracks / selectHierarchy / displayLevel / pinFaciesLevel /
// featureDisplayLevel / 证据四动作 / assignFacies / referencePanel /
// references / compare / consistency / polygonize / copy / save。
WbCmdStep runWorkbenchEditFamily(const WorkbenchCmdDeps &d,
                                 const QString &action, const QString &id,
                                 const QStringList &inputs,
                                 const QVariantMap &p, bool &ok,
                                 QString &error) {
  if (action == "labels")
    ok = d.workbench->setLabelMode(id, p.value("label_mode").toInt(),
                                   &error);
  else if (action == "welltracks")
    d.openWells(id);
  else if (action == "selectHierarchy") {
    ok = d.workbench->selectHierarchyMembers(
        id, p.value("edit_level").toString(), &error);
  } else if (action == "displayLevel") {
    ok = d.workbench->setDisplayMode(
        id, p.value("display_mode").toString(), &error);
  } else if (action == "pinFaciesLevel") {
    // 相类固定显示层级：图层属性，非要素编辑，无需编辑会话。
    ok = d.workbench->setPinnedLevel(id, p.value("pin_key").toString(),
                                     p.value("display_mode").toString(), &error);
  } else if (action == "featureDisplayLevel") {
    // #522（显示锁定）：要素级显示层级——需编辑会话（beginEdit 同族守卫）。
    if (!d.beginEdit(id)) {
      ok = false;
      error = PaleoMainWindow::tr("请先结束其他图层的编辑");
    } else {
      ok = d.workbench->setFeatureDisplayMode(
          id, d.workbench->selectedFeatures(id), p.value("display_mode").toString(), &error);
    }
  } else if (action == "addEvidence" || action == "removeEvidence" || action == "updateEvidence" || action == "verifyEvidence" || action == "reviewInterpretation") {
    if (!d.beginEdit(id)) {
      ok = false;
      error = PaleoMainWindow::tr("请先结束其他图层的编辑");
    } else if (action == "addEvidence")
      ok = d.workbench->addEvidence(id, d.workbench->selectedFeatures(id),
                                    p, &error);
    else {
      auto parameters = p;
      if (action == "removeEvidence" && parameters.value("reason").toString().trimmed().isEmpty()) parameters.insert("reason", PaleoMainWindow::tr("解除错误的证据关联"));
      const auto change = action == "removeEvidence" ? QStringLiteral("unlink") : action == "updateEvidence" ? QStringLiteral("update_evidence") :
          action == "verifyEvidence" ? QStringLiteral("verify_evidence") : QStringLiteral("review");
      ok = d.workbench->changeEvidence(id, d.workbench->selectedFeatures(id), change, parameters, &error);
    }
  } else if (action == "assignFacies") {
    auto *v =
        qobject_cast<QgsVectorLayer *>(d.layerSvc->instantiate(id));
    if (!v || v->selectedFeatureIds().isEmpty()) {
      ok = false;
      error = PaleoMainWindow::tr("请先用画布选择工具选中要素");
    } else if (!d.beginEdit(id)) {
      ok = false;
      error = PaleoMainWindow::tr("请先结束其他图层的编辑");
    } else
      ok = d.workbench->assignHierarchy(
          id, v->selectedFeatureIds().values(), p.value("code").toInt(),
          p.value("edit_level", "micro_facies").toString(), &error);
  } else if (action == "referencePanel") {
    ok = d.compare(QString(), &error);
  } else if (action == "references") {
    for (const auto &ref : inputs) {
      if (!d.compare(ref, &error)) {
        ok = false;
        break;
      }
    }
  } else if (action == "compare")
    ok = d.compare(id, &error);
  else if (action == "consistency") {
    ok = d.workbench->startFaciesConsistency(id, &error);
  } else if (action == "polygonize")
    ok = d.workbench->startPolygonize(id, &error);
  else if (action == "copy") {
    const auto draft = d.workbench->copyForEditing(id, inputs, &error, d.page->mode() == "compose");
    ok = !draft.isEmpty();
    if (ok && d.edit) {
      // productReady 已显示工作副本；重复 show 会把编辑中的局部视图缩回全图。
      ok = d.beginEdit(draft);
      if (!ok)
        error = PaleoMainWindow::tr("副本已创建，请先结束其他图层的编辑");
    }
  } else if (action == "save") {
    auto *vector =
        qobject_cast<QgsVectorLayer *>(d.layerSvc->layer(id));
    if (vector && vector->isEditable()) {
      if (d.edit && d.edit->currentLayer() == vector) {
        if (d.edit->saveEditing())
          return WbCmdStep::Return;
        ok = false;
        error = PaleoMainWindow::tr("画布编辑未能提交，请检查编辑提示");
      } else {
        ok = false;
        error = PaleoMainWindow::tr("请先保存正在编辑的图层");
      }
    } else
      ok = d.workbench->saveEditingVersion(id, &error);
  } else
    return WbCmdStep::Skip;
  return WbCmdStep::Handled;
}

// 生命周期族：delete / import / draw / factor / contours / compose / schema。
WbCmdStep runWorkbenchLifecycleFamily(const WorkbenchCmdDeps &d,
                                      const QString &action, const QString &h,
                                      const QString &id,
                                      const QStringList &inputs,
                                      const QVariantMap &p, bool &ok,
                                      QString &error, bool &declarationOnly) {
  if (action == "newMap") {
    const auto draft = d.workbench->createBlankMap(h, &error);
    ok = !draft.isEmpty();
    if (ok) {
      ok = d.show(draft, &error) && d.beginEdit(draft);
      if (!ok && error.isEmpty()) error = PaleoMainWindow::tr("空白图已创建，请先结束其他图层的编辑");
    }
  } else if (action == "delete") {
    const auto title = d.workbench->declaration(id).title;
    const auto products = d.workbench->removalProducts(id, &error);
    declarationOnly = products.size() == 1 && products.first().toMap().value("declaration_only").toBool();
    QStringList entries, versionIds;
    for (const auto &value : products) {
      const auto product = value.toMap();
      entries << (declarationOnly ? product.value("title").toString()
                     : PaleoMainWindow::tr("• %1（%2 个版本）").arg(product.value("title").toString())
                           .arg(product.value("versions").toInt()));
      if (d.previewDoc && d.previewDoc->catalog())
        for (const auto &version : d.previewDoc->catalog()->versionsForAsset(
                 product.value("asset_id").toString())) versionIds << version.id;
    }
    if (products.isEmpty()) {
      ok = false;
    } else if (!PaleoNotify::ask(
            d.self, PaleoMainWindow::tr("删除图件"),
            declarationOnly
                ? PaleoMainWindow::tr("从工程中移除未登记图件「%1」？\n"
                     "仅移除图件列表和地图中的引用，保留原始文件及其他图层。")
                      .arg(title.isEmpty() ? id : title)
                : PaleoMainWindow::tr("永久删除图件「%1」？\n"
               "以下图件（含关联派生图件）的全部版本、数据管理记录与受管文件将一并删除，不可恢复：\n%2")
                .arg(title.isEmpty() ? id : title, entries.join("\n")),
            PaleoNotify::AskButtons::OkCancel,
            PaleoNotify::AskDefault::Reject,
            PaleoNotify::AskIcon::Warning))
      return WbCmdStep::Return;
    else if (d.workbench->removalProducts(id, &error) != products) {
      ok = false;
      if (error.isEmpty()) error = PaleoMainWindow::tr("图件或来源记录已变化，请重新确认删除");
    }
    else {
      auto *deleteCanvas = d.canvasCtl->canvas();
      const bool wasFrozen = deleteCanvas->isFrozen();
      deleteCanvas->freeze(true);
      ok = d.workbench->removeProduct(id, &error, true);
      deleteCanvas->freeze(wasFrozen);
      if (!wasFrozen) deleteCanvas->refresh();
      if (ok) {
        for (auto *view : d.self->findChildren<MappingReferencePanel *>())
          if (versionIds.contains(view->property("paleo.referenceVersion").toString()))
            view->clearReference(PaleoMainWindow::tr("所选证据版本已删除，请选择其他证据。"));
        if (!d.wellPanel->layerId().isEmpty() &&
            d.workbench->versionForLayer(d.wellPanel->layerId()).id.isEmpty()) {
          d.wellPanel->clear();
          d.wellDock->hide();
        }
        for (auto it = d.shown->begin(); it != d.shown->end();)
          if (d.workbench->declaration(it.value()).layerId.isEmpty()) it = d.shown->erase(it);
          else ++it;
        auto *canvas = d.canvasCtl->canvas();
        d.legend(canvas->currentLayer()
                     ? canvas->currentLayer()->customProperty("paleoLayerId").toString() : QString());
        canvas->refresh();
      }
    }
  } else if (action == "import") {
    const auto path = QFileDialog::getOpenFileName(
        d.self, PaleoMainWindow::tr("导入局部米制约束线"), QString(),
        PaleoMainWindow::tr("矢量文件 (*.gpkg *.geojson *.json *.shp)"));
    if (path.isEmpty())
      return WbCmdStep::Return;
    paleo::ConstraintImportFailure failure = paleo::ConstraintImportFailure::None;
    ok = d.workbench->importConstraints(h, path, QStringLiteral("auto"),
                                        &error, &failure);
    // auto 判定失败 → 询问用户角色后重试（方向线 / 打断线）。
    // 按功能层原因码分支：此前匹配 error 里的 tr() 译文，而该译文来自
    // constraintimport 的 QObject::tr 上下文、与本文件的 PaleoMainWindow::tr
    // 上下文不同，启用翻译后两者不再保持子串包含关系，对话框会静默失配不弹。
    if (!ok && failure == paleo::ConstraintImportFailure::RoleUndetermined) {
      const QString choice = QInputDialog::getItem(
          d.self, PaleoMainWindow::tr("约束线类型"), PaleoMainWindow::tr("无法自动判断，请选择约束线类型："),
          {PaleoMainWindow::tr("方向线"), PaleoMainWindow::tr("打断线")}, 0, false, &ok);
      if (ok)
        ok = d.workbench->importConstraints(
            h, path,
            choice == PaleoMainWindow::tr("方向线") ? QStringLiteral("direction")
                                  : QStringLiteral("barrier"),
            &error);
      else
        return WbCmdStep::Return;
    }
  } else if (action == "draw") {
    if (d.constraint) {
      d.constraint->drawConstraintRequested(h, "line", -1);
      d.page->showMessage(
          PaleoMainWindow::tr("在画布逐点绘制约束线，右键结束；Esc 取消。"));
      return WbCmdStep::Return;
    } else {
      ok = false;
      error = PaleoMainWindow::tr("约束绘制工具不可用");
    }
  } else if (action == "factor") {
    if (d.factorTask && d.factorTask->running()) {
      ok = false;
      error = PaleoMainWindow::tr("已有单因素计算在进行");
    } else
      ok = d.workbench->generateFactor(h, p.value("factor").toString(), p,
                                       &error);
  }
  else if (action == "contours")
    ok = d.workbench->generateContours(
        h, id, p.value("interval").toDouble(), &error);
  else if (action == "compose")
    ok = d.workbench->startCompose(h, inputs, p, &error);
  else if (action == "schema") {
    ok = d.workbench->saveFacies(h, p.value("facies").toList(), &error);
    if (ok) d.page->markFaciesSaved();
    auto *layer = d.canvasCtl->canvas()->currentLayer();
    d.legend(layer ? layer->customProperty("paleoLayerId").toString()
                   : QString());
  } else
    return WbCmdStep::Skip;
  return WbCmdStep::Handled;
}

}  // namespace

void PaleoMainWindow::runWorkbenchCommand(
    MappingWorkbench *workbench, MappingWorkbenchPage *page,
    PaleoEditingToolbar *edit, ConstraintPage *constraint,
    WellPredictionPanel *wellPanel, QDockWidget *wellDock,
    const WorkbenchLegendFn &legend, const WorkbenchShowFn &show,
    const WorkbenchShowFn &compare, const WorkbenchOpenWellsFn &openWells,
    const WorkbenchBeginEditFn &beginEdit,
    const std::shared_ptr<QHash<QString, QString>> &shown,
    const QString &action, const QVariantMap &p) {
  const WorkbenchShowFn exportView = [this](const QString &id, QString *error) {
    auto *layer = m_layerSvc->layer(id);
    if (!layer || !m_canvasCtl->canvas()->layers().contains(layer)) {
      if (error) *error = tr("请先显示所选图件，再输出当前地图范围和显示层级");
      return false;
    }
    return true; // 保留当前范围、比例尺和三级显示，不在导出前重新缩放。
  };
  QString error;
  bool ok = true;
  // 分支量化详情成功文案（影响分析/重算对比/受控重算）：见查询族 statusDetail。
  QString statusDetail;
  // review-fix H1：未登记动作显式失败，不再落到默认成功文案。
  if (!paleo::workbenchcmd::isKnownAction(action)) {
    qWarning("runWorkbenchCommand: unknown action '%s'", qPrintable(action));
    const QString message = tr("未知操作：%1").arg(action);
    page->showMessage(message);
    statusBar()->showMessage(message, 10000);
    reportWorkbenchFailure(action, message);
    return;
  }
  if (runMappingEditFlow(page, action, p, action == "exportMap" ? exportView : show)) return;
  bool declarationOnly = false;
  const auto h = p.value("horizon").toString(),
             id = p.value("layer").toString();
  const auto inputs = p.value("inputs").toStringList();
  // #538：依赖面显式化（私有成员/私有段回调类型经结构传入，见上方注释），
  // 动作族分派保持原 if-else 链「至多一个分支命中」的语义。
  WorkbenchCmdDeps d;
  d.self = this;
  d.workbench = workbench;
  d.page = page;
  d.edit = edit;
  d.constraint = constraint;
  d.wellPanel = wellPanel;
  d.wellDock = wellDock;
  d.canvasCtl = m_canvasCtl;
  d.layerSvc = m_layerSvc;
  d.previewTabs = m_previewTabs;
  d.previewDoc = m_previewDoc;
  d.factorTask = m_factorTask;
  d.lastImpact = &m_lastImpact;
  d.legend = legend;
  d.show = show;
  d.compare = compare;
  d.openWells = openWells;
  d.beginEdit = beginEdit;
  d.shown = shown;
  WbCmdStep step = runWorkbenchQueryFamily(d, action, h, id, inputs, p, ok,
                                           error, statusDetail);
  if (step == WbCmdStep::Skip)
    step = runWorkbenchEditFamily(d, action, id, inputs, p, ok, error);
  if (step == WbCmdStep::Skip)
    step = runWorkbenchLifecycleFamily(d, action, h, id, inputs, p, ok,
                                       error, declarationOnly);
  if (step == WbCmdStep::Return)
    return;
  if (step == WbCmdStep::Skip) {
    // 已登记却无分支受理（如编辑流前置条件不满足）：按失败回报。
    qWarning("runWorkbenchCommand: action '%s' not handled", qPrintable(action));
    ok = false;
    error = tr("操作未执行：%1 当前不可用").arg(action);
  }
  QString message;
  // 分支已产出量化详情（像元数/报告路径/逐条 reason）时直接复用为成功文案：
  // 走下方通用分支会让 10s「操作完成，图件与版本已更新。」盖掉详情，
  // 这类命令唯一的量化结果反馈随之不可见。
  const bool detailed = ok && !statusDetail.isEmpty();
  if (!ok)
    message = error.isEmpty() ? tr("操作未完成，请检查输入。") : error;
  else if (detailed)
    message = statusDetail;
  else if (action == "predict")
    message = tr("预测已在后台执行，进度见底栏任务中心；完成后自动登记并显示图件。");
  else if (action == "polygonize")
    message = d.workbench->polygonizeBusy()
                  ? tr("相栅格转相面已在后台执行，进度见底栏任务中心；完成后自动登记并显示在对应地层组。")
                  : tr("相栅格已转为相面。");
  else if (action == "compose" || action == "factor" || action == "contours")
    message =
        tr("任务已提交，进度和取消操作见任务中心；完成后显示结果。");
  else if (action == "wellattributes")
    message = tr("已打开测井矢量属性表；保存后更新井道与单因素输入。");
  else if (action == "wellformations")
    message = tr("已打开分层沉积相属性表；每口井每个地层单元（组/段）一行。解释微相用于维护当前解释，预测微相保留模型结果；厚度、占比和覆盖率来自相段汇总，直接改名不重算。");
  else if (action == "cancel")
    message = tr("已请求取消预测。");
  else if (action == "show")
    message = tr("已在画布显示并定位选中图件。");
  else if (action == "compare" || action == "references" || action == "referencePanel")
    message =
        tr("已打开证据联动面板；可选择参考图件、添加并排视图，并开关范围与光标联动。");
  else if (action == "catalog")
    message = tr("已在数据管理中定位图件的保存版本和来源谱系。");
  else if (action == "displayLevel")
    message = tr("相图显示层级、标注与图例已更新。");
  else if (action == "pinFaciesLevel") {
    // #754：固定相显示层级单独文案并带上层级名；display_mode=auto 为解除固定。
    const QString mode = p.value("display_mode").toString();
    message = mode == QStringLiteral("auto")
                  ? tr("已解除相显示层级固定，恢复跟随图层。")
                  : tr("已固定相显示层级：%1。").arg(FaciesHierarchy::title(mode));
  }
  else if (action == "addEvidence" || action == "removeEvidence")
    message = tr("解释证据已更新，可撤销；保存图件版本后持久化。");
  else if (action == "labels")
    message = tr("选中图件的标注已更新。");
  else if (action == "schema")
    message = tr("当前层位相分类已保存，已有图件保留原分类。");
  else if (action == "draw")
    message = tr("请在画布逐点绘制约束线，右键结束，Esc 取消。");
  else if (action == "assignFacies")
    message = tr("选中要素的相类别已更新，请保存编辑以登记新版本。");
  else if (action == "welltracks")
    message = tr("已打开井道与预测相修订面板。");
  else if (action == "consistency")
    // #751：同步路径当场出报告、后台路径转任务中心——按 queued 标志分流，
    // 两条文案都不再是死分支（此前 consistency 被 :565 先捕获，本分支永不达）。
    message = d.workbench->consistencyQueued()
                  ? tr("任务已提交，进度和取消操作见任务中心；完成后显示结果。")
                  : tr("已生成地震相与测井相一致性报告并留档；双击逐井行可在画布定位。");
  else if (action == "delete")
    message = declarationOnly ? tr("已从工程移除未登记图件，原始文件已保留。")
                              : tr("已删除图件及版本记录，受管文件正在后台清理。");
  else
    message = tr("操作完成，图件与版本已更新。");
  page->showMessage(message);
  statusBar()->showMessage(message, detailed ? 20000 : 10000);
  if (!ok && action == "delete")
    PaleoNotify::warning(this, tr("图件未删除"), message);
  else if (!ok)
    reportWorkbenchFailure(action, message);
}
