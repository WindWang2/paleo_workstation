// 层：功能
#pragma once
#include "../services/previewdoc.h"
#include "../domain/wellcompositemodel.h"
#include <QObject>
#include <atomic>
#include <memory>
class QTemporaryDir;

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
  void open(const QString &path, const QString &expectedSha = QString());
signals:
  void ready(const XmlPreviewData &data);
private:
  std::shared_ptr<QTemporaryDir> m_directory;
  std::shared_ptr<std::atomic_bool> m_verificationCancelled;
  quint64 m_generation = 0;
};
