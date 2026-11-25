// 层：视图
#include "paleomainwindow.h"
#include "ai/aiassistdock.h"
#include "../metadata/paleoprojectstore.h"
#include "../qgis/qgisprojectservice.h"
#include "../workflow/aichatcontroller.h"
#include "../workflow/folderimport.h"
#include <QDir>
#include <QFileInfo>
#include <QStatusBar>

namespace {
QString outcomeText(const FolderRowResult &row) {
  switch (row.outcome) {
  case FolderRowResult::Outcome::Imported: return PaleoMainWindow::tr("已导入");
  case FolderRowResult::Outcome::Unresolved: return PaleoMainWindow::tr("已登记，关联待解析");
  case FolderRowResult::Outcome::Failed: return PaleoMainWindow::tr("导入失败");
  case FolderRowResult::Outcome::Skipped:
    return row.assetId.isEmpty() ? PaleoMainWindow::tr("已跳过") : PaleoMainWindow::tr("项目中已有相同内容");
  }
  return {};
}

QString importReceipt(const QString &project, const QVector<FolderRowResult> &rows,
                      const QString &error, bool cancelled) {
  QStringList lines{PaleoMainWindow::tr("工程「%1」文件导入结果").arg(project)};
  if (cancelled) lines << PaleoMainWindow::tr("已取消导入。");
  if (!error.isEmpty()) lines << PaleoMainWindow::tr("导入遇到错误。");
  constexpr int visibleRows = 12;
  for (int i = 0; i < qMin<qsizetype>(rows.size(), visibleRows); ++i) {
    const auto &row = rows.at(i);
    QStringList fields{QFileInfo(row.path).fileName().simplified().left(128), outcomeText(row)};
    if (!row.classifiedType.isEmpty()) fields << row.classifiedType;
    if (!row.assetId.isEmpty()) fields << PaleoMainWindow::tr("资产 ID %1").arg(row.assetId);
    lines << fields.join(QStringLiteral(" · "));
  }
  if (rows.size() > visibleRows)
    lines << PaleoMainWindow::tr("共 %1 项，已列出前 %2 项。").arg(rows.size()).arg(visibleRows);
  return lines.join(QLatin1Char('\n'));
}
}

QString PaleoMainWindow::agentImportUnavailableReason() const {
  if (!m_projectSvc || m_projectSvc->projectPath().isEmpty()) return tr("请先打开可写工程");
  if (m_projectSvc->isOpening()) return tr("工程正在切换，请稍后再导入");
  if (isProjectReadOnly() || m_projectSvc->isReadOnly()) return tr("当前工程只读，无法导入文件");
  if (!m_importSvc) return tr("工程导入服务未接入");
  return {};
}

void PaleoMainWindow::wireAgentFileImport(AiAssistDock *dock) {
  const auto refresh = [this, dock] {
    dock->setFileImportContext(m_projectSvc ? m_projectSvc->sessionId() : 0, agentImportUnavailableReason());
  };
  refresh();
  if (m_projectSvc) {
    connect(m_projectSvc, &QgisProjectService::openActiveChanged, dock, refresh);
    connect(m_projectSvc, &QgisProjectService::projectOpened, dock, refresh);
    connect(m_projectSvc, &QgisProjectService::projectClosed, dock, refresh);
    connect(m_projectSvc, &QgisProjectService::projectAboutToClose, dock, [dock] {
      dock->setFileImportContext(0, tr("工程正在切换，请稍后再导入"));
    });
  }
  if (m_projectStore)
    connect(m_projectStore, &PaleoProjectStore::readOnlyChanged, dock, refresh);
  connect(dock, &AiAssistDock::importFilesRequested, this,
    [this, dock](const QStringList &paths, quint64 projectSession) {
      const QPointer<PaleoMainWindow> window(this);
      const QPointer<AiChatController> chat(dock->controller());
      const QPointer<AiAssistDock> panel(dock);
      const auto current = [window, projectSession] {
        return window && window->agentImportUnavailableReason().isEmpty() &&
               window->m_projectSvc->sessionId() == projectSession;
      };
      if (!current() || !chat || paths.isEmpty()) {
        dock->showFileImportStatus(tr("无法导入；请在当前可写工程中重新选择文件。"));
        return;
      }
      const QString session = chat->session().id;
      const QString project = QFileInfo(m_projectSvc->projectPath()).dir().dirName().simplified().left(128);
      const auto record = [chat, session, window](const QString &text) {
        if (!chat) return;
        QString error;
        if (!chat->recordImportReceipt(session, text, &error) && window)
          window->statusBar()->showMessage(tr("文件导入记录未能保存：%1").arg(error), 10000);
      };
      record(tr("已选择 %1 个文件导入工程「%2」。").arg(paths.size()).arg(project));
      if (!current() || !panel) {
        record(tr("工程已变化，所选文件尚未导入。"));
        return;
      }
      panel->showFileImportStatus(tr("正在识别并导入所选文件…"));
      const FileImportReport report = [current, chat, session, panel, record, project](
          const QVector<FolderRowResult> &rows, const QString &error, bool cancelled) {
        if (rows.isEmpty() && error.isEmpty() && !cancelled)
          return;
        record(importReceipt(project, rows, error, cancelled));
        if (!current() || !panel || !chat || panel->controller() != chat || chat->session().id != session)
          return;
        QStringList details;
        if (!error.isEmpty()) details << error.left(240);
        for (const auto &row : rows) {
          if (details.size() >= 3) break;
          if (row.outcome == FolderRowResult::Outcome::Failed && !row.message.isEmpty())
            details << tr("%1：%2").arg(QFileInfo(row.path).fileName(), row.message.left(240));
        }
        panel->showFileImportStatus(details.join(QLatin1Char('\n')));
      };
      folderImportWorkflow()->importAgentAttachments(paths, current, report,
        [window, current, projectSession, panel, chat, session, report](const QVector<FolderPreviewRow> &preview) {
          if (!current() || preview.isEmpty()) return;
          if (panel && chat && panel->controller() == chat && chat->session().id == session)
            panel->showFileImportStatus(tr("请核对待确认文件的类型或关联。"));
          QStringList reviewPaths;
          for (const auto &row : preview) reviewPaths << row.path;
          window->confirmFileImport(reviewPaths, preview, {}, projectSession, report);
        });
    });
}
