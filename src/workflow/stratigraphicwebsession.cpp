// 层：功能
#include "stratigraphicwebsession.h"

#include <QDir>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QSettings>
#include <QStandardPaths>
#ifdef Q_OS_UNIX
#include <csignal>
#include <unistd.h>
#endif

StratigraphicWebSession::StratigraphicWebSession(QObject *parent) : QObject(parent)
{
  QSettings settings(QStringLiteral("paleo"), QStringLiteral("paleo"));
  m_endpoint = QUrl(settings.value(QStringLiteral("correlationWeb/url"),
      QStringLiteral("http://127.0.0.1:8771/web_prototype/workspace.html")).toString());
  m_directory = qEnvironmentVariable("PALEO_CORRELATION_PROJECT_DIR",
      settings.value(QStringLiteral("correlationWeb/projectDirectory")).toString());
  m_python = qEnvironmentVariable("PALEO_CORRELATION_PYTHON",
      settings.value(QStringLiteral("correlationWeb/pythonExecutable")).toString());
  const QString endpoint = qEnvironmentVariable("PALEO_CORRELATION_URL");
  if (!endpoint.isEmpty())
    m_endpoint = QUrl(endpoint);
  m_retry.setSingleShot(true);
  m_retry.setInterval(250);
  connect(&m_retry, &QTimer::timeout, this, &StratigraphicWebSession::probe);
  m_process.setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_UNIX
  // 独立进程组包含该服务拉起的计算 worker，关闭宿主时一并回收。
  m_process.setChildProcessModifier([] { ::setsid(); });
#endif
  connect(&m_process, &QProcess::readyReadStandardOutput, this, [this] {
    m_output = (m_output + m_process.readAllStandardOutput()).right(4096);
  });
  connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart)
      fail(tr("无法启动独立服务：%1").arg(m_process.errorString()));
  });
  connect(&m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
      [this](int code, QProcess::ExitStatus) {
    m_output = (m_output + m_process.readAllStandardOutput()).right(4096);
    if (m_endpoint == m_ownedEndpoint)
      fail(tr("独立服务已退出（%1）。%2").arg(code).arg(QString::fromUtf8(m_output).trimmed()));
  });
}

StratigraphicWebSession::~StratigraphicWebSession()
{
  m_retry.stop();
  if (m_reply)
  {
    m_reply->disconnect(this);
    m_reply->abort();
  }
  m_process.disconnect(this);
  // 只回收本会话启动的进程；连接的已有服务从不交给 QProcess。
  if (ownsService())
  {
#ifdef Q_OS_UNIX
    const auto group = static_cast<pid_t>(m_process.processId());
    if (group > 0)
      ::kill(-group, SIGINT); // Python 正常执行 finally / Application.close
    if (!m_process.waitForFinished(1000))
    {
      if (group > 0)
        ::kill(-group, SIGKILL);
      m_process.kill();
      m_process.waitForFinished(1000);
    }
#else
    m_process.terminate();
    if (!m_process.waitForFinished(1000))
    {
      m_process.kill();
      m_process.waitForFinished(1000);
    }
#endif
  }
}

bool StratigraphicWebSession::setEndpoint(const QString &text)
{
  if (m_busy)
    return false;
  QString value = text.trimmed();
  if (!value.contains(QStringLiteral("://")) && !value.isEmpty())
    value.prepend(QStringLiteral("http://"));
  QUrl url(value);
  if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() ||
      (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https")))
  {
    emit failed(tr("请输入有效的 http:// 或 https:// 服务地址"));
    return false;
  }
  if (url.path().isEmpty() || url.path() == QLatin1String("/"))
    url.setPath(QStringLiteral("/web_prototype/workspace.html"));
  m_endpoint = url;
  saveConfiguration();
  return true;
}

void StratigraphicWebSession::setProjectDirectory(const QString &directory)
{
  if (!m_busy)
  {
    m_directory = directory;
    saveConfiguration();
  }
}

void StratigraphicWebSession::setPythonExecutable(const QString &executable)
{
  if (!m_busy)
  {
    m_python = executable;
    saveConfiguration();
  }
}

void StratigraphicWebSession::saveConfiguration()
{
  // 机器路径只进用户配置，不写 .paleo/QGIS 工程或仓库。
  QSettings settings(QStringLiteral("paleo"), QStringLiteral("paleo"));
  settings.setValue(QStringLiteral("correlationWeb/url"), m_endpoint.toString());
  settings.setValue(QStringLiteral("correlationWeb/projectDirectory"), m_directory);
  settings.setValue(QStringLiteral("correlationWeb/pythonExecutable"), m_python);
}

void StratigraphicWebSession::setBusy(bool busy)
{
  if (m_busy == busy)
    return;
  m_busy = busy;
  emit busyChanged(busy);
}

void StratigraphicWebSession::connectToService() { begin(false); }
void StratigraphicWebSession::startLocalService() { begin(true); }

void StratigraphicWebSession::begin(bool startLocal)
{
  if (m_busy || !setEndpoint(m_endpoint.toString()))
    return;
  if (startLocal && (m_endpoint.scheme() != QLatin1String("http") ||
      (m_endpoint.host() != QLatin1String("127.0.0.1") &&
       m_endpoint.host() != QLatin1String("localhost") &&
       m_endpoint.host() != QLatin1String("::1"))))
  {
    emit failed(tr("启动服务需要本机 http 地址；远程服务请使用「连接 / 刷新」"));
    return;
  }
  m_startLocal = startLocal;
  m_launched = ownsService() && m_ownedEndpoint == m_endpoint;
  m_startTime.start();
  setBusy(true);
  emit statusChanged(tr("正在连接地层对比工作台…"));
  probe();
}

void StratigraphicWebSession::probe()
{
  if (!m_busy || m_reply)
    return;
  QNetworkRequest request(m_endpoint);
  request.setTransferTimeout(2000);
  // 每次探测检查服务本身，不拿磁盘缓存冒充可用。
  request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
  m_reply = m_network.get(request);
  QNetworkReply *reply = m_reply;
  connect(reply, &QNetworkReply::finished, this, [this, reply] {
    const bool ok = reply->error() == QNetworkReply::NoError &&
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200;
    const QString reason = reply->errorString();
    m_reply = nullptr;
    reply->deleteLater();
    if (!m_busy)
      return;
    if (ok)
    {
      setBusy(false);
      emit statusChanged(tr("已连接地层对比工作台"));
      emit ready(m_endpoint);
    }
    else if (m_startLocal && !m_launched)
      launch();
    else if (m_startLocal && m_startTime.elapsed() < 30000)
      m_retry.start();
    else
      fail(tr("无法连接地层对比服务：%1。请检查地址，或选择独立项目后启动服务。")
          .arg(reason));
  });
}

void StratigraphicWebSession::launch()
{
  const QDir project(m_directory);
  if (m_directory.isEmpty() || !QFileInfo::exists(project.filePath(QStringLiteral("run.py"))) ||
      !QFileInfo::exists(project.filePath(QStringLiteral("web_prototype/workspace.html"))))
  {
    fail(tr("请先选择包含 run.py 和 web_prototype/workspace.html 的独立项目目录"));
    return;
  }
  QString python = m_python;
  if (python.isEmpty())
  {
#ifdef Q_OS_WIN
    const QString candidate = project.absoluteFilePath(QStringLiteral(".venv/Scripts/python.exe"));
#else
    const QString candidate = project.absoluteFilePath(QStringLiteral(".venv/bin/python"));
#endif
    python = QFileInfo(candidate).isExecutable() ? candidate
        : QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (python.isEmpty())
      python = QStandardPaths::findExecutable(QStringLiteral("python"));
  }
  if (python.isEmpty())
  {
    fail(tr("未找到 Python；请在 ribbon 中选择独立项目使用的 Python"));
    return;
  }
  m_output.clear();
  if (ownsService())
  {
    fail(tr("已启动的本机服务在另一个地址运行，请连接原地址或重启程序后启动新服务"));
    return;
  }
  m_launched = true;
  m_ownedEndpoint = m_endpoint;
  m_process.setWorkingDirectory(project.absolutePath());
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
  environment.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
  m_process.setProcessEnvironment(environment);
  m_process.start(python, {QStringLiteral("-B"), project.absoluteFilePath(QStringLiteral("run.py")),
      QStringLiteral("--host"), m_endpoint.host(), QStringLiteral("--port"),
      QString::number(m_endpoint.port(80))});
  emit statusChanged(tr("正在启动独立地层对比服务…"));
  m_retry.start();
}

void StratigraphicWebSession::fail(const QString &reason)
{
  m_retry.stop();
  setBusy(false);
  if (m_reply)
  {
    m_reply->disconnect(this);
    m_reply->abort();
    m_reply->deleteLater();
    m_reply = nullptr;
  }
  emit failed(reason);
}
