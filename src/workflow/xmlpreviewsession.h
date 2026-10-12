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
  void open(const QString &path, const QString &expectedSha = QString());
signals:
  void ready(const XmlPreviewData &data);
private:
  PaleoTaskService *m_tasks = nullptr;
  std::shared_ptr<QTemporaryDir> m_directory;
  std::shared_ptr<std::atomic_bool> m_verificationCancelled;
  quint64 m_generation = 0;
};
