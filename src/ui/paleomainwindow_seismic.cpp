// 层：视图
// paleomainwindow：地震体同步到各停靠面板（file-budget 拆分）

#include "paleomainwindow.h"
#include "paleomainwindow_internal.h"
#include "uienv_internal.h"
#include "paleodockmanager.h"
#include "paleotheme.h" // T32：焦点环/mono 数字面 token 出口；暗色翻案 token 全集
#include "paleoribbon.h" // SARibbon 壳公用件：主题/命令镜像
#include "correlationpanel.h"
#include "datapreview/datapreviewtabs.h"
#include "decorations/paleodecorations.h"
#include "edittools/editingtoolbar.h"
#include "horizonchipbar.h"
#include "layout/mapbookcontroller.h" // #148：工程关闭时 resetProject
#include "layout/mapbookpanel.h"
#include "notifications/paleonotify.h"
#include "pages/pageshared.h" // kPageIds（W4 跨 TU 页序表）
#include "pages/predictpage.h" // setHorizons（工程打开填充预测页层位下拉）
#include "pages/constraintpage.h" // setHorizons（同上，单因素页）
#include "pages/validatepage.h" // clearResults（工程边界清验证产物展示）
#include "shortcuts/shortcutcatalog.h" // 方向63：注册表健康度进启动日志
#include "help/whatsthiscatalog.h"     // 方向63：壳控件 whatsThis 回填
#include <QComboBox> // findChildren<QComboBox*>（层位下拉 chip 联动）
#include <QGuiApplication>
#include <QScreen>
#include "ui/seismic3d/seismic3dviewpanel.h"
#include "ui/seismic3d/seismic3dviewportwidget.h" // 3D 视口 presetView/fitToBounds
#include "ui/seismicsection/seismicsectiondockwidget.h"
#include "../catalog/datacatalog.h"
#include "../domain/arearules.h"
#include "../domain/mappinghorizons.h" // baseHorizonFor（工程打开后的层位文案）
#include "../linkage/selectioncontext.h" // ctor 的 activeHorizonChanged 接线
#include "../workflow/sectionworkbench.h" // m_sectionWorkbench->cancelPreviewData（resetProjectScopedState）
#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/qgisprojectservice.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgislayerprofile.h"
#include "../qgis/qgiseditingservice.h" // resolveDirtyLayerEdits 走编辑服务提交/回滚
#include "../services/previewdoc.h"
#include "../services/paleotaskservice.h"
#include "../services/toolavailability.h" // 全局门控（无工程置灰导入命令）
#include "../services/fspathutils.h" // #291 QString↔filesystem::path 走 UTF-16（MSVC 窄构造按 ANSI 解码）
#include "../workflow/registration.h"
#include "../workflow/sectionworkbench.h" // cancelPreviewData 解引用需完整类型（#156；resetProjectScopedState 同款）
#include "domain/seismic/sgyvolume.h"
#include "metadata/paleoprojectstore.h"
#include "qgis/projectmapreference.h" // restoreCanvasExtent 的跨 CRS 范围换算
#include <qgis.h> // Qgis::MapToolUnit / MessageLevel
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QProgressBar>
#include <QSettings>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTableWidget>
#include <QTimer>
#include <memory>

void PaleoMainWindow::syncSeismicVolumeToDocks(bool includeLargeVolumes)
{
  if (m_projectSvc && m_projectSvc->isOpening()) return;
  DataCatalog *cat = m_previewDoc ? m_previewDoc->catalog() : nullptr;
  if (!cat || !cat->isOpen() || !m_seismicTaskSvc)
    return;
  for (const CatalogAsset &asset : cat->assets())
  {
    if (asset.type != QLatin1String("seismic"))
      continue;
    const auto version = cat->currentVersion(asset.id);
    // #226：解释登记链生产接线——dock 注入 catalog + 源体资产/版本 +
    // 解释产物目录（工程受管 artifacts/derived/interpretation：拾取 CSV/
    // 断层/属性扫描产物全部落此并登记 DERIVED，对 catalog/治理/版本溯源
    // 可见）。调用时现取工程目录（attach 期没有工程——同 #275 口径）。
    if (m_seismicSectionDock && !version.id.isEmpty())
    {
      const QString projDir =
          m_projectSvc && !m_projectSvc->projectPath().isEmpty()
              ? QFileInfo(m_projectSvc->projectPath()).absolutePath()
              : QString();
      m_seismicSectionDock->setInterpretationCatalog(
          cat, asset.id, version.id,
          projDir.isEmpty()
              ? QString()
              : QDir(projDir).filePath(
                    QStringLiteral("artifacts/derived/interpretation")));
    }
    const QString path = m_previewDoc->absolutePathForVersion(version);
    if (path.isEmpty() || !QFile::exists(path))
      continue;
    if (!includeLargeVolumes && QFileInfo(path).size() > 2LL * 1024 * 1024 * 1024)
      continue;
    const auto matches = [&path](const auto &volume) {
      return volume && paleo::fromFsPath(volume->Path()) == path;
    };
    const bool need3d = m_seismic3dPanel && !matches(m_seismic3dPanel->volume());
    const bool needSection = m_seismicSectionDock && !matches(m_seismicSectionDock->volume());
    // #481 P3-3：已就位/已记账/在途的体 continue 跳过（旧 return 吞后续体）；余体由终态回调串行推进。
    if ((!need3d && !needSection) || m_seismicAutoLoadedPaths.contains(path) ||
        (m_seismicOpenPath == path && m_seismicOpenTask && m_seismicOpenTask->running()))
      continue;
    if (m_seismicOpenTask)
      m_seismicOpenTask->requestCancel();
    const quint64 generation = ++m_seismicOpenGeneration;
    m_seismicOpenPath = path;
    m_seismicAutoLoadedPaths.insert(path);
    const quint64 session = m_projectSvc ? m_projectSvc->sessionId() : 0;
    double origin = 0;
    for (const auto &link : cat->linksForAsset(asset.id))
      if (!link.unresolved && link.entityType == QLatin1String("seismic_survey")) {
        origin = cat->entityById(link.entityId).startTimeMs;
        break;
      }
    auto *status = findChild<QLabel *>(QStringLiteral("projectSeismicLoadStatus"));
    auto *progress = findChild<QProgressBar *>(QStringLiteral("projectSeismicLoadProgress"));
    if (!status) {
      status = new QLabel(this);
      status->setObjectName(QStringLiteral("projectSeismicLoadStatus"));
      progress = new QProgressBar(this);
      progress->setObjectName(QStringLiteral("projectSeismicLoadProgress"));
      progress->setMaximumWidth(160);
      statusBar()->addPermanentWidget(status);
      statusBar()->addPermanentWidget(progress);
    }
    status->setText(tr("后台加载地震体：%1").arg(QFileInfo(path).fileName()));
    progress->setRange(0, 0);
    status->show(); progress->show();
    QPointer<PaleoMainWindow> guard(this);
    // #481 P3-6：状态条控件按 QPointer 捕获（attach_models 范本），窗口先亡不解引用。
    const QPointer<QLabel> statusGuard(status);
    const QPointer<QProgressBar> progressGuard(progress);
    m_seismicOpenTask = m_seismicTaskSvc->startVolumeLoad(path,
        [guard, generation, session, origin, statusGuard, progressGuard, includeLargeVolumes](bool ok,
              std::shared_ptr<seismic::SgyVolume> volume, const QString &error) {
      if (!guard || generation != guard->m_seismicOpenGeneration ||
          (guard->m_projectSvc && session != guard->m_projectSvc->sessionId()))
        return;
      if (statusGuard) statusGuard->hide();
      if (progressGuard) progressGuard->hide();
      guard->m_seismicOpenPath.clear();
      if (!ok || !volume) {
        guard->statusBar()->showMessage(error, 10000);
        guard->syncSeismicVolumeToDocks(includeLargeVolumes); // #481 P3-8：失败终态同样推进余体
        return;
      }
      if (guard->m_seismic3dPanel) {
        guard->m_seismic3dPanel->setTaskService(guard->m_seismicTaskSvc.get());
        guard->m_seismic3dPanel->setVolume(volume);
        if (guard->m_seismic3dPanel->viewport()) {
          guard->m_seismic3dPanel->viewport()->setPresetView(seismic::SeismicCameraController::PresetView::Isometric);
          guard->m_seismic3dPanel->viewport()->fitToBounds();
        }
      }
      if (guard->m_seismicSectionDock) {
        guard->m_seismicSectionDock->setTimeOriginMs(origin);
        if (guard->m_sectionLink)
          paleo::mainwindow_internal::setSectionLinkVolume(guard->m_sectionLink, volume);
        else
          guard->m_seismicSectionDock->setVolume(volume);
      }
      guard->statusBar()->showMessage(QObject::tr("地震体加载完成"), 5000);
      // #481 P3-8 后半：终态后重扫余体（多体串行装载，已记账路径不重试）；循环尾 return 保单在途体。
      guard->syncSeismicVolumeToDocks(includeLargeVolumes);
    }, /*quiet=*/true); // 状态栏已有进度；自动加载不反复展开/收起底栏。
    if (m_seismicOpenTask) {
      QPointer<PaleoTask> task = m_seismicOpenTask;
      connect(task, &PaleoTask::changed, this, [task, progressGuard, generation, this] {
        if (!task || !progressGuard || generation != m_seismicOpenGeneration) return;
        const int percent = task->percent();
        progressGuard->setRange(0, percent < 0 ? 0 : 100);
        if (percent >= 0) progressGuard->setValue(percent);
      });
    }
    return;
  }
}
