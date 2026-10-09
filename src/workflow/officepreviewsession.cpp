// 层：功能
#include "officepreviewsession.h"
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtConcurrent>

OfficePreviewSession::OfficePreviewSession(QObject *parent) : QObject(parent)
{
  m_deadline.setSingleShot(true);
  connect(&m_deadline, &QTimer::timeout, this, [this] { fail(tr("Office 预览超时，请重试")); });
}
OfficePreviewSession::~OfficePreviewSession() { stop(); }
bool OfficePreviewSession::supports(const QString &path)
{
  return QStringList{"doc", "docx", "xls", "xlsx", "ppt", "pptx"}.contains(QFileInfo(path).suffix().toLower());
}
QString OfficePreviewSession::rendererPath()
{
  const QString override = qEnvironmentVariable("PALEO_OFFICE_RENDERER");
  if (!override.isEmpty()) return QFileInfo(override).isExecutable() ? override : QString();
#ifdef Q_OS_WIN
  const QString binary = QStringLiteral("paleo_office_renderer.exe");
#else
  const QString binary = QStringLiteral("paleo_office_renderer");
#endif
  const QDir app(QCoreApplication::applicationDirPath());
  const QStringList candidates = {app.filePath(binary), app.filePath("../vendor/calligra/bin/" + binary),
                                 app.filePath("../libexec/" + binary)};
  for (const QString &candidate : candidates)
    if (QFileInfo(candidate).isExecutable()) return QFileInfo(candidate).absoluteFilePath();
  return {};
}
void OfficePreviewSession::open(const QString &path, const QString &expectedSha)
{
  stop();
  if (!supports(path)) { fail(tr("不支持的 Office 文件格式")); return; }
  if (rendererPath().isEmpty()) { fail(tr("Office 预览组件未安装，请联系管理员安装文档预览组件")); return; }
  const quint64 generation = m_generation;
  m_verificationCancelled = std::make_shared<std::atomic_bool>(false);
  const auto cancelled = m_verificationCancelled;
  auto *watcher = new QFutureWatcher<QString>(this);
  connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation, path] {
    const QString error = watcher->result();
    watcher->deleteLater();
    if (generation != m_generation) return;
    if (!error.isEmpty()) { fail(error); return; }
    launch(path);
  });
  watcher->setFuture(QtConcurrent::run([path, expectedSha, cancelled] {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return tr("无法读取 Office 原件：%1").arg(path);
    if (!expectedSha.isEmpty()) {
      QCryptographicHash hash(QCryptographicHash::Sha256);
      while (!file.atEnd()) {
        if (cancelled->load()) return QString();
        const QByteArray block = file.read(1024 * 1024);
        if (block.isEmpty() && file.error() != QFileDevice::NoError) return tr("无法读取 Office 原件：%1").arg(path);
        hash.addData(block);
      }
      if (QString::fromLatin1(hash.result().toHex()).compare(expectedSha, Qt::CaseInsensitive))
        return tr("Office 原件与入库时的 SHA-256 不一致，请重新导入");
    }
    return QString();
  }));
}
void OfficePreviewSession::launch(const QString &path)
{
  m_directory = std::make_shared<QTemporaryDir>(QDir::tempPath() + "/paleo-office-XXXXXX");
  if (!m_directory->isValid()) { fail(tr("无法创建 Office 预览临时目录")); return; }
  const QString temporaryFiles = m_directory->path() + "/tmp";
  if (!QDir().mkpath(temporaryFiles)) { fail(tr("无法创建 Office 预览临时目录")); return; }
  auto *process = new QProcess(this);
  m_process = process;
  const QString renderer = rendererPath();
  const QDir prefix(QFileInfo(renderer).dir().absoluteFilePath(".."));
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  env.insert("QT_QPA_PLATFORM", "offscreen");
  env.insert("QT_PLUGIN_PATH", prefix.filePath("lib/plugins") + QDir::listSeparator() + env.value("QT_PLUGIN_PATH"));
  env.insert("XDG_DATA_DIRS", prefix.filePath("share") + QDir::listSeparator() + env.value("XDG_DATA_DIRS", "/usr/local/share:/usr/share"));
  env.insert("XDG_CONFIG_HOME", m_directory->path() + "/config");
  env.insert("XDG_CACHE_HOME", m_directory->path() + "/cache");
  env.insert("XDG_DATA_HOME", m_directory->path() + "/data");
  env.insert("TMPDIR", temporaryFiles);
  env.insert("TMP", temporaryFiles);
  env.insert("TEMP", temporaryFiles);
  process->setProcessEnvironment(env);
  connect(process, &QProcess::readyReadStandardError, this, [process] { process->readAllStandardError(); });
  connect(process, &QProcess::readyReadStandardOutput, this, &OfficePreviewSession::readMessages);
  connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) fail(tr("Calligra 预览组件启动失败"));
  });
  connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
          [this](int code, QProcess::ExitStatus) { fail(tr("Office 预览组件已退出（%1）").arg(code)); });
  m_deadline.start(125000);
  process->start(renderer, {QFileInfo(path).absoluteFilePath(), m_directory->path()});
}
void OfficePreviewSession::requestPage(int index)
{
  if (!m_process || index < 0 || index >= m_pageCount) return;
  m_requestedPage = index;
  if (m_inFlight >= 0) return; // coalesce navigation; one decode at a time
  m_inFlight = index;
  const QJsonObject message{{"type", "page"}, {"index", index}, {"request", ++m_request}};
  m_deadline.start(60000);
  m_process->write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}
void OfficePreviewSession::readMessages()
{
  if (!m_process) return;
  m_messages += m_process->readAllStandardOutput();
  if (m_messages.size() > 1024 * 1024) { fail(tr("预览组件返回的数据异常")); return; }
  while (m_messages.contains('\n')) {
    const qsizetype end = m_messages.indexOf('\n');
    const QJsonObject message = QJsonDocument::fromJson(m_messages.left(end)).object();
    m_messages.remove(0, end + 1);
    const QString type = message.value("type").toString();
    if (type == "error") { fail(message.value("message").toString()); return; }
    if (type == "ready") {
      if (m_pageCount) { fail(tr("预览组件重复返回文档信息")); return; }
      QStringList labels;
      const QJsonArray pages = message.value("pages").toArray();
      if (pages.isEmpty() || pages.size() > 4000) { fail(tr("Office 页数无效")); return; }
      for (const auto &label : pages) labels.append(label.toString());
      m_pageCount = labels.size();
      m_deadline.stop();
      emit ready(labels);
      return; // receivers may stop/reopen this session
    }
    if (type != "page") continue;
    const int index = message.value("index").toInt(-1);
    if (index != m_inFlight || message.value("request").toInt(-1) != m_request) {
      fail(tr("预览组件返回的页码异常")); return;
    }
    const QString file = QStringLiteral("page-%1.png").arg(index);
    if (message.value("file").toString() != file) { fail(tr("预览页面路径无效")); return; }
    const auto directory = m_directory;
    const auto generation = m_generation;
    auto *watcher = new QFutureWatcher<QImage>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation, index] {
      const QImage image = watcher->result();
      watcher->deleteLater();
      if (generation != m_generation) return;
      m_deadline.stop();
      m_inFlight = -1;
      if (image.isNull()) { fail(tr("无法读取预览页面")); return; }
      if (index == m_requestedPage) emit pageReady(index, image);
      if (generation == m_generation && index != m_requestedPage) requestPage(m_requestedPage);
    });
    watcher->setFuture(QtConcurrent::run([directory, file] {
      QImageReader reader(QDir(directory->path()).filePath(file));
      const QSize size = reader.size();
      if (size.isEmpty() || size.width() > 2200 || size.height() > 2200) return QImage();
      const QImage image = reader.read();
      QFile::remove(reader.fileName());
      return image;
    }));
  }
}
void OfficePreviewSession::fail(const QString &reason) { stop(); emit failed(reason); }
void OfficePreviewSession::stop()
{
  ++m_generation;
  if (m_verificationCancelled) m_verificationCancelled->store(true);
  m_verificationCancelled.reset();
  m_deadline.stop(); m_messages.clear(); m_pageCount = 0;
  m_request = 0; m_inFlight = -1; m_requestedPage = -1;
  if (auto *process = m_process.data()) {
    m_process = nullptr;
    process->disconnect(this);
    process->setParent(QCoreApplication::instance());
    const auto directory = m_directory;
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), process,
            [process, directory] { process->deleteLater(); });
    if (process->state() == QProcess::NotRunning) process->deleteLater();
    else process->kill(); // closing a tab never waits for the engine
  }
  m_directory.reset();
}
