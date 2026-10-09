// 层：视图
#pragma once
#include <QWidget>
class OfficePreviewSession;
class QLabel;
class QPushButton;
class WebViewPanel;

class OfficePreviewWidget : public QWidget
{
  Q_OBJECT
public:
  explicit OfficePreviewWidget(const QString &path, const QString &expectedSha = {}, QWidget *parent = nullptr);
  ~OfficePreviewWidget() override;
  void showMessage(const QString &text);
signals:
  void editSaved(const QString &path);
  void previewClosed();
private:
  void open();
  QString m_path, m_sha;
  OfficePreviewSession *m_session;
  WebViewPanel *m_web;
  QLabel *m_status;
  QPushButton *m_retry;
};
