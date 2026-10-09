// 层：功能
#pragma once
#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPair>
#include <QUrl>
#include <atomic>
#include <memory>
class DataCatalog;
class QTcpServer;
class QTcpSocket;
class QTemporaryDir;

// 本机静态页托管 ranuts/document（OnlyOffice WASM）。只绑定 127.0.0.1，
// 一次只暴露当前原件。保存写入临时文件，不覆盖原件，也不转 PDF。
class OfficePreviewSession : public QObject
{
  Q_OBJECT
public:
  explicit OfficePreviewSession(QObject *parent = nullptr);
  ~OfficePreviewSession() override;
  static bool supports(const QString &path);
  static QString editorRoot();
  QUrl endpoint() const { return m_endpoint; }
  QUrl documentUrl() const { return m_documentUrl; }
  void open(const QString &path, const QString &expectedSha = {});
  void stop();
  // 把编辑结果登记为同一资产的 DERIVED 版本。原件字节保持不动。
  static bool commitEdit(DataCatalog *catalog, const QString &assetId, const QString &parentVersionId,
                         const QString &savedFile, QString *error = nullptr);
signals:
  void ready(const QUrl &url);
  void failed(const QString &reason);
  void documentSaved(const QString &path);
private:
  void launch(const QString &path);
  void fail(const QString &reason);
  void acceptConnection();
  void readSocket(QTcpSocket *socket);
  void handleRequest(QTcpSocket *socket, const QByteArray &method, const QByteArray &target,
                     const QList<QPair<QByteArray, QByteArray>> &headers, const QByteArray &body);
  void respond(QTcpSocket *socket, int status, const QByteArray &reason, const QByteArray &type,
               const QByteArray &body, const QByteArray &extra = {});
  void respondFile(QTcpSocket *socket, const QString &path, const QByteArray &type,
                   const QList<QPair<QByteArray, QByteArray>> &headers);
  QString hostPage() const;
  QTcpServer *m_server = nullptr;
  QUrl m_endpoint;
  QUrl m_documentUrl;
  QString m_token;
  QString m_documentPath;
  QString m_editorRoot;
  std::shared_ptr<QTemporaryDir> m_saves;
  std::shared_ptr<std::atomic_bool> m_verificationCancelled;
  quint64 m_generation = 0;
  int m_saveSerial = 0;
};
