// 层：功能
#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

class QgisProjectService;

// workflow/projectopen — 「打开/新建工程」编排（W2：从主窗 openPath 下沉）。
//
// 路径判别（文件 .qgz/.paleo / 目录含 paleo / 目录含 qgz / 空目录→新建）
// 与「从工区文件夹新建」的 sourceArea 回写都在这里；视图只接信号：
//   openFailed —— 弹错误框（壳决定静默或 QMessageBox）；
//   folderImportRequested —— 空目录新工程建好后，壳按导入服务就绪与否
//                            决定「引文件夹导入」还是提示手动导入。
// 本类不碰控件、不弹窗。
class ProjectOpenWorkflow : public QObject
{
  Q_OBJECT

  public:
    explicit ProjectOpenWorkflow(QgisProjectService *projSvc,
                                 QObject *parent = nullptr);

    // 打开工程异步提交；返回 true 表示读取已启动。新建目录保持原有创建流程。
    bool openPath(const QString &path);

    // 「从工区文件夹新建」回写 sourceArea：工程目录 == 导入目录才写（目录
    // 不匹配静默拒绝）；读/写 project.paleo 失败只告警不阻断导入。
    void stampSourceArea(const QString &dir, const QVariantMap &stats);

  signals:
    void openFailed(const QString &title, const QString &detail, bool fatal);
    void folderImportRequested(const QString &dir);

  private:
    QgisProjectService *m_projSvc = nullptr;
};
