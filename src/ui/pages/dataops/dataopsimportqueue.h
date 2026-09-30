// 层：视图
// ui/pages/dataops/dataopsimportqueue — B2（wave/deepen-perf）：导入队列的
// 生产 runner 适配器（GAPS G-2.3 的收口件）。
//
// 职责：把「逐文件导入」接进 ImportQueuePanel 的 Runner 注入钩子——
//   · 串行驱动：一次只跑一项（队列语义），完成/失败/取消后自动泵下一项；
//   · 每项跑 PaleoTaskService 任务池（用户主动长任务，loud——任务中心可见、
//     可取消）；item.cancelHook 桥接面板取消 → PaleoTask::requestCancel；
//   · 终态回写 markDone/markFailed（失败进 D8.2 自动重试状态机）；取消
//     不改写面板已落的 Canceled 态（取消 ≠ 失败，不重试）。
// 生产绑定：默认经 PreviewDocService::importSingleFile（视图层不 include
// io/dataimportservice.h——白名单纪律）；测试用 setImportFn 注入假执行面。
//
// 非 QObject：跨任务回调持 shared_ptr<Hub> 续命，连接上下文用面板
//（GUI 线程投递 + 面板析构自动断连）。attach 后适配器本体可随壳析构，
// 已在途任务的终态闭包持 Hub，不悬空。
#pragma once

#include <QFileInfo>
#include <QPointer>
#include <QString>
#include <functional>
#include <memory>

#include "../../../services/paleotaskservice.h"
#include "../../../services/previewdoc.h"
#include "../dataopsimportui.h"

namespace paleo::dataops
{

class FolderImportQueueAdapter
{
  public:
    // 执行面：返回资产 id（空 = 失败，*error 带原因）。在任务池线程调用。
    using ImportFn = std::function<QString(const QString &kind,
                                           const QString &path, QString *error)>;

    FolderImportQueueAdapter(PreviewDocService *doc, PaleoTaskService *tasks)
        : m_doc(doc), m_tasks(tasks)
    {
    }

    void setImportFn(ImportFn fn) { m_importFn = std::move(fn); }

    // 绑定面板：panel->setRunner(...)。可重复调用（重绑即替换）。
    void attach(ImportQueuePanel *panel)
    {
        if (!panel)
            return;
        m_panel = panel;
        auto hub = m_hub = std::make_shared<Hub>();
        hub->doc = m_doc;
        hub->tasks = m_tasks;
        hub->panel = panel;
        hub->queue = nullptr; // start() 时按面板实际队列指针对上
        hub->fn = m_importFn;
        panel->setRunner([hub](int index, ImportQueueItem &item,
                               ImportRetryQueue *queue) {
            Hub::start(hub, index, item, queue);
        });
    }

    bool busy() const { return m_hub && m_hub->busy; }

  private:
    struct Hub
    {
        QPointer<PreviewDocService> doc;
        QPointer<PaleoTaskService> tasks;
        QPointer<ImportQueuePanel> panel;
        ImportFn fn; // 可空：空则每次现取 doc->importSingleFile（QPointer 防悬空）
        ImportRetryQueue *queue = nullptr; // 面板成员（面板在场即有效）
        bool busy = false;

        // Runner 入口：串行——忙时不动（条目留 Queued，泵会捡）；闲时启动。
        static void start(const std::shared_ptr<Hub> &hub, int index,
                          ImportQueueItem &item, ImportRetryQueue *queue)
        {
            if (hub->busy || !queue)
                return;
            if (item.state != ImportItemState::Queued)
                return; // 快照到启动之间被取消/改判——如实跳过
            PaleoTaskService *tasks = hub->tasks;
            if (!tasks)
            {
                // 无任务服务（测试/小环境）：同步旧路径当场落状态。
                QString err;
                const QString id = runImport(hub, item.type, item.path, &err);
                queue->markRunning(index);
                if (id.isEmpty())
                    queue->markFailed(index, err);
                else
                    queue->markDone(index, id);
                return;
            }
            hub->busy = true;
            hub->queue = queue;
            queue->markRunning(index);
            const QString kind = item.type;
            const QString path = item.path;
            auto outId = std::make_shared<QString>();
            auto outErr = std::make_shared<QString>();
            // loud：用户主动导入（任务中心可见 + 可取消）。
            PaleoTask *task = tasks->start(
                QObject::tr("导入 %1").arg(QFileInfo(path).fileName()),
                [hub, kind, path, outId, outErr](PaleoTask *) -> QString {
                    *outId = runImport(hub, kind, path, outErr.get());
                    return outId->isEmpty() ? *outErr : QString();
                });
            item.cancelHook = [task] {
                if (task)
                    task->requestCancel();
            };
            QObject::connect(task, &PaleoTask::finished, hub->panel,
                             [hub, path, outId, outErr, task] {
                                 finish(hub, path, task->state(),
                                        task->errorText(), *outId, *outErr);
                             });
        }

        // 终态回写 + 泵下一项（PaleoTask::finished 已排队回面板线程）。
        static void finish(const std::shared_ptr<Hub> &hub, const QString &path,
                           PaleoTask::State st, const QString &errText,
                           const QString &id, const QString &err)
        {
            hub->busy = false;
            ImportQueuePanel *panel = hub->panel;
            ImportRetryQueue *queue = hub->queue;
            if (!panel || !queue)
                return; // 面板没了——结果没人等
            const int idx = queue->indexOfPath(path);
            if (idx >= 0)
            {
                const ImportItemState stNow = queue->items().at(idx).state;
                if (stNow == ImportItemState::Canceled)
                {
                    // 用户取消先落了态：不改写（取消 ≠ 失败，不进重试）。
                }
                else if (st == PaleoTask::State::Succeeded && !id.isEmpty())
                {
                    queue->markDone(idx, id);
                }
                else if (st == PaleoTask::State::Failed)
                {
                    queue->markFailed(idx, err.isEmpty() ? errText : err);
                }
                // 任务 Cancelled 而条目非 Canceled（异常序）：结果未知——
                // 不动条目、不装作失败，只继续泵队列。
            }
            panel->refresh();
            panel->runPending(); // 泵下一项（Queued 态由 runner 串行捡起）
        }

      private:
        static QString runImport(const std::shared_ptr<Hub> &hub,
                                 const QString &kind, const QString &path,
                                 QString *error)
        {
            if (hub->fn)
                return hub->fn(kind, path, error);
            PreviewDocService *doc = hub->doc;
            if (!doc)
            {
                if (error)
                    *error = QObject::tr("导入服务未就绪");
                return QString();
            }
            return doc->importSingleFile(kind, path, error);
        }
    };

    PreviewDocService *m_doc = nullptr;
    PaleoTaskService *m_tasks = nullptr;
    ImportFn m_importFn;
    QPointer<ImportQueuePanel> m_panel;
    std::shared_ptr<Hub> m_hub;
};

} // namespace paleo::dataops
