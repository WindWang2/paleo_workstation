// 层：功能
#pragma once
#include "../services/previewdoc.h"
#include "../domain/wellcompositemodel.h"
#include <QObject>

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
  quint64 m_generation = 0;
};
