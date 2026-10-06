// 层：视图
// paleomainwindow_attach_data — 数据管理页接线（W4 拆分段）
#include "paleomainwindow.h"

#include "paleotheme.h"
#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/qgisprojectservice.h"
#include "../qgis/qgislayerservice.h"
#include "../linkage/selectioncontext.h"
#include "../domain/projectclassifier.h"
#include "../services/previewdoc.h"
#include "../catalog/datacatalog.h"
#include "dialogs/folderconfirm.h"
#include "correlationpanel.h"
#include "datapreview/datapreviewtabs.h"
#include "pages/pagepanels.h"
#include "pages/dataops/dataopsimportqueue.h"
#include "pages/datalist.h"
#include "../workflow/folderimport.h"

#include <qgsmapcanvas.h>
#include <qgsproject.h>
#include <qgsmaplayer.h>
#include <qgslayertree.h>
#include <qgslayertreelayer.h>
#include <qgsmessagelog.h>

#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QLabel>
#include <QPushButton>
#include <QStatusBar>
#include <QTabWidget>
#include <QVBoxLayout>

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
    // B2（wave/deepen-perf）：导入队列生产 runner（GAPS G-2.3 收口）——串行
    // 驱动 + loud 任务池逐文件导入 + 取消/重试状态机。面板持有 runner 闭包
    //（shared_ptr Hub 续命，GUI 线程投递），适配器本体随本栈析构不悬空。
    if (taskSvc && dataPage->listPanel())
    {
      paleo::dataops::FolderImportQueueAdapter importRunner(m_previewDoc, taskSvc);
      importRunner.attach(dataPage->listPanel()->importQueuePanel());
    }
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
      connect(dataPage, &DataPage::versionActivated, preview, &DataPreviewTabs::openVersion);
      connect(preview, &DataPreviewTabs::versionContextChanged, dataPage, &DataPage::focusVersion);
      connect(dataPage, &DataPage::assetActivated, preview, &DataPreviewTabs::openAsset);
      connect(dataPage, &DataPage::assetActivated, dataPage,
              [preview, dataPage](const QString &assetId) {
                dataPage->selectAsset(assetId);
                // 已有预览可能保留历史版本；列表重选不能用 latest 覆盖它。
                auto *tabs = preview->findChild<QTabWidget *>(QStringLiteral("dataPreviewTabs"));
                if (tabs && preview->assetIdAt(tabs->currentIndex()) == assetId)
                {
                  const QString vid = preview->versionIdAt(tabs->currentIndex());
                  if (!vid.isEmpty()) dataPage->focusVersion(assetId, vid);
                }
              });
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
          if (idx >= 0 && preview && dataPage && !preview->property("paleo.versionNavigation").toBool())
          {
            const QString aid = preview->assetIdAt(idx);
            if (!aid.isEmpty())
            {
              const QString vid = preview->versionIdAt(idx);
              if (!vid.isEmpty()) dataPage->focusVersion(aid, vid);
              else dataPage->selectAsset(aid);
            }
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
                    QgsMessageLog::logMessage(tr("在地图上显示失败：%1").arg(err),
                                              QStringLiteral("Paleo"), Qgis::Critical);
                    return;
                  }
                  if (m_canvasCtl)
                  {
                    m_canvasCtl->zoomToLayer(layerId);
                    flashHorizonLayer(layer);
                  }
                  QgsProject *proj = m_projectSvc ? m_projectSvc->project() : nullptr;
                  QgsLayerTree *treeRoot = proj ? proj->layerTreeRoot() : nullptr;
                  if (QgsLayerTreeLayer *node =
                          treeRoot ? treeRoot->findLayer(layer->id()) : nullptr)
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
                PaleoTheme::applyThemedStyleSheet(
                    crsNote, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
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
                        QObject::tr("导入失败：%1").arg(err),
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
              // LAS imports pull the GR curve into the column when present
              // (#156：井集与工程打开同一来源——catalog 全部 well_log)。
              if (corrPanel && kind == QLatin1String("well_log"))
                refreshCorrelationWells(assetId);
            });
}
