// 层：功能
#include "officepreviewsession.h"
#include "../catalog/datacatalog.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrlQuery>
#include <QtConcurrent>

namespace {
QString hashFile(const QString &path, const std::shared_ptr<std::atomic_bool> &cancelled, QString *error)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
  {
    *error = QStringLiteral("read");
    return {};
  }
  QCryptographicHash hash(QCryptographicHash::Sha256);
  while (!file.atEnd())
  {
    if (cancelled->load()) return {};
    const QByteArray block = file.read(1024 * 1024);
    if (block.isEmpty() && file.error() != QFileDevice::NoError)
    {
      *error = QStringLiteral("read");
      return {};
    }
    hash.addData(block);
  }
  return QString::fromLatin1(hash.result().toHex());
}

QByteArray mimeFor(const QString &path)
{
  const QString ext = QFileInfo(path).suffix().toLower();
  if (ext == QLatin1String("html") || ext == QLatin1String("htm")) return "text/html; charset=utf-8";
  if (ext == QLatin1String("js") || ext == QLatin1String("mjs")) return "text/javascript; charset=utf-8";
  if (ext == QLatin1String("css")) return "text/css; charset=utf-8";
  if (ext == QLatin1String("json")) return "application/json";
  if (ext == QLatin1String("wasm")) return "application/wasm";
  if (ext == QLatin1String("svg")) return "image/svg+xml";
  if (ext == QLatin1String("png")) return "image/png";
  if (ext == QLatin1String("jpg") || ext == QLatin1String("jpeg")) return "image/jpeg";
  if (ext == QLatin1String("gif")) return "image/gif";
  if (ext == QLatin1String("woff")) return "font/woff";
  if (ext == QLatin1String("woff2")) return "font/woff2";
  if (ext == QLatin1String("ttf")) return "font/ttf";
  if (ext == QLatin1String("otf")) return "font/otf";
  if (ext == QLatin1String("docx")) return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
  if (ext == QLatin1String("xlsx")) return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
  if (ext == QLatin1String("pptx")) return "application/vnd.openxmlformats-officedocument.presentationml.presentation";
  if (ext == QLatin1String("doc")) return "application/msword";
  if (ext == QLatin1String("xls")) return "application/vnd.ms-excel";
  if (ext == QLatin1String("ppt")) return "application/vnd.ms-powerpoint";
  if (ext == QLatin1String("map")) return "application/json";
  return "application/octet-stream";
}

QByteArray headerValue(const QList<QPair<QByteArray, QByteArray>> &headers, const QByteArray &name)
{
  for (const auto &header : headers)
    if (header.first.toLower() == name.toLower()) return header.second.trimmed();
  return {};
}

bool safeSegments(const QString &path, QStringList *parts)
{
  if (path.contains(QLatin1Char('\\')) || path.contains(QChar(QChar::Null))) return false;
  const QStringList raw = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
  for (const QString &part : raw)
  {
    if (part == QLatin1String(".") || part == QLatin1String("..") || part.contains(QLatin1String("..")))
      return false;
  }
  *parts = raw;
  return true;
}
} // namespace

OfficePreviewSession::OfficePreviewSession(QObject *parent) : QObject(parent) {}
OfficePreviewSession::~OfficePreviewSession() { stop(); }

bool OfficePreviewSession::supports(const QString &path)
{
  return QStringList{"doc", "docx", "xls", "xlsx", "ppt", "pptx"}.contains(QFileInfo(path).suffix().toLower());
}

QString OfficePreviewSession::editorRoot()
{
  const auto valid = [](const QString &dir) {
    return QFileInfo(QDir(dir).filePath(QStringLiteral("index.html"))).isFile()
               ? QFileInfo(dir).absoluteFilePath()
               : QString();
  };
  const QString override = qEnvironmentVariable("PALEO_OFFICE_EDITOR");
  if (!override.isEmpty()) return valid(override);
  const QDir app(QCoreApplication::applicationDirPath());
  for (const QString &candidate : {app.filePath(QStringLiteral("office-editor")),
                                  app.filePath(QStringLiteral("../vendor/ranuts-document")),
                                  app.filePath(QStringLiteral("../libexec/office-editor"))})
  {
    const QString hit = valid(candidate);
    if (!hit.isEmpty()) return hit;
  }
  return {};
}

void OfficePreviewSession::open(const QString &path, const QString &expectedSha)
{
  stop();
  if (!supports(path))
  {
    fail(tr("不支持的 Office 文件格式"));
    return;
  }
  m_editorRoot = editorRoot();
  if (m_editorRoot.isEmpty())
  {
    fail(tr("Office 编辑组件未安装，请联系管理员安装文档编辑组件"));
    return;
  }
  const quint64 generation = m_generation;
  m_verificationCancelled = std::make_shared<std::atomic_bool>(false);
  const auto cancelled = m_verificationCancelled;
  auto *watcher = new QFutureWatcher<QString>(this);
  connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation, path] {
    const QString error = watcher->result();
    watcher->deleteLater();
    if (generation != m_generation) return;
    if (error == QLatin1String("read"))
      fail(tr("无法读取 Office 原件：%1").arg(path));
    else if (error == QLatin1String("sha"))
      fail(tr("Office 原件与入库时的 SHA-256 不一致，请重新导入"));
    else
      launch(path);
  });
  watcher->setFuture(QtConcurrent::run([path, expectedSha, cancelled] {
    QString error;
    const QString digest = hashFile(path, cancelled, &error);
    if (!error.isEmpty()) return error;
    if (cancelled->load()) return QString();
    if (!expectedSha.isEmpty() && digest.compare(expectedSha, Qt::CaseInsensitive) != 0)
      return QStringLiteral("sha");
    return QString();
  }));
}

void OfficePreviewSession::launch(const QString &path)
{
  m_documentPath = QFileInfo(path).absoluteFilePath();
  if (!DataCatalog::isSafePathSegment(QFileInfo(m_documentPath).fileName()))
  {
    fail(tr("Office 文件名不能作为本机地址的一段"));
    return;
  }
  m_saves = std::make_shared<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/paleo-office-edit-XXXXXX"));
  if (!m_saves->isValid())
  {
    fail(tr("无法创建 Office 编辑临时目录"));
    return;
  }
  m_token.clear();
  for (int i = 0; i < 4; ++i)
    m_token += QString::number(QRandomGenerator::system()->generate(), 16).rightJustified(8, QLatin1Char('0'));
  m_server = new QTcpServer(this);
  connect(m_server, &QTcpServer::newConnection, this, &OfficePreviewSession::acceptConnection);
  if (!m_server->listen(QHostAddress::LocalHost, 0))
  {
    fail(tr("无法在本机打开 Office 编辑页"));
    return;
  }
  const QString origin = QStringLiteral("http://127.0.0.1:%1").arg(m_server->serverPort());
  m_documentUrl = QUrl(origin + QStringLiteral("/paleo-doc/") + m_token + QLatin1Char('/')
                       + QString::fromUtf8(QUrl::toPercentEncoding(QFileInfo(m_documentPath).fileName())));
  m_endpoint = QUrl(origin + QStringLiteral("/paleo-host.html"));
  emit ready(m_endpoint);
}

QString OfficePreviewSession::hostPage() const
{
  const QJsonArray args{m_token, m_documentUrl.toString(QUrl::FullyEncoded)};
  const QString injected = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
  const QString editorSrc = QStringLiteral("/editor?embed=1&amp;locale=zh-CN&amp;src=%1")
                                .arg(QString::fromUtf8(QUrl::toPercentEncoding(m_documentUrl.toString(QUrl::FullyEncoded))));
  return QStringLiteral(
             "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>Office</title>"
             "<style>html,body,iframe{margin:0;border:0;width:100%;height:100%}</style></head><body>"
             "<iframe id=\"ed\" src=\"%2\"></iframe><script>"
             "const cfg=%1;const token=cfg[0];"
             "window.addEventListener('message',(event)=>{"
             "if(event.origin!==location.origin||!event.data||typeof event.data!=='object')return;"
             "const file=event.data.payload&&event.data.payload.file;"
             "if(event.data.type==='document:saved'&&file)"
             "fetch('/paleo-save/'+token,{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:file});"
             "});</script></body></html>")
      .arg(injected, editorSrc);
}

void OfficePreviewSession::acceptConnection()
{
  while (m_server && m_server->hasPendingConnections())
  {
    QTcpSocket *socket = m_server->nextPendingConnection();
    socket->setParent(m_server);
    socket->setProperty("buf", QByteArray());
    connect(socket, &QTcpSocket::readyRead, this, [this, socket] { readSocket(socket); });
  }
}

void OfficePreviewSession::readSocket(QTcpSocket *socket)
{
  if (!socket) return;
  QByteArray buf = socket->property("buf").toByteArray();
  buf += socket->readAll();
  const int headerEnd = buf.indexOf("\r\n\r\n");
  if (headerEnd < 0)
  {
    if (buf.size() > 16384)
    {
      respond(socket, 431, "Headers Too Large", "text/plain; charset=utf-8", "headers");
      return;
    }
    socket->setProperty("buf", buf);
    return;
  }
  const QByteArray head = buf.left(headerEnd);
  const QList<QByteArray> lines = head.split('\n');
  if (lines.isEmpty())
  {
    respond(socket, 400, "Bad Request", "text/plain; charset=utf-8", "request");
    return;
  }
  const QList<QByteArray> request = lines.first().trimmed().split(' ');
  if (request.size() < 2)
  {
    respond(socket, 400, "Bad Request", "text/plain; charset=utf-8", "request");
    return;
  }
  QList<QPair<QByteArray, QByteArray>> headers;
  for (int i = 1; i < lines.size(); ++i)
  {
    const QByteArray line = lines.at(i).trimmed();
    const int colon = line.indexOf(':');
    if (colon > 0) headers.append({line.left(colon).trimmed(), line.mid(colon + 1).trimmed()});
  }
  const int length = headerValue(headers, "content-length").toInt();
  if (length < 0 || length > 256 * 1024 * 1024)
  {
    respond(socket, 413, "Payload Too Large", "text/plain; charset=utf-8", "body");
    return;
  }
  if (buf.size() - (headerEnd + 4) < length)
  {
    if (headerValue(headers, "expect").toLower() == "100-continue" && !socket->property("continued").toBool())
    {
      socket->setProperty("continued", true);
      socket->write("HTTP/1.1 100 Continue\r\n\r\n");
    }
    socket->setProperty("buf", buf);
    return;
  }
  const QByteArray body = buf.mid(headerEnd + 4, length);
  handleRequest(socket, request.first(), request.at(1), headers, body);
}

void OfficePreviewSession::handleRequest(QTcpSocket *socket, const QByteArray &method, const QByteArray &target,
                                        const QList<QPair<QByteArray, QByteArray>> &headers, const QByteArray &body)
{
  const QUrl url = QUrl::fromEncoded(target);
  QStringList parts;
  if (!safeSegments(url.path(), &parts))
  {
    respond(socket, 404, "Not Found", "text/plain; charset=utf-8", "missing");
    return;
  }
  const bool get = method == "GET" || method == "HEAD";
  // 编辑器自带的 Service Worker 会在第一次打开后接管来源。空的
  // document_editor_service_worker.js 接班后，下一次打开停在加载。
  if (get && parts.size() == 1
      && (parts.first() == QLatin1String("sw.js")
          || parts.first() == QLatin1String("document_editor_service_worker.js")))
  {
    respond(socket, 404, "Not Found", "text/plain; charset=utf-8", "missing");
    return;
  }
  if (parts.size() == 1 && parts.first() == QLatin1String("paleo-host.html") && get)
  {
    respond(socket, 200, "OK", "text/html; charset=utf-8", hostPage().toUtf8());
    return;
  }
  if (parts.size() == 3 && parts.at(0) == QLatin1String("paleo-doc") && get)
  {
    if (parts.at(1) != m_token || parts.at(2) != QFileInfo(m_documentPath).fileName())
    {
      respond(socket, 404, "Not Found", "text/plain; charset=utf-8", "missing");
      return;
    }
    respondFile(socket, m_documentPath, mimeFor(m_documentPath), headers);
    return;
  }
  if (method == "POST" && parts.size() == 2 && parts.at(0) == QLatin1String("paleo-save") && parts.at(1) == m_token)
  {
    if (!m_saves)
    {
      respond(socket, 404, "Not Found", "text/plain; charset=utf-8", "missing");
      return;
    }
    const QString name = QStringLiteral("save-%1.%2").arg(++m_saveSerial).arg(QFileInfo(m_documentPath).suffix());
    const QString saved = m_saves->filePath(name);
    QFile file(saved);
    if (!file.open(QIODevice::WriteOnly) || file.write(body) != body.size())
    {
      respond(socket, 500, "Save Failed", "text/plain; charset=utf-8", "save");
      return;
    }
    file.close();
    respond(socket, 204, "No Content", "text/plain; charset=utf-8", {});
    emit documentSaved(saved);
    return;
  }
  if (method == "POST" && parts.size() == 3 && parts.at(0) == QLatin1String("paleo-event") && parts.at(1) == m_token)
  {
    respond(socket, 204, "No Content", "text/plain; charset=utf-8", {});
    return;
  }
  if (!get)
  {
    respond(socket, 405, "Method Not Allowed", "text/plain; charset=utf-8", "method");
    return;
  }
  const QString root = QFileInfo(m_editorRoot).canonicalFilePath();
  QString relative = parts.join(QLatin1Char('/'));
  QString clean = QDir::cleanPath(QDir(root).filePath(relative));
  if (!(clean == root || clean.startsWith(root + QLatin1Char('/'))))
  {
    respond(socket, 404, "Not Found", "text/plain; charset=utf-8", "missing");
    return;
  }
  if (!QFileInfo(clean).isFile())
  {
    if (!parts.isEmpty() && parts.first() == QLatin1String("editor"))
      clean = QDir(root).filePath(QStringLiteral("editor.html"));
    else if (parts.isEmpty() || QFileInfo(clean).isDir())
      clean = QDir(root).filePath(QStringLiteral("index.html"));
  }
  const QFileInfo info(clean);
  const QString canonical = info.exists() ? info.canonicalFilePath() : QString();
  if (canonical.isEmpty() || !(canonical == root || canonical.startsWith(root + QLatin1Char('/'))))
  {
    respond(socket, 404, "Not Found", "text/plain; charset=utf-8", "missing");
    return;
  }
  respondFile(socket, canonical, mimeFor(canonical), headers);
}

void OfficePreviewSession::respond(QTcpSocket *socket, int status, const QByteArray &reason, const QByteArray &type,
                                  const QByteArray &body, const QByteArray &extra)
{
  if (!socket) return;
  QByteArray head = "HTTP/1.1 " + QByteArray::number(status) + " " + reason + "\r\n";
  head += "Content-Type: " + type + "\r\n";
  head += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
  head += "Connection: close\r\n";
  head += "Cache-Control: no-store\r\n";
  head += "Cross-Origin-Resource-Policy: same-origin\r\n";
  head += "Cross-Origin-Opener-Policy: same-origin\r\n";
  head += "Cross-Origin-Embedder-Policy: require-corp\r\n";
  head += extra;
  head += "\r\n";
  socket->write(head);
  socket->write(body);
  socket->disconnectFromHost();
}

void OfficePreviewSession::respondFile(QTcpSocket *socket, const QString &path, const QByteArray &type,
                                      const QList<QPair<QByteArray, QByteArray>> &headers)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
  {
    respond(socket, 404, "Not Found", "text/plain; charset=utf-8", "missing");
    return;
  }
  const qint64 size = file.size();
  qint64 start = 0;
  qint64 end = size > 0 ? size - 1 : 0;
  bool partial = false;
  const QByteArray range = headerValue(headers, "range");
  if (range.startsWith("bytes=") && size > 0)
  {
    const QList<QByteArray> bounds = range.mid(6).split('-');
    if (bounds.size() == 2)
    {
      bool okStart = false;
      bool okEnd = false;
      const qint64 parsedStart = bounds.first().isEmpty() ? 0 : bounds.first().toLongLong(&okStart);
      const qint64 parsedEnd = bounds.last().isEmpty() ? size - 1 : bounds.last().toLongLong(&okEnd);
      if ((bounds.first().isEmpty() || okStart) && (bounds.last().isEmpty() || okEnd) && parsedStart >= 0
          && parsedEnd >= parsedStart && parsedStart < size)
      {
        start = parsedStart;
        end = qMin(parsedEnd, size - 1);
        partial = true;
      }
      else
      {
        respond(socket, 416, "Range Not Satisfiable", "text/plain; charset=utf-8", "range",
                "Content-Range: bytes */" + QByteArray::number(size) + "\r\n");
        return;
      }
    }
  }
  if (!file.seek(start))
  {
    respond(socket, 404, "Not Found", "text/plain; charset=utf-8", "missing");
    return;
  }
  const qint64 length = size == 0 ? 0 : end - start + 1;
  const QByteArray body = file.read(length);
  QByteArray extra;
  int status = 200;
  QByteArray reason = "OK";
  if (partial)
  {
    status = 206;
    reason = "Partial Content";
    extra = "Content-Range: bytes " + QByteArray::number(start) + "-" + QByteArray::number(end) + "/"
            + QByteArray::number(size) + "\r\n";
    extra += "Accept-Ranges: bytes\r\n";
  }
  else
    extra = "Accept-Ranges: bytes\r\n";
  respond(socket, status, reason, type, body, extra);
}

void OfficePreviewSession::fail(const QString &reason)
{
  stop();
  emit failed(reason);
}

void OfficePreviewSession::stop()
{
  ++m_generation;
  if (m_verificationCancelled) m_verificationCancelled->store(true);
  m_token.clear();
  m_documentPath.clear();
  m_documentUrl.clear();
  m_endpoint.clear();
  m_saves.reset();
  if (m_server)
  {
    m_server->close();
    m_server->deleteLater();
    m_server = nullptr;
  }
}

bool OfficePreviewSession::commitEdit(DataCatalog *catalog, const QString &assetId, const QString &parentVersionId,
                                      const QString &savedFile, QString *error)
{
  if (!catalog || !catalog->isOpen())
  {
    if (error) *error = tr("工程目录未打开，编辑结果未登记");
    return false;
  }
  const CatalogVersion parent = catalog->versionById(parentVersionId);
  if (parent.id.isEmpty() || parent.assetId != assetId)
  {
    if (error) *error = tr("找不到要挂接的原件版本");
    return false;
  }
  if (!QFileInfo(savedFile).isFile())
  {
    if (error) *error = tr("编辑结果文件不存在");
    return false;
  }
  QString fileName = parent.fileName.isEmpty() ? QFileInfo(savedFile).fileName() : parent.fileName;
  if (!DataCatalog::isSafePathSegment(fileName))
    fileName = QStringLiteral("edit.") + QFileInfo(parent.fileName).suffix().toLower();
  if (!DataCatalog::isSafePathSegment(fileName))
  {
    if (error) *error = tr("编辑结果的文件名不能写入工程目录");
    return false;
  }
  int number = 0;
  for (const CatalogVersion &version : catalog->versionsForAsset(assetId))
    number = qMax(number, version.versionNumber);
  CatalogVersion version;
  version.id = catalog->nextVersionId();
  version.assetId = assetId;
  version.stage = QStringLiteral("DERIVED");
  version.versionNumber = number + 1;
  version.managed = true;
  version.fileName = fileName;
  version.path = QStringLiteral("artifacts/")
                 + DataCatalog::managedPath(QStringLiteral("derived"), assetId, version.id, fileName);
  version.sourceUri = QStringLiteral("office-edit");
  version.parentVersionIds = {parentVersionId};
  version.extra.insert(QStringLiteral("producer"), QStringLiteral("ranuts-document"));
  version.extra.insert(QStringLiteral("sourceVersion"), parentVersionId);
  if (version.path.endsWith(QLatin1Char('/')) || version.path.isEmpty())
  {
    if (error) *error = tr("编辑结果的受管路径无效");
    return false;
  }
  const QString absolute = QDir(catalog->projectDir()).absoluteFilePath(version.path);
  QDir().mkpath(QFileInfo(absolute).absolutePath());
  if (QFile::exists(absolute)) QFile::remove(absolute);
  if (!QFile::copy(savedFile, absolute))
  {
    if (error) *error = tr("无法把编辑结果写入工程目录");
    return false;
  }
  QString shaError;
  version.sha256 = DataCatalog::sha256FileHex(absolute, &shaError);
  if (version.sha256.isEmpty() || !catalog->addVersion(version, error))
  {
    QFile::remove(absolute);
    if (error && error->isEmpty()) *error = shaError;
    return false;
  }
  QFile::setPermissions(absolute, QFileDevice::ReadOwner | QFileDevice::ReadUser | QFileDevice::ReadGroup
                                     | QFileDevice::ReadOther);
  return true;
}
