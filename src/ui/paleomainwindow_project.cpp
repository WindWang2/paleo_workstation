// 层：视图
// paleomainwindow_project — 工程打开/导入编排入口域（方向 83 拆 TU）：
// openPath（#282 脏状态三选一 + ProjectOpenWorkflow）、文件夹导入确认表
// （T22/§3：目录拾取 + 对话框 exec + 视图出口回调）、懒建编排器。工程打开
// 后的壳落地（onProjectOpened）留在本体（生命周期域）。自本体移入，逐行保持。
#include "paleomainwindow.h"
#include "uienv_internal.h"

#include "datapreview/datapreviewtabs.h" // m_previewTabs->openAsset 需完整类型
#include "dialogs/folderconfirm.h" // PaleoFolderConfirm（T22 确认表）
#include "notifications/paleonotify.h"
#include "pages/pagepanels.h" // DataPage（未解析过滤入口）
#include "../workflow/folderimport.h"
#include "../workflow/projectopen.h"

#include <QDialog>
#include <QFileDialog>
#include <QMetaObject>
#include <QVariantMap>

using paleo::ui_detail::isOffscreen;

// ---------------------------------------------------------------------------
// T22 文件夹导入确认表：类型下拉从分类器词表构建（label↔type 稳定映射，
// type 存 Qt::UserRole——不靠显示文本反推）；HZ28-6-1 行锁定为参考；
// 「参考资料」目录内井类/未判内容默认显示「参考」（可改，成 override 送达
// 后端）；覆盖只收「合法且不同于分类器原类型」的行；Failed 行给「重试」。
// T22/§3 契约句：工区导入统一展示的 CRS 说明（文件夹确认表 + 单文件
// 导入确认都只读挂这句）。状态栏短句另行，与 PDF 页脚同一文案。
// ---------------------------------------------------------------------------

void PaleoMainWindow::runFolderImport(DataImportService *svc)
{
  if (!svc)
    return;
  const QString dir =
      QFileDialog::getExistingDirectory(this, tr("导入工区文件夹"));
  if (dir.isEmpty())
    return;
  runFolderImportAt(svc, dir);
}

FolderImportWorkflow *PaleoMainWindow::folderImportWorkflow()
{
  // W2：文件夹导入编排在 workflow/folderimport；壳只留目录拾取 +
  // 对话框 exec + 视图出口回调。首次用到时按当前服务实例懒建。
  if (!m_folderImportWf && m_importSvc)
  {
    m_folderImportWf = new FolderImportWorkflow(m_importSvc, m_taskSvc, this);
    connect(m_folderImportWf, &FolderImportWorkflow::importActiveChanged, this,
            [this](bool active) { m_folderImportActive = active; });
  }
  return m_folderImportWf;
}

void PaleoMainWindow::runFolderImportAt(DataImportService *svc,
                                        const QString &dir)
{
  if (!svc)
    return;
  auto *wf = folderImportWorkflow();
  QString err;
  const auto preview = wf ? wf->previewFolder(dir, &err)
                          : QVector<FolderPreviewRow>();
  if (preview.isEmpty())
  {
    PaleoNotify::warning(this, tr("导入工区文件夹"),
                         err.isEmpty() ? tr("目录里没有可导入的文件") : err);
    return;
  }

  QDialog dlg(this);
  PaleoFolderConfirm::Hooks hooks;
  hooks.importRow = [wf](const QString &path, const QString &force) {
    return wf->importFolderRow(path, force, nullptr);
  };
  hooks.importAll = [wf, dir](const QMap<QString, QString> &overrides,
                              FolderImportWorkflow::ImportDone done) {
    wf->importFolder(dir, overrides, std::move(done));
  };
  hooks.importedWellHead = [wf](const QString &rowPath) {
    return wf->importedWellHeadAsset(rowPath);
  };
  hooks.showUnresolved = [this] {
    showPage(QStringLiteral("data"));
    if (auto *page = findChild<DataPage *>())
      page->setUnresolvedFilter(true);
  };
  // 保持旧语义：井口入库的预览标签排队到对话框信号处理完成后开。
  hooks.previewAsset = [this](const QString &assetId) {
    QMetaObject::invokeMethod(
        this,
        [this, assetId] {
          if (m_previewTabs)
            m_previewTabs->openAsset(assetId);
        },
        Qt::QueuedConnection);
  };
  hooks.stampSourceArea = [this, dir](const QVariantMap &stats) {
    stampSourceArea(dir, stats);
  };
  PaleoFolderConfirm::buildFolderConfirmDialog(&dlg, dir, preview, hooks);
  dlg.exec();
}

void PaleoMainWindow::stampSourceArea(const QString &dir,
                                      const QVariantMap &stats)
{
  if (auto *wf = projectOpenWorkflow())
    wf->stampSourceArea(dir, stats);
}

ProjectOpenWorkflow *PaleoMainWindow::projectOpenWorkflow()
{
  // W2：openPath 编排在 workflow/projectopen（路径判别/新建/sourceArea
  // 回写）；壳只剩错误弹窗与「文件夹导入」意图的接线。
  if (!m_projectOpenWf && m_projectSvc)
  {
    m_projectOpenWf = new ProjectOpenWorkflow(m_projectSvc, this);
    connect(m_projectOpenWf, &ProjectOpenWorkflow::openFailed, this,
            [this](const QString &title, const QString &detail, bool fatal) {
              if (isOffscreen())
                return;
              if (fatal)
                PaleoNotify::critical(this, title, detail);
              else
                PaleoNotify::warning(this, title, detail);
            });
    connect(m_projectOpenWf, &ProjectOpenWorkflow::folderImportRequested, this,
            [this](const QString &dir) {
              if (m_importSvc)
                runFolderImportAt(m_importSvc, dir);
              else if (!isOffscreen())
                PaleoNotify::information(
                    this, tr("从工区文件夹新建"),
                    tr("工程已创建于 %1；导入服务未就绪，请在数据页手动导入该文件夹。")
                        .arg(dir));
            });
  }
  return m_projectOpenWf;
}

bool PaleoMainWindow::openPath(const QString &path)
{
  if (path.isEmpty())
    return false;
  // #282：所有走 workflow 的工程切换入口（启动页「从工区文件夹新建」等）
  // 先问当前工程脏状态——取消则原地不动。启动首开（无工程）时为空操作。
  if (!maybeSaveProject())
    return false;
  auto *wf = projectOpenWorkflow();
  return wf ? wf->openPath(path) : false;
}
