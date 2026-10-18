// 层：视图
// paleomainwindow_workbench_commands — attachWorkbench 命令分发链（#473）：
// MappingWorkbenchPage::commandRequested 的动作分支表，逐字平移自
// paleomainwindow_workbench.cpp 原 lambda 体（仅整体去一层缩进），行为守恒。
// 参数即原 lambda 捕获面（this 之外全部显式化）；私有成员访问经
// paleomainwindow.h 的成员函数声明（同 W4 拆分段先例）。
#include "paleomainwindow.h"

#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/qgislayerservice.h"
#include "../services/paleotaskservice.h" // QPointer<PaleoTask>::running 需完整类型
#include "../services/previewdoc.h"
#include "../workflow/mappingworkbench.h"
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
#include <QInputDialog>
#include <QStatusBar>
#include <algorithm>
#include <qgsmapcanvas.h>
#include <qgsmaplayer.h>
#include <qgsvectorlayer.h>

void PaleoMainWindow::runWorkbenchCommand(
    MappingWorkbench *workbench, MappingWorkbenchPage *page,
    PaleoEditingToolbar *edit, ConstraintPage *constraint,
    WellPredictionPanel *wellPanel, QDockWidget *wellDock,
    const WorkbenchLegendFn &legend, const WorkbenchShowFn &show,
    const WorkbenchShowFn &compare, const WorkbenchOpenWellsFn &openWells,
    const WorkbenchBeginEditFn &beginEdit,
    const std::shared_ptr<QHash<QString, QString>> &shown,
    const QString &action, const QVariantMap &p) {
  QString error;
  bool ok = true;
  const auto h = p.value("horizon").toString(),
             id = p.value("layer").toString();
  const auto inputs = p.value("inputs").toStringList();
  if (action == "predict")
    ok = workbench->predict(h, p.value("kind").toString(), inputs,
                            &error, p.value("horizon_file").toString());
  else if (action == "wellattributes") {
    const auto layer = workbench->openWellAttributes(&error);
    ok = !layer.isEmpty();
    if (ok) showAttributeTable(layer);
  }
  else if (action == "wellformations") {
    const auto layer = workbench->openWellFormations(&error);
    ok = !layer.isEmpty();
    if (ok) showAttributeTable(layer);
  }
  else if (action == "cancel")
    workbench->cancelPrediction();
  else if (action == "show")
    ok = show(id, &error);
  else if (action == "graph") {
    const auto version = workbench->versionForLayer(id);
    if (version.id.isEmpty()) { ok = false; error = tr("所选图件尚未登记到数据管理"); }
    else openDataGraph(version.assetId, version.id);
  }
  else if (action == "catalog") {
    const auto version = workbench->versionForLayer(id);
    auto *dataPage = findChild<DataPage *>();
    if (!dataPage || version.id.isEmpty()) {
      ok = false;
      error = tr("所选图件尚未登记到数据管理");
    } else {
      showPage("data");
      if (m_previewTabs)
        m_previewTabs->openVersion(version.id);
      dataPage->focusVersion(version.assetId, version.id);
    }
  } else if (action == "reverseQuery") {
    QVariantMap meta;
    const auto rows = workbench->evidenceReverseQuery(
        p.value("reverse_kind").toString(),
        p.value("reverse_argument").toString(), &error, &meta);
    ok = error.isEmpty();
    page->setReverseResults(rows, ok ? QString() : error, meta);
    error.clear();
  } else if (action == "impactAnalysis") {
    // X13：版本对选择在功能层（同资产上一版 = 变化前），视图只传图件。
    // 重放为同步长操作（≤64 产品 × 2 × 400 万像元）：等待光标 +
    // 状态栏提示（DESIGN.md Motion「>1s 显示进度条」的同步路径
    // 最小满足；异步任务化在 full-audit 台账记为剩余工作）。
    // 光标 RAII：分析抛异常（bad_alloc 族）也不卡死在等待态（R2-07②）。
    statusBar()->showMessage(tr("正在分析影响（重放冻结输入）…"), 0);
    const struct WaitCursorGuard
    {
      ~WaitCursorGuard() { QApplication::restoreOverrideCursor(); }
    } waitCursor;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    m_lastImpact = workbench->analyzeLayerVersionImpact(id, h, &error);
    ok = !m_lastImpact.isEmpty();
    page->setImpactAvailable(ok);
    if (ok) {
      const auto determined = m_lastImpact.value("determined").toList();
      qint64 changedCells = 0;
      for (const auto &value : determined)
        changedCells += value.toMap().value("changed_cells").toLongLong();
      const auto reportPath =
          m_lastImpact.value("report_path").toString();
      statusBar()->showMessage(
          reportPath.isEmpty()
              ? tr("影响分析完成：直接依赖 %1 项，潜在影响 %2 项，可确定"
                   "变化 %3 像元（报告未能保存到工程目录）")
                    .arg(m_lastImpact.value("direct_products").toList().size())
                    .arg(m_lastImpact.value("potential_products").toList().size())
                    .arg(changedCells)
              : tr("影响分析完成：直接依赖 %1 项，潜在影响 %2 项，可确定变化 "
                   "%3 像元（详见 %4）")
                    .arg(m_lastImpact.value("direct_products").toList().size())
                    .arg(m_lastImpact.value("potential_products").toList().size())
                    .arg(changedCells)
                    .arg(reportPath),
          20000);
    }
  } else if (action == "compareRecompute") {
    // X13：受控重算成果 vs 其替代的旧版本（像元级 diff + 报告落盘）。
    const auto report =
        workbench->compareWithRecomputeSource(id, &error);
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
        top << tr("%1：%2 像元")
                   .arg(value.toMap().value("transition").toString())
                   .arg(value.toMap().value("cells").toLongLong());
      statusBar()->showMessage(
          tr("重算对比：变化 %1/%2 像元；主要转移 %3（详见 %4）")
              .arg(report.value("cells_changed").toLongLong())
              .arg(report.value("total_cells").toLongLong())
              .arg(top.isEmpty() ? tr("无") : top.join("；"))
              .arg(report.value("report_path").toString()),
          20000);
    }
  } else if (action == "recompute") {
    if (m_lastImpact.isEmpty()) {
      ok = false;
      error = tr("请先运行影响分析，再按其结果受控重算");
    } else {
      const auto plan = workbench->recomputePlan(m_lastImpact, &error);
      ok = !plan.isEmpty() && plan.value("status").toString() != QStringLiteral("empty");
      if (ok) {
        const struct WaitCursorGuard
        {
          ~WaitCursorGuard() { QApplication::restoreOverrideCursor(); }
        } waitCursor;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const auto execution = workbench->executeRecompute(plan, &error);
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
        statusBar()->showMessage(
            tr("受控重算已启动 %1/%2 项：%3新版本由任务中心生成，历史版本"
               "与已确认解释保持不变")
                .arg(started)
                .arg(results.size())
                .arg(notes.isEmpty() ? QString()
                                     : notes.join("；") + QStringLiteral("。")),
            20000);
      } else if (error.isEmpty())
        error = tr("影响报告中没有可重算的综合编图成果");
    }
  } else if (action == "labels")
    ok = workbench->setLabelMode(id, p.value("label_mode").toInt(),
                                 &error);
  else if (action == "welltracks")
    openWells(id);
  else if (action == "selectHierarchy") {
    ok = workbench->selectHierarchyMembers(
        id, p.value("edit_level").toString(), &error);
  } else if (action == "displayLevel") {
    ok = workbench->setDisplayMode(
        id, p.value("display_mode").toString(), &error);
  } else if (action == "addEvidence" || action == "removeEvidence" || action == "updateEvidence" || action == "verifyEvidence" || action == "reviewInterpretation") {
    if (!beginEdit(id)) {
      ok = false;
      error = tr("请先结束其他图层的编辑");
    } else if (action == "addEvidence")
      ok = workbench->addEvidence(id, workbench->selectedFeatures(id),
                                  p, &error);
    else {
      auto parameters = p;
      if (action == "removeEvidence" && parameters.value("reason").toString().trimmed().isEmpty()) parameters.insert("reason", tr("解除错误的证据关联"));
      const auto change = action == "removeEvidence" ? QStringLiteral("unlink") : action == "updateEvidence" ? QStringLiteral("update_evidence") :
          action == "verifyEvidence" ? QStringLiteral("verify_evidence") : QStringLiteral("review");
      ok = workbench->changeEvidence(id, workbench->selectedFeatures(id), change, parameters, &error);
    }
  } else if (action == "assignFacies") {
    auto *v =
        qobject_cast<QgsVectorLayer *>(m_layerSvc->instantiate(id));
    if (!v || v->selectedFeatureIds().isEmpty()) {
      ok = false;
      error = tr("请先用画布选择工具选中要素");
    } else if (!beginEdit(id)) {
      ok = false;
      error = tr("请先结束其他图层的编辑");
    } else
      ok = workbench->assignHierarchy(
          id, v->selectedFeatureIds().values(), p.value("code").toInt(),
          p.value("edit_level", "micro_facies").toString(), &error);
  } else if (action == "referencePanel") {
    ok = compare(QString(), &error);
  } else if (action == "references") {
    for (const auto &ref : inputs) {
      if (!compare(ref, &error)) {
        ok = false;
        break;
      }
    }
  } else if (action == "compare")
    ok = compare(id, &error);
  else if (action == "consistency") {
    ok = workbench->startFaciesConsistency(id, &error);
  } else if (action == "polygonize")
    ok = workbench->startPolygonize(id, &error);
  else if (action == "copy") {
    const auto draft = workbench->copyForEditing(id, inputs, &error);
    ok = !draft.isEmpty();
    if (ok && edit) {
      show(draft, &error);
      ok = beginEdit(draft);
      if (!ok)
        error = tr("副本已创建，请先结束其他图层的编辑");
    }
  } else if (action == "save") {
    auto *vector =
        qobject_cast<QgsVectorLayer *>(m_layerSvc->layer(id));
    if (vector && vector->isEditable()) {
      if (edit && edit->currentLayer() == vector) {
        if (edit->saveEditing())
          return;
        ok = false;
        error = tr("画布编辑未能提交，请检查编辑提示");
      } else {
        ok = false;
        error = tr("请先保存正在编辑的图层");
      }
    } else
      ok = workbench->saveEditingVersion(id, &error);
  } else if (action == "delete") {
    const auto title = workbench->declaration(id).title;
    const auto products = workbench->removalProducts(id, &error);
    QStringList entries, versionIds;
    for (const auto &value : products) {
      const auto product = value.toMap();
      entries << tr("• %1（%2 个版本）").arg(product.value("title").toString())
                     .arg(product.value("versions").toInt());
      if (m_previewDoc && m_previewDoc->catalog())
        for (const auto &version : m_previewDoc->catalog()->versionsForAsset(
                 product.value("asset_id").toString())) versionIds << version.id;
    }
    if (products.isEmpty()) {
      ok = false;
    } else if (!PaleoNotify::ask(
            this, tr("删除图件"),
            tr("永久删除图件「%1」？\n"
               "以下图件（含关联派生图件）的全部版本、数据管理记录与受管文件将一并删除，不可恢复：\n%2")
                .arg(title.isEmpty() ? id : title, entries.join("\n")),
            PaleoNotify::AskButtons::OkCancel,
            PaleoNotify::AskDefault::Reject,
            PaleoNotify::AskIcon::Warning))
      return;
    else {
      auto *deleteCanvas = m_canvasCtl->canvas();
      const bool wasFrozen = deleteCanvas->isFrozen();
      deleteCanvas->freeze(true);
      ok = workbench->removeProduct(id, &error, true);
      deleteCanvas->freeze(wasFrozen);
      if (!wasFrozen) deleteCanvas->refresh();
      if (ok) {
        for (auto *view : findChildren<MappingReferencePanel *>())
          if (versionIds.contains(view->property("paleo.referenceVersion").toString()))
            view->clearReference(tr("所选证据版本已删除，请选择其他证据。"));
        if (!wellPanel->layerId().isEmpty() &&
            workbench->versionForLayer(wellPanel->layerId()).id.isEmpty()) {
          wellPanel->clear();
          wellDock->hide();
        }
        for (auto it = shown->begin(); it != shown->end();)
          if (workbench->declaration(it.value()).layerId.isEmpty()) it = shown->erase(it);
          else ++it;
        auto *canvas = m_canvasCtl->canvas();
        legend(canvas->currentLayer()
                   ? canvas->currentLayer()->customProperty("paleoLayerId").toString() : QString());
        canvas->refresh();
      }
    }
  } else if (action == "import") {
    const auto path = QFileDialog::getOpenFileName(
        this, tr("导入局部米制约束线"), QString(),
        tr("矢量文件 (*.gpkg *.geojson *.json *.shp)"));
    if (path.isEmpty())
      return;
    ok = workbench->importConstraints(h, path, QStringLiteral("auto"),
                                      &error);
    // auto 判定失败 → 询问用户角色后重试（方向线 / 打断线）。
    if (!ok && error.contains(tr("无法判断约束线类型"))) {
      const QString choice = QInputDialog::getItem(
          this, tr("约束线类型"), tr("无法自动判断，请选择约束线类型："),
          {tr("方向线"), tr("打断线")}, 0, false, &ok);
      if (ok)
        ok = workbench->importConstraints(
            h, path,
            choice == tr("方向线") ? QStringLiteral("direction")
                                  : QStringLiteral("barrier"),
            &error);
      else
        return;
    }
  } else if (action == "draw") {
    if (constraint) {
      constraint->drawConstraintRequested(h, "line", -1);
      page->showMessage(
          tr("在画布逐点绘制约束线，右键结束；Esc 取消。"));
      return;
    } else {
      ok = false;
      error = tr("约束绘制工具不可用");
    }
  } else if (action == "factor") {
    if (m_factorTask && m_factorTask->running()) {
      ok = false;
      error = tr("已有单因素计算在进行");
    } else
      ok = workbench->generateFactor(h, p.value("factor").toString(), p,
                                     &error);
  }
  else if (action == "contours")
    ok = workbench->generateContours(
        h, id, p.value("interval").toDouble(), &error);
  else if (action == "compose")
    ok = workbench->startCompose(h, inputs, p, &error);
  else if (action == "schema") {
    ok = workbench->saveFacies(h, p.value("facies").toList(), &error);
    auto *layer = m_canvasCtl->canvas()->currentLayer();
    legend(layer ? layer->customProperty("paleoLayerId").toString()
                 : QString());
  }
  QString message;
  if (!ok)
    message = error.isEmpty() ? tr("操作未完成，请检查输入。") : error;
  else if (action == "predict")
    message = tr("预测已在后台执行，进度见底栏任务中心；完成后自动登记并显示图件。");
  else if (action == "polygonize")
    message = workbench->polygonizeBusy()
                  ? tr("相栅格转相面已在后台执行，进度见底栏任务中心；完成后自动登记并显示在对应地层组。")
                  : tr("相栅格已转为相面。");
  else if (action == "compose" || action == "consistency" ||
           action == "factor" || action == "contours")
    message =
        tr("任务已提交，进度和取消操作见任务中心；完成后显示结果。");
  else if (action == "wellattributes")
    message = tr("已打开测井矢量属性表；保存后更新井道与单因素输入。");
  else if (action == "wellformations")
    message = tr("已打开地层相属性表；每口井每个地层（组/段）一行，预测输出即该地层的相，可直接维护。");
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
    message = tr("已生成地震相与测井相一致性报告并留档；双击逐井行可在画布定位。");
  else if (action == "delete")
    message = tr("已删除图件及版本记录，受管文件正在后台清理。");
  else
    message = tr("操作完成，图件与版本已更新。");
  page->showMessage(message);
  statusBar()->showMessage(message, 10000);
  if (!ok && action == "delete")
    PaleoNotify::warning(this, tr("图件未删除"), message);
}
