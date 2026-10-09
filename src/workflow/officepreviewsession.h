// 层：功能
#pragma once
#include <QObject>
#include <QImage>
#include <QPointer>
#include <QTimer>
#include <memory>
#include <atomic>
class QProcess;
class QTemporaryDir;

// Verification and image decoding run in workers; Calligra runs in an owned
// child process. No external window handles, PDF conversion or GUI waits.
class OfficePreviewSession : public QObject
{
  Q_OBJECT
public:
  explicit OfficePreviewSession(QObject *parent = nullptr);
  ~OfficePreviewSession() override;
  static bool supports(const QString &path);
  static QString rendererPath();
  void open(const QString &path, const QString &expectedSha = {});
  void requestPage(int index);
  void stop();
signals:
  void ready(const QStringList &pageLabels);
  void pageReady(int index, const QImage &image);
  void failed(const QString &reason);
private:
  void launch(const QString &path);
  void readMessages();
  void fail(const QString &reason);
  QPointer<QProcess> m_process;
  std::shared_ptr<QTemporaryDir> m_directory;
  std::shared_ptr<std::atomic_bool> m_verificationCancelled;
  QTimer m_deadline;
  QByteArray m_messages;
  quint64 m_generation = 0;
  int m_pageCount = 0;
  int m_request = 0;
  int m_inFlight = -1;
  int m_requestedPage = -1;
};
