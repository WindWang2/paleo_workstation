// 层：视图
// paleomainwindow_attach_compose — 综合编图页接线（W4 拆分段）
#include "paleomainwindow.h"

#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/qgisprojectservice.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgislayerprofile.h"
#include "../qgis/qgislayoutservice.h"
#include "../linkage/selectioncontext.h"
#include "../catalog/datacatalog.h"
#include "../workflow/workflows.h"
#include "../workflow/horizonbatchexport.h"
#include "pages/pagepanels.h"
#include "layoutdesignershell.h"
#include "edittools/editingtoolbar.h"

#include <qgsmapcanvas.h>
#include <qgsproject.h>
#include <qgsmaplayer.h>
#include <qgsvectorlayer.h>
#include <qgslayertree.h>
#include <qgslayertreelayer.h>
#include <qgslayout.h>
#include <qgslayoutitemmap.h>
#include <qgsprintlayout.h>
#include <qgsmessagelog.h>

#include <QCoreApplication>
#include <QFileInfo>
#include <QLabel>
#include "notifications/paleonotify.h"
#include <QProgressDialog>
#include <QPointer>
#include <QStatusBar>


// ---------------------------------------------------------------------------
// 编图页接线：因子融合 + 相面多边形化（W4 拆分段）
// ---------------------------------------------------------------------------
void PaleoMainWindow::attachComposePage(ComposePage *composePage,
                                        CompositionWorkflow *compose,
                                        QgisLayoutService *layoutSvc)
{
  if (compose && composePage)
  {
    connect(composePage, &ComposePage::fuseRequested, this,
            [this, compose, composePage](const QStringList &factorIds) {
              QString err;
              const QString horizon = m_selection ? m_selection->activeHorizon() : QString();
              if (compose->fuseFactors(horizon, factorIds, &err))
                composePage->refreshFactors();
              else
              {
                // 失败原因落地到页面状态行（空清单/未声明/解释性层拒绝等
                // 全在 workflow 的 err 里），不再静默吞掉。
                if (!err.isEmpty())
                  QgsMessageLog::logMessage(err, QStringLiteral("Paleo"),
                                            Qgis::MessageLevel::Warning);
                if (auto *status = composePage->findChild<QLabel *>(QStringLiteral("statusLabel")))
                  status->setText(err.isEmpty()
                                      ? tr("合成编图失败，请检查所选图层与当前层位")
                                      : err);
              }
            });
    connect(composePage, &ComposePage::polygonizeRequested, this,
            [this, compose, composePage](const QString &rasterId, double minArea, double simplify) {
              QString err;
              const QString horizon = m_selection ? m_selection->activeHorizon() : QString();
              QVariantMap params;
              params.insert(QStringLiteral("MIN_AREA"), minArea);
              params.insert(QStringLiteral("SIMPLIFY"), simplify);
              // facies-ux 1：转换进任务中心后台执行（未注入任务服务时同步）；
              // 完成经 faciesPolygonsReady 收尾，失败经 faciesPolygonsFailed。
              if (!compose->startDeriveFaciesPolygons(horizon, rasterId, params, &err))
              {
                if (!err.isEmpty())
                {
                  QgsMessageLog::logMessage(err, QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
                  statusBar()->showMessage(err, 10000);
                }
              }
              else if (compose->faciesPolygonsBusy())
                statusBar()->showMessage(
                    tr("相栅格转相多边形已在后台执行，进度见底栏任务中心。"), 10000);
              else
                composePage->refreshFactors();
            });
    connect(compose, &CompositionWorkflow::faciesPolygonsFailed, this,
            [this](const QString &, const QString &error) {
              if (!error.isEmpty())
                statusBar()->showMessage(tr("相多边形转换未完成：%1").arg(error), 10000);
            });

    // ---- m2(C): 矢量化成功 → 自动进入相界编辑态（z3 PaleoVertexTool）------
    // 拿 facies.<horizon> 矢量层 instantiate → 编辑条选层 + 触发顶点工具
    // （QgsVertexTool 是 app-only，顶点编辑走 PaleoVertexTool；snapping 在
    // 画布控制器侧已开，拓扑编辑由编辑条「拓扑」开关驱动——默认关）。派生
    // gpkg 按 T26 纪律只读——进编辑前先铺
    // 可编辑工作副本（prepareFaciesForEditing）。编辑条缺席（无画布环境）
    // → 降级为选中层 + 状态文案提示手动进入编辑。
    connect(compose, &CompositionWorkflow::compositionDone, this,
            [this](const QString &, const QString &layerId) {
              revealDeclaredLayer(layerId, true);
            });
    connect(compose, &CompositionWorkflow::faciesPolygonsReady, this,
            [this, compose, composePage](const QString &h, const QString &layerId) {
              Q_UNUSED(h);
              composePage->refreshFactors();
              composePage->setFaciesEditTarget(layerId);
              if (!m_layerSvc)
                return;
              QString target = layerId;
              QString prepErr;
              const QString prepared = compose->prepareFaciesForEditing(layerId, &prepErr);
              if (prepared.isEmpty())
              {
                // 铺设被拒（如约束层有未保存编辑）或失败：只读原件不可编辑，
                // 不自动进编辑（否则编辑条会对只读层/他层会话做出意外动作），
                // 原因上状态栏而不只进日志。
                QgsMessageLog::logMessage(
                    tr("相界工作副本铺设失败：%1").arg(prepErr), QStringLiteral("Paleo"),
                    Qgis::MessageLevel::Warning);
                revealDeclaredLayer(layerId, true);
                if (auto *status = composePage->findChild<QLabel *>(QStringLiteral("statusLabel")))
                  status->setText(tr("相界已生成，但未进入编辑：%1").arg(prepErr));
                return;
              }
              target = prepared;
              revealDeclaredLayer(target, true);
              QgsMapLayer *l = m_layerSvc->instantiate(target);
              auto *vl = qobject_cast<QgsVectorLayer *>(l);
              auto *status = composePage->findChild<QLabel *>(QStringLiteral("statusLabel"));
              auto *editTb = findChild<PaleoEditingToolbar *>(QStringLiteral("editingToolbar"));
              if (vl && editTb && m_canvasCtl)
              {
                editTb->refreshFromProject(); // 新层入列（instantiate 已挂工程）
                editTb->setCurrentLayer(vl);
                editTb->actionVertexEdit()->trigger(); // 自动开编辑（走编辑服务纪律）+ 装顶点工具
                if (editTb->isEditing() && editTb->currentLayer() == vl)
                {
                  if (status)
                    status->setText(tr("相界就绪，已进入编辑：%1（顶点工具 — 选中要素改属性）")
                                        .arg(layerId));
                  return;
                }
              }
              // 降级：选中层 + 文案提示手动进入编辑（如实报告，不假装在编辑）。
              if (vl && m_canvasCtl)
                m_canvasCtl->canvas()->setCurrentLayer(vl);
              if (status)
                status->setText(tr("相界就绪：%1 — 请在「要素编辑」组手动进入编辑")
                                    .arg(layerId));
            });

    // ---- m2(C): 相属性保存 → edit buffer 回写 ------------------------------
    connect(composePage, &ComposePage::faciesAttributesSaveRequested, this,
            [this, compose, composePage](const QString &layerId, const QVariantMap &attrs) {
              if (!compose)
                return;
              QString err;
              if (compose->saveFaciesAttributes(layerId, attrs, &err))
              {
                if (auto *status = composePage->findChild<QLabel *>(QStringLiteral("statusLabel")))
                  status->setText(tr("相属性已写入编辑缓冲：%1（随「保存编辑」提交）")
                                      .arg(layerId));
              }
              else
              {
                if (auto *status = composePage->findChild<QLabel *>(QStringLiteral("statusLabel")))
                  status->setText(err.isEmpty() ? tr("相属性保存失败") : err);
                if (!err.isEmpty())
                  QgsMessageLog::logMessage(err, QStringLiteral("Paleo"),
                                            Qgis::MessageLevel::Warning);
              }
            });

    // ---- 方向 39: 相界边界核查 → faciesqa 引擎（只接不重写）----------------
    connect(composePage, &ComposePage::boundaryQaRequested, this,
            [this, compose, composePage](const QString &layerId) {
              if (!compose)
                return;
              auto *status =
                  composePage->findChild<QLabel *>(QStringLiteral("statusLabel"));
              QString err;
              const QVariantList issues = compose->runFaciesBoundaryQa(layerId, &err);
              if (!err.isEmpty())
              {
                if (status)
                  status->setText(err);
                QgsMessageLog::logMessage(err, QStringLiteral("Paleo"),
                                          Qgis::MessageLevel::Warning);
                return;
              }
              if (issues.isEmpty())
              {
                if (status)
                  status->setText(tr("边界核查通过：%1 无核查项").arg(layerId));
                return;
              }
              // 报告有名有因：状态栏给条数 + 首条，全量逐条进消息日志。
              QStringList lines;
              for (const QVariant &v : issues)
              {
                const QVariantMap row = v.toMap();
                lines << tr("【%1】要素 %2：%3")
                             .arg(row.value(QStringLiteral("name")).toString(),
                                  row.value(QStringLiteral("regionIds")).toStringList()
                                      .join(QStringLiteral(",")),
                                  row.value(QStringLiteral("reason")).toString());
              }
              for (const QString &line : lines)
                QgsMessageLog::logMessage(
                    tr("边界核查 %1：%2").arg(layerId, line), QStringLiteral("Paleo"),
                    Qgis::MessageLevel::Warning);
              if (status)
                status->setText(tr("边界核查：%1 项核查项（%2；全部见消息日志）")
                                    .arg(QString::number(issues.size()), lines.first()));
            });

    // ---- m2(C): 参考图勾选 → instantiate + 图层树节点勾选/取消 ------------
    connect(composePage, &ComposePage::referenceVisibilityRequested, this,
            [this](const QString &layerId, bool visible) {
              if (!m_layerSvc || layerId.isEmpty())
                return;
              QgsMapLayer *l = visible ? m_layerSvc->instantiate(layerId)
                                       : m_layerSvc->layer(layerId);
              if (!l)
                return;
              QgsProject *proj = m_projectSvc ? m_projectSvc->project() : nullptr;
              QgsLayerTree *root = proj ? proj->layerTreeRoot() : nullptr;
              if (!root)
                return;
              for (QgsLayerTreeLayer *node : root->findLayers())
                if (node->layer() == l)
                {
                  if (visible)
                    node->setItemVisibilityCheckedParentRecursive(true);
                  else
                    node->setItemVisibilityChecked(false);
                }
              // 回写页主题：切页按 page:* 主题回放树态，程序化拨勾选后不
              // 回写会被旧主题回滚（对齐预测页「显示结果」的既有回写）。
              if (m_profileSvc)
                m_profileSvc->captureCurrentAsTheme(
                    QgisLayerProfileService::pageThemeName(currentPage()));
            });

    // ---- m2(C): 在布局设计器中打开（layoutdesignershell 公共入口）---------
    // 打开（或新建）本层位的版面：优先复用导出同名布局 "<h>_map"；新空布局
    // 没有地图项时主题钉定自然跳过（设计器里由模板/手工补地图项）。
    // 方向 25 M6：openLayoutDesigner(name) 同时服务版面库的按名打开。
    const auto openLayoutDesigner = [this, layoutSvc](const QString &name) {
      if (!layoutSvc)
      {
        QgsMessageLog::logMessage(tr("布局服务未接入 — 无法打开图件设计器"),
                                  QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
        return;
      }
      if (!m_projectSvc || m_projectSvc->projectPath().isEmpty())
      {
        QgsMessageLog::logMessage(tr("无打开工程 — 无法打开图件设计器"),
                                  QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
        return;
      }
      QString err;
      QgsLayout *layout = layoutSvc->layout(name);
      if (!layout)
        layout = layoutSvc->createLayout(name, &err);
      if (!layout)
      {
        QgsMessageLog::logMessage(
            err.isEmpty() ? tr("创建布局失败：%1").arg(name) : err,
            QStringLiteral("Paleo"), Qgis::MessageLevel::Critical);
        return;
      }
      // 版面地图项钉本页主题（m1 setLayoutMapTheme 接缝）。
      if (QgsLayoutItemMap *mapItem =
              qobject_cast<QgsLayoutItemMap *>(layout->itemById(QStringLiteral("map"))))
        pinLayoutTheme(mapItem, QStringLiteral("compose"));
      auto *shell = new PaleoLayoutDesignerShell(layout, this);
      shell->setTaskService(m_taskSvc); // #85：导出走任务池 worker
      shell->setAttribute(Qt::WA_DeleteOnClose);
      shell->setModal(false);
      shell->show();
    };
    connect(composePage, &ComposePage::layoutDesignerRequested, this,
            [this, openLayoutDesigner]() {
              const QString h = m_selection ? m_selection->activeHorizon() : QString();
              openLayoutDesigner(h.isEmpty() ? tr("编图布局")
                                             : QStringLiteral("%1_map").arg(h));
            });

    // ---- 方向 25 M6：版面库（打开/删除/批量出图）----------------------------
    const auto refreshLayoutNames = [composePage, layoutSvc]() {
      composePage->setLayoutNames(layoutSvc ? layoutSvc->layoutNames() : QStringList());
    };
    refreshLayoutNames();
    if (layoutSvc)
    {
      connect(layoutSvc, &QgisLayoutService::layoutAdded, composePage, refreshLayoutNames);
      connect(layoutSvc, &QgisLayoutService::layoutRemoved, composePage, refreshLayoutNames);
    }
    connect(composePage, &ComposePage::layoutOpenRequested, this,
            [openLayoutDesigner](const QString &name) { openLayoutDesigner(name); });
    connect(composePage, &ComposePage::layoutDeleteRequested, this,
            [this, composePage, layoutSvc, refreshLayoutNames](const QString &name) {
              if (!layoutSvc || name.isEmpty())
                return;
              if (!PaleoNotify::ask(
                      this, tr("删除版面"),
                      tr("删除版面「%1」？随工程保存的布局将一并移除。").arg(name),
                      PaleoNotify::AskButtons::OkCancel, PaleoNotify::AskDefault::Reject))
                return;
              if (!layoutSvc->removeLayout(name))
                PaleoNotify::warning(this, tr("删除失败"), tr("无法删除版面「%1」。").arg(name));
              refreshLayoutNames();
            });
    connect(composePage, &ComposePage::batchFigureExportRequested, this,
            [this, composePage, layoutSvc]() {
              // 骨架版面：活动层位版面优先，退第一个现存版面；都没有 → 提示。
              const QString h = m_selection ? m_selection->activeHorizon() : QString();
              QString skeletonName = h.isEmpty() ? QString()
                                                 : QStringLiteral("%1_map").arg(h);
              QgsPrintLayout *skeleton = skeletonName.isEmpty()
                                             ? nullptr
                                             : qobject_cast<QgsPrintLayout *>(
                                                   layoutSvc ? layoutSvc->layout(skeletonName)
                                                             : nullptr);
              if (!skeleton && layoutSvc)
              {
                const QStringList names = layoutSvc->layoutNames();
                if (!names.isEmpty())
                {
                  skeletonName = names.first();
                  skeleton =
                      qobject_cast<QgsPrintLayout *>(layoutSvc->layout(skeletonName));
                }
              }
              if (!skeleton)
              {
                PaleoNotify::information(
                    this, tr("批量出图"),
                    tr("先在设计器里准备一个版面（作为批量出图的骨架）。"));
                return;
              }
              if (!m_layerSvc)
                return;
              DataCatalog *catalog = m_previewDoc ? m_previewDoc->catalog() : nullptr;
              const QString projectDir =
                  m_projectSvc && !m_projectSvc->projectPath().isEmpty()
                      ? QFileInfo(m_projectSvc->projectPath()).absolutePath()
                      : QString();
              if (!catalog || projectDir.isEmpty())
              {
                PaleoNotify::warning(this, tr("批量出图"),
                                     tr("需要打开工程（catalog 受管区）再批量出图。"));
                return;
              }

              PaleoHorizonBatchExport::Request request;
              request.layout = skeleton;
              request.projectName = QFileInfo(projectDir).fileName();
              request.horizonLayers =
                  PaleoHorizonBatchExport::resolveHorizonLayers(m_layerSvc);
              request.catalog = catalog;
              request.projectDir = projectDir;
              request.dpi = 300.0;
              request.format = PaleoHorizonBatchExport::Format::Pdf;

              const QPointer<QWidget> page(composePage);
              const auto report = [this, page](const PaleoHorizonBatchExport::Result &result,
                                               bool cancelled) {
                QStringList lines{result.summary()};
                if (cancelled)
                  lines << tr("（已取消或工程已切换，未出图层位已入账为失败；版面已复原）");
                for (const auto &outcome : result.horizons)
                  lines << (outcome.ok ? tr("· %1 → %2").arg(outcome.horizon, outcome.file)
                                       : tr("· %1 失败：%2").arg(outcome.horizon, outcome.error));
                PaleoNotify::report(this, tr("批量出图"), lines.join(QLatin1Char('\n')));
                if (page)
                  if (auto *status = page->findChild<QLabel *>(QStringLiteral("statusLabel")))
                    status->setText(result.summary());
              };
              // UIS-03：任务中心驱动（逐层位出图，层位间界面可响应、可取消）；
              // 无任务服务时保留同步旧路径。
              if (m_taskSvc) {
                QString error;
                if (!PaleoHorizonBatchExport::startAsync(request, m_taskSvc, report, &error))
                  PaleoNotify::warning(this, tr("批量出图"), error);
                else
                  statusBar()->showMessage(
                      tr("批量出图已在后台按层位执行，进度见底栏任务中心；期间请勿编辑该版面。"), 10000);
                return;
              }
              QProgressDialog progress(tr("正在按层位组批量出图…"), QString(), 0, 0, this);
              progress.setWindowTitle(tr("批量出图"));
              progress.setWindowModality(Qt::WindowModal);
              progress.setMinimumDuration(0);
              QCoreApplication::processEvents();
              const auto result = PaleoHorizonBatchExport::run(request);
              progress.cancel();
              report(result, false);
            });
    // ---- 方向 25 M6 end ----
  }
}
