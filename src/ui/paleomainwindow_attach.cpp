// 层：视图
// paleomainwindow_attach — W4 壳瘦身：attachWorkflows 按页拆私有成员函数
//（attachDataPage/attachPredictPage/attachConstraintPage/attachComposePage/
// attachValidatePage + attachShellSurfaces），attachMapping 按段拆（发布门
// /导出接线/版本状态机）。仍是 PaleoMainWindow 成员函数，定义在本 TU；
// 入口函数只排顺序 + 幂等守卫。
#include "paleomainwindow.h"

#include "paleotheme.h"
#include "paleoicons.h"
#include "paleoribbon.h"

#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/qgisprojectservice.h"
#include "../qgis/qgislayerservice.h"
#include "../services/toolavailability.h"
#include "../linkage/selectioncontext.h"
#include "../workflow/workflows.h"
#include "../domain/arearules.h"
#include "../domain/projectclassifier.h"
#include "../linkage/seismicmaplink.h"
#include "../linkage/threewaylocator.h"
#include "../qgis/qgisprocessingservice.h"
#include "../metadata/paleoprojectstore.h"
#include "../metadata/layermanifest.h"
#include "../metadata/mapversionstore.h"
#include "../services/projectdata.h"
#include "../services/previewdoc.h"
#include "../workflow/folderimport.h"
#include "../workflow/registration.h"
#include "../workflow/mappingworkflow.h"
#include "../workflow/mapexport.h"
#include "../workflow/mapversioncontroller.h"
#include "locator/paleolocatorfilters.h"
#include "releasepanel.h"
#include "taskpanel.h"
#include "attributetablepanel.h"
#include "pages/pagepanels.h"
#include "pages/pageshared.h" // kPageIds
#include "constraintdrawcontroller.h"
#include "typedconstraintdrawcontroller.h" // m2(B)：物源线/展布线/控制点类型化捕获
#include "dialogs/folderconfirm.h"
#include "correlationpanel.h"
#include "datapreview/datapreviewtabs.h"
#include "../catalog/datacatalog.h"
#include "layoutdesignershell.h"
#include "edittools/editingtoolbar.h"
#include "layout/layoutexportactions.h"     // m2(C)：导出版面钉 compose 主题后走同一 PDF 出口
#include "../qgis/qgislayoutservice.h"
#include "../qgis/qgislayerprofile.h"       // m1 页面档案：setLayoutMapTheme/pageThemeName
#include "../qgis/qgiseditingservice.h"
#include "../services/paleotaskservice.h"
#include "ui/seismicsection/seismicsectiondockwidget.h"
#include "ui/seismic3d/seismic3dviewpanel.h"
#include "services/seismictaskservice.h"
#include "domain/seismic/sgyvolume.h"

#include <qgsmapcanvas.h>
#include <qgsproject.h>
#include <qgsmaplayer.h>
#include <qgslayertree.h>
#include <qgslayertreeview.h>
#include <qgsmessagelog.h>
#include <qgslocatorwidget.h>
#include <qgslocator.h>
#include <qgsvectorlayer.h>
#include <qgslayertreelayer.h>
#include <qgslayout.h>
#include <qgslayoutitemmap.h>
#include <qgsprintlayout.h>

#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QShortcut>
#include <QToolButton>
#include <QSettings>
#include <QStatusBar>
#include <QStackedLayout>
#include <QTabWidget>
#include <QTextEdit>
#include <QPointer>
#include <QVBoxLayout>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>

#include <memory>

void PaleoMainWindow::attachWorkflows(PredictionWorkflow *pred, ConstraintWorkflow *constraint,
                                      CompositionWorkflow *compose, ValidationWorkflow *validate,
                                      DataImportService *importSvc, SeismicMapLink *seismicLink,
                                      QgisProcessingService *procSvc, PaleoProjectStore *store,
                                      QgisEditingService *editSvc, QgisLayoutService *layoutSvc,
                                      PaleoTaskService *taskSvc)
{
  // 幂等守卫：本函数不是增量接线，而是整建——清空右栏页面栈重建、往
  // bottomTabs/状态栏加面板（correlationPanel、releasePanel、任务/属性表、
  // statusCatalogError…）、在 topBar 建按钮（processingButton、designerButton…）
  // 并往 importSvc/preview/pages 上叠信号连接。同一窗口二次执行会重复建
  // dock/按钮（各对象双份），且旧页面 deleteLater 后残留的信号捕获会悬空，
  // 后续用例段错误。测试套件会二次触达同一窗口（tst_ui 单跑用例的补调
  // 路径 + attachWorkflowsIsIdempotent 直证），因此首次完整接线后早退；
  // rightPanelHost 缺席的早退不算完成，不置位。
  if (m_workflowsAttached)
    return;
  auto *host = findChild<QWidget *>(QStringLiteral("rightPanelHost"));
  auto *stack = host ? static_cast<QStackedLayout *>(host->layout()) : nullptr;
  if (!stack)
    return;
  m_taskSvc = taskSvc; // D1b：导入任务池（nullptr 时保持同步旧路径）
  m_importSvc = importSvc; // 「从工区文件夹新建」直达入口
  if (importSvc)
    m_previewDoc = new PreviewDocService(importSvc, this); // 壳唯一数据门面（页属性/预览共用）
  if (taskSvc && !m_seismicTaskSvc)
  {
    m_seismicTaskSvc = std::make_unique<seismic::SeismicTaskService>(taskSvc, 256, this);
  }
  if (m_seismicTaskSvc && m_seismic3dPanel)
  {
    m_seismic3dPanel->setTaskService(m_seismicTaskSvc.get());
  }

  // Replace placeholders in page order (data, predict, constraint, compose, validate).
  while (stack->count() > 0)
  {
    QLayoutItem *item = stack->takeAt(0);
    if (item->widget())
      item->widget()->deleteLater();
    delete item;
  }

  // 数据管理页（ribbon 布局）：DataPage 本体进中央「数据列表」；实体数据
  // 视图段挂到右 dock 第 0 页「数据属性」；导入段藏起——导入命令在 ribbon
  // 「数据导入」组（镜像这些按钮，门控/原因仍写在按钮上）。
  auto *dataPage = new DataPage(m_dataListHost ? m_dataListHost : host);
  auto *dataProps = new QWidget(host);
  dataProps->setObjectName(QStringLiteral("dataPropertiesPage"));
  {
    auto *pl = new QVBoxLayout(dataProps);
    pl->setContentsMargins(8, 8, 8, 8); // spacing.sm
    if (m_dataListHost)
    {
      m_dataListHost->layout()->addWidget(dataPage);
      if (QWidget *entity = dataPage->entityViewSection())
        pl->addWidget(entity, 1); // 重新挂父：刷新按段查找，不受影响
      if (auto *imports = dataPage->findChild<QWidget *>(QStringLiteral("dataImportSection")))
        imports->hide();
    }
    else
      pl->addWidget(dataPage);
  }
  auto *predictPage = new PredictPage(pred, m_layerSvc, host);
  auto *constraintPage = new ConstraintPage(constraint, host);
  auto *composePage = new ComposePage(compose, m_layerSvc, host);
  auto *validatePage = new ValidatePage(validate, host);
  stack->addWidget(dataProps);
  stack->addWidget(predictPage);
  stack->addWidget(constraintPage);
  stack->addWidget(composePage);
  stack->addWidget(validatePage);

  // Phase 5: 井-震-图联动 (SeismicMapLink) 接线——linkage 不碰 ui 类型：
  // 剖面画布的悬停/点击信号连进联动器槽，联动器的体量推送连回剖面控件。
  if (seismicLink && m_seismicSectionDock && m_seismicSectionDock->canvas())
  {
    connect(m_seismicSectionDock->canvas(), &seismic::SeismicSectionCanvas::traceHovered,
            seismicLink, &SeismicMapLink::onSectionTraceHovered);
    connect(m_seismicSectionDock->canvas(), &seismic::SeismicSectionCanvas::traceClicked,
            seismicLink, &SeismicMapLink::onSectionTraceClicked);
    connect(seismicLink, &SeismicMapLink::sectionVolumeChanged,
            m_seismicSectionDock, &seismic::SeismicSectionDockWidget::setVolume);
    // 地图折线剖面意图 → dock 异步提取 + 露出（W3b：linkage 只发信号）。
    connect(seismicLink, &SeismicMapLink::sectionExtractRequested,
            m_seismicSectionDock,
            [this](std::shared_ptr<const seismic::SgyVolume> volume,
                   std::vector<glm::ivec2> pathPoints, QString title,
                   std::vector<glm::dvec2> mapPolyline) {
              m_seismicSectionDock->extractSectionFromVolumeAsync(
                  volume, pathPoints, title, mapPolyline);
              m_seismicSectionDock->show();
              m_seismicSectionDock->raise();
            });
    if (auto volume = seismicLink->activeVolume())
      m_seismicSectionDock->setVolume(volume);
  }
  WellCorrelationPanel *corrPanel = nullptr;
  if (auto *bottomTabs = findChild<QTabWidget *>(QStringLiteral("bottomTabs")))
  {
    if (m_selection)
    {
      corrPanel = new WellCorrelationPanel(m_selection, bottomTabs);
      corrPanel->setObjectName(QStringLiteral("correlationPanel"));
      bottomTabs->addTab(corrPanel, QStringLiteral("连井剖面"));
    }
  }

  // W4：按页拆接线——入口只排顺序；每页一段私有成员函数（本 TU）。
  attachDataPage(dataPage, corrPanel, importSvc, taskSvc);
  attachPredictPage(predictPage, pred);
  attachConstraintPage(constraintPage, constraint);
  attachComposePage(composePage, compose, layoutSvc);
  attachValidatePage(validatePage, validate, corrPanel, importSvc);
  PaleoEditingToolbar *editTb =
      attachShellSurfaces(store, procSvc, editSvc, layoutSvc, taskSvc);

  buildRibbonPanels(dataPage, predictPage, constraintPage, composePage, validatePage, editTb,
                    corrPanel);

  // Re-sync visible page index with current tab.
  const int idx = paleo::pagesinternal::kPageIds.indexOf(m_currentPage);
  stack->setCurrentIndex(idx >= 0 ? idx : 0);
  m_workflowsAttached = true; // 走到末尾才算接线完成（幂等守卫置位）
}
// ---------------------------------------------------------------------------
// 数据页接线：门面绑定 / catalog 联动 / 预览分栏 / 导入意图（W4 拆分段）
// ---------------------------------------------------------------------------
void PaleoMainWindow::attachDataPage(DataPage *dataPage,
                                     WellCorrelationPanel *corrPanel,
                                     DataImportService *importSvc,
                                     PaleoTaskService *taskSvc)
{
  // Panel intents → workflows / selection. Params stay minimal for the shell
  // milestone — full parameter dialogs are per-panel follow-up work.
  if (importSvc && dataPage)
  {
    // §3/§4 数据契约接线：数据页绑定导入服务，资产表跟 catalog 走，
    // 列表选中在中央预览标签（地图下方分栏）打开（确认入库后才开标签）。
    dataPage->setProperty("paleo.page.importsvc", QVariant::fromValue<QObject *>(m_previewDoc));
    connect(m_previewDoc->catalog(), &DataCatalog::changed, this,
            [this, dataPage]() {
              QMetaObject::invokeMethod(dataPage, "refreshAssetTable");
              syncSeismicVolumeToDocks();
            });
    dataPage->refreshAssetTable();
    syncSeismicVolumeToDocks();
    // D6 地图→表联动：画布上拾取的实体（WellMapLink → ctx）→ 资产表选中
    // 其已决关联的行；选中走同一条 assetActivated → 预览照开。
    if (m_selection)
      connect(m_selection, &SelectionContext::selectionChanged, dataPage,
              [dataPage](const QStringList &ids, const QString &origin) {
                if (origin != QLatin1String("datapreview") && origin != QLatin1String("datatree"))
                  dataPage->selectAssetsForEntities(ids);
              });

    // ---- T20 余项：catalogOpenFailed 的状态栏露出 ----
    // 常驻红胶囊（DESIGN.md error token）写打开失败原因；恢复（工程重开且
    // catalog 打开成功）前，数据页的导入类动作保持禁用——失败的 catalog 拒
    // 绝写入，导入必然失败，不给用户一个假入口。
    auto *catalogError =
        PaleoTheme::capsuleLabel(QString(), PaleoTheme::CapsuleKind::Error, this);
    catalogError->setObjectName(QStringLiteral("statusCatalogError"));
    catalogError->hide();
    statusBar()->addPermanentWidget(catalogError);
    const auto setImportsEnabled = [this](bool enabled) {
      for (const char *name : {"importWells", "importSeismic", "importBoundary",
                               "importFolder"})
        if (auto *btn = findChild<QPushButton *>(QLatin1String(name)))
        {
          btn->setEnabled(enabled);
          if (!enabled)
            btn->setToolTip(tr("数据目录打开失败 — 导入暂不可用（见状态栏）"));
          else
            btn->setToolTip(QString());
        }
    };
    connect(m_previewDoc, &PreviewDocService::catalogOpenFailed, this,
            [catalogError, setImportsEnabled](const QString &error) {
              catalogError->setText(tr("数据目录打开失败：%1").arg(error));
              catalogError->show();
              setImportsEnabled(false);
            });
    // 恢复：projectOpened 后（AppContext 已同步重设 projectDir）错误清空 →
    // 收告警、放开导入。
    if (m_projectSvc)
      connect(m_projectSvc, &QgisProjectService::projectOpened, this,
              [this, dataPage, importSvc, catalogError, setImportsEnabled](const QString &) {
                if (m_previewTabs && m_projectSvc)
                  m_previewTabs->setProject(m_projectSvc->project());
                if (m_previewDoc->catalogOpenError().isEmpty())
                {
                  catalogError->hide();
                  setImportsEnabled(true);
                }
                else
                  setImportsEnabled(false); // 换了个工程仍然失败 → 保持禁用
                dataPage->refreshAssetTable();
                syncSeismicVolumeToDocks();
              });
    DataPreviewTabs *preview = m_previewTabs;
    if (preview)
    {
      preview->setDocService(m_previewDoc);
      preview->setTaskService(taskSvc); // D1：剖面索引/解码异步化（nullptr 时保持同步）
      if (m_projectSvc)
        preview->setProject(m_projectSvc->project());
      // D11 临时配准：GeoJSON 标签手工仿射 → DERIVED + 水印图层。
      connect(preview, &DataPreviewTabs::provisionalRegistrationRequested, this,
              [this, importSvc](const QString &assetId, const QVariantMap &params) {
                applyProvisionalRegistration(importSvc, assetId, params);
              });
      connect(dataPage, &DataPage::assetActivated, preview, &DataPreviewTabs::openAsset);
      connect(dataPage, &DataPage::assetActivated, dataPage, &DataPage::selectAsset);
      connect(dataPage, &DataPage::assetWellActivated, preview, &DataPreviewTabs::openAssetForWell);
      connect(dataPage, &DataPage::seismicLineActivated, preview,
              [preview](const QString &aid, const QString &mode) {
                preview->openSeismicLine(aid, mode, 0, 0.0);
              });
      connect(dataPage, &DataPage::surveyAreaActivated, preview, &DataPreviewTabs::openSurveyArea);
      connect(preview, &DataPreviewTabs::requestShowOnMainCanvas, this, [this]() {
        if (m_workspaceStack)
          m_workspaceStack->setCurrentIndex(0);
      });
      if (auto *inner = preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs")))
      {
        connect(inner, &QTabWidget::currentChanged, this, [preview, dataPage](int idx) {
          if (idx >= 0 && preview && dataPage)
          {
            const QString aid = preview->assetIdAt(idx);
            if (!aid.isEmpty())
              dataPage->selectAsset(aid);
          }
        });
      }
      // well_head 预览 / 井树选中 → 地图高亮该井（§4；Direction B 经 SelectionContext）。
      if (m_selection)
      {
        connect(dataPage, &DataPage::wellSelected, this,
                [this](const QString &wellEntityId) {
                  m_selection->setSelection({wellEntityId}, QStringLiteral("datatree"));
                });
        connect(preview, &DataPreviewTabs::wellSelected, this,
                [this](const QString &wellEntityId) {
                  m_selection->setSelection({wellEntityId}, QStringLiteral("datapreview"));
                });
      }
      // horizon 预览「在地图上显示」（§4/T29 双向同步）：实例化派生栅格 →
      // 缩放到该图层 → 闪烁定位 ~400ms → 勾上图层树节点 → 按钮置「已在
      // 地图上」；图层树里取消勾选时按钮态跟随（node visibilityChanged，
      // QGIS 4：可见性归图层树管，不在 QgsMapLayer 上）。
      if (m_layerSvc)
        connect(preview, &DataPreviewTabs::showHorizonOnMapRequested, this,
                [this, preview](const QString &layerId) {
                  QString err;
                  QgsMapLayer *layer = m_layerSvc->instantiate(layerId, &err);
                  if (!layer)
                  {
                    QgsMessageLog::logMessage(tr("Show on map failed: %1").arg(err),
                                              QStringLiteral("Paleo"), Qgis::Critical);
                    return;
                  }
                  if (m_canvasCtl)
                  {
                    m_canvasCtl->zoomToLayer(layerId);
                    flashHorizonLayer(layer);
                  }
                  QgsProject *proj =
                      m_projectSvc ? m_projectSvc->project() : nullptr;
                  if (QgsLayerTreeLayer *node =
                          proj ? proj->layerTreeRoot()->findLayer(layer->id())
                               : nullptr)
                  {
                    node->setItemVisibilityChecked(true); // 显示意图（可能已在）
                    // 重复点击同一图层不叠加 connect：节点属性作去重标记
                    // （UniqueConnection 只支持成员函数槽，lambda 不可用）。
                    if (!node->property("paleo.visSync").toBool())
                    {
                      node->setProperty("paleo.visSync", true);
                      connect(node, &QgsLayerTreeNode::visibilityChanged, preview,
                              [preview, layerId](QgsLayerTreeNode *n) {
                                preview->setHorizonOnMap(
                                    layerId, n->itemVisibilityChecked());
                              });
                    }
                  }
                  preview->setHorizonOnMap(layerId, true);
                });
    }
    connect(dataPage, &DataPage::importRequested, this,
            [this, importSvc, preview](const QString &kind) {
              if (kind == QLatin1String("folder"))
              {
                runFolderImport(importSvc);
                return;
              }
              const QString filter = (kind == QLatin1String("seismic"))
                                         ? tr("地震数据文件 (*.sgy *.segy);;All files (*)")
                                         : ((kind == QLatin1String("well_log"))
                                                ? tr("测井与柱状图文件 (*.las *.xml *.txt *.csv);;All files (*)")
                                                : tr("数据文件 (*.las *.xml *.csv *.dat *.gpkg *.shp *.tif *.img);;All files (*)"));
              const QString path = QFileDialog::getOpenFileName(
                  this, tr("导入 %1").arg(kind), QString(), filter);
              if (path.isEmpty())
                return;
              // T22：单文件导入同样展示 CRS 契约句（确认一步，含识别类型）。
              {
                QDialog confirmDlg(this);
                confirmDlg.setObjectName(QStringLiteral("singleImportDialog"));
                confirmDlg.setWindowTitle(tr("导入数据"));
                auto *cl = new QVBoxLayout(&confirmDlg);
                const QString recogType = classifyProjectImport(path).type;
                auto *fileLabel = new QLabel(
                    tr("文件：%1\n识别类型：%2").arg(path, PaleoFolderConfirm::folderTypeLabel(recogType)),
                    &confirmDlg);
                fileLabel->setWordWrap(true);
                cl->addWidget(fileLabel);
                auto *crsNote = new QLabel(PaleoFolderConfirm::engineeringCrsSentence(), &confirmDlg);
                crsNote->setObjectName(QStringLiteral("singleImportCrsNote"));
                crsNote->setWordWrap(true);
                crsNote->setStyleSheet(QStringLiteral("color: #5D6E80;"));
                cl->addWidget(crsNote);
                auto *bb = new QDialogButtonBox(&confirmDlg);
                bb->addButton(tr("导入"), QDialogButtonBox::AcceptRole);
                bb->addButton(tr("取消"), QDialogButtonBox::RejectRole);
                QObject::connect(bb, &QDialogButtonBox::accepted, &confirmDlg,
                                 &QDialog::accept);
                QObject::connect(bb, &QDialogButtonBox::rejected, &confirmDlg,
                                 &QDialog::reject);
                cl->addWidget(bb);
                if (confirmDlg.exec() != QDialog::Accepted)
                  return;
              }
              // D1b：LAS 解析/SEG-Y 索引等大文件在任务池跑——无任务服务时保持
              // 同步旧路径。imported 信号照常排队回 GUI（预览标签在终态后开）。
              // D1b：LAS 解析/SEG-Y 索引等大文件在任务池跑（编排在
              // FolderImportWorkflow）；imported 信号照常排队回 GUI。
              if (auto *wf = folderImportWorkflow())
                wf->importFile(kind, path, [](const QString &assetId,
                                              const QString &err) {
                  if (assetId.isEmpty())
                    QgsMessageLog::logMessage(
                        QObject::tr("Import failed: %1").arg(err),
                        QStringLiteral("Paleo"), Qgis::Critical);
                });
            });
  }
    // Imported assets feed the bottom-dock correlation panel and the central
    // preview tabs (§4: the preview tab opens only after the import is
    // confirmed; 地震预览不再走底栏面板，预览壳直接按资产渲染测线控件)。
    DataPreviewTabs *previewForImport = m_previewTabs;
    connect(m_previewDoc, &PreviewDocService::assetImported, this,
            [this, corrPanel, previewForImport](const QString &kind, const QString &assetId, const QString &) {
              // 文件夹导入期间不逐文件开标签——确认后只开井口标签（§4）。
              if (previewForImport && !m_folderImportActive)
                previewForImport->openAsset(assetId);
              if (corrPanel && kind == QLatin1String("well_log"))
              {
                QList<QPair<QString, QString>> wells;
                for (const QString &id : m_previewDoc->assetIds(QStringLiteral("well_log")))
                  wells.append({id, m_previewDoc->assetSource(id)});
                corrPanel->setWells(wells);
                // LAS imports pull the GR curve into the column when present.
                const QString src = m_previewDoc->assetSource(assetId);
                if (src.endsWith(QLatin1String(".las"), Qt::CaseInsensitive))
                  corrPanel->loadWellLas(assetId,
                                         QDir(m_projectSvc ? QFileInfo(m_projectSvc->projectPath()).absolutePath()
                                                           : QString()).absoluteFilePath(src),
                                         QStringLiteral("GR"));
              }
            });
}

// ---------------------------------------------------------------------------
// 预测页接线（W4 拆分段）
// ---------------------------------------------------------------------------
void PaleoMainWindow::attachPredictPage(PredictPage *predictPage,
                                        PredictionWorkflow *pred)
{
  if (pred && predictPage)
  {
    // ---- m2(A): 预测运行任务化 + 历史结果显示接线 ----
    // 任务池在场 → runPrediction 跑 worker 线程（模仿导入任务化样例：结果经
    // PaleoTask 终态回 GUI；changed→进度、finished→解忙+失败文案）；取消是
    // 协作式的——预测运算本体无法中断，取消后以实际完成状态如实呈现。
    // 无任务服务时保持同步旧路径。
    connect(predictPage, &PredictPage::runRequested, this,
            [this, pred, predictPage](const QString &horizon, const QString &algId,
                                      const QVariantMap &params) {
              auto *status = predictPage->findChild<QLabel *>(QStringLiteral("statusLabel"));
              const auto logFail = [status](const QString &msg) {
                if (status)
                  status->setText(msg);
                QgsMessageLog::logMessage(msg, QStringLiteral("Paleo"), Qgis::Critical);
              };
              if (!m_taskSvc)
              {
                QString err;
                pred->runPrediction(horizon, algId, params, &err);
                return;
              }
              predictPage->setRunBusy(true);
              auto outErr = std::make_shared<QString>();
              PaleoTask *task = m_taskSvc->start(
                  tr("预测 %1 · %2").arg(horizon, algId),
                  [pred, horizon, algId, params, outErr](PaleoTask *) -> QString {
                    QString err;
                    if (pred->runPrediction(horizon, algId, params, &err))
                      return QString();
                    return err.isEmpty() ? QStringLiteral("prediction failed") : err;
                  });
              QObject::connect(task, &PaleoTask::changed, predictPage,
                               [predictPage, task] {
                                 const int pct = task->percent();
                                 if (pct >= 0) // 无进度回调的算法不伪造进度
                                   predictPage->updateProgress(pct);
                               });
              QObject::connect(task, &PaleoTask::finished, predictPage,
                               [predictPage, task, outErr, status, logFail] {
                                 predictPage->setRunBusy(false);
                                 if (task->state() == PaleoTask::State::Cancelled)
                                 {
                                   const QString msg = tr(
                                       "已请求取消——预测运算无法中断，结果以实际完成为准");
                                   if (status)
                                     status->setText(msg);
                                   QgsMessageLog::logMessage(msg, QStringLiteral("Paleo"),
                                                             Qgis::MessageLevel::Warning);
                                 }
                                 else if (task->state() == PaleoTask::State::Failed)
                                 {
                                   logFail(tr("预测失败：%1").arg(*outErr));
                                 }
                               });
            });
    // 取消按钮 → 协作式取消请求（worker 自查 cancelRequested）。
    connect(predictPage, &PredictPage::runCancelRequested, this, [this]( ) {
      if (!m_taskSvc || m_taskSvc->tasks().isEmpty())
        return;
      // 最近一个「预测 ·」任务是本页发起的运行（页内同一时刻至多一个）。
      for (int i = m_taskSvc->tasks().size() - 1; i >= 0; --i)
      {
        PaleoTask *t = m_taskSvc->tasks().at(i);
        if (t->running() && t->title().startsWith(tr("预测")))
        {
          t->requestCancel();
          return;
        }
      }
    });
    // 历史结果「显示」：instantiate → 图层树勾选 → zoomToLayer（§37 按需实例化）。
    connect(predictPage, &PredictPage::showResultRequested, this,
            [this](const QString &layerId) {
              if (!m_layerSvc)
                return;
              QString err;
              QgsMapLayer *layer = m_layerSvc->instantiate(layerId, &err);
              if (!layer)
              {
                QgsMessageLog::logMessage(tr("Show on map failed: %1").arg(err),
                                          QStringLiteral("Paleo"), Qgis::Critical);
                return;
              }
              if (m_canvasCtl)
                m_canvasCtl->zoomToLayer(layerId);
              QgsProject *proj = m_projectSvc ? m_projectSvc->project() : nullptr;
              if (QgsLayerTreeLayer *node =
                      proj ? proj->layerTreeRoot()->findLayer(layer->id()) : nullptr)
                node->setItemVisibilityChecked(true); // 显示意图（可能已在）
            });
    // ---- m2(A) end ----
  }
}

// ---------------------------------------------------------------------------
// 约束页接线：绘制捕获 + IDW 插值（W4 拆分段）
// ---------------------------------------------------------------------------
void PaleoMainWindow::attachConstraintPage(ConstraintPage *constraintPage,
                                           ConstraintWorkflow *constraint)
{
  if (constraint && constraintPage)
  {
    // 约束页端到端: 绘制请求 → 画布上的捕获工具 → workflow 提交 (§42).
    if (m_canvasCtl)
    {
      auto *drawCtl = new ConstraintDrawController(m_canvasCtl, constraint, this);
      connect(constraintPage, &ConstraintPage::drawConstraintRequested, drawCtl,
              [drawCtl](const QString &horizon, const QString &shape, int faciesCode) {
                drawCtl->startCapture(horizon, shape, faciesCode);
              });
      connect(drawCtl, &ConstraintDrawController::captureFailed, this,
              [](const QString &err) {
                QgsMessageLog::logMessage(err, QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
              });
    }
    connect(constraintPage, &ConstraintPage::runIdwRequested, this,
            [this, constraint, constraintPage](const QString &horizon) {
              auto *status = constraintPage->findChild<QLabel *>(QStringLiteral("statusLabel"));
              const auto fail = [status](const QString &msg) {
                if (status)
                  status->setText(msg);
                QgsMessageLog::logMessage(msg, QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
              };

              QString field = QStringLiteral("z");
              if (auto *edit = constraintPage->findChild<QLineEdit *>(QStringLiteral("idwField")))
              {
                const QString typed = edit->text().trimmed();
                if (!typed.isEmpty())
                  field = typed;
              }
              double cellSize = 1.0;
              if (auto *spin = constraintPage->findChild<QDoubleSpinBox *>(QStringLiteral("idwCellSize")))
                cellSize = spin->value();

              QString pointsId;
              if (!m_layerSvc)
              {
                fail(tr("图层服务未就绪"));
                return;
              }
              QVector<LayerDeclaration> decls;
              QString readErr;
              if (!m_layerSvc->tryDeclared(&decls, &readErr))
              {
                fail(readErr.isEmpty() ? tr("无法读取图层清单") : readErr);
                return;
              }
              QString agnostic;
              for (const LayerDeclaration &d : decls)
              {
                if (d.type.compare(QStringLiteral("vector"), Qt::CaseInsensitive) != 0)
                  continue;
                if (!d.layerId.startsWith(QStringLiteral("wells")))
                  continue;
                if (d.horizon == horizon)
                {
                  pointsId = d.layerId;
                  break;
                }
                if (agnostic.isEmpty() && d.horizon.isEmpty())
                  agnostic = d.layerId;
              }
              if (pointsId.isEmpty())
                pointsId = agnostic;
              if (pointsId.isEmpty())
              {
                fail(tr("层位 %1 没有井点图层").arg(horizon));
                return;
              }

              QString err;
              if (!constraint->runConstraintIDW(horizon, pointsId, field, cellSize, &err))
                fail(err.isEmpty() ? tr("约束插值失败") : err);
            });

    // ---- m2(B): 单因素图页（deliverable 2）接线——三入口的类型化捕获。----
    // 状态列刷新：页面监听 layerDeclared/factorGenerated（含重开工程重读）。
    if (m_layerSvc)
      constraintPage->bindLayerService(m_layerSvc);
    // 生成链：generateFactorRequested(factorId, horizon, params) →
    // ConstraintWorkflow::generateFactor（井点解析在 workflow 侧）。
    connect(constraintPage, &ConstraintPage::generateFactorRequested, this,
            [this, constraint, constraintPage](const QString &factorId, const QString &horizon,
                                               const QVariantMap &params) {
              auto *status = constraintPage->findChild<QLabel *>(QStringLiteral("statusLabel"));
              QString err;
              if (!constraint->generateFactor(horizon, factorId, params, &err))
              {
                const QString msg = err.isEmpty() ? tr("单因素生成失败") : err;
                if (status)
                  status->setText(msg);
                QgsMessageLog::logMessage(msg, QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
              }
            });
    // 等值线（§12 GIS LineString）：horizon 从 factor layerId 前缀取（"factor.<h>.<fid>"）。
    connect(constraintPage, &ConstraintPage::contourRequested, this,
            [this, constraint, constraintPage](const QString &factorLayerId, double interval) {
              const QString horizon = factorLayerId.startsWith(QStringLiteral("factor."))
                                         ? factorLayerId.mid(QStringLiteral("factor.").size())
                                              .section(QLatin1Char('.'), 0, 0)
                                         : QString();
              auto *status = constraintPage->findChild<QLabel *>(QStringLiteral("statusLabel"));
              QString err;
              if (!constraint->generateContours(horizon, factorLayerId, interval, &err))
              {
                const QString msg = err.isEmpty() ? tr("等值线生成失败") : err;
                if (status)
                  status->setText(msg);
                QgsMessageLog::logMessage(msg, QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
              }
            });
    // 互斥上图：visible=true → instantiate + 图层树勾选该层，04_SingleFactor 组
    // 其它已实例化层取消勾选；false 只取消该层（业务上同时只看一张单因素图）。
    connect(constraintPage, &ConstraintPage::factorVisibilityRequested, this,
            [this](const QString &layerId, bool visible) {
              if (!m_layerSvc || layerId.isEmpty())
                return;
              QgsProject *proj = m_projectSvc ? m_projectSvc->project() : nullptr;
              if (!proj)
                return;
              const auto setNodeChecked = [this, proj](const QString &id, bool checked) {
                QgsMapLayer *layer = m_layerSvc->layer(id); // 只拨已实例化层
                if (!layer)
                  return;
                if (QgsLayerTreeLayer *node = proj->layerTreeRoot()->findLayer(layer->id()))
                  node->setItemVisibilityChecked(checked);
              };
              if (visible)
              {
                QString err;
                QgsMapLayer *layer = m_layerSvc->instantiate(layerId, &err);
                if (!layer)
                {
                  QgsMessageLog::logMessage(
                      tr("单因素上图失败：%1").arg(err.isEmpty() ? layerId : err),
                      QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
                  return;
                }
                // 组内互斥：04_SingleFactor（含 Contours 子组，按前缀）先全下。
                QVector<LayerDeclaration> decls;
                if (m_layerSvc->tryDeclared(&decls))
                {
                  for (const LayerDeclaration &d : decls)
                  {
                    if (d.layerId == layerId)
                      continue;
                    if (d.group == QLatin1String("04_SingleFactor") ||
                        d.group.startsWith(QLatin1String("04_SingleFactor/")))
                      setNodeChecked(d.layerId, false);
                  }
                }
                setNodeChecked(layerId, true);
              }
              else
              {
                setNodeChecked(layerId, false);
              }
            });
    // 三入口（物源线/展布线/控制点）：类型化捕获工具（type 列落地质类型词表）。
    if (m_canvasCtl)
    {
      auto *typedCtl = new TypedConstraintDrawController(m_canvasCtl, constraint, this);
      connect(constraintPage, &ConstraintPage::drawTypedConstraintRequested, typedCtl,
              [typedCtl](const QString &horizon, const QString &shape,
                         const QString &constraintType, int faciesCode) {
                typedCtl->startCapture(horizon, shape, constraintType, faciesCode);
              });
      connect(typedCtl, &TypedConstraintDrawController::captureFailed, this,
              [](const QString &err) {
                QgsMessageLog::logMessage(err, QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
              });
    }
    // ---- m2(B) end ----
  }
}

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
            });
    connect(composePage, &ComposePage::polygonizeRequested, this,
            [this, compose, composePage](const QString &rasterId, double minArea, double simplify) {
              QString err;
              const QString horizon = m_selection ? m_selection->activeHorizon() : QString();
              QVariantMap params;
              params.insert(QStringLiteral("MIN_AREA"), minArea);
              params.insert(QStringLiteral("SIMPLIFY"), simplify);
              if (compose->deriveFaciesPolygons(horizon, rasterId, params, &err))
                composePage->refreshFactors();
              else if (!err.isEmpty())
                QgsMessageLog::logMessage(err, QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
            });

    // ---- m2(C): 矢量化成功 → 自动进入相界编辑态（z3 PaleoVertexTool）------
    // 拿 facies.<horizon> 矢量层 instantiate → 编辑条选层 + 触发顶点工具
    // （QgsVertexTool 是 app-only，顶点编辑走 PaleoVertexTool；snapping/
    // 拓扑在画布控制器侧已开）。派生 gpkg 按 T26 纪律只读——进编辑前先铺
    // 可编辑工作副本（prepareFaciesForEditing）。编辑条缺席（无画布环境）
    // → 降级为选中层 + 状态文案提示手动进入编辑。
    connect(compose, &CompositionWorkflow::faciesPolygonsReady, this,
            [this, compose, composePage](const QString &h, const QString &layerId) {
              Q_UNUSED(h);
              composePage->setFaciesEditTarget(layerId);
              if (!m_layerSvc)
                return;
              QString target = layerId;
              QString prepErr;
              const QString prepared = compose->prepareFaciesForEditing(layerId, &prepErr);
              if (prepared.isEmpty())
                QgsMessageLog::logMessage(
                    tr("相界工作副本铺设失败：%1").arg(prepErr), QStringLiteral("Paleo"),
                    Qgis::MessageLevel::Warning);
              else
                target = prepared;
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
                  node->setItemVisibilityChecked(visible);
            });

    // ---- m2(C): 在布局设计器中打开（layoutdesignershell 公共入口）---------
    // 打开（或新建）本层位的版面：优先复用导出同名布局 "<h>_map"；新空布局
    // 没有地图项时主题钉定自然跳过（设计器里由模板/手工补地图项）。
    connect(composePage, &ComposePage::layoutDesignerRequested, this,
            [this, layoutSvc]() {
              if (!layoutSvc)
              {
                QgsMessageLog::logMessage(tr("布局服务未接入 — 无法打开图件设计器"),
                                          QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
                return;
              }
              if (m_projectSvc->projectPath().isEmpty())
              {
                QgsMessageLog::logMessage(tr("无打开工程 — 无法打开图件设计器"),
                                          QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
                return;
              }
              const QString h = m_selection ? m_selection->activeHorizon() : QString();
              const QString name = h.isEmpty() ? tr("编图布局")
                                               : QStringLiteral("%1_map").arg(h);
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
              shell->setAttribute(Qt::WA_DeleteOnClose);
              shell->setModal(false);
              shell->show();
            });
    // ---- m2(C) end ----
  }
}

// ---------------------------------------------------------------------------
// m2(C) 接缝：版面地图项钉页面档案主题（实现说明见 paleomainwindow.h）
// ---------------------------------------------------------------------------
void PaleoMainWindow::pinLayoutTheme(QgsLayoutItemMap *mapItem, const QString &pageId)
{
  const QString theme = QgisLayerProfileService::pageThemeName(pageId);
  if (m_profileSvc)
  {
    m_profileSvc->setLayoutMapTheme(mapItem, theme);
    return;
  }
  mapItem->setFollowVisibilityPreset(true);
  mapItem->setFollowVisibilityPresetName(theme);
}

// ---------------------------------------------------------------------------
// 验证页接线：三方定位 + 剖面跳页 + 发布门刷新钩（W4 拆分段）
// ---------------------------------------------------------------------------
void PaleoMainWindow::attachValidatePage(ValidatePage *validatePage,
                                         ValidationWorkflow *validate,
                                         WellCorrelationPanel *corrPanel,
                                         DataImportService *importSvc)
{
  if (validate && validatePage)
  {
    // 问题 → 地图/连井联动（阶段C，预览壳重排）：地图移到井点 + 连井滚到
    // 井/分层；地震测线不再走底栏 gotoLine，由「在数据页看这条剖面」显式
    // 切页打开（threewaylocator.h）。缺面板的字段自动跳过。
    auto *bottomTabs = findChild<QTabWidget *>(QStringLiteral("bottomTabs"));
    auto *threeWay = new ThreeWayLocator(m_canvasCtl, this);
    connect(validatePage, &ValidatePage::locateRequested, threeWay,
            &ThreeWayLocator::locate);
    // 联动器的意图信号回壳订阅：底栏切到连井剖面页 + 滚到井分层。
    connect(threeWay, &ThreeWayLocator::bottomTabFocusRequested, this,
            [this, bottomTabs, corrPanel](const QString &tabId) {
              if (tabId == QLatin1String("correlation") && bottomTabs && corrPanel)
                bottomTabs->setCurrentWidget(corrPanel);
            });
    connect(threeWay, &ThreeWayLocator::correlationFocusRequested, this,
            [corrPanel](const QString &wellId, const QString &horizon) {
              if (corrPanel)
                corrPanel->scrollToWellTop(wellId, horizon);
            });
    // 「在数据页看这条剖面」：切到数据管理页（预览分栏随之可见），打开/
    // 聚焦地震资产标签并把测线拨到载荷里的 inline/time_ms。载荷没有
    // asset_id——测线号落在哪个 survey 的 inline 范围就开它的链接资产
    // （主关联优先）；一条地震资产都没有就不跳页，不造假定位。
    connect(validatePage, &ValidatePage::seismicSectionRequested, this,
            [this, importSvc](const QVariantMap &payload) {
              if (!m_previewTabs || !importSvc)
                return;
              const int line = payload.value(QStringLiteral("inline"), -1).toInt();
              if (line < 0)
                return;
              const double timeMs =
                  payload.value(QStringLiteral("time_ms"), -1.0).toDouble();
              DataCatalog *cat = m_previewDoc->catalog();
              QString assetId, firstSeismic;
              for (const EntityAssetLink &l : cat->links())
              {
                if (l.unresolved || l.role != QLatin1String("seismic_volume") ||
                    l.assetId.isEmpty())
                  continue;
                if (firstSeismic.isEmpty())
                  firstSeismic = l.assetId;
                const CatalogEntity s = cat->entityById(l.entityId);
                if (s.entityType == QLatin1String("seismic_survey") &&
                    line >= s.inlineMin && line <= s.inlineMax &&
                    (assetId.isEmpty() || l.isPrimary))
                  assetId = l.assetId;
              }
              if (assetId.isEmpty())
                assetId = firstSeismic;
              if (assetId.isEmpty())
                return;
              showPage(QStringLiteral("data"));
              m_previewTabs->openSeismicLine(
                  assetId, QStringLiteral("inline"), line, timeMs);
            });
    // 阶段E 发布门：验证跑完 → 重算逐井残差覆盖（attachMapping 装的钩子，
    // 未装则无事发生）。
    connect(validate, &ValidationWorkflow::validationDone, this,
            [this](int) { if (m_refreshPublishGate) m_refreshPublishGate(); });
  }
}

// ---------------------------------------------------------------------------
// 壳面接线：locator / 保存 / 底栏面板 / 处理算法 / 编辑工具 / 图件设计
// （W4 拆分段）——返回逻辑宿主编辑条供 buildRibbonPanels 镜像进 ribbon。
// ---------------------------------------------------------------------------
PaleoEditingToolbar *PaleoMainWindow::attachShellSurfaces(
    PaleoProjectStore *store, QgisProcessingService *procSvc,
    QgisEditingService *editSvc, QgisLayoutService *layoutSvc,
    PaleoTaskService *taskSvc)
{
  // ---- ribbon 右侧组的搜索槽 + 快速访问栏的保存 ----
  if (auto *topBar = findChild<QWidget *>(QStringLiteral("locatorSlot")))
  {
    if (m_layerSvc && m_selection && m_canvasCtl)
    {
      auto *locatorWidget = new QgsLocatorWidget(topBar);
      locatorWidget->setObjectName(QStringLiteral("paleoLocator"));
      locatorWidget->setMapCanvas(m_canvasCtl->canvas());
      locatorWidget->setPlaceholderText(tr("搜索井位/层位  Ctrl+K"));
      locatorWidget->setMinimumWidth(220);

      // Wells filter: resolve the declared "wells" layer lazily (instantiate
      // on demand — the search must not force materialization at open time).
      WellLocatorFilter::WellLayerProvider wellProvider = [this]() {
        QPair<QgsVectorLayer *, QString> out{nullptr, QString()};
        QgsMapLayer *l = m_layerSvc->layer(QStringLiteral("wells"));
        if (!l)
          l = m_layerSvc->instantiate(QStringLiteral("wells"));
        auto *vl = qobject_cast<QgsVectorLayer *>(l);
        if (!vl)
          return out;
        const int nameIdx = vl->fields().lookupField(QStringLiteral("name"));
        out.first = vl;
        out.second = nameIdx >= 0 ? QStringLiteral("name")
                                  : (vl->fields().isEmpty() ? QString()
                                                            : vl->fields().at(0).name());
        return out;
      };
      locatorWidget->locator()->registerFilter(
          new WellLocatorFilter(wellProvider, m_canvasCtl->canvas()));

      // Horizons filter: manifest horizon set → activate + materialize.
      HorizonLocatorFilter::HorizonListProvider horizonProvider = [this]() {
        QStringList hs;
        QVector<LayerDeclaration> declared;
        QString manifestErr;
        if (!m_layerSvc || !m_layerSvc->tryDeclared(&declared, &manifestErr))
        {
          QgsMessageLog::logMessage(tr("Horizon locator: manifest read failed: %1")
                                        .arg(manifestErr),
                                    QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
          return hs;
        }
        for (const LayerDeclaration &d : declared)
          if (!d.horizon.isEmpty() && !hs.contains(d.horizon))
            hs << d.horizon;
        hs.sort();
        return hs;
      };
      HorizonLocatorFilter::ActivateFn activate = [this](const QString &h) {
        if (m_selection)
          m_selection->setActiveHorizon(h);
        if (m_layerSvc)
          m_layerSvc->setActiveHorizon(h);
      };
      locatorWidget->locator()->registerFilter(
          new HorizonLocatorFilter(horizonProvider, activate));

      // Note: no IssueLocatorFilter registration — the validation workflow
      // keeps no queryable issue store, so the filter could only ever sit on
      // a permanently-empty provider. Issue navigation lives on the
      // validation page's issueTable → ThreeWayLocator path instead.

      topBar->layout()->addWidget(locatorWidget);
      auto *focus = new QShortcut(QKeySequence(QStringLiteral("Ctrl+K")), this);
      connect(focus, &QShortcut::activated, locatorWidget,
              [locatorWidget] { locatorWidget->search(QString()); });
    }

    if (store && m_projectSvc)
    {
      // 保存 = 快速访问栏第一颗（Office 惯例）+「文件」菜单；Ctrl+S 走同一函数。
      auto *saveAct = new QAction(PaleoIcons::qgisTheme(QStringLiteral("mActionFileSave.svg")),
                                  tr("保存工程"), this);
      saveAct->setObjectName(QStringLiteral("saveProjectAction"));
      saveAct->setToolTip(tr("保存工程（Ctrl+S）"));
      // §41.2 ordering through the write queue: gpkg commit (no-op until edit
      // buffers report dirty state) then the atomic .qgz write.
      auto saveFn = [this, store]() {
        if (m_projectSvc->projectPath().isEmpty())
        {
          QgsMessageLog::logMessage(tr("无打开工程 — 无法保存"),
                                  QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
          return;
        }
        const auto res = store->saveAll(
            [] { return PaleoProjectStore::WriteResult{true, QString()}; },
            [this] {
              saveCanvasExtent(); // display state travels inside the .qgz
              const bool ok = m_projectSvc->writeProject();
              return PaleoProjectStore::WriteResult{
                  ok, ok ? QString() : m_projectSvc->lastErrors().join(QLatin1Char(';'))};
            });
        QgsMessageLog::logMessage(
            res.ok ? tr("工程已保存") : tr("保存失败：%1").arg(res.error),
            QStringLiteral("Paleo"),
            res.ok ? Qgis::MessageLevel::Info : Qgis::MessageLevel::Critical);
      };
      connect(saveAct, &QAction::triggered, this, saveFn);
      auto *saveShortcut = new QShortcut(QKeySequence::Save, this);
      connect(saveShortcut, &QShortcut::activated, this, saveFn);
      if (SARibbonQuickAccessBar *qab = ribbonBar()->quickAccessBar())
      {
        qab->addAction(saveAct);
        if (auto *saveBtn = qobject_cast<QToolButton *>(qab->widgetForAction(saveAct)))
        {
          saveBtn->setObjectName(QStringLiteral("saveButton"));
          saveBtn->setAccessibleName(tr("保存工程"));
        }
      }
      if (auto *fileMenu = findChild<QMenu *>(QStringLiteral("fileMenu")))
        for (QAction *a : fileMenu->actions())
          if (a->objectName() == QLatin1String("fileMenuSaveAnchor"))
          {
            fileMenu->insertAction(a, saveAct);
            fileMenu->insertSeparator(saveAct);
            break;
          }
    }
  }

  // Release management tab in the bottom dock.
  if (store)
    if (auto *bottomTabs = findChild<QTabWidget *>(QStringLiteral("bottomTabs")))
    {
      auto *releasePanel = new ReleasePanel(bottomTabs);
      releasePanel->setObjectName(QStringLiteral("releasePanel"));
      releasePanel->setProviders(
          [store]() { return store->metaDbPath(); },
          [this]() {
            QVector<LayerDeclaration> declared;
            QString manifestErr;
            if (m_layerSvc && !m_layerSvc->tryDeclared(&declared, &manifestErr))
              QgsMessageLog::logMessage(tr("Release panel: manifest read failed: %1")
                                          .arg(manifestErr),
                                      QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
            return declared;
          });
      connect(releasePanel, &ReleasePanel::statusMessage, this,
              [](const QString &msg) {
                QgsMessageLog::logMessage(msg, QStringLiteral("Paleo"), Qgis::MessageLevel::Info);
              });
      connect(m_projectSvc, &QgisProjectService::projectOpened, releasePanel,
              &ReleasePanel::refresh);
      bottomTabs->addTab(releasePanel, QStringLiteral("发布"));
    }

  // Task panel replaces the placeholder in the 任务 tab (index 1); attribute
  // table joins as its own tab, fed by the instantiated-layer set.
  if (store)
    if (auto *bottomTabs = findChild<QTabWidget *>(QStringLiteral("bottomTabs")))
    {
      const int idx = bottomTabs->indexOf(
          bottomTabs->findChild<QTextEdit *>(QStringLiteral("tasksPlaceholder")));
      auto *taskPanel = new TaskPanel(store, taskSvc, bottomTabs);
      if (idx >= 0)
      {
        QWidget *old = bottomTabs->widget(idx);
        bottomTabs->removeTab(idx);
        delete old;
        bottomTabs->insertTab(idx, taskPanel, QStringLiteral("任务"));
      }
      else
        bottomTabs->addTab(taskPanel, QStringLiteral("任务"));

      if (m_layerSvc && m_canvasCtl)
      {
        auto *attrPanel = new AttributeTablePanel(
            m_canvasCtl->canvas(),
            [this](const QString &layerId) -> QgsVectorLayer * {
              QgsMapLayer *l = m_layerSvc->layer(layerId);
              if (!l)
                l = m_layerSvc->instantiate(layerId);
              return qobject_cast<QgsVectorLayer *>(l);
            },
            bottomTabs);
        attrPanel->setObjectName(QStringLiteral("attributeTablePanel"));
        auto refreshIds = [this, attrPanel] {
          QVector<LayerDeclaration> declared;
          QString manifestErr;
          if (!m_layerSvc->tryDeclared(&declared, &manifestErr))
          {
            QgsMessageLog::logMessage(tr("Attribute panel: manifest read failed: %1")
                                          .arg(manifestErr),
                                      QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
            return; // keep the existing layer list instead of blanking it
          }
          QStringList ids;
          for (const LayerDeclaration &d : declared)
            ids << d.layerId;
          attrPanel->setLayerIds(ids);
        };
        refreshIds();
        connect(m_layerSvc, &QgisLayerService::layerInstantiated, this,
                [refreshIds](const QString &) { refreshIds(); });
        connect(m_projectSvc, &QgisProjectService::projectOpened, this,
                [refreshIds](const QString &) { refreshIds(); });
        bottomTabs->addTab(attrPanel, QStringLiteral("属性表"));
      }
    }

  // Processing entry point in the ribbon's right group (global tool, like the
  // QGIS Processing Toolbox): paleo:* algorithms first-class, the full
  // registry grouped under per-provider submenus. Each item opens the native
  // QGIS algorithm dialog (non-blocking, offscreen-safe).
  if (procSvc)
  {
    if (SARibbonButtonGroupWidget *topBar = ribbonBar()->rightButtonGroup())
    {
      auto *btn = new QToolButton(topBar);
      btn->setObjectName(QStringLiteral("processingButton"));
      btn->setText(tr("处理算法"));
      btn->setAccessibleName(tr("处理算法选择"));
      btn->setIcon(PaleoIcons::qgisTheme(QStringLiteral("processingAlgorithm.svg")));
      btn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
      btn->setPopupMode(QToolButton::InstantPopup);
      auto *menu = new QMenu(btn);

      for (const QString &id : procSvc->paleoAlgorithmIds())
        menu->addAction(id, this,
                        [this, procSvc, id]() {
                          QString err;
                          if (!procSvc->showAlgorithmDialog(id, QVariantMap(), this, &err))
                            QgsMessageLog::logMessage(err, QStringLiteral("Paleo"),
                                                      Qgis::MessageLevel::Warning);
                        });

      QMap<QString, QMenu *> providerMenus;
      for (const QString &id : procSvc->algorithmIds())
      {
        if (id.startsWith(QStringLiteral("paleo:")))
          continue;
        const QString provider = id.section(QLatin1Char(':'), 0, 0);
        QMenu *&sub = providerMenus[provider];
        if (!sub)
          sub = menu->addMenu(provider);
        const QString algName = id.section(QLatin1Char(':'), 1);
        sub->addAction(algName.isEmpty() ? id : algName, this,
                       [this, procSvc, id]() {
                         QString err;
                         if (!procSvc->showAlgorithmDialog(id, QVariantMap(), this, &err))
                           QgsMessageLog::logMessage(err, QStringLiteral("Paleo"),
                                                     Qgis::MessageLevel::Warning);
                       });
      }

      btn->setMenu(menu);
      // 插在「面板」钮之前：[搜索][处理算法][面板][Web 服务]。
      QAction *before = nullptr;
      if (auto *panelsBtn = topBar->findChild<QToolButton *>(QStringLiteral("panelsMenuButton")))
        for (QAction *a : topBar->actions())
          if (topBar->widgetForAction(a) == panelsBtn)
            before = a;
      topBar->insertWidget(before, btn);
    }
  }

  // 编辑工具 (wave/edit-tools): digitizing toolset — add/reshape/move/delete +
  // vertex editing routed through the editing service; undo/redo follows the
  // selected layer. Ribbon 形态：PaleoEditingToolbar 只当逻辑宿主（工具
  // 生命周期、图层下拉、门控/原因），本体不上屏；它的 QAction 进三个编图页
  // 的「要素编辑」组（buildRibbonPanels），撤销/重做进快速访问栏。页作用域
  // = 只有编图链三页的页签里有这组命令。
  PaleoEditingToolbar *editTb = nullptr;
  if (m_canvasCtl)
  {
    editTb = new PaleoEditingToolbar(m_canvasCtl->canvas(), this);
    editTb->setObjectName(QStringLiteral("editingToolbar"));
    if (editSvc)
      editTb->setEditingService(editSvc);
    if (m_projectSvc)
      editTb->setProject(m_projectSvc->project());
    editTb->refreshFromProject();
    if (m_projectSvc)
    {
      connect(m_projectSvc, &QgisProjectService::projectOpened, editTb,
              [editTb](const QString &) { editTb->refreshFromProject(); });
      if (QgsProject *proj = m_projectSvc->project())
      {
        connect(proj, &QgsProject::layersAdded, editTb,
                [editTb](const QList<QgsMapLayer *> &) { editTb->refreshFromProject(); });
        connect(proj, &QgsProject::layersRemoved, editTb,
                [editTb](const QStringList &) { editTb->refreshFromProject(); });
      }
    }
    editTb->hide(); // 逻辑宿主，不进布局
    if (SARibbonQuickAccessBar *qab = ribbonBar()->quickAccessBar())
    {
      qab->addAction(editTb->actionUndo());
      qab->addAction(editTb->actionRedo());
    }
  }

  // 图件设计 entry (wave/layout-designer): create a print layout via the
  // layout service and open the designer shell dialog non-modally. 入口在
  // 「智能编图 › 图件输出」组（buildRibbonPanels 按 objectName 取这颗动作）。
  if (layoutSvc)
    {
      auto *designerAct = new QAction(
          PaleoIcons::qgisTheme(QStringLiteral("mActionNewLayout.svg")), tr("图件设计"), this);
      designerAct->setObjectName(QStringLiteral("ribbonDesignerAction"));
      designerAct->setToolTip(tr("新建布局并打开图件设计器"));
      connect(designerAct, &QAction::triggered, this, [this, layoutSvc] {
        if (m_projectSvc->projectPath().isEmpty())
        {
          QgsMessageLog::logMessage(tr("无打开工程 — 无法创建布局"),
                                  QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
          return;
        }
        static int s_layoutSeq = 0;
        QString err;
        QgsLayout *layout = layoutSvc->createLayout(
            tr("布局 %1").arg(++s_layoutSeq), &err);
        if (!layout)
        {
          QgsMessageLog::logMessage(tr("创建布局失败：%1").arg(err),
                                  QStringLiteral("Paleo"), Qgis::MessageLevel::Critical);
          return;
        }
        auto *shell = new PaleoLayoutDesignerShell(layout, this);
        shell->setAttribute(Qt::WA_DeleteOnClose);
        shell->setModal(false);
        shell->show();
      });
    }
  return editTb;
}

// ---------------------------------------------------------------------------
// wave/mapping-pipeline 阶段C+E — 编图链 / 层位图导出 / 版本状态机接线。
// W4：attachMapping 按段拆（发布门 → 导出接线 → 版本状态机），入口只排
// 顺序 + 早退守卫；发布门本体存成员 m_refreshPublishGate（其他段经成员
// 重算，不靠 lambda 捕获串接）。
// ---------------------------------------------------------------------------
void PaleoMainWindow::attachMapping(MappingWorkflow *mapping, MapVersionController *versions,
                                    MapVersionStore *versionStore, ProjectDataFacade *projectData,
                                    DataCatalog *catalog)
{
  auto *composePage = findChild<ComposePage *>();
  if (!composePage || !mapping || !versions)
    return;

  attachMappingPublishGate(composePage, versions, versionStore, projectData, catalog);
  attachMappingExport(composePage, mapping, versionStore, projectData, catalog);
  attachMappingVersions(composePage, versions);

  // 工程打开时恢复发布门状态（版本行的 PDF 资产 + 残差覆盖重算）。
  if (m_projectSvc)
    connect(m_projectSvc, &QgisProjectService::projectOpened, this,
            [this]() { if (m_refreshPublishGate) m_refreshPublishGate(); });
  if (m_refreshPublishGate)
    m_refreshPublishGate(); // 初始态：缺什么写什么，按钮禁用
}

// ---------------------------------------------------------------------------
// 发布门（§177/§260）：版本行上的 PDF 资产 id + 逐井残差摘要完整性共同
// 决定按钮状态；导出成功 / 保存 / 发布 / 验证跑完 / 工程打开后都重算。
// ---------------------------------------------------------------------------
void PaleoMainWindow::attachMappingPublishGate(ComposePage *composePage,
                                               MapVersionController *versions,
                                               MapVersionStore *versionStore,
                                               ProjectDataFacade *projectData,
                                               DataCatalog *catalog)
{
  const auto status = [composePage](const QString &text) {
    if (auto *label = composePage->findChild<QLabel *>(QStringLiteral("statusLabel")))
      label->setText(text);
  };
  const auto activeHorizon = [this]() -> QString {
    return m_selection ? m_selection->activeHorizon() : QString();
  };
  // 发布门（§177/§260）：版本行上的 PDF 资产 id + 逐井残差摘要完整性共同
  // 决定按钮状态；tooltip 写缺的那条。导出成功 / 保存 / 发布 / 验证跑完 /
  // 工程打开后都重算 —— 门是「当前状态」而不是一次性开关。
  const auto refreshPublishGate = [this, versionStore, projectData, catalog]() {
    auto *page = findChild<ComposePage *>();
    if (!page)
      return;
    const QString h = m_selection ? m_selection->activeHorizon() : QString();
    const MapVersion v = (versionStore && !h.isEmpty()) ? versionStore->latest(h)
                                                       : MapVersion();
    const QString summary = h.isEmpty()
        ? QString()
        : MapVersionController::residualSummaryJson(projectData, h);
    int covered = -1, total = -1;
    MapVersionStore::residualSummaryComplete(summary, &covered, &total);
    page->setPublishState(!v.pdfAssetId.isEmpty(), covered, total);
    page->setVersionState(v.version, v.state == QLatin1String("Published"));
    // B 包 staleness-lite advisory：目标工程里有过时下游产物 → 发布按钮
    // tooltip 如实列出（可见但不阻断——enable 态仍由 setPublishState 决定）。
    const QString advisory = MapVersionController::stalePublishAdvisory(catalog);
    if (!advisory.isEmpty())
      if (auto *btn = page->findChild<QPushButton *>(QStringLiteral("publishButton")))
        btn->setToolTip(btn->toolTip().isEmpty()
                            ? advisory
                            : btn->toolTip() + QLatin1Char('\n') + advisory);
  };
  m_refreshPublishGate = refreshPublishGate;

  // 逐井残差摘要随发布冻结进版本行。B 包 staleness-lite：有过时下游产物
  // 时确认文案如实列出（advisory——不阻断，Ok/Cancel 照常由人决断）。
  connect(composePage, &ComposePage::publishRequested, this,
          [this, versions, versionStore, projectData, catalog, composePage,
           activeHorizon, status]() {
            const QString h = activeHorizon();
            if (h.isEmpty())
            {
              status(tr("先选择层位再发布"));
              return;
            }
            const QString summary =
                MapVersionController::residualSummaryJson(projectData, h);
            int covered = -1, total = -1;
            MapVersionStore::residualSummaryComplete(summary, &covered, &total);
            const MapVersion v = versionStore ? versionStore->latest(h) : MapVersion();
            const QString pdfName =
                QFileInfo(versionStore ? versionStore->latestLayoutProduct(h) : QString())
                    .fileName();
            const QString advisory =
                MapVersionController::stalePublishAdvisory(catalog);
            const auto choice = QMessageBox::question(
                this, tr("发布版本"),
                tr("发布 %1 v%2？\n\nPDF：%3\n覆盖井数：%4/%5\n\n发布后快照只读，"
                   "继续编辑请保存新版本。")
                    .arg(h)
                    .arg(v.version)
                    .arg(pdfName.isEmpty() ? tr("（未登记）") : pdfName)
                    .arg(covered < 0 ? 0 : covered)
                    .arg(total < 0 ? 0 : total)
                    + (advisory.isEmpty()
                           ? QString()
                           : QStringLiteral("\n\n注意：") + advisory),
                QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel);
            if (choice != QMessageBox::Ok)
              return;
            QString err;
            const QString dir = versions->publish(h, summary, &err);
            if (!dir.isEmpty())
            {
              status(tr("已发布：%1 v%2 → %3").arg(h).arg(v.version).arg(dir));
              composePage->setVersionState(v.version, true);
            }
            else
              status(err.isEmpty() ? tr("发布失败") : err);
            if (m_refreshPublishGate) m_refreshPublishGate();
          });

}

// ---------------------------------------------------------------------------
// 导出接线：链路状态 / 厚度链 / 层位图 PDF 导出 + catalog 登记（W4 拆分段）
// ---------------------------------------------------------------------------
void PaleoMainWindow::attachMappingExport(ComposePage *composePage,
                                          MappingWorkflow *mapping,
                                          MapVersionStore *versionStore,
                                          ProjectDataFacade *projectData,
                                          DataCatalog *catalog)
{
  const auto status = [composePage](const QString &text) {
    if (auto *label = composePage->findChild<QLabel *>(QStringLiteral("statusLabel")))
      label->setText(text);
  };
  const auto activeHorizon = [this]() -> QString {
    return m_selection ? m_selection->activeHorizon() : QString();
  };

  // 链路状态文案走 statusLabel（成功/失败都落页面，再进日志）。
  connect(mapping, &MappingWorkflow::chainDone, this,
          [this, composePage, status](const QString &h, const QString &layerId) {
            status(tr("编图链完成：%1 → %2").arg(h, layerId));
            composePage->refreshFactors();
          });
  connect(mapping, &MappingWorkflow::chainFailed, this,
          [status](const QString &, const QString &error) { status(error); });

  // 「选层位 → 算厚度 → IDW → 转相面」：层位来自 chip 的 activeHorizon，
  // 提示里的目标层位随工程参数（AreaRules targetHorizon）。
  connect(composePage, &ComposePage::thicknessChainRequested, this,
          [this, mapping, activeHorizon, status]() {
            const QString h = activeHorizon();
            if (h.isEmpty())
            {
              status(tr("先在顶部 chip 选择层位（本阶段目标 %1）")
                         .arg(AreaRules::active().targetHorizon));
              return;
            }
            QString err;
            if (!mapping->runThicknessChain(h, &err))
              QgsMessageLog::logMessage(err, QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
          });

  // D8 使能态：无层位时按钮禁用（tooltip 写原因）；chip 切换即时联动命名。
  composePage->setThicknessHorizon(activeHorizon());
  if (m_selection)
    connect(m_selection, &SelectionContext::activeHorizonChanged, composePage,
            &ComposePage::setThicknessHorizon);

  // 层位图 PDF（阶段C+E）：导出 → catalog OUTPUT 资产登记 → 布局产物记录 →
  // 发布门重算。失败弹「导出失败 + 原因 + 重试」；成功弹路径 + SHA-256。
  connect(composePage, &ComposePage::exportPdfRequested, this,
          [this, versionStore, projectData, catalog, activeHorizon, status]() {
            const QString h = activeHorizon();
            if (h.isEmpty() || !m_layerSvc)
            {
              status(tr("先在顶部 chip 选择层位再导出"));
              return;
            }
            const QString projectDir = m_projectSvc
                                           ? QFileInfo(m_projectSvc->projectPath()).absolutePath()
                                           : QDir::temp().absolutePath();
            const QString target = QDir(projectDir).filePath(
                QStringLiteral("%1_map.pdf").arg(h));
            // ---- m2(C): 版面地图项钉本页主题（paleo.page.compose）---------
            // exportHorizonMapPdf 内建布局不外露地图项，这里走同一
            // buildHorizonMapLayout + PaleoLayoutExportActions（300dpi PDF，
            // 与既有导出参数逐字一致），在导出前把布局里 id="map" 的地图项
            // 钉到 compose 页主题（m1 setLayoutMapTheme 接缝；服务缺席时
            // pinLayoutTheme 直写原生 follow-visibility 预设）。
            const auto exportPinned = [this](const QString &horizon, const QString &outPath,
                                             QString *errOut) -> QString {
              QgsPrintLayout *layout = buildHorizonMapLayout(m_layerSvc, m_projectSvc,
                                                             horizon, errOut);
              if (!layout)
                return QString();
              if (QgsLayoutItemMap *mapItem = qobject_cast<QgsLayoutItemMap *>(
                      layout->itemById(QStringLiteral("map"))))
                pinLayoutTheme(mapItem, QStringLiteral("compose"));
              PaleoLayoutExportActions exports;
              const auto outcome =
                  exports.exportLayout(layout, outPath, PaleoLayoutExportActions::Format::Pdf,
                                       300.0, PaleoLayoutExportActions::PageRange());
              delete layout;
              if (!outcome.ok)
              {
                if (errOut)
                  *errOut = outcome.error;
                return QString();
              }
              return outcome.files.value(0);
            };
            QString pdf;
            while (true) // 失败 → 重试 / 取消（§215：导出失败要写原因）
            {
              QString err;
              pdf = exportPinned(h, target, &err);
              if (!pdf.isEmpty())
                break;
              QgsMessageLog::logMessage(err, QStringLiteral("Paleo"), Qgis::MessageLevel::Warning);
              const auto choice = QMessageBox::warning(
                  this, tr("导出失败"),
                  tr("%1\n\n导出目标：%2").arg(err, target),
                  QMessageBox::Retry | QMessageBox::Cancel, QMessageBox::Retry);
              if (choice != QMessageBox::Retry)
              {
                status(err);
                return;
              }
            }

            // 阶段E：登记 catalog OUTPUT 受管资产 —— 登记失败的导出不算完成
            // （发布门要求的是「已登记的 PDF」，不是「写出过文件」）。
            QString sha, managedPath, regErr;
            const QString assetId =
                registerMapPdfAsset(catalog, projectDir, pdf, &sha, &managedPath, &regErr);
            if (assetId.isEmpty())
            {
              status(regErr.isEmpty() ? tr("PDF 资产登记失败") : regErr);
              QgsMessageLog::logMessage(regErr, QStringLiteral("Paleo"),
                                        Qgis::MessageLevel::Warning);
              return;
            }
            if (versionStore)
            {
              QString productErr;
              const QString recorded = managedPath.isEmpty() ? pdf : managedPath;
              if (!versionStore->recordLayoutProduct(h, recorded, assetId, sha, &productErr))
                QgsMessageLog::logMessage(productErr, QStringLiteral("Paleo"),
                                          Qgis::MessageLevel::Warning);
            }
            status(tr("层位图已导出：%1").arg(pdf));
            QMessageBox::information(this, tr("导出成功"),
                                     tr("已导出层位图：\n%1\n\nSHA-256：%2")
                                         .arg(pdf, sha));
            if (m_refreshPublishGate) m_refreshPublishGate();
          });

  // 保存版本：commit + 版本号递增（undo 清空在 controller 内，§1223）；
}

// ---------------------------------------------------------------------------
// 版本状态机：保存版本（commit + 递增）（W4 拆分段）
// ---------------------------------------------------------------------------
void PaleoMainWindow::attachMappingVersions(ComposePage *composePage,
                                            MapVersionController *versions)
{
  const auto status = [composePage](const QString &text) {
    if (auto *label = composePage->findChild<QLabel *>(QStringLiteral("statusLabel")))
      label->setText(text);
  };
  const auto activeHorizon = [this]() -> QString {
    return m_selection ? m_selection->activeHorizon() : QString();
  };

  connect(composePage, &ComposePage::saveVersionRequested, this,
          [this, versions, composePage, activeHorizon, status]() {
            const QString h = activeHorizon();
            if (h.isEmpty())
            {
              status(tr("先选择层位再保存版本"));
              return;
            }
            QVariantMap provenance;
            provenance.insert(QStringLiteral("saved_from"),
                              QStringLiteral("compose_page"));
            QString err;
            const MapVersion v = versions->saveVersion(h, provenance, &err);
            if (v.version > 0)
            {
              status(tr("已保存版本：%1 v%2").arg(h).arg(v.version));
              composePage->setVersionState(v.version, false);
            }
            else
              status(err.isEmpty() ? tr("保存版本失败") : err);
            if (m_refreshPublishGate) m_refreshPublishGate();
          });

}
