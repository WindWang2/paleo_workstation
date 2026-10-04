// 层：功能
#pragma once

#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <QUrl>

class QNetworkReply;

// 外部工作台的连接/进程生命周期；源码、数据和工程均由外部目录持有。
class StratigraphicWebSession : public QObject
{
  Q_OBJECT
public:
  explicit StratigraphicWebSession(QObject *parent = nullptr);
  ~StratigraphicWebSession() override;
  QUrl endpoint() const { return m_endpoint; }
  QString projectDirectory() const { return m_directory; }
  QString pythonExecutable() const { return m_python; }
  bool busy() const { return m_busy; }
  bool ownsService() const { return m_process.state() != QProcess::NotRunning; }
  bool setEndpoint(const QString &text);
  void setProjectDirectory(const QString &directory);
  void setPythonExecutable(const QString &executable);

public slots:
  void connectToService();
  void startLocalService();

signals:
  void ready(const QUrl &url);
  void failed(const QString &reason);
  void statusChanged(const QString &message);
  void busyChanged(bool busy);

private:
  void begin(bool startLocal);
  void probe();
  void launch();
  void fail(const QString &reason);
  void setBusy(bool busy);
  void saveConfiguration();
  QUrl m_endpoint;
  QUrl m_ownedEndpoint;
  QString m_directory;
  QString m_python;
  QNetworkAccessManager m_network;
  QPointer<QNetworkReply> m_reply;
  QProcess m_process;
  QTimer m_retry;
  QElapsedTimer m_startTime;
  QByteArray m_output;
  bool m_busy = false;
  bool m_startLocal = false;
  bool m_launched = false;
};
