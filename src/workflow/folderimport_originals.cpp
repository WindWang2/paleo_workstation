// 层：功能
#include "folderimport.h"
#include "io/dataimportservice.h"
#include "io/originalpackage.h"
#include "services/paleotaskservice.h"

void FolderImportWorkflow::receiveOriginalPackage(const QString &directory,
    const ProjectGuard &projectStillCurrent, const OriginalsReceived &done, const std::function<QString()> &identifySender)
{
  receiveFromSender(directory, {}, projectStillCurrent, done, identifySender);
}

void FolderImportWorkflow::receiveFromSender(const QString &directory, const QString &senderId,
    const ProjectGuard &projectStillCurrent, const OriginalsReceived &done, const std::function<QString()> &identifySender)
{
  if (!done) return;
  if (taskServiceStopped()) {
    done({}, 0, tr("后台任务已关闭，请重试。"), true);
    return;
  }
  if (!projectStillCurrent || !projectStillCurrent() || !m_svc || !m_svc->catalog() ||
      m_svc->catalog()->refusesWrites() || m_svc->projectDir().isEmpty()) {
    done({}, 0, tr("请先打开可写工程，再接收原件包。"), false);
    return;
  }
  const QString project = m_svc->projectDir();
  auto result = std::make_shared<paleo::exchange::ReceiveResult>();
  const auto receive = [project, directory, senderId, result, canPrompt = bool(identifySender)](PaleoTask *task) {
    *result = paleo::exchange::receiveOriginalPackage(project, directory, senderId,
        [task] { return task && task->cancelRequested(); },
        [task](qint64 bytes, qint64 total, const QString &file) {
          if (task) { task->reportBytes(bytes, total); task->reportDetail(file); }
        });
    return result->needsSenderId && canPrompt ? QString{} : result->error;
  };
  const auto finish = [this, directory, projectStillCurrent, identifySender, done, result] {
    if (!projectStillCurrent()) {
      QString detail = result->error;
      if (!detail.isEmpty()) detail += QLatin1Char('\n');
      detail += tr("工程已切换，请在原工程查看收件结果。");
      done(result->files, result->reused.size(), detail, result->cancelled);
      return;
    }
    if (!result->cancelled && result->needsSenderId && identifySender) {
      const QPointer<FolderImportWorkflow> alive(this);
      const QString id = identifySender().trimmed();
      if (!alive) return;
      if (id.isEmpty()) done({}, 0, {}, true);
      else receiveFromSender(directory, id, projectStillCurrent, done, {});
      return;
    }
    done(result->files, result->reused.size(), result->error, result->cancelled);
  };
  if (!m_taskSvc) {
    receive(nullptr);
    finish();
    return;
  }
  const QPointer<FolderImportWorkflow> alive(this);
  const auto service = m_taskSvc;
  const quint64 taskSession = service->session();
  auto *task = service->start(tr("校验并接收原件包"), receive);
  if (!alive) return;
  observeTask(task, [finish, result, done, service, taskSession](PaleoTask *task) {
    if (!task) {
      done({}, 0, tr("后台任务已关闭，请重试。"), false);
      return;
    }
    result->cancelled |= task->cancelRequested() || !service || service->shutdownRequested() ||
                         service->session() != taskSession;
    if (result->error.isEmpty() && task->state() == PaleoTask::State::Failed)
      result->error = task->errorText();
    finish();
  });
}
