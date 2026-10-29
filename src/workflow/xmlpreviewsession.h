// 层：功能
#pragma once
#include "../services/previewdoc.h"
#include "../domain/wellcompositemodel.h"
#include <QObject>
#include <atomic>
#include <memory>
class QTemporaryDir;
class PaleoTaskService;

struct XmlPreviewData {
  WellComposite::ComprehensiveWellData chart;
  PreviewDocService::WorkbookPreview table;
  bool chartOk = false;
  QString chartError;
};
Q_DECLARE_METATYPE(XmlPreviewData)

class XmlPreviewSession : public QObject {
  Q_OBJECT
public:
  explicit XmlPreviewSession(QObject *parent = nullptr) : QObject(parent) {}
  ~XmlPreviewSession() override {
    if (m_verificationCancelled)
      m_verificationCancelled->store(true);
  }
  void setTaskService(PaleoTaskService *tasks) { m_tasks = tasks; }
  void open(const QString &path, const QString &expectedSha = QString(),
            const QString &modelProject = QString(), qlonglong modelSource = 0,
            qlonglong modelWell = 0);
  // 只按数据基座井号读取井道图，不读源文件。在任务池里跑，不占用界面线程。
  void openModelWell(const QString &modelProject, qlonglong modelWell, const QString &wellName);
signals:
  void ready(const XmlPreviewData &data);
private:
  PaleoTaskService *m_tasks = nullptr;
  std::shared_ptr<QTemporaryDir> m_directory;
  std::shared_ptr<std::atomic_bool> m_verificationCancelled;
  quint64 m_generation = 0;
};
